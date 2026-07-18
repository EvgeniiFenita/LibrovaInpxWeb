from __future__ import annotations

import argparse
import sys
import warnings
import zipfile
from pathlib import Path

sys.dont_write_bytecode = True
sys.pycache_prefix = str(Path(__file__).resolve().parent.parent / "out" / "pycache")

from _common import (  # noqa: E402
    TimedStage,
    build_cmake_build_command,
    build_cmake_configure_command,
    build_out_tool_environment,
    default_parallel_jobs,
    out_root,
    remove_path_under_root,
    repository_root,
    run_command,
    run_timed_stages,
    validate_parallel_jobs,
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Build and run the FB2 and INPX/ZIP libFuzzer lane on Linux.")
    parser.add_argument("--preset", default="linux-fuzz")
    parser.add_argument("--parallel-jobs", type=int, default=default_parallel_jobs())
    parser.add_argument("--skip-configure", action="store_true")
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--max-total-time", type=int, default=30)
    return parser.parse_args()


def ensure_linux_host() -> None:
    if sys.platform != "linux":
        raise RuntimeError("RunFuzzers.py is supported only on Linux.")


def prepare_fuzz_workspace(repo_root: Path) -> tuple[Path, Path]:
    workspace = out_root(repo_root) / "fuzz" / "fb2"
    if workspace.exists():
        remove_path_under_root(
            workspace,
            out_root(repo_root),
            path_description="FB2 fuzz workspace",
            root_description="repository out directory",
        )
    corpus = workspace / "corpus"
    artifacts = workspace / "artifacts"
    corpus.mkdir(parents=True, exist_ok=True)
    artifacts.mkdir(parents=True, exist_ok=True)
    (corpus / "minimal-valid.fb2").write_text(
        """<?xml version="1.0" encoding="utf-8"?>
<FictionBook><description><title-info><book-title>Seed</book-title></title-info></description>
<body><section><p>Seed text</p></section></body></FictionBook>
""",
        encoding="utf-8",
    )
    (corpus / "malformed.fb2").write_bytes(b"<FictionBook>\x00\xff</broken>")
    return corpus, artifacts


def _write_zip_entry(
    archive: zipfile.ZipFile,
    name: str,
    payload: bytes,
    compression: int = zipfile.ZIP_DEFLATED,
) -> None:
    entry = zipfile.ZipInfo(name, date_time=(2024, 1, 1, 0, 0, 0))
    entry.compress_type = compression
    archive.writestr(entry, payload)


def prepare_inpx_archive_fuzz_workspace(repo_root: Path) -> tuple[Path, Path, Path]:
    workspace = out_root(repo_root) / "fuzz" / "inpx-archive"
    if workspace.exists():
        remove_path_under_root(
            workspace,
            out_root(repo_root),
            path_description="INPX/archive fuzz workspace",
            root_description="repository out directory",
        )
    corpus = workspace / "corpus"
    artifacts = workspace / "artifacts"
    runtime = workspace / "runtime"
    corpus.mkdir(parents=True, exist_ok=True)
    artifacts.mkdir(parents=True, exist_ok=True)
    runtime.mkdir(parents=True, exist_ok=True)

    separator = b"\x04"
    utf8_record = separator.join(
        [
            b"Author", b"genre", "Книга".encode(), b"", b"", b"book-1", b"123", b"1",
            b"0", b"fb2", b"", b"ru", b"", b"", b"",
        ]
    ) + b"\n"
    with zipfile.ZipFile(corpus / "minimal-valid.inpx", "w") as archive:
        _write_zip_entry(archive, "fb2-main.zip.inp", utf8_record)

    cp866_record = separator.join(
        [
            "Автор".encode("cp866"), b"genre", "тут".encode("cp866"), b"", b"", b"book-1",
            b"3", b"1", b"0", b"fb2", b"", b"ru", b"", b"", b"",
        ]
    ) + b"\n"
    with zipfile.ZipFile(corpus / "legacy-cp866.inpx", "w") as archive:
        _write_zip_entry(archive, "fb2-main.zip.inp", cp866_record)

    with warnings.catch_warnings():
        warnings.simplefilter("ignore", UserWarning)
        with zipfile.ZipFile(corpus / "duplicate-names.zip", "w") as archive:
            _write_zip_entry(archive, "book.fb2", b"first")
            _write_zip_entry(archive, "book.fb2", b"second")

    corrupt_path = corpus / "corrupt-stored-payload.zip"
    stored_payload = b"UNIQUE-STORED-PAYLOAD"
    with zipfile.ZipFile(corrupt_path, "w") as archive:
        _write_zip_entry(archive, "book.fb2", stored_payload, zipfile.ZIP_STORED)
    corrupt_bytes = bytearray(corrupt_path.read_bytes())
    payload_offset = corrupt_bytes.find(stored_payload)
    if payload_offset < 0:
        raise RuntimeError("Could not locate deterministic stored ZIP payload seed.")
    corrupt_bytes[payload_offset] ^= 0x01
    corrupt_path.write_bytes(corrupt_bytes)

    (corpus / "malformed.zip").write_bytes(b"PK\x03\x04\x00\xfftruncated")
    return corpus, artifacts, runtime


def main() -> int:
    args = parse_args()
    ensure_linux_host()
    validate_parallel_jobs(args.parallel_jobs)
    if args.seed < 0:
        raise RuntimeError("--seed must not be negative.")
    if args.max_total_time < 1:
        raise RuntimeError("--max-total-time must be at least 1 second.")

    repo_root = repository_root()
    build_root = repo_root / "out" / "build" / args.preset
    fb2_corpus, fb2_artifacts = prepare_fuzz_workspace(repo_root)
    inpx_corpus, inpx_artifacts, inpx_runtime = prepare_inpx_archive_fuzz_workspace(repo_root)
    fb2_fuzzer = build_root / "tests" / "Fuzz" / "InpxWebReaderFb2Fuzzer"
    inpx_fuzzer = build_root / "tests" / "Fuzz" / "InpxWebReaderInpxArchiveFuzzer"
    env = build_out_tool_environment(repo_root, "fuzz")
    env["INPX_WEB_READER_FUZZ_RUNTIME_ROOT"] = str(inpx_runtime)
    stages: list[TimedStage] = []
    if not args.skip_configure:
        stages.append(TimedStage(
            f"Configure fuzz build ({args.preset})",
            lambda: run_command(build_cmake_configure_command(args.preset), repo_root, env),
        ))
    stages.append(TimedStage(
        "Build fuzzers",
        lambda: run_command(
            build_cmake_build_command(
                args.preset,
                "Debug",
                args.parallel_jobs,
                False,
                targets=("InpxWebReaderFb2Fuzzer", "InpxWebReaderInpxArchiveFuzzer"),
            ),
            repo_root,
            env,
        ),
    ))
    stages.append(TimedStage(
        "Run FB2 fuzzer",
        lambda: run_command(
            [
                str(fb2_fuzzer),
                str(fb2_corpus),
                f"-artifact_prefix={fb2_artifacts}/",
                f"-seed={args.seed}",
                f"-max_total_time={args.max_total_time}",
                "-timeout=5",
                "-rss_limit_mb=2048",
                "-print_final_stats=1",
            ],
            repo_root,
            env,
        ),
    ))
    stages.append(TimedStage(
        "Run INPX/archive fuzzer",
        lambda: run_command(
            [
                str(inpx_fuzzer),
                str(inpx_corpus),
                f"-artifact_prefix={inpx_artifacts}/",
                f"-seed={args.seed}",
                f"-max_total_time={args.max_total_time}",
                "-timeout=5",
                "-rss_limit_mb=2048",
                "-max_len=268435456",
                "-print_final_stats=1",
            ],
            repo_root,
            env,
        ),
    ))
    run_timed_stages(stages, total_label="Total RunFuzzers.py time")
    print(f"Fuzzer corpus and artifacts: {out_root(repo_root) / 'fuzz'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
