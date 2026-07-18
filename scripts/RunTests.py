from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

sys.dont_write_bytecode = True
sys.pycache_prefix = str(Path(__file__).resolve().parent.parent / "out" / "pycache")

from _common import (  # noqa: E402
    TimedStage,
    build_cmake_build_command,
    build_cmake_configure_command,
    build_ctest_command,
    default_parallel_jobs,
    repository_root,
    run_command,
    run_timed_stages,
    validate_parallel_jobs,
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Build and test InpxWebReader on Linux.")
    parser.add_argument("--preset", default="linux-debug")
    parser.add_argument("--parallel-jobs", type=int, default=default_parallel_jobs())
    parser.add_argument("--skip-configure", action="store_true")
    parser.add_argument("--skip-native", action="store_true")
    parser.add_argument("--skip-script-tests", action="store_true")
    parser.add_argument("--skip-web-tests", action="store_true")
    parser.add_argument("--build-target", action="append", default=[])
    parser.add_argument("--ctest-label", action="append", default=[])
    parser.add_argument("--ctest-test-regex", default="")
    return parser.parse_args()


def ensure_linux_host() -> None:
    if sys.platform != "linux":
        raise RuntimeError("RunTests.py is a Linux worker. Use RunRemoteLinux.py test from the development host.")


def main() -> int:
    started_at = time.perf_counter()
    args = parse_args()
    ensure_linux_host()
    validate_parallel_jobs(args.parallel_jobs)

    repo_root = repository_root()
    scripts_dir = Path(__file__).resolve().parent
    stages: list[TimedStage] = []

    if not args.skip_native:
        if not args.skip_configure:
            stages.append(TimedStage(
                f"Configure Linux build ({args.preset})",
                lambda: run_command(build_cmake_configure_command(args.preset), repo_root),
            ))
        stages.append(TimedStage(
            "Build native targets",
            lambda: run_command(
                build_cmake_build_command(
                    args.preset,
                    "Debug",
                    args.parallel_jobs,
                    False,
                    targets=args.build_target,
                ),
                repo_root,
            ),
        ))
        stages.append(TimedStage(
            "Run native tests",
            lambda: run_command(
                build_ctest_command(
                    repo_root / "out" / "build" / args.preset,
                    "Debug",
                    args.parallel_jobs,
                    False,
                    label_regexes=args.ctest_label,
                    test_regex=args.ctest_test_regex,
                ),
                repo_root,
            ),
        ))

    if not args.skip_script_tests:
        stages.append(TimedStage(
            "Run Python script tests",
            lambda: run_command([sys.executable, str(scripts_dir / "RunScriptTests.py")], repo_root),
        ))

    if not args.skip_web_tests:
        stages.append(TimedStage(
            "Build web UI",
            lambda: run_command([sys.executable, str(scripts_dir / "RunWebUi.py"), "build"], repo_root),
        ))
        stages.append(TimedStage(
            "Run web UI tests",
            lambda: run_command([sys.executable, str(scripts_dir / "RunWebUi.py"), "test"], repo_root),
        ))

    run_timed_stages(
        stages,
        total_started_at=started_at,
        total_label="Total RunTests.py time",
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
