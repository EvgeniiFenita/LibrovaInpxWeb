from __future__ import annotations

import argparse
import shlex
import shutil
import sys
import uuid
from pathlib import Path, PurePosixPath

sys.dont_write_bytecode = True
sys.pycache_prefix = str(Path(__file__).resolve().parent.parent / "out" / "pycache")

from _common import (  # noqa: E402
    configure_console_streams,
    out_root,
    remove_path_under_root,
    repository_root,
    resolve_under_root,
)
from _remote_linux import (  # noqa: E402
    DEFAULT_ENV_FILE,
    CRemoteLinuxConfig,
    CRemoteLinuxTransport,
    load_remote_config,
    remote_shell_command,
)


REMOTE_BUNDLE_PATH = PurePosixPath("out/deploy/inpx-web-reader")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Sync InpxWebReader to a Linux build machine and run tests or prepare a deployment bundle."
    )
    parser.add_argument("--env-file", type=Path, default=DEFAULT_ENV_FILE)
    parser.add_argument("--skip-sync", action="store_true")
    subparsers = parser.add_subparsers(dest="action", required=True)

    test_parser = subparsers.add_parser("test", help="Run the Linux native, Docker image, and Compose smoke lane.")
    test_parser.add_argument("--preset", default="linux-debug")
    test_parser.add_argument("--image-tag", default="inpx-web-reader:local")
    test_parser.add_argument("--docker-platform", default="linux/amd64")
    test_parser.add_argument("--parallel-jobs", type=int)
    test_parser.add_argument("--build-jobs", type=int)
    test_parser.add_argument("--test-jobs", type=int)
    test_parser.add_argument("--skip-configure", action="store_true")
    test_parser.add_argument("--skip-native", action="store_true")
    test_parser.add_argument("--skip-docker", action="store_true")
    test_parser.add_argument("--skip-compose", action="store_true")
    test_parser.add_argument("--keep-smoke-workspace", action="store_true")
    test_parser.add_argument("--smoke-book-count", type=int)

    e2e_parser = subparsers.add_parser("e2e", help="Run Playwright against the real Linux server.")
    e2e_parser.add_argument("--artifacts-root")
    e2e_parser.add_argument("tool_args", nargs=argparse.REMAINDER)

    bundle_parser = subparsers.add_parser("bundle", help="Build and retrieve the Linux Docker deployment bundle.")
    bundle_parser.add_argument("--nas-source-root", required=True)
    bundle_parser.add_argument("--nas-app-root", required=True)
    bundle_parser.add_argument("--host-port", type=int, default=8080)
    bundle_parser.add_argument("--output", type=Path)
    bundle_parser.add_argument("--image-tag", default="inpx-web-reader:nas-local")
    bundle_parser.add_argument("--docker-platform", default="linux/amd64")
    bundle_parser.add_argument("--build-jobs", type=int)
    bundle_parser.add_argument("--token-file", type=Path)
    bundle_parser.add_argument("--converter-version", default="latest")
    bundle_parser.add_argument("--converter-asset-name", default="fbc-linux-amd64.zip")
    bundle_parser.add_argument("--skip-image-build", action="store_true")
    bundle_parser.add_argument("--skip-converter-download", action="store_true")
    return parser.parse_args()


def append_option(command: list[str], name: str, value: object | None) -> None:
    if value is not None:
        command.extend([name, str(value)])


def build_test_command(args: argparse.Namespace) -> list[str]:
    command = [
        "python3",
        "scripts/RunLinuxTests.py",
        "--preset",
        args.preset,
        "--image-tag",
        args.image_tag,
        "--docker-platform",
        args.docker_platform,
    ]
    append_option(command, "--parallel-jobs", args.parallel_jobs)
    append_option(command, "--build-jobs", args.build_jobs)
    append_option(command, "--test-jobs", args.test_jobs)
    append_option(command, "--smoke-book-count", args.smoke_book_count)
    for option, enabled in (
        ("--skip-configure", args.skip_configure),
        ("--skip-native", args.skip_native),
        ("--skip-docker", args.skip_docker),
        ("--skip-compose", args.skip_compose),
        ("--keep-smoke-workspace", args.keep_smoke_workspace),
    ):
        if enabled:
            command.append(option)
    return command


def build_e2e_command(args: argparse.Namespace) -> list[str]:
    command = ["python3", "scripts/RunWebUi.py"]
    append_option(command, "--artifacts-root", args.artifacts_root)
    command.append("test:e2e")
    command.extend(args.tool_args)
    return command


def resolve_bundle_output(repoRoot: Path, requestedOutput: Path | None) -> Path:
    repo_out_root = out_root(repoRoot)
    candidate = requestedOutput if requestedOutput is not None else Path("deploy/inpx-web-reader")
    if not candidate.is_absolute():
        candidate = repo_out_root / candidate
    return resolve_under_root(
        candidate,
        repo_out_root,
        path_description="--output",
        root_description="repository out directory",
    )


