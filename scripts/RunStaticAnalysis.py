from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import shutil
import subprocess
import sys
from pathlib import Path

sys.dont_write_bytecode = True
sys.pycache_prefix = str(Path(__file__).resolve().parent.parent / "out" / "pycache")

from _common import (  # noqa: E402
    TimedStage,
    build_cmake_configure_command,
    build_out_tool_environment,
    default_parallel_jobs,
    repository_root,
    run_command,
    run_timed_stages,
    validate_parallel_jobs,
)

SOURCE_ROOTS = ("apps/InpxWebReader/", "libs/")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run Linux C++ and Python static analysis.")
    parser.add_argument("--preset", default="linux-analysis")
    parser.add_argument("--parallel-jobs", type=int, default=default_parallel_jobs())
    parser.add_argument("--skip-configure", action="store_true")
    parser.add_argument("--skip-clang-tidy", action="store_true")
    parser.add_argument("--skip-python", action="store_true")
    parser.add_argument("--clang-tidy-exe", default="clang-tidy")
    parser.add_argument("--ruff-exe", default="ruff")
    return parser.parse_args()


def ensure_linux_host() -> None:
    if sys.platform != "linux":
        raise RuntimeError("RunStaticAnalysis.py is supported only on Linux.")


def require_executable(name: str) -> str:
    executable = shutil.which(name)
    if executable is None:
        raise RuntimeError(f"Required executable was not found in PATH: {name}")
    return executable


def load_translation_units(repo_root: Path, preset: str) -> tuple[Path, list[Path]]:
    compile_commands = repo_root / "out" / "build" / preset / "compile_commands.json"
    if not compile_commands.is_file():
        raise RuntimeError(f"compile_commands.json was not found: {compile_commands}")

    units: list[Path] = []
    seen: set[Path] = set()
    for entry in json.loads(compile_commands.read_text(encoding="utf-8")):
        source = Path(entry["file"]).resolve()
        try:
            relative = source.relative_to(repo_root.resolve()).as_posix()
        except ValueError:
            continue
        if source.suffix != ".cpp" or not relative.startswith(SOURCE_ROOTS) or source in seen:
            continue
        seen.add(source)
        units.append(source)
    if not units:
        raise RuntimeError("No production C++ translation units were found for clang-tidy.")
    return compile_commands, units


def run_clang_tidy(repo_root: Path, preset: str, executable: str, parallel_jobs: int, env: dict[str, str]) -> None:
    compile_commands, units = load_translation_units(repo_root, preset)

    def analyze(source: Path) -> tuple[Path, subprocess.CompletedProcess[str]]:
        completed = subprocess.run(
            [executable, "--quiet", "-p", str(compile_commands.parent), str(source)],
            cwd=repo_root,
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            errors="replace",
            check=False,
        )
        return source, completed

    failures: list[Path] = []
    with ThreadPoolExecutor(max_workers=min(parallel_jobs, len(units))) as executor:
        for source, completed in executor.map(analyze, units):
            output = completed.stdout or ""
            if output:
                print(output, end="" if output.endswith("\n") else "\n")
            if completed.returncode != 0:
                failures.append(source)
    if failures:
        raise RuntimeError(f"clang-tidy failed for {len(failures)} translation unit(s).")


def main() -> int:
    args = parse_args()
    ensure_linux_host()
    validate_parallel_jobs(args.parallel_jobs)
    repo_root = repository_root()
    env = build_out_tool_environment(repo_root, "analysis")
    stages: list[TimedStage] = []

    if not args.skip_clang_tidy and not args.skip_configure:
        stages.append(TimedStage(
            f"Configure analysis build ({args.preset})",
            lambda: run_command(build_cmake_configure_command(args.preset), repo_root, env),
        ))
    if not args.skip_clang_tidy:
        stages.append(TimedStage(
            "Run clang-tidy",
            lambda: run_clang_tidy(
                repo_root,
                args.preset,
                require_executable(args.clang_tidy_exe),
                args.parallel_jobs,
                env,
            ),
        ))
    if not args.skip_python:
        stages.append(TimedStage(
            "Run ruff",
            lambda: run_command(
                [require_executable(args.ruff_exe), "check", "scripts", "tests/Scripts"],
                repo_root,
                env,
            ),
        ))
    run_timed_stages(stages, total_label="Total RunStaticAnalysis.py time")
    return 0


if __name__ == "__main__":
    sys.exit(main())
