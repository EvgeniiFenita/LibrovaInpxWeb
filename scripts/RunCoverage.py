from __future__ import annotations

import argparse
import shutil
import sys
from pathlib import Path

sys.dont_write_bytecode = True
sys.pycache_prefix = str(Path(__file__).resolve().parent.parent / "out" / "pycache")

from _common import (  # noqa: E402
    TimedStage,
    build_cmake_build_command,
    build_cmake_configure_command,
    build_ctest_command,
    build_out_tool_environment,
    default_parallel_jobs,
    out_root,
    remove_path_under_root,
    repository_root,
    run_command,
    run_timed_stages,
    validate_parallel_jobs,
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Build, test, and report native Linux line coverage.")
    parser.add_argument("--preset", default="linux-coverage")
    parser.add_argument("--parallel-jobs", type=int, default=default_parallel_jobs())
    parser.add_argument("--skip-configure", action="store_true")
    parser.add_argument("--ctest-label", action="append", default=[])
    parser.add_argument("--ctest-test-regex", default="")
    return parser.parse_args()


def ensure_linux_host() -> None:
    if sys.platform != "linux":
        raise RuntimeError("RunCoverage.py is supported only on Linux.")


def require_executable(name: str) -> str:
    executable = shutil.which(name)
    if executable is None:
        raise RuntimeError(f"Required coverage executable was not found in PATH: {name}")
    return executable


def prepare_report_root(repo_root: Path) -> Path:
    report_root = out_root(repo_root) / "reports" / "coverage" / "native"
    if report_root.exists():
        remove_path_under_root(
            report_root,
            out_root(repo_root),
            path_description="Native coverage report root",
            root_description="repository out directory",
        )
    report_root.mkdir(parents=True)
    return report_root


def capture_coverage(repo_root: Path, build_root: Path, report_root: Path, env: dict[str, str]) -> None:
    lcov = require_executable("lcov")
    genhtml = require_executable("genhtml")
    temporary_root = Path(env["TMPDIR"])
    raw_info = report_root / "native-raw.info"
    filtered_info = report_root / "native.info"
    run_command(
        [
            lcov,
            "--capture",
            "--directory",
            str(build_root),
            "--output-file",
            str(raw_info),
            "--tempdir",
            str(temporary_root),
            "--ignore-errors",
            "mismatch,negative,inconsistent",
        ],
        repo_root,
        env,
    )
    run_command(
        [
            lcov,
            "--remove",
            str(raw_info),
            "/usr/*",
            "*/vcpkg_installed/*",
            "*/tests/*",
            "--output-file",
            str(filtered_info),
            "--tempdir",
            str(temporary_root),
            "--ignore-errors",
            "unused,inconsistent",
        ],
        repo_root,
        env,
    )
    run_command([lcov, "--summary", str(filtered_info)], repo_root, env)
    run_command(
        [
            genhtml,
            str(filtered_info),
            "--output-directory",
            str(report_root / "html"),
            "--tempdir",
            str(temporary_root),
        ],
        repo_root,
        env,
    )


def reset_coverage_counters(repo_root: Path, build_root: Path, env: dict[str, str]) -> None:
    run_command(
        [require_executable("lcov"), "--zerocounters", "--directory", str(build_root)],
        repo_root,
        env,
    )


def main() -> int:
    args = parse_args()
    ensure_linux_host()
    validate_parallel_jobs(args.parallel_jobs)
    repo_root = repository_root()
    build_root = repo_root / "out" / "build" / args.preset
    report_root = prepare_report_root(repo_root)
    env = build_out_tool_environment(repo_root, "coverage")
    stages: list[TimedStage] = []
    if not args.skip_configure:
        stages.append(TimedStage(
            f"Configure coverage build ({args.preset})",
            lambda: run_command(build_cmake_configure_command(args.preset), repo_root, env),
        ))
    stages.append(TimedStage(
        "Discard stale coverage counters",
        lambda: reset_coverage_counters(repo_root, build_root, env),
    ))
    stages.append(TimedStage(
        "Build coverage test target",
        lambda: run_command(
            build_cmake_build_command(
                args.preset,
                "Debug",
                args.parallel_jobs,
                False,
                targets=("InpxWebReaderCoreTests",),
            ),
            repo_root,
            env,
        ),
    ))
    stages.append(TimedStage(
        "Reset coverage counters after test discovery",
        lambda: reset_coverage_counters(repo_root, build_root, env),
    ))
    stages.append(TimedStage(
        "Run coverage tests",
        lambda: run_command(
            build_ctest_command(
                build_root,
                "Debug",
                args.parallel_jobs,
                False,
                label_regexes=args.ctest_label,
                test_regex=args.ctest_test_regex,
            ),
            repo_root,
            env,
        ),
    ))
    stages.append(TimedStage(
        "Capture native coverage",
        lambda: capture_coverage(repo_root, build_root, report_root, env),
    ))
    run_timed_stages(stages, total_label="Total RunCoverage.py time")
    print(f"Native coverage report: {report_root / 'html' / 'index.html'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
