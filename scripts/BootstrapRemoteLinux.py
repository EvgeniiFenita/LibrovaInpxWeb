from __future__ import annotations

import argparse
import shlex
import sys
from pathlib import Path

sys.dont_write_bytecode = True
sys.pycache_prefix = str(Path(__file__).resolve().parent.parent / "out" / "pycache")

from _common import configure_console_streams, repository_root  # noqa: E402
from _remote_linux import (  # noqa: E402
    DEFAULT_ENV_FILE,
    CRemoteLinuxTransport,
    load_remote_config,
)


VCPKG_REF = "b5d1a94fb7f88fd835e360fd23a45a09ceedbf48"
RUFF_VERSION = "0.15.12"
BASE_PACKAGES = (
    "build-essential",
    "ca-certificates",
    "clang",
    "clang-tidy",
    "cmake",
    "curl",
    "git",
    "lcov",
    "ninja-build",
    "nodejs",
    "npm",
    "pkg-config",
    "pipx",
    "python3",
    "python3-pip",
    "python3-venv",
    "procps",
    "rsync",
    "unzip",
    "util-linux",
    "zip",
)
DOCKER_PACKAGES = (
    "docker.io",
    "docker-buildx",
    "docker-compose-v2",
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Install or verify InpxWebReader Linux build-machine prerequisites over SSH."
    )
    parser.add_argument("--env-file", type=Path, default=DEFAULT_ENV_FILE)
    parser.add_argument("--check-only", action="store_true")
    parser.add_argument("--skip-docker", action="store_true")
    return parser.parse_args()


def package_install_script(includeDocker: bool) -> str:
    packages = [*BASE_PACKAGES, *(DOCKER_PACKAGES if includeDocker else ())]
    lines = [
        "set -eu",
        ". /etc/os-release",
        (
            "case \"${ID:-}\" in ubuntu|debian) ;; *) "
            "echo 'Only Ubuntu/Debian build machines are supported.' >&2; exit 1 ;; esac"
        ),
        "export DEBIAN_FRONTEND=noninteractive",
        (
            "while pgrep -x apt >/dev/null || pgrep -x apt-get >/dev/null || "
            "pgrep -x dpkg >/dev/null; do "
            "echo 'Waiting for another apt/dpkg process to finish...'; sleep 5; done"
        ),
        "dpkg --configure -a",
        "apt-get update",
        f"apt-get install -y {' '.join(shlex.quote(package) for package in packages)}",
        (
            f"if ! command -v ruff >/dev/null || [ \"$(ruff --version)\" != \"ruff {RUFF_VERSION}\" ]; then "
            f"PIPX_HOME=/opt/pipx PIPX_BIN_DIR=/usr/local/bin pipx install --force ruff=={RUFF_VERSION}; fi"
        ),
    ]
    if includeDocker:
        lines.extend(
            [
                (
                    "if command -v systemctl >/dev/null && [ \"$(ps -p 1 -o comm=)\" = systemd ]; "
                    "then systemctl enable --now docker; else service docker start; fi"
                ),
                "usermod -aG docker \"$SUDO_USER\"",
            ]
        )
    return "\n".join(lines)


def vcpkg_install_script(workdir: str, vcpkgRoot: str) -> str:
    git_root = shlex.quote(vcpkgRoot + "/.git")
    quoted_root = shlex.quote(vcpkgRoot)
    bootstrap = shlex.quote(vcpkgRoot + "/bootstrap-vcpkg.sh")
    return "\n".join(
        [
            "set -eu",
            f"mkdir -p {shlex.quote(workdir)}",
            (
                f"if [ -e {quoted_root} ] && [ ! -d {git_root} ]; then "
                "echo 'Configured vcpkg path exists but is not a git checkout.' >&2; exit 1; fi"
            ),
            (
                f"if [ ! -d {git_root} ]; then "
                f"git clone https://github.com/microsoft/vcpkg.git {quoted_root}; fi"
            ),
            f"previous_ref=$(git -C {quoted_root} rev-parse HEAD 2>/dev/null || true)",
            f"git -C {quoted_root} fetch origin",
            f"git -C {quoted_root} checkout --detach {shlex.quote(VCPKG_REF)}",
            (
                f"if [ \"$previous_ref\" != {shlex.quote(VCPKG_REF)} ] || "
                f"[ ! -x {shlex.quote(vcpkgRoot + '/vcpkg')} ]; then {bootstrap} -disableMetrics; fi"
            ),
        ]
    )


def readiness_script(vcpkgRoot: str, includeDocker: bool) -> str:
    commands = [
        "set -eu",
        "test \"$(uname -m)\" = x86_64 || { echo 'Only x86_64 Linux build machines are supported.' >&2; exit 1; }",
        "cmake --version | head -n 1",
        "clang++ --version | head -n 1",
        "clang-tidy --version | head -n 1",
        "lcov --version | head -n 1",
        "ninja --version",
        "node --version",
        "npm --version",
        f"test \"$(ruff --version)\" = \"ruff {RUFF_VERSION}\"",
        "python3 --version",
        "g++ --version | head -n 1",
        "git --version",
        "curl --version | head -n 1",
        "rsync --version | head -n 1",
        "flock --version | head -n 1",
        "setsid --version | head -n 1",
        f"test -x {shlex.quote(vcpkgRoot + '/vcpkg')}",
        f"test \"$(git -C {shlex.quote(vcpkgRoot)} rev-parse HEAD)\" = {shlex.quote(VCPKG_REF)}",
    ]
    if includeDocker:
        commands.extend(
            [
                "docker version --format 'docker client={{.Client.Version}} server={{.Server.Version}}'",
                "docker compose version",
            ]
        )
    return "\n".join(commands)


def main() -> int:
    configure_console_streams()
    try:
        args = parse_args()
        repo_root = repository_root()
        config = load_remote_config(args.env_file, repo_root)
        transport = CRemoteLinuxTransport(config, repo_root)
        transport.CheckLocalPrerequisites()

        if not args.check_only:
            print(f"==> Installing build-machine packages on {config.target}", flush=True)
            install_script = package_install_script(not args.skip_docker)
            if config.password:
                sudo_command = f"sudo -S -p '' bash -lc {shlex.quote(install_script)}"
                transport.RunSsh(sudo_command, inputText=config.password + "\n")
            else:
                sudo_command = f"sudo -n bash -lc {shlex.quote(install_script)}"
                transport.RunSsh(sudo_command)

            print(f"==> Installing pinned vcpkg at {config.vcpkg_root}", flush=True)
            transport.RunSsh(
                f"bash -lc {shlex.quote(vcpkg_install_script(str(config.workdir), str(config.vcpkg_root)))}"
            )

        print(f"==> Verifying build machine {config.target}", flush=True)
        transport.RunSsh(
            f"bash -lc {shlex.quote(readiness_script(str(config.vcpkg_root), not args.skip_docker))}"
        )
        print("==> Remote Linux build machine is ready.", flush=True)
        return 0
    except Exception as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
