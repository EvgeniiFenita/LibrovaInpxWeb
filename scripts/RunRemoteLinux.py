from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import shlex
import shutil
import subprocess
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
from _deploy import (  # noqa: E402
    validate_absolute_nas_path,
    validate_access_password as validate_deploy_access_password,
    validate_host_port,
    validate_non_overlapping_nas_roots,
)
from _remote_linux import (  # noqa: E402
    DEFAULT_ENV_FILE,
    CRemoteLinuxConfig,
    CRemoteLinuxTransport,
    load_env_values,
    load_remote_config,
    remote_shell_command,
)


REMOTE_BUNDLE_PATH = PurePosixPath("out/deploy/inpx-web-reader")
DEFAULT_DEPLOY_ENV_FILE = Path(".env.deploy")
DEFAULT_CONVERTER_ASSET_NAME = "fbc-linux-amd64.zip"


@dataclass(frozen=True)
class CDeployConfig:
    nas_source_root: str
    nas_app_root: str
    access_password: str = ""
    host_port: int = 8080
    converter_enabled: bool = True
    converter_version: str = "latest"
    converter_asset_name: str = DEFAULT_CONVERTER_ASSET_NAME


def add_deploy_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "--deploy-env-file",
        type=Path,
        default=DEFAULT_DEPLOY_ENV_FILE,
        help="Deployment defaults file. Defaults to the ignored .env.deploy in the repository root.",
    )
    parser.add_argument("--nas-source-root")
    parser.add_argument("--nas-app-root")
    parser.add_argument("--host-port", type=int)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--image-tag")
    parser.add_argument("--docker-platform", default="linux/amd64")
    parser.add_argument("--build-jobs", type=int)
    parser.add_argument("--token-file", type=Path)
    parser.add_argument("--converter-version")
    parser.add_argument("--converter-asset-name")
    parser.add_argument("--skip-converter-download", action="store_true", default=None)


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

    release_parser = subparsers.add_parser(
        "release",
        help="Verify once, package the same image, and retrieve the NAS deployment bundle.",
    )
    add_deploy_arguments(release_parser)
    release_parser.add_argument("--parallel-jobs", type=int)
    release_parser.add_argument("--test-jobs", type=int)
    release_parser.add_argument("--smoke-book-count", type=int)
    release_parser.add_argument("--skip-e2e", action="store_true")

    bundle_parser = subparsers.add_parser(
        "bundle",
        help="Package an unverified Docker deployment bundle (advanced compatibility command).",
    )
    add_deploy_arguments(bundle_parser)
    bundle_parser.add_argument("--skip-image-build", action="store_true")
    return parser.parse_args()


def append_option(command: list[str], name: str, value: object | None) -> None:
    if value is not None:
        command.extend([name, str(value)])


def parse_deploy_bool(value: str, name: str) -> bool:
    normalized = value.strip().lower()
    if normalized in {"1", "true", "yes", "on"}:
        return True
    if normalized in {"0", "false", "no", "off"}:
        return False
    raise RuntimeError(f"{name} must be true or false.")


def parse_deploy_port(value: str, name: str) -> int:
    try:
        port = int(value)
    except ValueError as exc:
        raise RuntimeError(f"{name} must be a number in range 1..65535.") from exc
    if port < 1 or port > 65535:
        raise RuntimeError(f"{name} must be in range 1..65535.")
    return port


def load_deploy_config(env_file: Path, repo_root: Path) -> CDeployConfig | None:
    resolved_path = env_file if env_file.is_absolute() else repo_root / env_file
    if not resolved_path.is_file():
        return None

    values = load_env_values(resolved_path)
    allowed_keys = {
        "INPX_WEB_READER_DEPLOY_NAS_SOURCE_ROOT",
        "INPX_WEB_READER_DEPLOY_NAS_APP_ROOT",
        "INPX_WEB_READER_DEPLOY_ACCESS_PASSWORD",
        "INPX_WEB_READER_DEPLOY_HOST_PORT",
        "INPX_WEB_READER_DEPLOY_CONVERTER_ENABLED",
        "INPX_WEB_READER_DEPLOY_CONVERTER_VERSION",
        "INPX_WEB_READER_DEPLOY_CONVERTER_ASSET_NAME",
    }
    unknown_keys = sorted(
        key for key in values if key.startswith("INPX_WEB_READER_DEPLOY_") and key not in allowed_keys
    )
    if unknown_keys:
        raise RuntimeError("Unsupported deployment fields: " + ", ".join(unknown_keys))

    source_root = values.get("INPX_WEB_READER_DEPLOY_NAS_SOURCE_ROOT", "").strip()
    app_root = values.get("INPX_WEB_READER_DEPLOY_NAS_APP_ROOT", "").strip()
    host_port_text = values.get("INPX_WEB_READER_DEPLOY_HOST_PORT", "8080")
    converter_enabled_text = values.get("INPX_WEB_READER_DEPLOY_CONVERTER_ENABLED", "true")
    return CDeployConfig(
        nas_source_root=source_root,
        nas_app_root=app_root,
        access_password=values.get("INPX_WEB_READER_DEPLOY_ACCESS_PASSWORD", ""),
        host_port=parse_deploy_port(host_port_text, "INPX_WEB_READER_DEPLOY_HOST_PORT"),
        converter_enabled=parse_deploy_bool(
            converter_enabled_text,
            "INPX_WEB_READER_DEPLOY_CONVERTER_ENABLED",
        ),
        converter_version=values.get("INPX_WEB_READER_DEPLOY_CONVERTER_VERSION", "latest").strip()
        or "latest",
        converter_asset_name=values.get(
            "INPX_WEB_READER_DEPLOY_CONVERTER_ASSET_NAME",
            DEFAULT_CONVERTER_ASSET_NAME,
        ).strip()
        or DEFAULT_CONVERTER_ASSET_NAME,
    )


