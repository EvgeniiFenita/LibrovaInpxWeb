from __future__ import annotations

import argparse
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
    repository_root,
    run_command,
    run_timed_stages,
    validate_parallel_jobs,
)


SUPPORTED_SANITIZER_PRESETS = frozenset({"linux-asan", "linux-tsan"})
TSAN_SUPPRESSIONS_PATH = Path("scripts/tsan-suppressions.txt")
TSAN_DEFAULT_TIME_ZONE = ":/etc/localtime"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run a Linux sanitizer test lane.")
    parser.add_argument("--preset", default="linux-asan")
    parser.add_argument("--parallel-jobs", type=int, default=default_parallel_jobs())
    parser.add_argument("--skip-configure", action="store_true")
    parser.add_argument("--build-target", action="append", default=[])
    parser.add_argument("--ctest-label", action="append", default=[])
    parser.add_argument("--ctest-test-regex", default="")
    return parser.parse_args()


def ensure_linux_host() -> None:
    if sys.platform != "linux":
        raise RuntimeError("RunSanitizers.py is supported only on Linux.")


def validate_sanitizer_preset(preset: str) -> None:
    if preset not in SUPPORTED_SANITIZER_PRESETS:
        supported = ", ".join(sorted(SUPPORTED_SANITIZER_PRESETS))
        raise RuntimeError(f"--preset must select an instrumented sanitizer preset: {supported}.")


def configure_tsan_environment(env: dict[str, str], repo_root: Path) -> None:
    suppression_path = (repo_root / TSAN_SUPPRESSIONS_PATH).resolve()
    if not suppression_path.is_file():
        raise RuntimeError(f"TSan suppression file was not found: {suppression_path}")
    # libzip converts ZIP timestamps through mktime. With TZ absent, concurrent
    # glibc calls repeatedly replace default-zone state and report a libc-only
    # malloc/free race before TSan can inspect application code. Name the same
    # system zone explicitly while preserving any caller-selected time zone.
    env.setdefault("TZ", TSAN_DEFAULT_TIME_ZONE)
    env["TSAN_OPTIONS"] = f"halt_on_error=1:suppressions={suppression_path}"


def main() -> int:
    args = parse_args()
    ensure_linux_host()
    validate_sanitizer_preset(args.preset)
    validate_parallel_jobs(args.parallel_jobs)
    repo_root = repository_root()
    env = build_out_tool_environment(repo_root, "sanitizers")
    if args.preset == "linux-tsan":
        configure_tsan_environment(env, repo_root)
    stages: list[TimedStage] = []

    if not args.skip_configure:
        stages.append(TimedStage(
            f"Configure sanitizer build ({args.preset})",
            lambda: run_command(build_cmake_configure_command(args.preset), repo_root, env),
        ))
    stages.append(TimedStage(
        "Build sanitizer targets",
        lambda: run_command(
            build_cmake_build_command(
                args.preset,
                "Debug",
                args.parallel_jobs,
                False,
                targets=args.build_target,
            ),
            repo_root,
            env,
        ),
    ))
    stages.append(TimedStage(
        "Run sanitizer tests",
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
            env,
        ),
    ))
    run_timed_stages(stages, total_label="Total RunSanitizers.py time")
    return 0


if __name__ == "__main__":
    sys.exit(main())
