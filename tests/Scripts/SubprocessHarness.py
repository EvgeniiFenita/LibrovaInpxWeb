from __future__ import annotations

import importlib.util
import os
import shutil
import subprocess
import sys
from pathlib import Path
from types import ModuleType


def write_fake_common(scripts_dir: Path, default_preset: str = "test-debug") -> None:
    (scripts_dir / "_common.py").write_text(
        f"""
from __future__ import annotations

import os
import shutil
import subprocess
import time
from pathlib import Path
from typing import Callable


class TimedStage:
    def __init__(self, title: str, action: Callable[[], None]) -> None:
        self.title = title
        self.action = action


def configure_console_streams() -> None:
    pass


def repository_root() -> Path:
    return Path(__file__).resolve().parents[1]


def out_root(repo_root: Path | None = None) -> Path:
    return (repo_root if repo_root is not None else repository_root()) / "out"


def is_under_path(candidate: Path, root: Path) -> bool:
    try:
        candidate.resolve(strict=False).relative_to(root.resolve(strict=False))
    except ValueError:
        return False
    return True


def default_parallel_jobs() -> int:
    return 2


def default_preset() -> str:
    return {default_preset!r}


def uses_multi_config_build(_preset: str) -> bool:
    return False


def build_ctest_command(
    build_root: Path,
    configuration: str,
    parallel_jobs: int,
    multi_config: bool,
    label_regexes: list[str] | tuple[str, ...] = (),
    test_regex: str = "",
    exclude_label_regexes: list[str] | tuple[str, ...] = (),
) -> list[str]:
    command = [
        "ctest",
        "--test-dir",
        str(build_root),
        "--output-on-failure",
        "--no-tests=error",
        "-j",
        str(parallel_jobs),
    ]
    if multi_config:
        command.extend(["-C", configuration])
    for label_regex in label_regexes:
        command.extend(["-L", label_regex])
    if test_regex:
        command.extend(["-R", test_regex])
    for label_regex in exclude_label_regexes:
        command.extend(["-LE", label_regex])
    return command


def build_cmake_configure_command(preset: str, definitions: list[str] | tuple[str, ...] = ()) -> list[str]:
    return [
        "cmake",
        "--preset",
        preset,
        *definitions,
    ]


def build_cmake_build_command(
    preset: str,
    configuration: str,
    parallel_jobs: int,
    multi_config: bool,
    targets: list[str] | tuple[str, ...] = (),
) -> list[str]:
    command = [
        "cmake",
        "--build",
        "--preset",
        preset,
    ]
    if targets:
        command.extend(["--target", *targets])
    command.extend(["--parallel", str(parallel_jobs)])
    if multi_config:
        command.extend(["--config", configuration])
    return command


def validate_parallel_jobs(parallel_jobs: int) -> None:
    if parallel_jobs < 1:
        raise RuntimeError("--parallel-jobs must be at least 1.")


def format_duration(seconds: float) -> str:
    if seconds < 60:
        return f"{{seconds:.2f}}s"

    minutes, remaining_seconds = divmod(seconds, 60)
    if minutes < 60:
        return f"{{int(minutes)}}m {{remaining_seconds:05.2f}}s"

    hours, remaining_minutes = divmod(minutes, 60)
    return f"{{int(hours)}}h {{int(remaining_minutes):02d}}m {{remaining_seconds:05.2f}}s"


def run_timed_stage(
    stage_index: int,
    stage_count: int,
    title: str,
    action: Callable[[], None],
    clock: Callable[[], float] = time.perf_counter,
) -> float:
    print("", flush=True)
    print(f"==> [{{stage_index}}/{{stage_count}}] {{title}}", flush=True)

    started_at = clock()
    try:
        action()
    except Exception:
        elapsed = clock() - started_at
        print(f"==> FAILED: {{title}} after {{format_duration(elapsed)}}", flush=True)
        raise

    elapsed = clock() - started_at
    print(f"==> DONE: {{title}} in {{format_duration(elapsed)}}", flush=True)
    return elapsed


def run_timed_stages(
    stages: list[TimedStage],
    *,
    total_started_at: float | None = None,
    total_label: str = "Total time",
    success_message: str = "All requested checks completed successfully.",
    clock: Callable[[], float] = time.perf_counter,
) -> list[tuple[str, float]]:
    if total_started_at is None:
        total_started_at = clock()

    stage_timings: list[tuple[str, float]] = []
    stage_count = len(stages)
    for index, stage in enumerate(stages, start=1):
        elapsed = run_timed_stage(index, stage_count, stage.title, stage.action, clock)
        stage_timings.append((stage.title, elapsed))

    total_elapsed = clock() - total_started_at
    print("", flush=True)
    print("==> Stage summary", flush=True)
    for title, elapsed in stage_timings:
        print(f"    {{title}}: {{format_duration(elapsed)}}", flush=True)
    print(f"==> {{total_label}}: {{format_duration(total_elapsed)}}", flush=True)
    print(f"==> {{success_message}}", flush=True)
    return stage_timings


def set_out_scoped_path(env: dict[str, str], key: str, default_path: Path, root: Path) -> None:
    current_value = env.get(key)
    if current_value and is_under_path(Path(current_value), root):
        return
    env[key] = str(default_path)


def build_out_tool_environment(repo_root: Path, lane_name: str, env: dict[str, str] | None = None) -> dict[str, str]:
    lane_root = out_root(repo_root) / lane_name
    temp_root = lane_root / "tmp"
    cache_root = lane_root / "cache"
    temp_root.mkdir(parents=True, exist_ok=True)
    cache_root.mkdir(parents=True, exist_ok=True)

    result = out_scoped_environment(repo_root, env)
    result["TMPDIR"] = str(temp_root)
    result["TEMP"] = str(temp_root)
    result["TMP"] = str(temp_root)
    result["XDG_CACHE_HOME"] = str(cache_root)
    return result


def out_scoped_environment(repo_root: Path | None = None, env: dict[str, str] | None = None) -> dict[str, str]:
    root = out_root(repo_root)
    temp_root = root / "tmp"
    cache_root = root / "cache"
    vcpkg_root = root / "vcpkg"
    vcpkg_downloads = vcpkg_root / "downloads"
    vcpkg_binary_cache = vcpkg_root / "binary-cache"

    for directory in (temp_root, cache_root, vcpkg_downloads, vcpkg_binary_cache):
        directory.mkdir(parents=True, exist_ok=True)

    result = dict(env) if env is not None else os.environ.copy()
    set_out_scoped_path(result, "TMPDIR", temp_root, root)
    set_out_scoped_path(result, "TEMP", temp_root, root)
    set_out_scoped_path(result, "TMP", temp_root, root)
    set_out_scoped_path(result, "XDG_CACHE_HOME", cache_root, root)
    result["VCPKG_DOWNLOADS"] = str(vcpkg_downloads)
    result["VCPKG_DEFAULT_BINARY_CACHE"] = str(vcpkg_binary_cache)
    result["VCPKG_BINARY_SOURCES"] = f"clear;files,{{vcpkg_binary_cache}},readwrite"
    return result


def python_no_bytecode_environment(
    repo_root: Path | None = None,
    env: dict[str, str] | None = None,
) -> dict[str, str]:
    root = out_root(repo_root)
    pycache_root = root / "pycache"
    pycache_root.mkdir(parents=True, exist_ok=True)

    result = out_scoped_environment(repo_root, env)
    result["PYTHONDONTWRITEBYTECODE"] = "1"
    result["PYTHONPYCACHEPREFIX"] = str(pycache_root)
    return result


def build_unittest_discovery_command(
    python_exe: list[str],
    pattern: str,
    start_dir: str = "tests/Scripts",
) -> list[str]:
    return [
        *python_exe,
        "-m",
        "unittest",
        "discover",
        "-s",
        start_dir,
        "-p",
        pattern,
        "-v",
    ]


def resolve_under_root(
    path: Path,
    root: Path,
    *,
    path_description: str = "Path",
    root_description: str = "root",
    allow_root: bool = True,
) -> Path:
    resolved_path = path.resolve(strict=False)
    resolved_root = root.resolve(strict=False)
    try:
        relative_path = resolved_path.relative_to(resolved_root)
    except ValueError as exc:
        raise RuntimeError(
            f"{{path_description}} must stay under {{root_description}}: {{resolved_root}} "
            f"(got outside path {{resolved_path}})"
        ) from exc

    if not allow_root and not relative_path.parts:
        raise RuntimeError(f"{{path_description}} must be a child of {{root_description}}: {{resolved_root}}")

    return resolved_path


def resolve_removal_path_under_root(
    path: Path,
    root: Path,
    *,
    path_description: str = "Path",
    root_description: str = "root",
) -> Path:
    removal_path = Path(os.path.abspath(path))
    resolved_parent = removal_path.parent.resolve(strict=False)
    resolved_root = root.resolve(strict=False)

    try:
        resolved_parent.relative_to(resolved_root)
    except ValueError as exc:
        raise RuntimeError(
            f"{{path_description}} must stay under {{root_description}}: {{resolved_root}} "
            f"(got outside path {{removal_path}})"
        ) from exc

    return removal_path


def remove_path_under_root(
    path: Path,
    root: Path,
    *,
    path_description: str = "Path",
    root_description: str = "root",
) -> None:
    removal_path = resolve_removal_path_under_root(
        path,
        root,
        path_description=path_description,
        root_description=root_description,
    )
    if removal_path.is_symlink() or removal_path.is_file():
        removal_path.unlink()
        return

    if removal_path.is_dir():
        shutil.rmtree(removal_path)
        return

    if not removal_path.exists():
        return

    removal_path.unlink()


def resolve_command(command: list[str]) -> list[str]:
    if not command:
        raise RuntimeError("Command must not be empty.")

    return command


def run_command(command: list[str], cwd: Path, env: dict[str, str] | None = None) -> None:
    subprocess.run(
        resolve_command(command),
        cwd=cwd,
        env=out_scoped_environment(env=env),
        check=True,
        stderr=subprocess.STDOUT,
    )


def docker_platform_arguments(platform_name: str) -> list[str]:
    return ["--platform", platform_name] if platform_name else []


def check_docker_engine(cwd: Path, env: dict[str, str] | None = None) -> str | None:
    if shutil.which("docker") is None:
        return "Docker CLI was not found on PATH."

    completed = subprocess.run(
        ["docker", "version", "--format", "{{{{.Server.Version}}}}"],
        cwd=cwd,
        env=out_scoped_environment(env=env),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        check=False,
    )
    if completed.returncode != 0:
        return f"Docker CLI is installed, but the Docker engine is not reachable.\\n{{completed.stdout.strip()}}"

    return None


def ensure_docker_engine(
    cwd: Path,
    env: dict[str, str] | None = None,
) -> None:
    error = check_docker_engine(cwd, env)
    if error is not None:
        raise RuntimeError(error)


""".lstrip(),
        encoding="utf-8",
    )


