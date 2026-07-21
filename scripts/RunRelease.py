from __future__ import annotations

import argparse
import platform
import sys
import time
from pathlib import Path

sys.dont_write_bytecode = True
sys.pycache_prefix = str(Path(__file__).resolve().parent.parent / "out" / "pycache")

from _common import (  # noqa: E402
    TimedStage,
    repository_root,
    run_command,
    run_timed_stages,
    validate_parallel_jobs,
)
from _deploy import (  # noqa: E402
    validate_absolute_host_path,
    validate_host_port,
    validate_non_overlapping_host_roots,
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Verify InpxWebReader and package the same Docker image as a Linux host release bundle."
    )
    parser.add_argument("--host-source-root", required=True)
    parser.add_argument("--host-app-root", required=True)
    parser.add_argument("--host-port", type=int, default=8080)
    parser.add_argument("--image-tag", required=True)
    parser.add_argument("--docker-platform", default="linux/amd64")
    parser.add_argument("--parallel-jobs", type=int)
    parser.add_argument("--build-jobs", type=int)
    parser.add_argument("--test-jobs", type=int)
    parser.add_argument("--smoke-book-count", type=int)
    parser.add_argument("--token-file", type=Path)
    parser.add_argument("--converter-version", default="latest")
    parser.add_argument("--converter-asset-name", default="fbc-linux-amd64.zip")
    parser.add_argument("--skip-converter-download", action="store_true")
    parser.add_argument("--skip-e2e", action="store_true")
    parser.add_argument("--git-commit", default="unknown")
    parser.add_argument("--git-dirty", action="store_true")
    return parser.parse_args()


def ensure_linux_host() -> None:
    if sys.platform != "linux":
        raise RuntimeError("RunRelease.py is a Linux worker. Use RunRemoteLinux.py release from the development host.")
    if platform.machine().lower() not in {"x86_64", "amd64"}:
        raise RuntimeError("RunRelease.py requires an x86_64 Linux worker.")


def append_option(command: list[str], name: str, value: object | None) -> None:
    if value is not None:
        command.extend([name, str(value)])


def build_linux_test_command(args: argparse.Namespace) -> list[str]:
    command = [
        sys.executable,
        "scripts/RunLinuxTests.py",
        "--image-tag",
        args.image_tag,
        "--docker-platform",
        args.docker_platform,
    ]
    append_option(command, "--parallel-jobs", args.parallel_jobs)
    append_option(command, "--build-jobs", args.build_jobs)
    append_option(command, "--test-jobs", args.test_jobs)
    append_option(command, "--smoke-book-count", args.smoke_book_count)
    return command


def build_bundle_command(args: argparse.Namespace) -> list[str]:
    command = [
        sys.executable,
        "scripts/PrepareDeployBundle.py",
        "--host-source-root",
        args.host_source_root,
        "--host-app-root",
        args.host_app_root,
        "--host-port",
        str(args.host_port),
        "--image-tag",
        args.image_tag,
        "--docker-platform",
        args.docker_platform,
        "--converter-version",
        args.converter_version,
        "--converter-asset-name",
        args.converter_asset_name,
        "--git-commit",
        args.git_commit,
        "--reuse-image",
    ]
    append_option(command, "--build-jobs", args.build_jobs)
    append_option(command, "--token-file", args.token_file)
    if args.skip_converter_download:
        command.append("--skip-converter-download")
    if args.git_dirty:
        command.append("--git-dirty")
    return command


def main() -> int:
    started_at = time.perf_counter()
    args = parse_args()
    ensure_linux_host()
    args.host_source_root = validate_absolute_host_path(args.host_source_root, "--host-source-root")
    args.host_app_root = validate_absolute_host_path(args.host_app_root, "--host-app-root")
    validate_non_overlapping_host_roots(args.host_source_root, args.host_app_root)
    validate_host_port(args.host_port)
    if args.token_file is not None and not args.token_file.is_file():
        raise RuntimeError(f"--token-file does not exist: {args.token_file}")
    for job_count in (args.parallel_jobs, args.build_jobs, args.test_jobs):
        if job_count is not None:
            validate_parallel_jobs(job_count)

    repo_root = repository_root()
    python_executable = sys.executable
    stages = [
        TimedStage(
            "Run Python script tests",
            lambda: run_command([python_executable, "scripts/RunScriptTests.py"], repo_root),
        ),
        TimedStage(
            "Run Python static analysis",
            lambda: run_command(
                [python_executable, "scripts/RunStaticAnalysis.py", "--skip-clang-tidy"],
                repo_root,
            ),
        ),
        TimedStage(
            "Build web UI",
            lambda: run_command([python_executable, "scripts/RunWebUi.py", "build"], repo_root),
        ),
        TimedStage(
            "Run web UI tests",
            lambda: run_command([python_executable, "scripts/RunWebUi.py", "test"], repo_root),
        ),
    ]
    if not args.skip_e2e:
        stages.append(
            TimedStage(
                "Run real-server browser E2E",
                lambda: run_command([python_executable, "scripts/RunWebUi.py", "test:e2e"], repo_root),
            )
        )
    stages.extend(
        [
            TimedStage(
                "Run native and Docker verification",
                lambda: run_command(build_linux_test_command(args), repo_root),
            ),
            TimedStage(
                "Package verified Linux host bundle",
                lambda: run_command(build_bundle_command(args), repo_root),
            ),
        ]
    )

    run_timed_stages(
        stages,
        total_started_at=started_at,
        total_label="Total RunRelease.py time",
        success_message="Verified Linux host release bundle prepared successfully.",
        clock=time.perf_counter,
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
