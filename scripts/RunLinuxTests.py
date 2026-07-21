from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import json
import os
import platform
import re
import shutil
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

sys.dont_write_bytecode = True
sys.pycache_prefix = str(Path(__file__).resolve().parent.parent / "out" / "pycache")

from _common import (
    TimedStage,
    build_cmake_build_command,
    build_cmake_configure_command,
    build_ctest_command,
    default_parallel_jobs,
    docker_platform_arguments,
    ensure_docker_engine,
    out_scoped_environment,
    repository_root,
    resolve_command,
    run_command,
    run_timed_stages,
    validate_parallel_jobs,
)
from _server_config import server_limit_env_lines


DEFAULT_PRESET = "linux-debug"
DEFAULT_IMAGE_TAG = "inpx-web-reader:local"
DEFAULT_DOCKER_PLATFORM = "linux/amd64"
DEFAULT_COMPOSE_FILE = Path("deploy/inpx-web-reader/docker-compose.yml")
DEFAULT_DOCKERFILE = Path("deploy/inpx-web-reader/Dockerfile")
DEFAULT_TOKEN = "docker-smoke-token"
DEFAULT_SMOKE_BOOK_COUNT = 2048
SMOKE_BOOKS_PER_ARCHIVE = 5
INITIAL_SMOKE_TITLE = "Docker Smoke Book"
UPDATED_SMOKE_TITLE = "Docker Smoke Book Updated"
ADDED_SMOKE_TITLE = "Fixture Added Book"
SMOKE_LIMITS = {
    "maxPageSize": 20,
    "maxHttpThreads": 2,
    "maxHttpQueuedRequests": 16,
    "maxBackendQueueDepth": 16,
    "maxScanWorkers": 1,
    "maxConcurrentDownloads": 2,
    "maxRequestBodyBytes": 65536,
    "httpReadTimeoutMs": 15000,
    "httpWriteTimeoutMs": 30000,
    "maxCoverCacheMiB": 32,
    "maxSteadyStateMemoryMiB": 1024,
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run InpxWebReader native and Docker deployment checks on Linux."
    )
    parser.add_argument("--preset", default=DEFAULT_PRESET)
    parser.add_argument("--image-tag", default=DEFAULT_IMAGE_TAG)
    parser.add_argument(
        "--docker-platform",
        default=DEFAULT_DOCKER_PLATFORM,
        help=(
            "Docker platform used for Linux build/test and server image builds; must be linux/amd64."
        ),
    )
    parser.add_argument(
        "--parallel-jobs",
        type=int,
        default=None,
        help="Use the same CMake build and CTest job count. Overridden by --build-jobs or --test-jobs.",
    )
    parser.add_argument(
        "--build-jobs",
        type=int,
        default=None,
        help="CMake build parallelism. Defaults to host CPU parallelism.",
    )
    parser.add_argument(
        "--test-jobs",
        type=int,
        default=None,
        help="CTest parallelism. Defaults to host CPU parallelism.",
    )
    parser.add_argument("--skip-configure", action="store_true")
    parser.add_argument("--skip-native", action="store_true")
    parser.add_argument("--skip-docker", action="store_true")
    parser.add_argument("--skip-compose", action="store_true")
    parser.add_argument("--keep-smoke-workspace", action="store_true")
    parser.add_argument(
        "--smoke-book-count",
        type=int,
        default=DEFAULT_SMOKE_BOOK_COUNT,
        help=(
            "Number of books in the initial Compose import. "
            f"Defaults to {DEFAULT_SMOKE_BOOK_COUNT}."
        ),
    )
    return parser.parse_args()


def ensure_linux_host() -> None:
    if sys.platform != "linux":
        raise RuntimeError(
            "RunLinuxTests.py is a Linux worker. Use scripts/RunRemoteLinux.py from the development host."
        )
    if platform.machine().lower() not in {"x86_64", "amd64"}:
        raise RuntimeError("RunLinuxTests.py requires an x86_64 Linux worker.")


def resolve_native_job_counts(args: argparse.Namespace) -> tuple[int, int]:
    default_jobs = default_parallel_jobs()
    build_jobs = args.build_jobs if args.build_jobs is not None else args.parallel_jobs
    test_jobs = args.test_jobs if args.test_jobs is not None else args.parallel_jobs
    return (
        build_jobs if build_jobs is not None else default_jobs,
        test_jobs if test_jobs is not None else default_jobs,
    )


def run(command: list[str], cwd: Path, env: dict[str, str] | None = None) -> None:
    run_command(command, cwd, env=env)


def run_capture(command: list[str], cwd: Path, env: dict[str, str] | None = None) -> str:
    completed = subprocess.run(
        resolve_command(command),
        cwd=cwd,
        env=out_scoped_environment(env=env),
        check=True,
        stdout=subprocess.PIPE,
        text=True,
        stderr=subprocess.STDOUT,
    )
    return completed.stdout


def resolve_docker_compose_command(repo_root: Path) -> list[str]:
    completed = subprocess.run(
        resolve_command(["docker", "compose", "version"]),
        cwd=repo_root,
        env=out_scoped_environment(repo_root),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        check=False,
    )
    if completed.returncode == 0:
        return ["docker", "compose"]

    if shutil.which("docker-compose") is not None:
        return ["docker-compose"]

    raise RuntimeError(
        "Docker Compose was not found. Install the Docker Compose plugin or docker-compose. "
        f"`docker compose version` output:\n{completed.stdout.strip()}"
    )


def find_free_port() -> int:
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        return int(listener.getsockname()[1])


def write_smoke_book_file(
    path: Path,
    title: str,
    author_first: str,
    author_last: str,
) -> None:
    cover_png_base64 = (
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/"
        "x8AAwMCAO+/p9sAAAAASUVORK5CYII="
    )
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        f"""<?xml version="1.0" encoding="utf-8"?>
<FictionBook xmlns:l="http://www.w3.org/1999/xlink">
  <description>
    <title-info>
      <genre>sf</genre>
      <author><first-name>{author_first}</first-name><last-name>{author_last}</last-name></author>
      <book-title>{title}</book-title>
      <lang>en</lang>
      <coverpage><image l:href="#cover.png"/></coverpage>
    </title-info>
  </description>
  <body><section><p>{title} smoke payload</p></section></body>
  <binary id="cover.png" content-type="image/png">{cover_png_base64}</binary>
</FictionBook>
""",
        encoding="utf-8",
    )


