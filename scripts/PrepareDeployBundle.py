from __future__ import annotations

import argparse
import hashlib
import json
import platform
import secrets
import shutil
import subprocess
import sys
import urllib.request
import zipfile
from pathlib import Path, PurePosixPath

sys.dont_write_bytecode = True
sys.pycache_prefix = str(Path(__file__).resolve().parent.parent / "out" / "pycache")

from _common import (  # noqa: E402
    TimedStage,
    build_out_tool_environment,
    default_parallel_jobs,
    docker_platform_arguments,
    ensure_docker_engine,
    out_root,
    out_scoped_environment,
    remove_path_under_root,
    repository_root,
    resolve_command,
    resolve_under_root,
    run_command,
    run_timed_stages,
    validate_parallel_jobs,
)
from _deploy import (  # noqa: E402
    validate_absolute_nas_path,
    validate_access_password,
    validate_host_port,
    validate_non_overlapping_nas_roots,
)


DEFAULT_IMAGE_TAG = "inpx-web-reader:nas-local"
DEFAULT_HOST_PORT = 8080
DEFAULT_CONVERTER_ASSET_NAME = "fbc-linux-amd64.zip"
DEFAULT_DOCKER_PLATFORM = "linux/amd64"
GITHUB_RELEASES_API_ROOT = "https://api.github.com/repos/rupor-github/fb2cng/releases"
ACCESS_PASSWORD_ALPHABET = "23456789abcdefghjkmnpqrstuvwxyz"
ACCESS_PASSWORD_GROUP_COUNT = 4
ACCESS_PASSWORD_GROUP_LENGTH = 4

SERVER_COMPOSE_TEMPLATE = Path("deploy/inpx-web-reader/docker-compose.yml")
SERVER_DOCKERFILE = Path("deploy/inpx-web-reader/Dockerfile")
IMAGE_ARCHIVE_NAME = "inpx-web-reader.tar"
IMAGE_ARCHIVE_CHECKSUM_NAME = f"{IMAGE_ARCHIVE_NAME}.sha256"
BUNDLE_MANIFEST_NAME = "manifest.json"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Prepare a Linux Docker deployment bundle for inpx-web-reader."
    )
    parser.add_argument(
        "--nas-source-root",
        required=True,
        help="Absolute NAS path to the INPX source directory mounted read-only.",
    )
    parser.add_argument(
        "--nas-app-root",
        required=True,
        help="Absolute NAS path where this bundle will be copied and run.",
    )
    parser.add_argument(
        "--host-port",
        type=int,
        default=DEFAULT_HOST_PORT,
        help="NAS host port exposed for the browser UI.",
    )
    parser.add_argument(
        "--output",
        type=Path,
        help="Output bundle directory. Defaults to out/deploy/inpx-web-reader.",
    )
    parser.add_argument(
        "--image-tag",
        default=DEFAULT_IMAGE_TAG,
        help="Docker image tag saved into the bundle.",
    )
    parser.add_argument(
        "--docker-platform",
        default=DEFAULT_DOCKER_PLATFORM,
        help="Docker platform for the saved NAS image; must be linux/amd64.",
    )
    parser.add_argument(
        "--build-jobs",
        type=int,
        help="Docker image vcpkg/CMake parallelism. Defaults to host CPU parallelism.",
    )
    parser.add_argument(
        "--token-file",
        type=Path,
        help=(
            "Local one-line access-password file to copy into the bundle secret file. "
            "A short high-entropy password is generated when omitted."
        ),
    )
    parser.add_argument(
        "--converter-version",
        default="latest",
        help="fb2cng/fbc release tag to download, or 'latest'.",
    )
    parser.add_argument(
        "--converter-asset-name",
        default=DEFAULT_CONVERTER_ASSET_NAME,
        help="fb2cng/fbc release asset to download.",
    )
    parser.add_argument(
        "--skip-image-build",
        action="store_true",
        help="Generate scripts/config without building and saving the Docker image.",
    )
    parser.add_argument(
        "--reuse-image",
        action="store_true",
        help="Save an already-built and verified local image without rebuilding it.",
    )
    parser.add_argument(
        "--skip-converter-download",
        action="store_true",
        help="Generate a bundle with optional EPUB conversion disabled.",
    )
    parser.add_argument("--git-commit", default="unknown", help=argparse.SUPPRESS)
    parser.add_argument("--git-dirty", action="store_true", help=argparse.SUPPRESS)
    return parser.parse_args()


def run(command: list[str], cwd: Path, env: dict[str, str] | None = None) -> None:
    run_command(command, cwd, env=env)


def ensure_linux_host() -> None:
    if sys.platform != "linux":
        raise RuntimeError(
            "PrepareDeployBundle.py is a Linux worker. Use scripts/RunRemoteLinux.py bundle from the development host."
        )
    if platform.machine().lower() not in {"x86_64", "amd64"}:
        raise RuntimeError("PrepareDeployBundle.py requires an x86_64 Linux host.")


def resolve_output_dir(repo_root: Path, requested_output: Path | None) -> Path:
    output = requested_output if requested_output is not None else out_root(repo_root) / "deploy" / "inpx-web-reader"
    return resolve_under_root(
        output,
        out_root(repo_root),
        path_description="Output bundle directory",
        root_description="repository out directory",
        allow_root=False,
    )


def shell_quote(value: str) -> str:
    return "'" + value.replace("'", "'\"'\"'") + "'"