def copy_entry_script(source_script: Path, isolated_repo: Path) -> Path:
    scripts_dir = isolated_repo / "scripts"
    scripts_dir.mkdir(parents=True, exist_ok=True)
    destination = scripts_dir / source_script.name
    shutil.copy2(source_script, destination)
    server_config = source_script.parent / "_server_config.py"
    if server_config.exists():
        shutil.copy2(server_config, scripts_dir / "_server_config.py")
    remote_linux = source_script.parent / "_remote_linux.py"
    if remote_linux.exists():
        shutil.copy2(remote_linux, scripts_dir / "_remote_linux.py")
    deploy_helpers = source_script.parent / "_deploy.py"
    if deploy_helpers.exists():
        shutil.copy2(deploy_helpers, scripts_dir / "_deploy.py")
    write_fake_common(scripts_dir)
    return destination


def load_script_module(source_script: Path, module_name: str) -> ModuleType:
    spec = importlib.util.spec_from_file_location(module_name, source_script)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"Could not load test module from {source_script}")

    module = importlib.util.module_from_spec(spec)
    previous_module = sys.modules.get(module_name)
    previous_dont_write_bytecode = sys.dont_write_bytecode
    scripts_dir = str(source_script.parent)
    inserted_scripts_dir = scripts_dir not in sys.path
    sys.dont_write_bytecode = True
    if inserted_scripts_dir:
        sys.path.insert(0, scripts_dir)
    sys.modules[module_name] = module
    try:
        spec.loader.exec_module(module)
    finally:
        sys.dont_write_bytecode = previous_dont_write_bytecode
        if inserted_scripts_dir:
            sys.path.remove(scripts_dir)
        if previous_module is not None:
            sys.modules[module_name] = previous_module
        else:
            sys.modules.pop(module_name, None)
    return module


def run_copied_entry_script(
    source_script: Path,
    isolated_repo: Path,
    args: list[str] | tuple[str, ...],
) -> subprocess.CompletedProcess[str]:
    copy_entry_script(source_script, isolated_repo)
    scripts_dir = isolated_repo / "scripts"

    env = dict(os.environ)
    env.pop("PYTHONDONTWRITEBYTECODE", None)

    return subprocess.run(
        [sys.executable, source_script.name, *args],
        cwd=scripts_dir,
        capture_output=True,
        text=True,
        check=True,
        env=env,
    )


def run_entry_script_help(source_script: Path, isolated_repo: Path) -> subprocess.CompletedProcess[str]:
    return run_copied_entry_script(source_script, isolated_repo, ("--help",))