def write_smoke_book(books_dir: Path, book_count: int = DEFAULT_SMOKE_BOOK_COUNT) -> None:
    books_dir.mkdir(parents=True, exist_ok=True)
    for index in range(book_count):
        title = INITIAL_SMOKE_TITLE if index == 0 else f"Fixture Load Book {index:03d}"
        author_first = "Docker" if index == 0 else "Fixture"
        author_last = "Author" if index == 0 else f"Author {index:03d}"
        file_name = "docker-smoke.fb2" if index == 0 else f"fixture-load-{index:06d}.fb2"
        write_smoke_book_file(books_dir / file_name, title, author_first, author_last)


def snapshot_files(root: Path) -> dict[str, str]:
    result: dict[str, str] = {}
    for path in sorted(root.rglob("*")):
        if not path.is_file():
            continue

        digest = hashlib.sha256()
        with path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
        result[path.relative_to(root).as_posix()] = digest.hexdigest()
    return result


def generate_smoke_catalog(
    repo_root: Path,
    books_dir: Path,
    output_dir: Path,
) -> None:
    run(
        [
            sys.executable,
            str(repo_root / "scripts" / "GenerateInpxFixtureCatalog.py"),
            str(books_dir),
            "--output-dir",
            str(output_dir),
            "--books-per-archive",
            str(SMOKE_BOOKS_PER_ARCHIVE),
            "--force",
        ],
        repo_root,
    )


def create_smoke_workspace(
    repo_root: Path,
    book_count: int = DEFAULT_SMOKE_BOOK_COUNT,
) -> Path:
    timestamp = f"{time.strftime('%Y%m%d%H%M%S')}-{time.time_ns()}"
    workspace = repo_root / "out" / "server-docker-smoke" / timestamp
    books_dir = workspace / "books"
    source_dir = workspace / "source"
    write_smoke_book(books_dir, book_count)
    generate_smoke_catalog(repo_root, books_dir, source_dir)
    return workspace


