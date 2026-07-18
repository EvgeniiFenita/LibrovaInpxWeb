from __future__ import annotations

import argparse
import contextlib
import hashlib
import json
import os
import shutil
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request
from collections.abc import Iterator
from pathlib import Path

sys.dont_write_bytecode = True
sys.pycache_prefix = str(Path(__file__).resolve().parent.parent / "out" / "pycache")

from _common import (
    build_cmake_build_command,
    build_cmake_configure_command,
    default_parallel_jobs,
    out_scoped_environment,
    remove_path_under_root,
    repository_root,
    resolve_under_root,
    run_command,
)


WEB_SOURCE_RELATIVE_ROOT = Path("web") / "inpx-web-reader"
WEB_OUT_RELATIVE_ROOT = Path("out") / "web" / "inpx-web-reader"
WEB_WORKSPACE_NAME = "workspace"
NPM_CACHE_NAME = "npm-cache"
NPM_STAMP_NAME = ".npm-ci.sha256"
E2E_SERVER_WORKSPACE_NAME = "real-server"
E2E_AUTH_TOKEN = "browser-server-contract-token"
PLAYWRIGHT_IMAGE_SUFFIX = "-noble"

SYNC_DIRECTORIES = ("public", "src", "tests", "tools")
SYNC_FILES = (
    "index.html",
    "package.json",
    "package-lock.json",
    "playwright.config.ts",
    "tsconfig.json",
    "vite.config.ts",
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run InpxWebReader UI tooling from an out/ workspace."
    )
    parser.add_argument(
        "--artifacts-root",
        help="Artifacts root under repository out/. Defaults to out/web/inpx-web-reader.",
    )
    parser.add_argument(
        "command",
        choices=("dev", "build", "test", "test:coverage", "test:e2e", "audit"),
        help="Web UI command to run.",
    )
    parser.add_argument(
        "tool_args",
        nargs=argparse.REMAINDER,
        help="Arguments forwarded to Vite, Vitest, Playwright, or npm audit.",
    )
    return parser.parse_args()


def source_root(repo_root: Path) -> Path:
    return repo_root / WEB_SOURCE_RELATIVE_ROOT


def out_root(repo_root: Path) -> Path:
    return repo_root / WEB_OUT_RELATIVE_ROOT


def resolve_artifacts_root(repo_root: Path, artifacts_root_arg: str | None) -> Path:
    if not artifacts_root_arg:
        return out_root(repo_root)

    artifacts_root = Path(artifacts_root_arg)
    if not artifacts_root.is_absolute():
        artifacts_root = repo_root / artifacts_root

    return resolve_under_root(
        artifacts_root,
        repo_root / "out",
        path_description="Web UI artifacts root",
        root_description="repository out directory",
    )


def workspace_root(repo_root: Path, artifacts_root: Path | None = None) -> Path:
    return (artifacts_root if artifacts_root is not None else out_root(repo_root)) / WEB_WORKSPACE_NAME


def ensure_within(path: Path, root: Path) -> Path:
    return resolve_under_root(
        path,
        root,
        path_description="Path",
        root_description=f"required root {root}",
    )


def remove_workspace_path(path: Path, workspace: Path) -> None:
    remove_path_under_root(
        path,
        workspace,
        path_description="Workspace path",
        root_description=f"web workspace {workspace}",
    )


def copy_directory(source: Path, destination: Path, workspace: Path) -> None:
    if not source.exists():
        return

    remove_workspace_path(destination, workspace)
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(source, destination)


def copy_file(source: Path, destination: Path, workspace: Path) -> None:
    if not source.exists():
        raise RuntimeError(f"Required web UI source file is missing: {source}")

    ensure_within(destination, workspace)
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)


def sync_workspace(repo_root: Path, artifacts_root: Path | None = None) -> Path:
    source = source_root(repo_root)
    workspace = workspace_root(repo_root, artifacts_root)
    workspace.mkdir(parents=True, exist_ok=True)

    for directory_name in SYNC_DIRECTORIES:
        copy_directory(source / directory_name, workspace / directory_name, workspace)

    for file_name in SYNC_FILES:
        copy_file(source / file_name, workspace / file_name, workspace)

    return workspace


def hash_file(hasher: "hashlib._Hash", path: Path) -> None:
    hasher.update(path.name.encode("utf-8"))
    hasher.update(b"\0")
    hasher.update(path.read_bytes())
    hasher.update(b"\0")


def dependency_fingerprint(workspace: Path) -> str:
    hasher = hashlib.sha256()
    hash_file(hasher, workspace / "package.json")
    hash_file(hasher, workspace / "package-lock.json")
    return hasher.hexdigest()


