from __future__ import annotations

from pathlib import PurePosixPath


MIN_ACCESS_PASSWORD_LENGTH = 12
MAX_ACCESS_PASSWORD_LENGTH = 256


def validate_host_port(port: int, source_description: str = "--host-port") -> None:
    if port < 1 or port > 65535:
        raise RuntimeError(f"{source_description} must be in range 1..65535.")


def validate_access_password(password: str, source_description: str) -> str:
    if password != password.strip():
        raise RuntimeError(f"{source_description} must not start or end with whitespace.")
    if len(password) < MIN_ACCESS_PASSWORD_LENGTH:
        raise RuntimeError(f"{source_description} must contain at least {MIN_ACCESS_PASSWORD_LENGTH} characters.")
    if len(password) > MAX_ACCESS_PASSWORD_LENGTH:
        raise RuntimeError(f"{source_description} must contain at most {MAX_ACCESS_PASSWORD_LENGTH} characters.")
    if any(ord(character) < 0x21 or ord(character) > 0x7E for character in password):
        raise RuntimeError(f"{source_description} must contain only printable ASCII characters without spaces.")
    return password


def validate_absolute_host_path(path: str, label: str) -> str:
    normalized_path = PurePosixPath(path)
    if not normalized_path.is_absolute():
        raise RuntimeError(f"{label} must be an absolute target Linux host path.")
    if ".." in normalized_path.parts:
        raise RuntimeError(f"{label} must not contain parent-directory components.")
    if any(ord(character) < 0x20 or ord(character) == 0x7F for character in path):
        raise RuntimeError(f"{label} must not contain control characters.")
    if any(character in path for character in "'\"$`\\"):
        raise RuntimeError(f"{label} must not contain quotes, '$', backticks, or backslashes.")
    normalized_text = str(normalized_path)
    if normalized_text in {"/", "//"}:
        raise RuntimeError(f"{label} must not be the filesystem root.")
    return normalized_text


def validate_non_overlapping_host_roots(source_root: str, app_root: str) -> None:
    source_path = PurePosixPath(source_root)
    app_path = PurePosixPath(app_root)
    if source_path.is_relative_to(app_path) or app_path.is_relative_to(source_path):
        raise RuntimeError("--host-source-root and --host-app-root must not overlap.")