def sync_token_file(
    transport: CRemoteLinuxTransport,
    config: CRemoteLinuxConfig,
    tokenFile: Path | None,
) -> PurePosixPath | None:
    if tokenFile is None:
        return None
    if not tokenFile.is_file():
        raise RuntimeError(f"--token-file does not exist: {tokenFile}")

    remote_input_root = config.workdir / "inputs"
    remote_token_path = remote_input_root / f"token-{uuid.uuid4().hex}.txt"
    local_input_root = out_root(transport.repo_root) / "remote-linux" / "inputs"
    local_input_root.mkdir(parents=True, exist_ok=True)
    local_input_root.chmod(0o700)
    staged_token = local_input_root / remote_token_path.name
    remote_cleanup_needed = False
    try:
        staged_token.touch(mode=0o600, exist_ok=False)
        shutil.copyfile(tokenFile, staged_token)
        staged_token.chmod(0o600)
        transport.RunSsh(f"install -d -m 700 {shlex.quote(str(remote_input_root))}")
        remote_cleanup_needed = True
        transport.RsyncTo(staged_token, remote_input_root)
        transport.RunSsh(f"chmod 600 {shlex.quote(str(remote_token_path))}")
    except Exception:
        if remote_cleanup_needed:
            try:
                transport.RunSsh(f"rm -f -- {shlex.quote(str(remote_token_path))}")
            except Exception:
                pass
        raise
    finally:
        staged_token.unlink(missing_ok=True)
    return remote_token_path


def build_bundle_command(args: argparse.Namespace, remoteTokenFile: PurePosixPath | None) -> list[str]:
    command = [
        "python3",
        "scripts/PrepareDeployBundle.py",
        "--nas-source-root",
        args.nas_source_root,
        "--nas-app-root",
        args.nas_app_root,
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
    ]
    append_option(command, "--build-jobs", args.build_jobs)
    if remoteTokenFile is not None:
        command.extend(["--token-file", str(remoteTokenFile)])
    if args.skip_image_build:
        command.append("--skip-image-build")
    if args.skip_converter_download:
        command.append("--skip-converter-download")
    return command


def main() -> int:
    configure_console_streams()
    try:
        args = parse_args()
        repo_root = repository_root()
        config = load_remote_config(args.env_file, repo_root)
        transport = CRemoteLinuxTransport(config, repo_root)
        transport.CheckLocalPrerequisites()

        active_job = transport.ActiveSshJob()
        if active_job is not None and active_job != args.action:
            raise RuntimeError(
                f"Remote {active_job} job is already running. Wait for it or rerun that action to reconnect."
            )
        resume_existing = active_job == args.action
        if resume_existing:
            print(f"==> Resuming active remote {args.action} job; source sync is skipped.", flush=True)
        elif not args.skip_sync:
            print(f"==> Syncing source to {config.target}:{config.source_root}", flush=True)
            transport.RsyncRepository()

        if args.action == "test":
            command = build_test_command(args)
            print(f"==> Running remote Linux verification on {config.target}", flush=True)
            transport.RunSshJob(
                remote_shell_command(config, command),
                "test",
                resumeExisting=resume_existing,
            )
            print("==> Remote Linux verification completed successfully.", flush=True)
            return 0

        if args.action == "e2e":
            command = build_e2e_command(args)
            print(f"==> Running remote Playwright E2E on {config.target}", flush=True)
            transport.RunSshJob(
                remote_shell_command(config, command),
                "e2e",
                resumeExisting=resume_existing,
            )
            print("==> Remote Playwright E2E completed successfully.", flush=True)
            return 0

        remote_token_file = None
        if not resume_existing:
            remote_token_file = sync_token_file(transport, config, args.token_file)
        command = build_bundle_command(args, remote_token_file)
        print(f"==> Building deployment bundle on {config.target}", flush=True)
        cleanup_paths = (remote_token_file,) if remote_token_file is not None else ()
        transport.RunSshJob(
            remote_shell_command(config, command, cleanupPaths=cleanup_paths),
            "bundle",
            resumeExisting=resume_existing,
            startCleanupPaths=cleanup_paths,
        )

        output_dir = resolve_bundle_output(repo_root, args.output)
        if output_dir.exists():
            remove_path_under_root(
                output_dir,
                out_root(repo_root),
                path_description="remote bundle output",
                root_description="repository out directory",
            )
        remote_bundle_root = config.source_root / REMOTE_BUNDLE_PATH
        print(f"==> Downloading deployment bundle to {output_dir}", flush=True)
        transport.RsyncFrom(remote_bundle_root, output_dir, delete=True, lockSource=True)
        print(f"==> Deployment bundle is ready: {output_dir}", flush=True)
        return 0
    except Exception as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