def build_environment(repo_root: Path, artifacts: Path) -> dict[str, str]:
    npm_cache = artifacts / NPM_CACHE_NAME
    temp_root = artifacts / "tmp"
    cache_root = artifacts / "cache"
    for directory in (npm_cache, temp_root, cache_root):
        directory.mkdir(parents=True, exist_ok=True)

    env = out_scoped_environment(repo_root)
    env["INPX_WEB_READER_WEB_ARTIFACTS_ROOT"] = str(artifacts)
    env["npm_config_cache"] = str(npm_cache)
    env["npm_config_update_notifier"] = "false"
    env["TMPDIR"] = str(temp_root)
    env["TEMP"] = str(temp_root)
    env["TMP"] = str(temp_root)
    env["XDG_CACHE_HOME"] = str(cache_root)
    return env


def run(command: list[str], cwd: Path, env: dict[str, str]) -> None:
    run_command(command, cwd, env=env)


def ensure_dependencies(repo_root: Path, workspace: Path, env: dict[str, str]) -> None:
    stamp_path = workspace.parent / NPM_STAMP_NAME
    current_fingerprint = dependency_fingerprint(workspace)
    node_modules = workspace / "node_modules"

    if node_modules.exists() and stamp_path.exists() and stamp_path.read_text(encoding="utf-8") == current_fingerprint:
        return

    print("==> Installing web UI dependencies under out/", flush=True)
    run(["npm", "ci"], workspace, env)
    stamp_path.write_text(current_fingerprint, encoding="utf-8")


def npx_command(tool: str, *args: str) -> list[str]:
    return ["npx", "--no-install", tool, *args]


def playwright_container_image(workspace: Path) -> str:
    lock_data = json.loads((workspace / "package-lock.json").read_text(encoding="utf-8"))
    try:
        version = lock_data["packages"]["node_modules/@playwright/test"]["version"]
    except (KeyError, TypeError) as exc:
        raise RuntimeError("package-lock.json does not contain @playwright/test.") from exc
    if not isinstance(version, str) or not version or any(ch not in "0123456789." for ch in version):
        raise RuntimeError("package-lock.json contains an invalid @playwright/test version.")
    return f"mcr.microsoft.com/playwright:v{version}{PLAYWRIGHT_IMAGE_SUFFIX}"


def run_playwright_tests(
    repo_root: Path,
    artifacts: Path,
    workspace: Path,
    base_url: str,
    extra_args: list[str],
    env: dict[str, str],
) -> None:
    worker_arguments = []
    if not any(argument == "--workers" or argument.startswith("--workers=") for argument in extra_args):
        worker_arguments = ["--workers", str(default_parallel_jobs())]
    run(
        [
            "docker",
            "run",
            "--rm",
            "--network",
            "host",
            "--ipc",
            "host",
            "--user",
            f"{os.getuid()}:{os.getgid()}",
            "--volume",
            f"{repo_root}:{repo_root}",
            "--workdir",
            str(workspace),
            "--env",
            f"INPX_WEB_READER_WEB_ARTIFACTS_ROOT={artifacts}",
            "--env",
            f"INPX_WEB_READER_WEB_E2E_BASE_URL={base_url}",
            "--env",
            (
                "INPX_WEB_READER_WEB_E2E_SERVER_ROOT="
                f"{artifacts / E2E_SERVER_WORKSPACE_NAME}"
            ),
            playwright_container_image(workspace),
            *npx_command("playwright", "test", *worker_arguments, *extra_args),
        ],
        workspace,
        env,
    )


def e2e_server_preset() -> str:
    return "linux-debug"


def e2e_parallel_jobs() -> int:
    return default_parallel_jobs()


def server_executable(repo_root: Path, preset: str) -> Path:
    return repo_root / "out" / "build" / preset / "apps" / "InpxWebReader" / "inpx-web-reader"


def find_free_port() -> int:
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        return int(listener.getsockname()[1])


def write_e2e_book(
    path: Path,
    *,
    title: str = "Browser Server Contract Книга",
    annotation: str = "Real browser to server contract fixture.",
) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    cover_png_base64 = (
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/"
        "x8AAwMCAO+/p9sAAAAASUVORK5CYII="
    )
    path.write_text(
        f"""<?xml version="1.0" encoding="utf-8"?>
<FictionBook xmlns:l="http://www.w3.org/1999/xlink">
  <description>
    <title-info>
      <genre>sf</genre>
      <author><first-name>Ada</first-name><last-name>Reader</last-name></author>
      <book-title>{title}</book-title>
      <annotation><p>{annotation}</p></annotation>
      <lang>en</lang>
      <coverpage><image l:href="#cover.png"/></coverpage>
    </title-info>
  </description>
  <body><section><p>{title} server payload.</p></section></body>
  <binary id="cover.png" content-type="image/png">{cover_png_base64}</binary>
</FictionBook>
""",
        encoding="utf-8",
    )