def publish_smoke_catalog_update(
    repo_root: Path,
    workspace: Path,
    initial_book_count: int,
) -> dict[str, str]:
    books_dir = workspace / "books"
    source_dir = workspace / "source"
    staged_source_dir = workspace / "source-update"
    write_smoke_book_file(
        books_dir / "docker-smoke.fb2",
        UPDATED_SMOKE_TITLE,
        "Docker",
        "Author",
    )
    write_smoke_book_file(
        books_dir / f"fixture-load-{initial_book_count:06d}.fb2",
        ADDED_SMOKE_TITLE,
        "Fixture",
        "Added Author",
    )
    generate_smoke_catalog(repo_root, books_dir, staged_source_dir)

    changed_archive_indexes = {
        1,
        (initial_book_count // SMOKE_BOOKS_PER_ARCHIVE) + 1,
    }
    active_archive_root = source_dir / "lib.rus.ec"
    staged_archive_root = staged_source_dir / "lib.rus.ec"
    for archive_index in sorted(changed_archive_indexes):
        archive_name = f"fb2-{archive_index:03d}.zip"
        shutil.copy2(staged_archive_root / archive_name, active_archive_root / archive_name)

    # Publish the INPX last so a manually triggered rescan cannot observe new metadata
    # before all changed payload archives are in place.
    shutil.copy2(staged_source_dir / "catalog.inpx", source_dir / "catalog.inpx")
    return snapshot_files(source_dir)


def write_compose_env(workspace: Path, image_tag: str, host_port: int) -> Path:
    env_path = workspace / "compose.env"
    data_path = workspace / "data"
    token_path = workspace / "auth-token.txt"
    data_path.mkdir(parents=True, exist_ok=True)
    data_path.chmod(0o777)
    token_path.write_text(DEFAULT_TOKEN + "\n", encoding="utf-8")
    token_path.chmod(0o644)
    env_path.write_text(
        "\n".join(
            [
                f"INPX_WEB_READER_IMAGE={image_tag}",
                f"INPX_WEB_READER_HOST_PORT={host_port}",
                f"INPX_WEB_READER_SOURCE_PATH={workspace / 'source'}",
                f"INPX_WEB_READER_DATA_PATH={data_path}",
                "INPX_WEB_READER_INPX_PATH=",
                "INPX_WEB_READER_ARCHIVE_ROOT=",
                f"INPX_WEB_READER_AUTH_TOKEN_FILE={token_path}",
                "INPX_WEB_READER_LOG_LEVEL=info",
                "INPX_WEB_READER_LOG_MAX_FILE_SIZE_MIB=20",
                "INPX_WEB_READER_LOG_MAX_ROTATED_FILES=4",
                *server_limit_env_lines(SMOKE_LIMITS),
                "",
            ]
        ),
        encoding="utf-8",
    )
    return env_path


def append_host_linux_native_stages(
    stages: list[TimedStage],
    repo_root: Path,
    build_root: Path,
    args: argparse.Namespace,
    build_jobs: int,
    test_jobs: int,
) -> None:
    native_environment = dict(os.environ)
    native_environment["VCPKG_MAX_CONCURRENCY"] = str(build_jobs)
    if not args.skip_configure:
        stages.append(
            TimedStage(
                f"Configure Linux server build ({args.preset})",
                lambda: run(
                    build_cmake_configure_command(args.preset),
                    repo_root,
                    env=native_environment,
                ),
            )
        )
    stages.append(
        TimedStage(
            f"Build Linux server targets ({args.preset}, --parallel {build_jobs})",
            lambda: run(
                build_cmake_build_command(args.preset, "Debug", build_jobs, False),
                repo_root,
                env=native_environment,
            ),
        )
    )
    stages.append(
        TimedStage(
            f"Run Linux server tests ({args.preset}, -j {test_jobs})",
            lambda: run(
                build_ctest_command(
                    build_root,
                    "Debug",
                    test_jobs,
                    False,
                ),
                repo_root,
                env=native_environment,
            ),
        )
    )


def request_json(
    base_url: str,
    path: str,
    method: str = "GET",
    body: object | None = None,
    auth_token: str | None = DEFAULT_TOKEN,
) -> tuple[int, dict]:
    data = None
    headers = {"Authorization": f"Bearer {auth_token}"} if auth_token is not None else {}
    if body is not None:
        data = json.dumps(body).encode("utf-8")
        headers["Content-Type"] = "application/json"

    request = urllib.request.Request(base_url + path, data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(request, timeout=10) as response:
            payload = response.read()
            return response.status, json.loads(payload.decode("utf-8")) if payload else {}
    except urllib.error.HTTPError as error:
        payload = error.read()
        return error.code, json.loads(payload.decode("utf-8")) if payload else {}


def request_bytes(base_url: str, path: str) -> tuple[int, bytes, dict[str, str]]:
    request = urllib.request.Request(
        base_url + path,
        headers={"Authorization": f"Bearer {DEFAULT_TOKEN}"},
        method="GET",
    )
    with urllib.request.urlopen(request, timeout=10) as response:
        return response.status, response.read(), dict(response.headers)


def wait_for_health(base_url: str) -> None:
    for _ in range(120):
        try:
            with urllib.request.urlopen(base_url + "/api/health", timeout=3) as response:
                health = json.loads(response.read().decode("utf-8"))
                if response.status == 200 and health.get("ok"):
                    return
        except (OSError, urllib.error.URLError, json.JSONDecodeError):
            pass
        time.sleep(0.5)

    raise RuntimeError("Docker Compose server health endpoint did not become ready.")


def verify_authentication(base_url: str) -> None:
    for auth_token in (None, "wrong-docker-smoke-token"):
        code, payload = request_json(base_url, "/api/status", auth_token=auth_token)
        if code != 401 or payload.get("error", {}).get("code") != "unauthorized":
            token_description = "missing" if auth_token is None else "incorrect"
            raise RuntimeError(
                f"Server accepted or misreported a {token_description} bearer token: HTTP {code}: {payload}"
            )


def wait_for_status(base_url: str) -> dict:
    for _ in range(120):
        code, status = request_json(base_url, "/api/status")
        if (
            code == 200
            and status.get("status") == "open"
            and isinstance(status.get("inpxSource"), dict)
        ):
            return status
        time.sleep(0.5)

    raise RuntimeError("Docker Compose server status endpoint did not become ready.")


def verify_runtime_status(status: dict) -> None:
    runtime = status.get("runtime")
    if not isinstance(runtime, dict):
        raise RuntimeError(f"Server status omitted runtime support metrics: {status}")

    http = runtime.get("http", {})
    backend = runtime.get("backend", {})
    scan = runtime.get("scan", {})
    downloads = runtime.get("downloads", {})
    storage = runtime.get("storage", {})
    resources = runtime.get("resources", {})
    if runtime.get("uptimeSeconds") is None:
        raise RuntimeError(f"Server status omitted uptime: {runtime}")
    if http.get("maxWorkers") != 2 or http.get("maxQueuedRequests") != 16:
        raise RuntimeError(f"Server HTTP runtime limits do not match Compose smoke env: {http}")
    if backend.get("maxQueueDepth") != 16:
        raise RuntimeError(f"Server backend queue metrics do not match Compose smoke env: {backend}")
    if scan.get("maxConcurrentJobs") != 1 or scan.get("maxWorkers") != 1:
        raise RuntimeError(f"Server scan runtime metrics do not match Compose smoke env: {scan}")
    if downloads.get("maxConcurrent") != 2:
        raise RuntimeError(f"Server download metrics do not match Compose smoke env: {downloads}")
    if (
        storage.get("cacheRootPresent") is not True
        or storage.get("cacheDatabasePresent") is not True
        or storage.get("runtimeWorkspacePresent") is not True
    ):
        raise RuntimeError(f"Server storage runtime summary did not report mounted data roots: {storage}")
    for key in ("coverCacheBytes", "inpxScanWorkspaceBytes", "downloadWorkspaceBytes"):
        if not isinstance(storage.get(key), int) or storage[key] < 0:
            raise RuntimeError(f"Server storage runtime summary omitted {key}: {storage}")

    resident = resources.get("residentMemoryBytes")
    peak = resources.get("peakResidentMemoryBytes")
    if not isinstance(resident, int) or resident <= 0:
        raise RuntimeError(f"Linux runtime status did not report resident RSS: {resources}")
    if not isinstance(peak, int) or peak <= 0:
        raise RuntimeError(f"Linux runtime status did not report peak RSS: {resources}")
    if resident > 1024 * 1024 * 1024:
        raise RuntimeError(f"Server resident memory exceeded the 1024 MiB smoke budget: {resident}")
    if resources.get("maxCoverCacheBytes") != 32 * 1024 * 1024:
        raise RuntimeError(f"Server cover-cache budget does not match Compose smoke env: {resources}")
    if resources.get("maxSteadyStateMemoryBytes") != 1024 * 1024 * 1024:
        raise RuntimeError(f"Server memory budget does not match Compose smoke env: {resources}")


def wait_for_scan_completion(
    base_url: str,
    mode: str,
    *,
    accept_existing_completed: bool = False,
) -> dict:
    progress: dict | None = None
    if accept_existing_completed:
        code, progress = request_json(base_url, "/api/scan/progress")
        if code != 200:
            raise RuntimeError(f"Initial scan progress failed: HTTP {code}: {progress}")
        if not progress.get("active") and progress.get("status") == "completed":
            return progress
        if not progress.get("active"):
            progress = None

    if progress is None:
        code, started = request_json(
            base_url,
            "/api/scan/start",
            method="POST",
            body={"mode": mode, "warningLimit": 5},
        )
        if code == 202:
            job_id = started.get("jobId")
            if not isinstance(job_id, int) or job_id <= 0:
                raise RuntimeError(f"Scan start omitted a valid job id: HTTP {code}: {started}")
        elif code == 409 and started.get("error", {}).get("code") == "conflict":
            code, progress = request_json(base_url, "/api/scan/progress")
            if code != 200:
                raise RuntimeError(f"Scan progress failed after start conflict: HTTP {code}: {progress}")
            if not progress.get("active"):
                if progress.get("status") == "completed":
                    return progress
                raise RuntimeError(f"Scan start conflicted but no active scan was reported: {progress}")
        else:
            raise RuntimeError(f"Scan start failed: HTTP {code}: {started}")

    for _ in range(240):
        if progress is None or progress.get("active"):
            code, progress = request_json(base_url, "/api/scan/progress")
            if code != 200:
                raise RuntimeError(f"Scan progress failed: HTTP {code}: {progress}")
        if not progress.get("active"):
            if progress.get("status") != "completed":
                raise RuntimeError(f"Scan did not complete: {progress}")
            return progress
        time.sleep(0.25)

    raise RuntimeError(f"Docker Compose {mode} scan did not complete in time.")


def require_scan_result(progress: dict) -> dict:
    result = progress.get("result")
    if progress.get("status") != "completed" or not isinstance(result, dict):
        raise RuntimeError(f"Completed scan omitted its result summary: {progress}")
    return result


def verify_initial_scan_result(progress: dict, expected_book_count: int) -> None:
    result = require_scan_result(progress)
    expected_segment_count = (
        expected_book_count + SMOKE_BOOKS_PER_ARCHIVE - 1
    ) // SMOKE_BOOKS_PER_ARCHIVE
    expected = {
        "totalRecords": expected_book_count,
        "scannedRecords": expected_book_count,
        "parsedFb2Records": expected_book_count,
        "addedRecords": expected_book_count,
        "updatedRecords": 0,
        "markedUnavailableRecords": 0,
        "unavailableRecords": 0,
        "skippedRecords": 0,
        "reusedRecords": 0,
        "segmentsTotal": expected_segment_count,
        "segmentsUnchanged": 0,
        "segmentsAdded": expected_segment_count,
        "segmentsChanged": 0,
        "segmentsRemoved": 0,
        "archivesSkipped": 0,
        "archivesOpened": expected_segment_count,
        "warningCount": 0,
    }
    mismatches = {
        key: {"expected": value, "actual": result.get(key)}
        for key, value in expected.items()
        if result.get(key) != value
    }
    archive_bytes_read = result.get("archiveBytesRead")
    if mismatches or not isinstance(archive_bytes_read, int) or archive_bytes_read <= 0:
        raise RuntimeError(
            "Initial import did not fully parse and publish the generated catalog: "
            f"mismatches={mismatches}, result={result}"
        )


def verify_incremental_update_result(progress: dict, initial_book_count: int) -> None:
    result = require_scan_result(progress)
    initial_segment_count = (
        initial_book_count + SMOKE_BOOKS_PER_ARCHIVE - 1
    ) // SMOKE_BOOKS_PER_ARCHIVE
    final_partial_segment_size = initial_book_count % SMOKE_BOOKS_PER_ARCHIVE
    adds_segment = final_partial_segment_size == 0
    existing_changed_segments = 1
    if initial_segment_count > 1 and final_partial_segment_size != 0:
        existing_changed_segments += 1
    updated_records = min(initial_book_count, SMOKE_BOOKS_PER_ARCHIVE)
    if initial_segment_count > 1 and final_partial_segment_size != 0:
        updated_records += final_partial_segment_size

    expected = {
        "totalRecords": initial_book_count + 1,
        "scannedRecords": initial_book_count + 1,
        "parsedFb2Records": updated_records + 1,
        "addedRecords": 1,
        "updatedRecords": updated_records,
        "markedUnavailableRecords": 0,
        "unavailableRecords": 0,
        "skippedRecords": 0,
        "reusedRecords": initial_book_count - updated_records,
        "segmentsTotal": initial_segment_count + int(adds_segment),
        "segmentsUnchanged": initial_segment_count - existing_changed_segments,
        "segmentsAdded": int(adds_segment),
        "segmentsChanged": existing_changed_segments,
        "segmentsRemoved": 0,
        "archivesSkipped": initial_segment_count - existing_changed_segments,
        "archivesOpened": existing_changed_segments + int(adds_segment),
        "warningCount": 0,
    }
    mismatches = {
        key: {"expected": value, "actual": result.get(key)}
        for key, value in expected.items()
        if result.get(key) != value
    }
    archive_bytes_read = result.get("archiveBytesRead")
    if mismatches or not isinstance(archive_bytes_read, int) or archive_bytes_read <= 0:
        raise RuntimeError(
            "Incremental rescan did not reuse unchanged segments and publish only the update: "
            f"mismatches={mismatches}, result={result}"
        )


def find_exact_title(base_url: str, title: str) -> dict:
    query = urllib.parse.urlencode(
        {
            "text": title,
            "fields": "title",
            "sort": "title",
            "direction": "asc",
            "limit": 10,
        }
    )
    code, books = request_json(base_url, f"/api/books?{query}")
    matching = [item for item in books.get("items", []) if item.get("title") == title]
    if code != 200 or books.get("totalCount") != 1 or len(matching) != 1:
        raise RuntimeError(f"Exact catalog title search failed for {title!r}: HTTP {code}: {books}")
    return matching[0]


def verify_book_payload(
    base_url: str,
    book: dict,
    expected_title: str,
    expected_payload: bytes | None,
    *,
    verify_cover: bool,
) -> int:
    book_id = int(book["id"])
    code, details = request_json(base_url, f"/api/books/{book_id}")
    if code != 200 or details.get("book", {}).get("title") != expected_title:
        raise RuntimeError(f"Book details failed for {expected_title!r}: HTTP {code}: {details}")

    if verify_cover:
        code, cover, cover_headers = request_bytes(base_url, f"/api/covers/{book_id}")
        if code != 200 or not cover or not cover_headers.get("Content-Type", "").startswith("image/"):
            raise RuntimeError(f"Book cover failed for {expected_title!r}: HTTP {code}")

    code, payload, headers = request_bytes(base_url, f"/api/books/{book_id}/download?format=original")
    disposition = headers.get("Content-Disposition", "")
    payload_matches = expected_payload is None or payload == expected_payload
    if code != 200 or not payload_matches or "filename*=" not in disposition:
        raise RuntimeError(f"Book download failed for {expected_title!r} or returned different source bytes.")

    return book_id


def verify_catalog_flow(
    base_url: str,
    expected_book_count: int,
    expected_primary_title: str,
    expected_primary_payload: bytes | None = None,
    expected_tail_title: str | None = None,
    expected_tail_payload: bytes | None = None,
) -> tuple[int, str]:
    code, catalog = request_json(base_url, "/api/books?limit=1")
    snapshot_id = catalog.get("catalogSnapshotId")
    if (
        code != 200
        or catalog.get("totalCount") != expected_book_count
        or not isinstance(snapshot_id, str)
        or not snapshot_id
    ):
        raise RuntimeError(
            f"Catalog scale probe expected {expected_book_count} books: HTTP {code}: {catalog}"
        )
    if expected_book_count > 1:
        cursor = catalog.get("nextCursor")
        if not isinstance(cursor, str) or not cursor:
            raise RuntimeError(f"Catalog scale probe omitted its continuation cursor: {catalog}")
        code, continuation = request_json(
            base_url,
            f"/api/books?limit=1&cursor={urllib.parse.quote(cursor, safe='')}",
        )
        if (
            code != 200
            or continuation.get("catalogSnapshotId") != snapshot_id
            or continuation.get("totalCount") is not None
            or continuation.get("facets") is not None
            or continuation.get("offset") != 1
            or len(continuation.get("items", [])) != 1
        ):
            raise RuntimeError(
                f"Catalog cursor scale continuation failed: HTTP {code}: {continuation}"
            )

    code, stats = request_json(base_url, "/api/stats")
    if code != 200 or stats.get("bookCount") != expected_book_count or stats.get("unavailableBookCount") != 0:
        raise RuntimeError(f"Catalog statistics disagree with the imported catalog: HTTP {code}: {stats}")
    code, source = request_json(base_url, "/api/source")
    source_summary = source.get("source", {})
    if (
        code != 200
        or source_summary.get("totalBookCount") != expected_book_count
        or source_summary.get("availableBookCount") != expected_book_count
        or source_summary.get("unavailableBookCount") != 0
        or source_summary.get("lastSeenSnapshotId") != snapshot_id
    ):
        raise RuntimeError(f"Source summary disagrees with the imported catalog: HTTP {code}: {source}")

    primary = find_exact_title(base_url, expected_primary_title)
    primary_id = verify_book_payload(
        base_url,
        primary,
        expected_primary_title,
        expected_primary_payload,
        verify_cover=True,
    )
    if expected_tail_title is not None:
        tail = find_exact_title(base_url, expected_tail_title)
        verify_book_payload(
            base_url,
            tail,
            expected_tail_title,
            expected_tail_payload,
            verify_cover=True,
        )

    return primary_id, snapshot_id


def verify_web_ui_serving(base_url: str) -> None:
    code, payload, headers = request_bytes(base_url, "/")
    content_type = headers.get("Content-Type", "")
    if code != 200 or "text/html" not in content_type or b"<html" not in payload.lower():
        raise RuntimeError(
            f"Web UI entry point was not served as HTML: HTTP {code}, Content-Type={content_type!r}."
        )


def verify_parallel_polling(base_url: str) -> None:
    def poll_client(client_index: int) -> None:
        for iteration in range(3):
            code, status = request_json(base_url, "/api/status")
            if (
                code != 200
                or status.get("status") != "open"
                or not isinstance(status.get("inpxSource"), dict)
            ):
                raise RuntimeError(f"Parallel status poll failed for client {client_index}: HTTP {code}: {status}")

            offset = (client_index + iteration) % 8
            code, books = request_json(base_url, f"/api/books?limit=5&offset={offset}")
            if code != 200 or not isinstance(books.get("items"), list):
                raise RuntimeError(f"Parallel catalog poll failed for client {client_index}: HTTP {code}: {books}")

    with concurrent.futures.ThreadPoolExecutor(max_workers=10) as executor:
        futures = [executor.submit(poll_client, index) for index in range(10)]
        for future in futures:
            future.result(timeout=20)


def verify_log_does_not_leak_token(log_text: str) -> None:
    if DEFAULT_TOKEN in log_text:
        raise RuntimeError("Server log leaked the bearer token used by the Docker smoke test.")


def verify_runtime_log_output(log_text: str, source_label: str) -> None:
    required_markers = (
        "InpxWebReader startup:",
        "logLevel=info logMaxFileSizeMiB=20 logMaxRotatedFiles=4",
        "source='/source/catalog.inpx' archiveRoot='/source/lib.rus.ec'",
        "INPX initial scan completed. jobId=",
        "INPX rescan completed. jobId=",
        "HTTP request: requestId=",
        " path=/ status=200 ",
    )
    missing_markers = [marker for marker in required_markers if marker not in log_text]
    if missing_markers:
        raise RuntimeError(
            f"{source_label} omitted required backend/web-serving log markers: {missing_markers}."
        )
    if re.search(r"(?m)^\[\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z\]", log_text) is None:
        raise RuntimeError(f"{source_label} did not contain a UTC timestamped server record.")
    verify_log_does_not_leak_token(log_text)


def wait_for_persistent_runtime_log(repo_root: Path, container_id: str) -> str:
    deadline = time.monotonic() + 10.0
    while True:
        log_text = run_capture(
            [
                "docker",
                "exec",
                container_id,
                "cat",
                "/data/runtime/Logs/inpx-web-reader.log",
            ],
            repo_root,
        )
        try:
            verify_runtime_log_output(log_text, "Persistent runtime log")
            return log_text
        except RuntimeError:
            if time.monotonic() >= deadline:
                raise
            time.sleep(0.25)


def verify_container_runtime_logs(repo_root: Path, container_id: str) -> None:
    files_present = run_capture(
        [
            "docker",
            "exec",
            container_id,
            "sh",
            "-lc",
            (
                "test -f /data/cache/Database/inpx-web-reader.db "
                "&& test -f /data/runtime/Logs/inpx-web-reader.log "
                "&& echo present"
            ),
        ],
        repo_root,
    ).strip()
    if files_present != "present":
        raise RuntimeError("Server database or runtime log was not created under /data.")

    wait_for_persistent_runtime_log(repo_root, container_id)
    container_log_text = run_capture(["docker", "logs", container_id], repo_root)
    verify_runtime_log_output(container_log_text, "Docker console log")


def verify_container_mounts(
    repo_root: Path,
    compose_file: Path,
    env_file: Path,
    project_name: str,
    compose_command: list[str] | None = None,
) -> str:
    resolved_compose_command = compose_command if compose_command is not None else ["docker", "compose"]
    container_id = run_capture(
        [
            *resolved_compose_command,
            "--env-file",
            str(env_file),
            "-f",
            str(compose_file),
            "-p",
            project_name,
            "ps",
            "-q",
            "inpx-web-reader",
        ],
        repo_root,
    ).strip()
    if not container_id:
        raise RuntimeError("Docker Compose did not report a running inpx-web-reader container.")

    inspect_text = run_capture(["docker", "inspect", container_id], repo_root)
    inspected = json.loads(inspect_text)
    mounts = inspected[0].get("Mounts", [])
    by_destination = {mount.get("Destination"): mount for mount in mounts}

    if by_destination.get("/source", {}).get("RW") is not False:
        raise RuntimeError("Compose /source mount is not read-only.")
    if by_destination.get("/data", {}).get("RW") is not True:
        raise RuntimeError("Compose /data mount is not writable.")

    host_config = inspected[0].get("HostConfig", {})
    if host_config.get("ReadonlyRootfs") is not True:
        raise RuntimeError("Compose container root filesystem is not read-only.")

    tmpfs = host_config.get("Tmpfs") or {}
    tmpfs_options = tmpfs.get("/tmp", "") if isinstance(tmpfs, dict) else ""
    actual_tmpfs_options = {option.strip() for option in tmpfs_options.split(",") if option.strip()}
    required_tmpfs_options = {"rw", "noexec", "nosuid", "nodev", "size=16m"}
    missing_tmpfs_options = sorted(required_tmpfs_options - actual_tmpfs_options)
    if missing_tmpfs_options:
        raise RuntimeError(f"Compose /tmp tmpfs is missing options: {', '.join(missing_tmpfs_options)}.")

    cap_drop = {str(value).upper() for value in (host_config.get("CapDrop") or [])}
    if "ALL" not in cap_drop:
        raise RuntimeError("Compose container does not drop all Linux capabilities.")

    security_opt = set(host_config.get("SecurityOpt") or [])
    if "no-new-privileges:true" not in security_opt:
        raise RuntimeError("Compose container does not enable no-new-privileges.")

    env_values = inspected[0].get("Config", {}).get("Env", [])
    if any(DEFAULT_TOKEN in value or value.startswith("INPX_WEB_READER_AUTH_TOKEN=") for value in env_values):
        raise RuntimeError("Compose container metadata exposes the bearer token.")

    user_id = run_capture(["docker", "exec", container_id, "id", "-u"], repo_root).strip()
    if user_id == "0":
        raise RuntimeError("Runtime container is running as root.")

    config_mode = run_capture(
        ["docker", "exec", container_id, "sh", "-lc", "stat -c '%a' /tmp/inpx-web-reader/server.json"],
        repo_root,
    ).strip()
    if config_mode != "600":
        raise RuntimeError(f"Generated server config permissions are {config_mode}, expected 600.")

    config_in_data = run_capture(
        ["docker", "exec", container_id, "sh", "-lc", "test ! -e /data/config/server.json && echo absent"],
        repo_root,
    ).strip()
    if config_in_data != "absent":
        raise RuntimeError("Generated server config was persisted under the mounted /data directory.")
    return container_id


def verify_runtime_image_shape(repo_root: Path, image_tag: str) -> None:
    image_platform = run_capture(
        ["docker", "image", "inspect", "--format", "{{.Os}}/{{.Architecture}}", image_tag],
        repo_root,
    ).strip()
    if image_platform != "linux/amd64":
        raise RuntimeError(
            f"Runtime image platform is {image_platform or '<empty>'}, expected linux/amd64."
        )
    image_title = run_capture(
        [
            "docker",
            "image",
            "inspect",
            "--format",
            '{{index .Config.Labels "org.opencontainers.image.title"}}',
            image_tag,
        ],
        repo_root,
    ).strip()
    if image_title != "InpxWebReader":
        raise RuntimeError(
            "Runtime image must carry org.opencontainers.image.title=InpxWebReader "
            "for scoped deployment cleanup."
        )
    run(
        [
            "docker",
            "run",
            "--rm",
            "--entrypoint",
            "/bin/sh",
            image_tag,
            "-lc",
            "test -x /opt/inpx-web-reader/inpx-web-reader "
            "&& test -f /opt/inpx-web-reader/web/index.html "
            "&& ! command -v node >/dev/null 2>&1 "
            "&& ! command -v npm >/dev/null 2>&1 "
            "&& ! command -v cmake >/dev/null 2>&1 "
            "&& ! command -v g++ >/dev/null 2>&1 "
            "&& ! test -d /src",
        ],
        repo_root,
    )


def verify_runtime_compose_identity(
    repo_root: Path,
    compose_file: Path,
    image_tag: str,
) -> None:
    expected_user = "2000:2001"
    fixture_root = repo_root / "out" / "server-docker-compose-identity"
    source_root = fixture_root / "source"
    data_root = fixture_root / "data"
    token_path = fixture_root / "token.txt"
    source_root.mkdir(parents=True, exist_ok=True)
    data_root.mkdir(parents=True, exist_ok=True)
    token_path.write_text("compose-identity-fixture-token\n", encoding="utf-8")
    environment = os.environ.copy()
    environment.update({
        "INPX_WEB_READER_IMAGE": image_tag,
        "INPX_WEB_READER_SOURCE_PATH": str(source_root),
        "INPX_WEB_READER_DATA_PATH": str(data_root),
        "INPX_WEB_READER_AUTH_TOKEN_FILE": str(token_path),
        "INPX_WEB_READER_CONTAINER_UID": "2000",
        "INPX_WEB_READER_CONTAINER_GID": "2001",
    })
    compose_command = resolve_docker_compose_command(repo_root)
    rendered = run_capture(
        [*compose_command, "-f", str(compose_file), "config"],
        repo_root,
        env=environment,
    )
    user_pattern = re.compile(
        rf"^\s+user:\s+['\"]?{re.escape(expected_user)}['\"]?\s*$",
        re.MULTILINE,
    )
    if user_pattern.search(rendered) is None:
        raise RuntimeError(
            "Rendered base Compose does not apply the selected non-root container identity "
            f"{expected_user}.\n{rendered}"
        )


def verify_runtime_entrypoint_rejects_partial_source(repo_root: Path, image_tag: str) -> None:
    fixture_root = repo_root / "out" / "server-docker-partial-source"
    source_root = fixture_root / "source"
    inpx_only_source_root = fixture_root / "inpx-only-source"
    source_root.mkdir(parents=True, exist_ok=True)
    inpx_only_source_root.mkdir(parents=True, exist_ok=True)
    (source_root / "catalog.inpx").write_bytes(b"partial-source-boundary-fixture")
    (source_root / "books.zip").write_bytes(b"partial-source-boundary-fixture")
    (inpx_only_source_root / "catalog.inpx").write_bytes(b"incomplete-auto-detection-fixture")

    for environment in (
        ["-e", "INPX_WEB_READER_INPX_PATH=/source/catalog.inpx"],
        ["-e", "INPX_WEB_READER_ARCHIVE_ROOT=/source"],
    ):
        completed = subprocess.run(
            resolve_command(
                [
                    "docker",
                    "run",
                    "--rm",
                    "--network",
                    "none",
                    "--mount",
                    f"type=bind,src={source_root},dst=/source,readonly",
                    "-e",
                    "INPX_WEB_READER_SERVER_HOST=127.0.0.1",
                    *environment,
                    image_tag,
                ]
            ),
            cwd=repo_root,
            env=out_scoped_environment(),
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        if completed.returncode == 0:
            raise RuntimeError("Runtime entrypoint accepted a partial INPX source configuration.")
        if "INPX source configuration is partial" not in completed.stdout:
            raise RuntimeError(
                "Runtime entrypoint rejected a partial source for an unexpected reason:\n"
                + completed.stdout
            )

    incomplete_auto_detection = subprocess.run(
        resolve_command(
            [
                "docker",
                "run",
                "--rm",
                "--network",
                "none",
                "--mount",
                f"type=bind,src={inpx_only_source_root},dst=/source,readonly",
                "-e",
                "INPX_WEB_READER_SERVER_HOST=127.0.0.1",
                image_tag,
            ]
        ),
        cwd=repo_root,
        env=out_scoped_environment(),
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    if "INPX source configuration is partial" in incomplete_auto_detection.stdout:
        raise RuntimeError(
            "Runtime entrypoint turned failed automatic detection into a partial explicit source:\n"
            + incomplete_auto_detection.stdout
        )
    if "INPX source config was not detected" not in incomplete_auto_detection.stdout:
        raise RuntimeError(
            "Runtime entrypoint did not preserve browse-only startup after incomplete automatic detection:\n"
            + incomplete_auto_detection.stdout
        )


def run_compose_smoke(
    repo_root: Path,
    compose_file: Path,
    image_tag: str,
    keep_workspace: bool,
    book_count: int = DEFAULT_SMOKE_BOOK_COUNT,
) -> None:
    workspace = create_smoke_workspace(repo_root, book_count)
    source_before = snapshot_files(workspace / "source")
    host_port = find_free_port()
    env_file = write_compose_env(workspace, image_tag, host_port)
    project_name = f"inpx-web-reader-smoke-{int(time.time())}"
    base_url = f"http://127.0.0.1:{host_port}"
    compose_command = resolve_docker_compose_command(repo_root)
    compose_base = [*compose_command, "--env-file", str(env_file), "-f", str(compose_file), "-p", project_name]
    primary_error: BaseException | None = None

    try:
        run([*compose_base, "up", "-d"], repo_root)
        wait_for_health(base_url)
        verify_authentication(base_url)
        verify_runtime_status(wait_for_status(base_url))
        container_id = verify_container_mounts(
            repo_root,
            compose_file,
            env_file,
            project_name,
            compose_command,
        )
        initial_progress = wait_for_scan_completion(
            base_url,
            "initial",
            accept_existing_completed=True,
        )
        verify_initial_scan_result(initial_progress, book_count)
        verify_runtime_status(wait_for_status(base_url))
        verify_web_ui_serving(base_url)
        initial_tail_title = f"Fixture Load Book {book_count - 1:03d}" if book_count > 1 else None
        initial_tail_payload = (
            (workspace / "books" / f"fixture-load-{book_count - 1:06d}.fb2").read_bytes()
            if book_count > 1
            else None
        )
        initial_book_id, initial_snapshot_id = verify_catalog_flow(
            base_url,
            book_count,
            INITIAL_SMOKE_TITLE,
            (workspace / "books" / "docker-smoke.fb2").read_bytes(),
            initial_tail_title,
            initial_tail_payload,
        )
        verify_parallel_polling(base_url)

        if source_before != snapshot_files(workspace / "source"):
            raise RuntimeError("Read-only INPX source changed during the initial Compose import.")

        expected_updated_source = publish_smoke_catalog_update(repo_root, workspace, book_count)
        update_progress = wait_for_scan_completion(base_url, "rescan")
        verify_incremental_update_result(update_progress, book_count)
        updated_book_id, updated_snapshot_id = verify_catalog_flow(
            base_url,
            book_count + 1,
            UPDATED_SMOKE_TITLE,
            (workspace / "books" / "docker-smoke.fb2").read_bytes(),
            ADDED_SMOKE_TITLE,
            (workspace / "books" / f"fixture-load-{book_count:06d}.fb2").read_bytes(),
        )
        if updated_book_id != initial_book_id:
            raise RuntimeError("Incremental rescan changed the stable id of the updated book.")
        if updated_snapshot_id == initial_snapshot_id:
            raise RuntimeError("Incremental rescan did not publish a new catalog snapshot id.")
        verify_parallel_polling(base_url)
        verify_container_runtime_logs(repo_root, container_id)

        if expected_updated_source != snapshot_files(workspace / "source"):
            raise RuntimeError("Read-only INPX source changed during the incremental rescan.")

        run([*compose_base, "restart", "inpx-web-reader"], repo_root)
        wait_for_health(base_url)
        verify_authentication(base_url)
        verify_runtime_status(wait_for_status(base_url))
        verify_web_ui_serving(base_url)
        restarted_book_id, restarted_snapshot_id = verify_catalog_flow(
            base_url,
            book_count + 1,
            UPDATED_SMOKE_TITLE,
            (workspace / "books" / "docker-smoke.fb2").read_bytes(),
            ADDED_SMOKE_TITLE,
            (workspace / "books" / f"fixture-load-{book_count:06d}.fb2").read_bytes(),
        )
        if restarted_book_id != updated_book_id or restarted_snapshot_id != updated_snapshot_id:
            raise RuntimeError("Restart did not reopen the catalog snapshot published by the rescan.")
        verify_parallel_polling(base_url)
        verify_container_runtime_logs(repo_root, container_id)

        if expected_updated_source != snapshot_files(workspace / "source"):
            raise RuntimeError("Read-only INPX source changed during the Compose restart check.")
    except BaseException as ex:
        primary_error = ex
        try:
            print("==> Docker Compose logs after smoke failure", flush=True)
            run([*compose_base, "logs", "--no-color", "--tail", "200"], repo_root)
        except Exception as log_error:
            print(f"==> Failed to capture Docker Compose logs: {log_error}", flush=True)
        raise
    finally:
        try:
            run([*compose_base, "down", "--volumes", "--remove-orphans"], repo_root)
        except Exception as cleanup_error:
            if primary_error is None:
                raise
            print(f"==> Docker Compose cleanup failed after primary error: {cleanup_error}", flush=True)
        if keep_workspace:
            print(f"==> Kept Docker smoke workspace: {workspace}", flush=True)
        else:
            # Leave logs in out when a failure happened; successful runs can be cleaned up by later out/ pruning.
            pass


def main() -> int:
    started_at = time.perf_counter()
    args = parse_args()
    for job_count in (args.parallel_jobs, args.build_jobs, args.test_jobs):
        if job_count is not None:
            validate_parallel_jobs(job_count)
    if args.smoke_book_count < 1:
        raise RuntimeError("--smoke-book-count must be at least 1.")
    if args.skip_docker and args.skip_compose:
        raise RuntimeError("--skip-docker already skips Compose; remove --skip-compose.")
    docker_platform_arguments(args.docker_platform)
    ensure_linux_host()

    repo_root = repository_root()
    build_root = repo_root / "out" / "build" / args.preset
    compose_file = repo_root / DEFAULT_COMPOSE_FILE
    dockerfile = repo_root / DEFAULT_DOCKERFILE

    stages: list[TimedStage] = []
    docker_engine_checked = False
    build_jobs, test_jobs = resolve_native_job_counts(args)

    def ensure_script_docker_engine() -> None:
        nonlocal docker_engine_checked
        if docker_engine_checked:
            return

        ensure_docker_engine(repo_root)
        docker_engine_checked = True

    if not args.skip_native:
        validate_parallel_jobs(build_jobs)
        validate_parallel_jobs(test_jobs)
        append_host_linux_native_stages(stages, repo_root, build_root, args, build_jobs, test_jobs)

    if not args.skip_docker:
        ensure_script_docker_engine()
        stages.append(
            TimedStage(
                f"Build Docker image ({args.image_tag}, {args.docker_platform or 'default platform'})",
                lambda: run(
                    [
                        "docker",
                        "build",
                        *docker_platform_arguments(args.docker_platform),
                        "--build-arg",
                        f"INPX_WEB_READER_BUILD_JOBS={build_jobs}",
                        "-f",
                        str(dockerfile),
                        "-t",
                        args.image_tag,
                        ".",
                    ],
                    repo_root,
                ),
            )
        )
        stages.append(
            TimedStage(
                "Verify runtime image contents",
                lambda: verify_runtime_image_shape(repo_root, args.image_tag),
            )
        )
        stages.append(
            TimedStage(
                "Verify runtime Compose identity",
                lambda: verify_runtime_compose_identity(
                    repo_root,
                    compose_file,
                    args.image_tag,
                ),
            )
        )
        stages.append(
            TimedStage(
                "Verify runtime entrypoint source validation",
                lambda: verify_runtime_entrypoint_rejects_partial_source(repo_root, args.image_tag),
            )
        )
        if not args.skip_compose:
            stages.append(
                TimedStage(
                    "Run Docker Compose smoke",
                    lambda: run_compose_smoke(
                        repo_root,
                        compose_file,
                        args.image_tag,
                        args.keep_smoke_workspace,
                        args.smoke_book_count,
                    ),
                )
            )

    run_timed_stages(
        stages,
        total_started_at=started_at,
        total_label="Total RunLinuxTests.py time",
        success_message="All requested server checks completed successfully.",
        clock=time.perf_counter,
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
