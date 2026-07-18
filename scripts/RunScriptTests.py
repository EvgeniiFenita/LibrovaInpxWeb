from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.dont_write_bytecode = True
sys.pycache_prefix = str(Path(__file__).resolve().parent.parent / "out" / "pycache")

from _common import (
    build_unittest_discovery_command,
    python_no_bytecode_environment,
    repository_root,
    run_command,
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run the repository Python script unittest suite."
    )
    parser.add_argument(
        "--pattern",
        help="unittest discovery pattern.",
    )
    parser.add_argument(
        "--python-exe",
        nargs="+",
        default=[sys.executable],
        help="Python command used to run unittest discovery.",
    )
    return parser.parse_args()


def discovery_pattern(args: argparse.Namespace) -> str:
    return args.pattern or "Test*.py"


def main() -> int:
    args = parse_args()
    repo_root = repository_root()
    env = python_no_bytecode_environment(repo_root)
    pattern = discovery_pattern(args)

    print(f"==> Running Python script tests ({pattern})", flush=True)
    run_command(
        build_unittest_discovery_command(args.python_exe, pattern),
        cwd=repo_root,
        env=env,
    )
    print("==> Python script tests completed successfully.", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