def write_e2e_converter(path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        """#!/bin/sh
set -eu
if [ "${1:-}" = "--help" ]; then
    printf '%s\n' 'NAME:' '   fbc - conversion engine for fiction book (FB2) files' \
        'USAGE:' '   fbc [global options] [command [command options]]' \
        'COMMANDS:' '   convert     Converts FB2 file(s) to specified format'
    exit 0
fi
if [ "$#" -ne 6 ]; then
    exit 64
fi
if [ "$1" != "convert" ] || [ "$2" != "--to" ] || [ "$3" != "epub2" ] || [ "$4" != "--overwrite" ]; then
    exit 64
fi
mkdir -p "$6"
cp "$5" "$6/converted.epub"
""",
        encoding="utf-8",
    )
    path.chmod(0o700)


def generate_e2e_catalog(
    repo_root: Path,
    books_root: Path,
    source_root: Path,
    env: dict[str, str],
) -> None:
    run(
        [
            sys.executable,
            str(repo_root / "scripts" / "GenerateInpxFixtureCatalog.py"),
            str(books_root),
            "--output-dir",
            str(source_root),
            "--force",
        ],
        repo_root,
        env,
    )


def server_log_details(log_path: Path, max_bytes: int = 4096) -> str:
    try:
        with log_path.open("rb") as log:
            log.seek(0, os.SEEK_END)
            log.seek(max(0, log.tell() - max_bytes))
            excerpt = log.read().decode("utf-8", errors="replace").strip()
    except OSError as exc:
        return f"Server log: {log_path} (could not read it: {exc})"

    if not excerpt:
        return f"Server log: {log_path} (no output was captured)"
    return f"Server log: {log_path}\nLast server output:\n{excerpt}"


def wait_for_server(
    base_url: str,
    process: subprocess.Popen[bytes],
    log_path: Path,
    timeout_seconds: float = 60.0,
    auth_token: str = "",
) -> None:
    deadline = time.monotonic() + timeout_seconds
    last_error = "server did not respond"
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(
                "InpxWebReader exited during Playwright setup "
                f"with code {process.returncode}.\n{server_log_details(log_path)}"
            )
        try:
            request = urllib.request.Request(
                f"{base_url}/api/status",
                headers={"Authorization": f"Bearer {auth_token}"} if auth_token else {},
            )
            with urllib.request.urlopen(request, timeout=1.0) as response:
                if response.status == 200:
                    return
        except (OSError, urllib.error.URLError) as exc:
            last_error = str(exc)
        time.sleep(0.1)

    raise RuntimeError(
        f"InpxWebReader did not become ready at {base_url}: {last_error}\n"
        f"{server_log_details(log_path)}"
    )


def stop_server(process: subprocess.Popen[bytes]) -> None:
    existing_return_code = process.poll()
    if existing_return_code is not None:
        raise RuntimeError(
            f"InpxWebReader exited with code {existing_return_code} before SIGTERM was requested."
        )

    process.terminate()

    try:
        process.wait(timeout=10)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=10)
        raise RuntimeError("InpxWebReader did not stop after SIGTERM and had to be killed.")

    if process.returncode != 0:
        raise RuntimeError(f"InpxWebReader exited with code {process.returncode} after SIGTERM.")