def env_value(value: str | int) -> str:
    text = str(value)
    if text == "":
        return ""
    if any(character in text for character in "\r\n"):
        raise RuntimeError("Environment values must not contain line breaks.")
    if any(ord(character) < 0x20 for character in text):
        raise RuntimeError("Environment values must not contain control characters.")
    if "`" in text or "\\" in text:
        raise RuntimeError("Environment values must not contain backticks or backslashes.")
    if "'" in text or '"' in text or "$" in text:
        raise RuntimeError("Environment values must not contain quotes or '$'.")
    if all(character.isalnum() or character in "._/:-+" for character in text):
        return text
    return f'"{text}"'


def write_text(path: Path, content: str, *, executable: bool = False) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8", newline="\n")
    if executable:
        path.chmod(path.stat().st_mode | 0o111)


def write_private_text(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.parent.chmod(0o700)
    path.touch(mode=0o600, exist_ok=False)
    try:
        path.write_text(content, encoding="utf-8", newline="\n")
        path.chmod(0o600)
    except Exception:
        path.unlink(missing_ok=True)
        raise


def prepare_output_directory(bundle_root: Path, repo_out_root: Path) -> None:
    remove_path_under_root(
        bundle_root,
        repo_out_root,
        path_description="Output bundle directory",
        root_description="repository out directory",
    )
    bundle_root.mkdir(parents=True, exist_ok=True)


def release_api_url(version: str) -> str:
    if version == "latest":
        return f"{GITHUB_RELEASES_API_ROOT}/latest"
    return f"{GITHUB_RELEASES_API_ROOT}/tags/{version}"


def read_url_json(url: str) -> dict[str, object]:
    request = urllib.request.Request(url, headers={"User-Agent": "InpxWebReader-deploy-bundle"})
    with urllib.request.urlopen(request, timeout=30) as response:
        return json.loads(response.read().decode("utf-8"))


def find_release_asset(release: dict[str, object], asset_name: str) -> dict[str, object]:
    assets = release.get("assets")
    if not isinstance(assets, list):
        raise RuntimeError("GitHub release response did not contain an asset list.")

    for asset in assets:
        if isinstance(asset, dict) and asset.get("name") == asset_name:
            return asset

    tag_name = str(release.get("tag_name", "<unknown>"))
    raise RuntimeError(f"Release {tag_name} does not contain asset {asset_name}.")


def digest_sha256(asset: dict[str, object]) -> str:
    digest = asset.get("digest")
    if not isinstance(digest, str) or not digest.startswith("sha256:"):
        asset_name = str(asset.get("name", "<unknown>"))
        raise RuntimeError(
            f"Release asset {asset_name} does not provide a sha256 digest. "
            "Refusing to stage an unaudited converter binary."
        )
    return digest.removeprefix("sha256:")


def download_file(url: str, target: Path, expected_sha256: str) -> str:
    target.parent.mkdir(parents=True, exist_ok=True)
    request = urllib.request.Request(url, headers={"User-Agent": "InpxWebReader-deploy-bundle"})
    digest = hashlib.sha256()
    with urllib.request.urlopen(request, timeout=120) as response, target.open("wb") as output:
        while True:
            chunk = response.read(1024 * 1024)
            if not chunk:
                break
            digest.update(chunk)
            output.write(chunk)

    actual_sha256 = digest.hexdigest()
    if actual_sha256.lower() != expected_sha256.lower():
        raise RuntimeError(
            f"Downloaded converter checksum mismatch: expected {expected_sha256}, got {actual_sha256}."
        )
    return actual_sha256


def extract_converter_binary(archive_path: Path, converter_dir: Path) -> Path:
    converter_dir.mkdir(parents=True, exist_ok=True)
    target = converter_dir / "fbc"
    with zipfile.ZipFile(archive_path) as archive:
        candidates = [
            name
            for name in archive.namelist()
            if not name.endswith("/") and PurePosixPath(name).name == "fbc"
        ]
        if not candidates:
            raise RuntimeError(f"Converter archive does not contain an fbc executable: {archive_path}")
        if len(candidates) > 1:
            raise RuntimeError(f"Converter archive contains multiple fbc executables: {archive_path}")

        with archive.open(candidates[0]) as source, target.open("wb") as output:
            shutil.copyfileobj(source, output)

    target.chmod(target.stat().st_mode | 0o111)
    return target


def download_converter(bundle_root: Path, version: str, asset_name: str) -> None:
    release = read_url_json(release_api_url(version))
    asset = find_release_asset(release, asset_name)
    download_url = asset.get("browser_download_url")
    if not isinstance(download_url, str) or not download_url:
        raise RuntimeError(f"Release asset {asset_name} does not have a download URL.")

    tag_name = str(release.get("tag_name", version))
    expected_sha256 = digest_sha256(asset)
    archive_path = bundle_root / "downloads" / asset_name
    actual_sha256 = download_file(download_url, archive_path, expected_sha256)
    converter_path = extract_converter_binary(archive_path, bundle_root / "converter")

    write_text(
        bundle_root / "converter" / "VERSION.txt",
        "\n".join(
            [
                f"tag={tag_name}",
                f"asset={asset_name}",
                f"url={download_url}",
                f"sha256={actual_sha256}",
                f"path={converter_path.name}",
                "",
            ]
        ),
    )


def write_env_file(
    bundle_root: Path,
    *,
    image_tag: str,
    host_port: int,
    nas_source_root: str,
    nas_app_root: str,
    converter_enabled: bool = True,
) -> None:
    lines = [
        "# Generated by scripts/PrepareDeployBundle.py.",
        "COMPOSE_PROJECT_NAME=inpx-web-reader",
        f"INPX_WEB_READER_IMAGE={env_value(image_tag)}",
        f"INPX_WEB_READER_HOST_PORT={env_value(host_port)}",
        f"INPX_WEB_READER_SOURCE_PATH={env_value(nas_source_root)}",
        f"INPX_WEB_READER_DATA_PATH={env_value(nas_app_root + '/data')}",
        "INPX_WEB_READER_INPX_PATH=",
        "INPX_WEB_READER_ARCHIVE_ROOT=",
        "INPX_WEB_READER_CONVERTER_PATH=" + ("/converter/fbc" if converter_enabled else ""),
        f"INPX_WEB_READER_AUTH_TOKEN_FILE={env_value(nas_app_root + '/secrets/inpx-web-reader-auth-token.txt')}",
        "INPX_WEB_READER_LOG_LEVEL=info",
        "INPX_WEB_READER_LOG_MAX_FILE_SIZE_MIB=20",
        "INPX_WEB_READER_LOG_MAX_ROTATED_FILES=4",
        "INPX_WEB_READER_MAX_HTTP_THREADS=4",
        "INPX_WEB_READER_MAX_HTTP_QUEUED_REQUESTS=32",
        "INPX_WEB_READER_MAX_BACKEND_QUEUE_DEPTH=64",
        "INPX_WEB_READER_MAX_SCAN_WORKERS=4",
        "INPX_WEB_READER_MAX_CONCURRENT_DOWNLOADS=2",
        "INPX_WEB_READER_MAX_REQUEST_BODY_BYTES=65536",
        "INPX_WEB_READER_HTTP_READ_TIMEOUT_MS=15000",
        "INPX_WEB_READER_HTTP_WRITE_TIMEOUT_MS=30000",
        "INPX_WEB_READER_MAX_COVER_CACHE_MIB=128",
        "INPX_WEB_READER_MAX_STEADY_STATE_MEMORY_MIB=1024",
        "INPX_WEB_READER_MAX_PAGE_SIZE=200",
        "",
    ]
    if converter_enabled:
        lines.insert(
            9,
            f"INPX_WEB_READER_CONVERTER_HOST_PATH={env_value(nas_app_root + '/converter')}",
        )
    write_text(bundle_root / ".env", "\n".join(lines))


def write_converter_compose_override(bundle_root: Path) -> None:
    write_text(
        bundle_root / "docker-compose.converter.yml",
        (
            """services:
  inpx-web-reader:
    volumes:
"""
            "      - ${INPX_WEB_READER_CONVERTER_HOST_PATH:?Set INPX_WEB_READER_CONVERTER_HOST_PATH "
            "to the converter directory}:/converter:ro\n"
            """    environment:
      INPX_WEB_READER_CONVERTER_PATH: /converter/fbc
"""
        ),
    )


def generate_access_password() -> str:
    return "-".join(
        "".join(secrets.choice(ACCESS_PASSWORD_ALPHABET) for _ in range(ACCESS_PASSWORD_GROUP_LENGTH))
        for _ in range(ACCESS_PASSWORD_GROUP_COUNT)
    )


def read_one_line_token_file(token_file: Path, source_description: str) -> str:
    text = token_file.read_text(encoding="utf-8")
    if text.startswith("\ufeff"):
        text = text.removeprefix("\ufeff")

    token = text.rstrip("\r\n")
    if "\r" in token or "\n" in token:
        raise RuntimeError(f"{source_description} must contain exactly one access-password line.")
    return validate_access_password(token, source_description)


def resolve_auth_token(bundle_root: Path, token_file: Path | None) -> str | None:
    if token_file is not None:
        return read_one_line_token_file(token_file, "--token-file")

    existing_token_file = bundle_root / "secrets" / "inpx-web-reader-auth-token.txt"
    if existing_token_file.exists():
        return read_one_line_token_file(existing_token_file, "Existing bundle token file")

    return None


def write_token_file(bundle_root: Path, token: str | None) -> None:
    resolved_token = validate_access_password(
        token if token is not None else generate_access_password(),
        "Access password",
    )

    token_path = bundle_root / "secrets" / "inpx-web-reader-auth-token.txt"
    write_private_text(token_path, resolved_token + "\n")


def write_run_script(
    bundle_root: Path,
    *,
    host_port: int,
    nas_source_root: str,
    nas_app_root: str,
    converter_enabled: bool,
) -> None:
    write_text(
        bundle_root / "RUN_ON_NAS.sh",
        f"""#!/bin/sh
set -eu

NAS_SOURCE_ROOT={shell_quote(nas_source_root)}
NAS_APP_ROOT={shell_quote(nas_app_root)}
HOST_PORT={host_port}
export COMPOSE_PROJECT_NAME=inpx-web-reader
CONTAINER_UID=10001
CONTAINER_GID=10001
CONVERTER_ENABLED={1 if converter_enabled else 0}

log()
{{
    printf '[inpx-web-reader] %s\\n' "$*"
}}

fail()
{{
    log "$*"
    exit 1
}}

compose()
{{
    if docker compose version >/dev/null 2>&1; then
        docker compose "$@"
        return
    fi
    if command -v docker-compose >/dev/null 2>&1; then
        docker-compose "$@"
        return
    fi
    fail "Docker Compose was not found. Install the Docker Compose plugin or docker-compose."
}}

converter_enabled()
{{
    [ "$CONVERTER_ENABLED" -eq 1 ]
}}

bundle_compose()
{{
    if converter_enabled; then
        compose --env-file .env -f docker-compose.yml -f docker-compose.converter.yml "$@"
        return
    fi
    compose --env-file .env -f docker-compose.yml "$@"
}}

detect_lan_ip()
{{
    if command -v ip >/dev/null 2>&1; then
        detected_ip="$(ip route get 1.1.1.1 2>/dev/null | awk '
            {{
                for (field_index = 1; field_index <= NF; field_index++) {{
                    if ($field_index == "src") {{
                        print $(field_index + 1)
                        exit
                    }}
                }}
            }}
        ')"
        if [ -n "$detected_ip" ]; then
            printf '%s\\n' "$detected_ip"
            return
        fi
    fi

    if command -v hostname >/dev/null 2>&1; then
        for detected_ip in $(hostname -I 2>/dev/null); do
            case "$detected_ip" in
                127.*|169.254.*|::1|fe80:*) ;;
                *) printf '%s\\n' "$detected_ip"; return ;;
            esac
        done
    fi
}}

port_is_listening()
{{
    if command -v ss >/dev/null 2>&1; then
        ss -ltn 2>/dev/null | awk '{{print $4}}' | grep -Eq "[:.]$HOST_PORT$"
        return
    fi

    if command -v netstat >/dev/null 2>&1; then
        netstat -ltn 2>/dev/null | awk '{{print $4}}' | grep -Eq "[:.]$HOST_PORT$"
        return
    fi

    return 1
}}

port_is_published_by_docker()
{{
    docker ps --format '{{{{.Ports}}}} {{{{.Names}}}}' |
        grep -Eq "(0\\.0\\.0\\.0|127\\.0\\.0\\.1|:::|\\[::\\]):$HOST_PORT->"
}}

load_generated_env()
{{
    INPX_WEB_READER_IMAGE="$(
        awk -F= '
            $1 == "INPX_WEB_READER_IMAGE" {{ print substr($0, index($0, "=") + 1); found = 1; exit }}
            END {{ if (!found) exit 1 }}
        ' ./.env
    )" ||
        fail "Could not read INPX_WEB_READER_IMAGE from .env."
    : "${{INPX_WEB_READER_IMAGE:=inpx-web-reader:nas-local}}"
    case "$INPX_WEB_READER_IMAGE" in
        *[!abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._/:+-]*)
            fail "INPX_WEB_READER_IMAGE in .env contains unsupported characters."
            ;;
    esac
    export INPX_WEB_READER_IMAGE
}}

image_id_for_tag()
{{
    docker image inspect --format '{{{{.Id}}}}' "$1" 2>/dev/null || true
}}

image_platform_for_tag()
{{
    docker image inspect --format '{{{{.Os}}}}/{{{{.Architecture}}}}' "$1" 2>/dev/null || true
}}

image_repository_for_tag()
{{
    case "$1" in
        *:*) printf '%s\n' "${{1%:*}}" ;;
        *) printf '%s\n' "$1" ;;
    esac
}}

image_is_used_by_container()
{{
    [ -n "$(docker ps -aq --filter "ancestor=$1" 2>/dev/null)" ]
}}

image_is_inpx_web_reader()
{{
    image_title="$(docker image inspect \
        --format '{{{{index .Config.Labels "org.opencontainers.image.title"}}}}' "$1" 2>/dev/null || true)"
    if [ "$image_title" = "InpxWebReader" ]; then
        return 0
    fi

    image_entrypoint="$(docker image inspect \
        --format '{{{{json .Config.Entrypoint}}}}' "$1" 2>/dev/null || true)"
    [ "$image_entrypoint" = '["/usr/local/bin/inpx-web-reader-entrypoint"]' ]
}}

remove_unused_image()
{{
    candidate_image_id="$1"
    current_image_id="$2"

    if [ -z "$candidate_image_id" ] || [ "$candidate_image_id" = "$current_image_id" ]; then
        return
    fi
    if image_is_used_by_container "$candidate_image_id"; then
        log "Keeping Docker image $candidate_image_id because a container still references it."
        return
    fi

    if docker image rm -f "$candidate_image_id" >/dev/null 2>&1; then
        log "Removed superseded InpxWebReader Docker image: $candidate_image_id."
        return
    fi

    log "WARNING: Could not remove superseded Docker image $candidate_image_id."
}}

cleanup_old_project_containers()
{{
    current_container_id="$1"
    for candidate_container_id in $(docker ps -aq \
        --filter "label=com.docker.compose.project=inpx-web-reader" \
        --filter "label=com.docker.compose.service=inpx-web-reader"); do
        if [ "$candidate_container_id" = "$current_container_id" ]; then
            continue
        fi
        candidate_running="$(docker inspect \
            --format '{{{{.State.Running}}}}' "$candidate_container_id" 2>/dev/null || true)"
        if [ "$candidate_running" = "false" ] && docker rm "$candidate_container_id" >/dev/null 2>&1; then
            log "Removed stopped superseded InpxWebReader container: $candidate_container_id."
        fi
    done
}}

cleanup_superseded_images()
{{
    current_image_id="$1"
    previous_image_id="$2"
    image_repository="$(image_repository_for_tag "$INPX_WEB_READER_IMAGE")"

    remove_unused_image "$previous_image_id" "$current_image_id"

    candidate_image_ids="$({{
        docker image ls --filter "reference=$image_repository:*" --format '{{{{.ID}}}}'
        docker image ls --filter dangling=true --format '{{{{.ID}}}}'
    }} | awk 'NF && !seen[$0]++')"
    for candidate_image_id in $candidate_image_ids; do
        if [ "$candidate_image_id" = "$current_image_id" ]; then
            continue
        fi
        if image_is_inpx_web_reader "$candidate_image_id"; then
            remove_unused_image "$candidate_image_id" "$current_image_id"
        fi
    done
}}

verify_image_archive_checksum()
{{
    checksum_file="$NAS_APP_ROOT/{IMAGE_ARCHIVE_CHECKSUM_NAME}"
    [ -f "$checksum_file" ] || fail "Docker image checksum is missing: $checksum_file"

    if command -v sha256sum >/dev/null 2>&1; then
        (cd "$NAS_APP_ROOT" && sha256sum -c "{IMAGE_ARCHIVE_CHECKSUM_NAME}") >/dev/null ||
            fail "Docker image archive checksum verification failed."
        return
    fi
    if command -v shasum >/dev/null 2>&1; then
        (cd "$NAS_APP_ROOT" && shasum -a 256 -c "{IMAGE_ARCHIVE_CHECKSUM_NAME}") >/dev/null ||
            fail "Docker image archive checksum verification failed."
        return
    fi

    fail "Neither sha256sum nor shasum is available to verify the Docker image archive."
}}

wait_for_container_health()
{{
    candidate_container_id="$1"
    for _attempt in $(seq 1 30); do
        health_status="$(docker inspect \
            --format '{{{{if .State.Health}}}}{{{{.State.Health.Status}}}}{{{{else}}}}none{{{{end}}}}' \
            "$candidate_container_id" 2>/dev/null || true)"
        if [ "$health_status" = "healthy" ]; then
            return 0
        fi
        sleep 2
    done
    return 1
}}

rollback_previous_deployment()
{{
    if [ -z "$previous_image_id" ]; then
        log "No previous InpxWebReader image is available for rollback."
        return 1
    fi
    if [ "$previous_image_id" = "$loaded_image_id" ]; then
        log "The loaded image is identical to the previous image; no distinct rollback image is available."
        return 1
    fi

    log "Rolling back to previous Docker image: $previous_image_id."
    docker tag "$previous_image_id" "$INPX_WEB_READER_IMAGE" || return 1
    bundle_compose up -d --force-recreate --remove-orphans || return 1
    rollback_container_id="$(bundle_compose ps -q inpx-web-reader || true)"
    [ -n "$rollback_container_id" ] || return 1
    wait_for_container_health "$rollback_container_id" || return 1

    log "Rollback completed; the previous InpxWebReader image is healthy."
    remove_unused_image "$loaded_image_id" "$previous_image_id"
    return 0
}}

set_container_identity()
{{
    CONTAINER_UID="$1"
    CONTAINER_GID="$2"
    export INPX_WEB_READER_CONTAINER_UID="$CONTAINER_UID"
    export INPX_WEB_READER_CONTAINER_GID="$CONTAINER_GID"
}}

container_can_read_source()
{{
    candidate_uid="$1"
    candidate_gid="$2"
    docker run --rm \\
        --entrypoint /bin/sh \\
        --user "$candidate_uid:$candidate_gid" \\
        --read-only \\
        --cap-drop ALL \\
        -v "$NAS_SOURCE_ROOT:/source:ro" \\
        "$INPX_WEB_READER_IMAGE" \\
        -lc 'test -r /source && test -x /source && find /source -maxdepth 1 -print -quit >/dev/null'
}}

choose_container_identity()
{{
    source_uid="$(stat -c '%u' "$NAS_SOURCE_ROOT")"
    source_gid="$(stat -c '%g' "$NAS_SOURCE_ROOT")"

    if [ "$source_uid" != "0" ] && [ "$source_gid" != "0" ] &&
        container_can_read_source "$source_uid" "$source_gid"; then
        set_container_identity "$source_uid" "$source_gid"
        log "Container uid/gid selected from source directory owner: $CONTAINER_UID:$CONTAINER_GID."
        return
    fi

    if [ -n "${{SUDO_UID:-}}" ] && [ -n "${{SUDO_GID:-}}" ] &&
        [ "$SUDO_UID" != "0" ] && [ "$SUDO_GID" != "0" ]; then
        if [ "$SUDO_UID" != "$source_uid" ] || [ "$SUDO_GID" != "$source_gid" ]; then
            if container_can_read_source "$SUDO_UID" "$SUDO_GID"; then
                set_container_identity "$SUDO_UID" "$SUDO_GID"
                log "Container uid/gid selected from sudo user: $CONTAINER_UID:$CONTAINER_GID."
                return
            fi
        fi
    fi

    if [ "$source_uid" = "0" ] || [ "$source_gid" = "0" ]; then
        log "Source directory owner is root ($source_uid:$source_gid); root container identity is forbidden."
    fi

    if container_can_read_source "10001" "10001"; then
        set_container_identity "10001" "10001"
        log "Container uid/gid selected from safe fallback: $CONTAINER_UID:$CONTAINER_GID."
        return
    fi

    log "INPX source is not readable by a safe non-root container user: $NAS_SOURCE_ROOT"
    fail "Fix NAS source permissions or run this script as a non-root owner that can read the source."
}}

prepare_host_paths()
{{
    token_file="$NAS_APP_ROOT/secrets/inpx-web-reader-auth-token.txt"
    data_dir="$NAS_APP_ROOT/data"

    mkdir -p "$data_dir"
    chown "$CONTAINER_UID:$CONTAINER_GID" "$data_dir" 2>/dev/null ||
        fail "Could not assign $data_dir to container uid $CONTAINER_UID. Run this script with sudo."
    chmod 700 "$data_dir" 2>/dev/null || true

    if converter_enabled; then
        converter_dir="$NAS_APP_ROOT/converter"
        converter_file="$converter_dir/fbc"
        chown "$CONTAINER_UID:$CONTAINER_GID" "$converter_dir" "$converter_file" 2>/dev/null ||
            fail "Could not assign $converter_file to container uid $CONTAINER_UID. Run this script with sudo."
        chmod 500 "$converter_dir" "$converter_file" 2>/dev/null ||
            fail "Could not restrict converter permissions: $converter_file"
    fi

    chown "$CONTAINER_UID:$CONTAINER_GID" "$token_file" 2>/dev/null ||
        fail "Could not assign $token_file to container uid $CONTAINER_UID. Run this script with sudo."
    chmod 400 "$token_file" 2>/dev/null ||
        fail "Could not restrict token file permissions: $token_file"
}}

verify_token_readable_by_container()
{{
    token_file="$NAS_APP_ROOT/secrets/inpx-web-reader-auth-token.txt"
    if docker run --rm \\
        --entrypoint /bin/sh \\
        --user "$CONTAINER_UID:$CONTAINER_GID" \\
        --read-only \\
        --cap-drop ALL \\
        -v "$token_file:/run/secrets/inpx_web_reader_auth_token:ro" \\
        "$INPX_WEB_READER_IMAGE" \\
        -lc 'test -r /run/secrets/inpx_web_reader_auth_token'
    then
        log "Token file permissions verified for container uid $CONTAINER_UID."
        return
    fi

    fail "Token file is not readable by the non-root container user."
}}

verify_converter_executable_by_container()
{{
    converter_dir="$NAS_APP_ROOT/converter"
    if docker run --rm \\
        --entrypoint /bin/sh \\
        --user "$CONTAINER_UID:$CONTAINER_GID" \\
        --read-only \\
        --cap-drop ALL \\
        -v "$converter_dir:/converter:ro" \\
        "$INPX_WEB_READER_IMAGE" \\
        -lc 'test -r /converter/fbc && test -x /converter/fbc'
    then
        log "Converter permissions verified for container uid $CONTAINER_UID."
        return
    fi

    fail "Converter is not readable and executable by the non-root container user."
}}

cd "$NAS_APP_ROOT"

[ -d "$NAS_SOURCE_ROOT" ] || fail "INPX source directory does not exist: $NAS_SOURCE_ROOT"
if [ ! -f "$NAS_APP_ROOT/{IMAGE_ARCHIVE_NAME}" ]; then
    fail "Docker image archive is missing: $NAS_APP_ROOT/{IMAGE_ARCHIVE_NAME}"
fi
if [ ! -f "$NAS_APP_ROOT/{BUNDLE_MANIFEST_NAME}" ]; then
    fail "Deployment manifest is missing: $NAS_APP_ROOT/{BUNDLE_MANIFEST_NAME}"
fi
if [ ! -f "$NAS_APP_ROOT/secrets/inpx-web-reader-auth-token.txt" ]; then
    fail "Access-password file is missing."
fi
if converter_enabled && [ ! -f "$NAS_APP_ROOT/docker-compose.converter.yml" ]; then
    fail "Converter Compose override is missing: $NAS_APP_ROOT/docker-compose.converter.yml"
fi
if converter_enabled && [ ! -f "$NAS_APP_ROOT/converter/fbc" ]; then
    fail "Converter executable is missing: $NAS_APP_ROOT/converter/fbc"
fi

load_generated_env
verify_image_archive_checksum

existing_container_id="$(bundle_compose ps -q inpx-web-reader || true)"
previous_image_id=""
if [ -z "$existing_container_id" ]; then
    if port_is_listening || port_is_published_by_docker; then
        fail "Host port $HOST_PORT is already in use. Regenerate the bundle with --host-port <free-port>."
    fi
    log "Host port $HOST_PORT appears free."
else
    previous_image_id="$(docker inspect --format '{{{{.Image}}}}' "$existing_container_id" 2>/dev/null || true)"
    log "Existing inpx-web-reader container found; update will reuse host port $HOST_PORT."
fi

log "Loading Docker image."
docker load -i "$NAS_APP_ROOT/{IMAGE_ARCHIVE_NAME}"
loaded_image_id="$(image_id_for_tag "$INPX_WEB_READER_IMAGE")"
[ -n "$loaded_image_id" ] || fail "Docker image archive did not provide image tag $INPX_WEB_READER_IMAGE."
loaded_image_platform="$(image_platform_for_tag "$INPX_WEB_READER_IMAGE")"
[ "$loaded_image_platform" = "linux/amd64" ] ||
    fail "Docker image has platform ${{loaded_image_platform:-<empty>}}; expected linux/amd64."
choose_container_identity
prepare_host_paths
if converter_enabled; then
    verify_converter_executable_by_container
else
    log "EPUB conversion is disabled for this bundle."
fi
verify_token_readable_by_container

log "Starting or updating inpx-web-reader through Docker Compose."
if ! bundle_compose up -d --force-recreate --remove-orphans; then
    log "Docker Compose could not start the new image."
    if rollback_previous_deployment; then
        fail "Deployment failed; the previous healthy image was restored."
    fi
    fail "Deployment failed and automatic rollback was not available."
fi

container_id="$(bundle_compose ps -q inpx-web-reader || true)"
if [ -z "$container_id" ]; then
    if rollback_previous_deployment; then
        fail "Deployment did not create a container; the previous healthy image was restored."
    fi
    fail "Deployment did not create a container and automatic rollback was not available."
fi

restart_policy="$(docker inspect --format '{{{{.HostConfig.RestartPolicy.Name}}}}' "$container_id" 2>/dev/null || true)"
if [ "$restart_policy" != "unless-stopped" ]; then
    log "Unexpected container restart policy: ${{restart_policy:-<empty>}}. Expected unless-stopped."
    if rollback_previous_deployment; then
        fail "Restart-policy verification failed; the previous healthy image was restored."
    fi
    fail "Restart-policy verification failed and automatic rollback was not available."
fi
log "Container restart policy verified: unless-stopped."

if command -v systemctl >/dev/null 2>&1; then
    if systemctl is-enabled docker >/dev/null 2>&1; then
        log "Docker service is enabled for host boot."
    else
        log "WARNING: Docker service is not reported as enabled by systemctl."
        log "Enable Docker autostart in UGOS if the container does not come back after a NAS reboot."
    fi
else
    log "systemctl is not available; verified container restart policy only."
    log "Make sure Docker itself starts after NAS reboot."
fi

log "Waiting for container health."
if ! wait_for_container_health "$container_id"; then
    log "Container did not become healthy. Recent logs:"
    bundle_compose logs --tail=120 inpx-web-reader || true
    if rollback_previous_deployment; then
        fail "Health check failed; the previous healthy image was restored."
    fi
    fail "Health check failed and automatic rollback was not available."
fi

cleanup_old_project_containers "$container_id"
cleanup_superseded_images "$loaded_image_id" "$previous_image_id"

lan_ip="$(detect_lan_ip || true)"
if [ -n "$lan_ip" ]; then
    log "inpx-web-reader is running at: http://$lan_ip:$HOST_PORT/"
else
    log "inpx-web-reader is running at: http://<NAS-IP>:$HOST_PORT/"
fi
log "Access-password file: $NAS_APP_ROOT/secrets/inpx-web-reader-auth-token.txt"
""",
        executable=True,
    )


def write_stop_script(
    bundle_root: Path,
    *,
    nas_app_root: str,
    converter_enabled: bool,
) -> None:
    write_text(
        bundle_root / "STOP_ON_NAS.sh",
        f"""#!/bin/sh
set -eu

NAS_APP_ROOT={shell_quote(nas_app_root)}
CONVERTER_ENABLED={1 if converter_enabled else 0}
export COMPOSE_PROJECT_NAME=inpx-web-reader

cd "$NAS_APP_ROOT"

if docker compose version >/dev/null 2>&1; then
    compose_command="docker compose"
elif command -v docker-compose >/dev/null 2>&1; then
    compose_command="docker-compose"
else
    echo "Docker Compose was not found." >&2
    exit 1
fi

if [ "$CONVERTER_ENABLED" -eq 1 ] && [ -f docker-compose.converter.yml ]; then
    $compose_command --env-file .env -f docker-compose.yml -f docker-compose.converter.yml down
else
    $compose_command --env-file .env -f docker-compose.yml down
fi
""",
        executable=True,
    )


def write_readme(
    bundle_root: Path,
    *,
    host_port: int,
    nas_app_root: str,
    converter_enabled: bool,
) -> None:
    conversion_status = (
        "EPUB conversion is enabled by the bundled read-only `fbc` executable."
        if converter_enabled
        else "EPUB conversion is disabled; original FB2 downloads remain available."
    )
    write_text(
        bundle_root / "README_NAS_DEPLOY.md",
        f"""# InpxWebReader deployment bundle

This folder is generated by `scripts/PrepareDeployBundle.py`.

After copying this folder's contents to:

```sh
{nas_app_root}
```

start or update the server on the NAS:

```sh
cd {nas_app_root}
sh RUN_ON_NAS.sh
```

The script verifies the image archive checksum, loads `{IMAGE_ARCHIVE_NAME}`,
starts Docker Compose, verifies the `unless-stopped` restart policy, and waits
for the container health check. If startup fails, it restores the previously
running image when one is available. Only after a healthy start does it remove
stopped containers from this Compose project and unused superseded
InpxWebReader images. It never runs a global Docker prune.

{conversion_status}

Open the web UI from your LAN:

```text
http://<NAS-IP>:{host_port}/
```

The access password is stored at:

```sh
{nas_app_root}/secrets/inpx-web-reader-auth-token.txt
```

When this bundle is regenerated into the same local output directory, the
existing password is reused unless `--token-file` or
`INPX_WEB_READER_DEPLOY_ACCESS_PASSWORD` is supplied.

The bundle also contains `manifest.json` and `{IMAGE_ARCHIVE_CHECKSUM_NAME}` so
the NAS can reject a damaged or partially copied image archive before loading
it.

Do not commit or publish this generated bundle. It contains a private password
and a local Docker image archive.
""",
    )


def inspect_image_property(
    repo_root: Path,
    image_tag: str,
    format_value: str,
    env: dict[str, str],
) -> str:
    return subprocess.run(
        resolve_command(
            ["docker", "image", "inspect", "--format", format_value, image_tag]
        ),
        cwd=repo_root,
        env=out_scoped_environment(repo_root, env),
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    ).stdout.strip()


def verify_image_platform(
    repo_root: Path,
    image_tag: str,
    env: dict[str, str],
) -> None:
    inspected_platform = inspect_image_property(
        repo_root,
        image_tag,
        "{{.Os}}/{{.Architecture}}",
        env,
    )
    if inspected_platform != "linux/amd64":
        raise RuntimeError(
            f"Saved runtime image platform is {inspected_platform or '<empty>'}, expected linux/amd64."
        )


def save_existing_image(
    repo_root: Path,
    bundle_root: Path,
    image_tag: str,
) -> None:
    ensure_linux_host()
    env = build_out_tool_environment(repo_root, "nas-deploy")
    ensure_docker_engine(repo_root, env)
    verify_image_platform(repo_root, image_tag, env)
    run(["docker", "save", "-o", str(bundle_root / IMAGE_ARCHIVE_NAME), image_tag], repo_root, env=env)


def build_and_save_image(
    repo_root: Path,
    bundle_root: Path,
    image_tag: str,
    docker_platform: str,
    build_jobs: int,
) -> None:
    ensure_linux_host()
    env = build_out_tool_environment(repo_root, "nas-deploy")
    ensure_docker_engine(repo_root, env)
    run(
        [
            "docker",
            "build",
            *docker_platform_arguments(docker_platform),
            "--build-arg",
            f"INPX_WEB_READER_BUILD_JOBS={build_jobs}",
            "-f",
            str(repo_root / SERVER_DOCKERFILE),
            "-t",
            image_tag,
            ".",
        ],
        repo_root,
        env=env,
    )
    verify_image_platform(repo_root, image_tag, env)
    run(["docker", "save", "-o", str(bundle_root / IMAGE_ARCHIVE_NAME), image_tag], repo_root, env=env)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while chunk := source.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def write_bundle_manifest(
    repo_root: Path,
    bundle_root: Path,
    image_tag: str,
    git_commit: str,
    git_dirty: bool,
    converter_enabled: bool,
) -> None:
    image_archive = bundle_root / IMAGE_ARCHIVE_NAME
    if not image_archive.is_file():
        return

    env = build_out_tool_environment(repo_root, "nas-deploy")
    image_id = inspect_image_property(repo_root, image_tag, "{{.Id}}", env)
    image_platform = inspect_image_property(
        repo_root,
        image_tag,
        "{{.Os}}/{{.Architecture}}",
        env,
    )
    archive_sha256 = sha256_file(image_archive)
    product_version = (repo_root / "VERSION.txt").read_text(encoding="utf-8").strip()
    manifest = {
        "schemaVersion": 1,
        "product": "InpxWebReader",
        "productVersion": product_version,
        "gitCommit": git_commit,
        "gitDirty": git_dirty,
        "imageTag": image_tag,
        "imageId": image_id,
        "imagePlatform": image_platform,
        "imageArchive": IMAGE_ARCHIVE_NAME,
        "imageArchiveSha256": archive_sha256,
        "converterEnabled": converter_enabled,
    }
    write_text(
        bundle_root / BUNDLE_MANIFEST_NAME,
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
    )
    write_text(
        bundle_root / IMAGE_ARCHIVE_CHECKSUM_NAME,
        f"{archive_sha256}  {IMAGE_ARCHIVE_NAME}\n",
    )


def copy_compose_template(repo_root: Path, bundle_root: Path) -> None:
    shutil.copy2(repo_root / SERVER_COMPOSE_TEMPLATE, bundle_root / "docker-compose.yml")


def prepare_bundle(args: argparse.Namespace) -> Path:
    ensure_linux_host()
    docker_platform_arguments(args.docker_platform)
    if args.skip_image_build and args.reuse_image:
        raise RuntimeError("--skip-image-build and --reuse-image cannot be combined.")
    repo_root = repository_root()
    repo_out_root = out_root(repo_root)
    output_dir = resolve_output_dir(repo_root, args.output)
    nas_source_root = validate_absolute_nas_path(args.nas_source_root, "--nas-source-root")
    nas_app_root = validate_absolute_nas_path(args.nas_app_root, "--nas-app-root")
    validate_non_overlapping_nas_roots(nas_source_root, nas_app_root)
    validate_host_port(args.host_port)
    build_jobs = args.build_jobs if args.build_jobs is not None else default_parallel_jobs()
    validate_parallel_jobs(build_jobs)
    auth_token = resolve_auth_token(output_dir, args.token_file)
    converter_enabled = not args.skip_converter_download

    prepare_output_directory(output_dir, repo_out_root)
    copy_compose_template(repo_root, output_dir)
    if converter_enabled:
        write_converter_compose_override(output_dir)
    write_env_file(
        output_dir,
        image_tag=args.image_tag,
        host_port=args.host_port,
        nas_source_root=nas_source_root,
        nas_app_root=nas_app_root,
        converter_enabled=converter_enabled,
    )
    write_token_file(output_dir, auth_token)
    write_run_script(
        output_dir,
        host_port=args.host_port,
        nas_source_root=nas_source_root,
        nas_app_root=nas_app_root,
        converter_enabled=converter_enabled,
    )
    write_stop_script(
        output_dir,
        nas_app_root=nas_app_root,
        converter_enabled=converter_enabled,
    )
    write_readme(
        output_dir,
        host_port=args.host_port,
        nas_app_root=nas_app_root,
        converter_enabled=converter_enabled,
    )

    stages: list[TimedStage] = []
    if not args.skip_converter_download:
        stages.append(
            TimedStage(
                f"Download {args.converter_asset_name} ({args.converter_version})",
                lambda: download_converter(output_dir, args.converter_version, args.converter_asset_name),
            )
        )
    if not args.skip_image_build:
        if args.reuse_image:
            stages.append(
                TimedStage(
                    f"Save verified Docker image ({args.image_tag})",
                    lambda: save_existing_image(repo_root, output_dir, args.image_tag),
                )
            )
        else:
            stages.append(
                TimedStage(
                    f"Build and save Docker image ({args.image_tag})",
                    lambda: build_and_save_image(
                        repo_root,
                        output_dir,
                        args.image_tag,
                        args.docker_platform,
                        build_jobs,
                    ),
                )
            )

    if stages:
        run_timed_stages(stages, success_message="NAS deployment bundle prepared successfully.")
    else:
        print("==> NAS deployment bundle files prepared successfully.", flush=True)

    write_bundle_manifest(
        repo_root,
        output_dir,
        args.image_tag,
        args.git_commit,
        args.git_dirty,
        converter_enabled,
    )

    print(f"==> Bundle: {output_dir}", flush=True)
    return output_dir


def main() -> int:
    try:
        prepare_bundle(parse_args())
        return 0
    except Exception as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