def resolve_deploy_args(args: argparse.Namespace, repo_root: Path) -> argparse.Namespace:
    config = load_deploy_config(args.deploy_env_file, repo_root)
    args.nas_source_root = args.nas_source_root or (config.nas_source_root if config else "")
    args.nas_app_root = args.nas_app_root or (config.nas_app_root if config else "")
    if not args.nas_source_root or not args.nas_app_root:
        raise RuntimeError(
            "NAS source and application roots are required. Copy .env.deploy.example to .env.deploy "
            "or pass --nas-source-root and --nas-app-root."
        )
    args.nas_source_root = validate_absolute_nas_path(args.nas_source_root, "--nas-source-root")
    args.nas_app_root = validate_absolute_nas_path(args.nas_app_root, "--nas-app-root")
    validate_non_overlapping_nas_roots(args.nas_source_root, args.nas_app_root)

    args.host_port = args.host_port if args.host_port is not None else (config.host_port if config else 8080)
    validate_host_port(args.host_port)
    args.converter_version = args.converter_version or (config.converter_version if config else "latest")
    args.converter_asset_name = args.converter_asset_name or (
        config.converter_asset_name if config else DEFAULT_CONVERTER_ASSET_NAME
    )
    if args.skip_converter_download is None:
        args.skip_converter_download = not config.converter_enabled if config else False
    args.access_password = config.access_password if config else ""
    if args.access_password:
        validate_access_password(args.access_password)
    return args


def git_output(repo_root: Path, *arguments: str) -> str:
    completed = subprocess.run(
        ["git", *arguments],
        cwd=repo_root,
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    return completed.stdout.strip()


def release_identity(repo_root: Path) -> tuple[str, str, bool]:
    version = (repo_root / "VERSION.txt").read_text(encoding="utf-8").strip()
    commit = git_output(repo_root, "rev-parse", "HEAD")
    dirty_diff = subprocess.run(
        ["git", "diff", "HEAD", "--binary", "--"],
        cwd=repo_root,
        check=True,
        stdout=subprocess.PIPE,
    ).stdout
    dirty = bool(dirty_diff)
    suffix = f"-dirty-{hashlib.sha256(dirty_diff).hexdigest()[:8]}" if dirty else ""
    image_tag = f"inpx-web-reader:{version}-{commit[:12]}{suffix}"
    return image_tag, commit, dirty


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


def validate_access_password(password: str) -> None:
    validate_deploy_access_password(password, "INPX_WEB_READER_DEPLOY_ACCESS_PASSWORD")


def sync_access_password(
    transport: CRemoteLinuxTransport,
    config: CRemoteLinuxConfig,
    password: str,
) -> PurePosixPath:
    validate_access_password(password)
    local_input_root = out_root(transport.repo_root) / "remote-linux" / "inputs"
    local_input_root.mkdir(parents=True, exist_ok=True)
    local_input_root.chmod(0o700)
    staged_password = local_input_root / f"password-{uuid.uuid4().hex}.txt"
    try:
        staged_password.touch(mode=0o600, exist_ok=False)
        staged_password.write_text(password + "\n", encoding="utf-8")
        staged_password.chmod(0o600)
        remote_path = sync_token_file(transport, config, staged_password)
        assert remote_path is not None
        return remote_path
    finally:
        staged_password.unlink(missing_ok=True)


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
        "--git-commit",
        args.git_commit,
    ]
    if args.git_dirty:
        command.append("--git-dirty")
    append_option(command, "--build-jobs", args.build_jobs)
    if remoteTokenFile is not None:
        command.extend(["--token-file", str(remoteTokenFile)])
    if args.skip_image_build:
        command.append("--skip-image-build")
    if args.skip_converter_download:
        command.append("--skip-converter-download")
    return command


def build_release_command(args: argparse.Namespace, remoteTokenFile: PurePosixPath | None) -> list[str]:
    command = [
        "python3",
        "scripts/RunRelease.py",
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
        "--git-commit",
        args.git_commit,
    ]
    append_option(command, "--parallel-jobs", args.parallel_jobs)
    append_option(command, "--build-jobs", args.build_jobs)
    append_option(command, "--test-jobs", args.test_jobs)
    append_option(command, "--smoke-book-count", args.smoke_book_count)
    if remoteTokenFile is not None:
        command.extend(["--token-file", str(remoteTokenFile)])
    if args.skip_converter_download:
        command.append("--skip-converter-download")
    if args.skip_e2e:
        command.append("--skip-e2e")
    if args.git_dirty:
        command.append("--git-dirty")
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
        else:
            if args.action in {"bundle", "release"}:
                args = resolve_deploy_args(args, repo_root)
                default_image_tag, args.git_commit, args.git_dirty = release_identity(repo_root)
                args.image_tag = args.image_tag or default_image_tag
            if not args.skip_sync:
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
            if args.token_file is not None:
                remote_token_file = sync_token_file(transport, config, args.token_file)
            elif args.access_password:
                remote_token_file = sync_access_password(transport, config, args.access_password)
        if resume_existing:
            command = ["true"]
        else:
            command = (
                build_release_command(args, remote_token_file)
                if args.action == "release"
                else build_bundle_command(args, remote_token_file)
            )
        operation = "Verifying and building release" if args.action == "release" else "Building deployment bundle"
        print(f"==> {operation} on {config.target}", flush=True)
        cleanup_paths = (remote_token_file,) if remote_token_file is not None else ()
        transport.RunSshJob(
            remote_shell_command(config, command, cleanupPaths=cleanup_paths),
            args.action,
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