@contextlib.contextmanager
def real_server_session(repo_root: Path, artifacts: Path, env: dict[str, str]) -> Iterator[str]:
    server_workspace = artifacts / E2E_SERVER_WORKSPACE_NAME
    if server_workspace.exists():
        remove_path_under_root(
            server_workspace,
            artifacts,
            path_description="Playwright server workspace",
            root_description="web UI artifacts root",
        )
    books_root = server_workspace / "books"
    source_root = server_workspace / "source"
    updated_books_root = server_workspace / "books-update"
    updated_source_root = server_workspace / "source-update"
    cache_root = server_workspace / "cache"
    runtime_root = server_workspace / "runtime"
    converter_path = server_workspace / "converter" / "fbc"
    write_e2e_book(books_root / "browser-server-contract.fb2")
    write_e2e_converter(converter_path)
    generate_e2e_catalog(repo_root, books_root, source_root, env)

    write_e2e_book(
        updated_books_root / "browser-server-contract.fb2",
        title="Browser Server Contract Книга — обновлена",
        annotation="The browser observes metadata published by an incremental rescan.",
    )
    write_e2e_book(
        updated_books_root / "browser-server-new.fb2",
        title="Новая книга после рескана",
        annotation="The browser observes a newly added book after rescan.",
    )
    generate_e2e_catalog(repo_root, updated_books_root, updated_source_root, env)

    port = find_free_port()
    preset = e2e_server_preset()
    parallel_jobs = e2e_parallel_jobs()
    run(build_cmake_configure_command(preset), repo_root, env)
    run(
        build_cmake_build_command(
            preset,
            "Debug",
            parallel_jobs,
            False,
            targets=("InpxWebReader",),
        ),
        repo_root,
        env,
    )

    runtime_root.mkdir(parents=True, exist_ok=True)
    cache_root.mkdir(parents=True, exist_ok=True)
    config_path = runtime_root / "config" / "server.json"
    config_path.parent.mkdir(parents=True, exist_ok=True)
    config = {
        "cacheRoot": str(cache_root),
        "runtimeWorkspaceRoot": str(runtime_root),
        "inpxSource": {
            "inpxPath": str(source_root / "catalog.inpx"),
            "archiveRoot": str(source_root / "lib.rus.ec"),
        },
        "converter": {
            "path": str(converter_path),
        },
        "server": {
            "host": "127.0.0.1",
            "port": port,
            "staticAssetsRoot": str(artifacts / "dist"),
        },
        "security": {
            "token": E2E_AUTH_TOKEN,
        },
        "startup": {
            "autoScan": True,
            "autoScanOnEmptyCache": True,
        },
    }
    config_path.write_text(json.dumps(config, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    config_path.chmod(0o600)

    executable = server_executable(repo_root, preset)
    if not executable.is_file():
        raise RuntimeError(f"Server executable was not produced: {executable}")

    log_path = server_workspace / "server-process.log"
    with log_path.open("wb") as log:
        process = subprocess.Popen(
            [str(executable), "--config", str(config_path)],
            cwd=repo_root,
            env=env,
            stdout=log,
            stderr=subprocess.STDOUT,
        )
        base_url = f"http://127.0.0.1:{port}"
        try:
            wait_for_server(base_url, process, log_path, auth_token=E2E_AUTH_TOKEN)
            yield base_url
        finally:
            stop_server(process)


def forwarded_args(args: argparse.Namespace) -> list[str]:
    if args.tool_args and args.tool_args[0] == "--":
        return args.tool_args[1:]
    return args.tool_args


def run_web_command(
    repo_root: Path,
    artifacts: Path,
    args: argparse.Namespace,
    workspace: Path,
    env: dict[str, str],
) -> None:
    extra_args = forwarded_args(args)
    if args.command == "dev":
        run(npx_command("vite", "--host", "127.0.0.1", *extra_args), workspace, env)
        return

    if args.command == "build":
        run(npx_command("tsc", "--noEmit"), workspace, env)
        run(
            npx_command(
                "vite",
                "build",
                "--outDir",
                str(artifacts / "dist"),
                "--emptyOutDir",
                *extra_args,
            ),
            workspace,
            env,
        )
        return

    if args.command == "test":
        run(npx_command("vitest", "run", *extra_args), workspace, env)
        return

    if args.command == "test:coverage":
        run(npx_command("vitest", "run", "--coverage", *extra_args), workspace, env)
        return

    if args.command == "test:e2e":
        run(npx_command("tsc", "--noEmit"), workspace, env)
        run(
            npx_command(
                "vite",
                "build",
                "--outDir",
                str(artifacts / "dist"),
                "--emptyOutDir",
            ),
            workspace,
            env,
        )
        with real_server_session(repo_root, artifacts, env) as base_url:
            run_playwright_tests(repo_root, artifacts, workspace, base_url, extra_args, env)
        return

    if args.command == "audit":
        run(["npm", "audit", "--audit-level=high", *extra_args], workspace, env)
        return

    raise RuntimeError(f"Unsupported web UI command: {args.command}")


def main() -> int:
    args = parse_args()
    if args.command == "test:e2e" and sys.platform != "linux":
        raise RuntimeError(
            "Web e2e tests require Linux; run `python3 scripts/RunRemoteLinux.py e2e` from the development host."
        )
    repo_root = repository_root()
    artifacts = resolve_artifacts_root(repo_root, args.artifacts_root)
    workspace = sync_workspace(repo_root, artifacts)
    env = build_environment(repo_root, artifacts)

    if args.command != "audit":
        ensure_dependencies(repo_root, workspace, env)
    run_web_command(repo_root, artifacts, args, workspace, env)
    return 0


if __name__ == "__main__":
    sys.exit(main())
