from __future__ import annotations

import argparse
import sys
import xml.etree.ElementTree as ET
import zipfile
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path

sys.dont_write_bytecode = True
sys.pycache_prefix = str(Path(__file__).resolve().parent.parent / "out" / "pycache")

from _common import remove_path_under_root, repository_root, resolve_under_root  # noqa: E402


FIELD_SEPARATOR = "\x04"
EXPECTED_FIELD_COUNT = 15
DEFAULT_BOOKS_PER_ARCHIVE = 5
DEFAULT_OUTPUT_DIR_NAME = "generated-inpx-catalog"
DEFAULT_ARCHIVE_DIR_NAME = "lib.rus.ec"


@dataclass(frozen=True)
class SFb2Metadata:
    title: str
    authors: str
    genres: str
    series_name: str
    series_number: str
    language: str
    keywords: str


@dataclass(frozen=True)
class SBookRecord:
    source_path: Path
    entry_stem: str
    archive_stem: str
    lib_id: str
    size_bytes: int
    date_added: str
    metadata: SFb2Metadata


def default_output_dir() -> Path:
    return repository_root() / "out" / DEFAULT_OUTPUT_DIR_NAME


def resolve_output_dir(output_dir_arg: str, repo_root: Path) -> Path:
    output_dir = Path(output_dir_arg)
    if not output_dir.is_absolute():
        output_dir = repo_root / output_dir

    return resolve_under_root(
        output_dir,
        repo_root / "out",
        path_description="INPX fixture output directory",
        root_description="repository out directory",
        allow_root=False,
    )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Generate a small multi-archive INPX fixture catalog from a folder of FB2 books."
    )
    parser.add_argument("input_dir", help="Folder scanned recursively for .fb2 files.")
    parser.add_argument(
        "--output-dir",
        default=str(default_output_dir()),
        help="Target directory for catalog.inpx and generated ZIP archives.",
    )
    parser.add_argument(
        "--books-per-archive",
        type=int,
        default=DEFAULT_BOOKS_PER_ARCHIVE,
        help="Maximum number of books stored in each generated ZIP archive.",
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="Overwrite an existing non-empty output directory.",
    )
    parser.add_argument(
        "--archive-dir-name",
        default=DEFAULT_ARCHIVE_DIR_NAME,
        help="Relative directory under the output directory where generated payload ZIP archives are written.",
    )
    return parser.parse_args()


def local_name(tag: str) -> str:
    if "}" in tag:
        return tag.rsplit("}", 1)[1]
    return tag


def find_child(element: ET.Element, name: str) -> ET.Element | None:
    for child in element:
        if local_name(child.tag) == name:
            return child
    return None


def find_children(element: ET.Element, name: str) -> list[ET.Element]:
    return [child for child in element if local_name(child.tag) == name]


def find_path(root: ET.Element, *names: str) -> ET.Element | None:
    current = root
    for name in names:
        next_element = find_child(current, name)
        if next_element is None:
            return None
        current = next_element
    return current


def text_or_empty(element: ET.Element | None) -> str:
    if element is None or element.text is None:
        return ""
    return " ".join(element.text.split())


def build_author(author_element: ET.Element) -> str:
    last_name = text_or_empty(find_child(author_element, "last-name"))
    first_name = text_or_empty(find_child(author_element, "first-name"))
    middle_name = text_or_empty(find_child(author_element, "middle-name"))
    nickname = text_or_empty(find_child(author_element, "nickname"))
    parts = [part for part in [last_name, first_name, middle_name] if part]
    if parts:
        return ",".join(parts)
    return nickname


def parse_fb2_metadata(source_path: Path, input_root: Path) -> SFb2Metadata:
    try:
        root = ET.fromstring(source_path.read_bytes())
    except ET.ParseError as exc:
        raise RuntimeError(f"Failed to parse FB2 XML: {source_path}") from exc

    title_info = find_path(root, "description", "title-info")
    if title_info is None:
        return SFb2Metadata(
            title=source_path.stem,
            authors="",
            genres="",
            series_name="",
            series_number="",
            language="",
            keywords=build_keywords(source_path, input_root),
        )

    authors = ":".join(
        author
        for author in (build_author(author_element) for author_element in find_children(title_info, "author"))
        if author
    )
    genres = ":".join(
        value
        for value in (text_or_empty(genre_element) for genre_element in find_children(title_info, "genre"))
        if value
    )
    sequence = find_child(title_info, "sequence")
    series_name = ""
    series_number = ""
    if sequence is not None:
        series_name = " ".join(sequence.attrib.get("name", "").split())
        series_number = " ".join(sequence.attrib.get("number", "").split())

    return SFb2Metadata(
        title=text_or_empty(find_child(title_info, "book-title")) or source_path.stem,
        authors=authors,
        genres=genres,
        series_name=series_name,
        series_number=series_number,
        language=text_or_empty(find_child(title_info, "lang")),
        keywords=build_keywords(source_path, input_root),
    )


def build_keywords(source_path: Path, input_root: Path) -> str:
    relative_parent = source_path.relative_to(input_root).parent
    if not relative_parent.parts:
        return ""
    return ":".join(part for part in relative_parent.parts if part not in {".", ""})


def discover_fb2_files(input_dir: Path) -> list[Path]:
    if not input_dir.exists():
        raise RuntimeError(f"Input directory does not exist: {input_dir}")
    if not input_dir.is_dir():
        raise RuntimeError(f"Input path is not a directory: {input_dir}")

    files = sorted(
        path
        for path in input_dir.rglob("*")
        if path.is_file() and path.suffix.lower() == ".fb2"
    )
    if not files:
        raise RuntimeError(f"No .fb2 files were found under: {input_dir}")
    return files


def prepare_output_dir(output_dir: Path, force: bool, repo_root: Path) -> None:
    if output_dir.exists():
        children = list(output_dir.iterdir())
        if children and not force:
            raise RuntimeError(
                f"Output directory already exists and is not empty: {output_dir}. Use --force to overwrite it."
            )
        if children and force:
            remove_path_under_root(
                output_dir,
                repo_root / "out",
                path_description="INPX fixture output directory",
                root_description="repository out directory",
            )
    output_dir.mkdir(parents=True, exist_ok=True)


def validate_archive_dir_name(archive_dir_name: str) -> Path:
    archive_dir = Path(archive_dir_name)
    if not archive_dir.parts or archive_dir.is_absolute() or ".." in archive_dir.parts:
        raise RuntimeError("--archive-dir-name must be a non-empty relative directory path.")
    return archive_dir


def build_book_records(input_dir: Path, books_per_archive: int) -> list[SBookRecord]:
    if books_per_archive < 1:
        raise RuntimeError("--books-per-archive must be at least 1.")

    book_paths = discover_fb2_files(input_dir)
    records: list[SBookRecord] = []
    for index, source_path in enumerate(book_paths, start=1):
        archive_index = ((index - 1) // books_per_archive) + 1
        entry_stem = f"book-{index:06d}"
        records.append(
            SBookRecord(
                source_path=source_path,
                entry_stem=entry_stem,
                archive_stem=f"fb2-{archive_index:03d}",
                lib_id=str(index),
                size_bytes=source_path.stat().st_size,
                date_added=datetime.fromtimestamp(source_path.stat().st_mtime).strftime("%Y%m%d"),
                metadata=parse_fb2_metadata(source_path, input_dir),
            )
        )
    return records


def build_record_line(record: SBookRecord) -> str:
    fields = [
        record.metadata.authors,
        record.metadata.genres,
        record.metadata.title,
        record.metadata.series_name,
        record.metadata.series_number,
        record.entry_stem,
        str(record.size_bytes),
        record.lib_id,
        "0",
        "fb2",
        record.date_added,
        record.metadata.language,
        "",
        record.metadata.keywords,
        "",
    ]
    if len(fields) != EXPECTED_FIELD_COUNT:
        raise RuntimeError("Unexpected generated INPX field count.")
    return FIELD_SEPARATOR.join(fields) + "\n"


def group_records_by_archive(records: list[SBookRecord]) -> dict[str, list[SBookRecord]]:
    grouped: dict[str, list[SBookRecord]] = {}
    for record in records:
        grouped.setdefault(record.archive_stem, []).append(record)
    return grouped


def write_payload_archives(
    output_dir: Path,
    records_by_archive: dict[str, list[SBookRecord]],
    archive_dir: Path,
) -> None:
    archive_root = output_dir / archive_dir
    archive_root.mkdir(parents=True, exist_ok=True)
    for archive_stem, records in records_by_archive.items():
        archive_path = archive_root / f"{archive_stem}.zip"
        with zipfile.ZipFile(archive_path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
            for record in records:
                archive.writestr(
                    f"{record.entry_stem}.fb2",
                    record.source_path.read_bytes(),
                )


def write_inpx_catalog(output_dir: Path, records_by_archive: dict[str, list[SBookRecord]]) -> Path:
    catalog_path = output_dir / "catalog.inpx"
    with zipfile.ZipFile(catalog_path, "w", compression=zipfile.ZIP_DEFLATED) as catalog:
        for archive_stem, records in records_by_archive.items():
            inp_entry_name = f"{archive_stem}.inp"
            payload = "".join(build_record_line(record) for record in records)
            catalog.writestr(inp_entry_name, payload.encode("utf-8"))
    return catalog_path


def main() -> int:
    args = parse_args()
    repo_root = repository_root()
    input_dir = Path(args.input_dir).resolve()
    output_dir = resolve_output_dir(args.output_dir, repo_root)
    archive_dir = validate_archive_dir_name(args.archive_dir_name)

    prepare_output_dir(output_dir, args.force, repo_root)
    records = build_book_records(input_dir, args.books_per_archive)
    records_by_archive = group_records_by_archive(records)
    write_payload_archives(output_dir, records_by_archive, archive_dir)
    catalog_path = write_inpx_catalog(output_dir, records_by_archive)

    print(
        f"Generated {len(records)} books into {len(records_by_archive)} archives at {output_dir}",
        flush=True,
    )
    print(f"Catalog: {catalog_path}", flush=True)
    print(f"Archive root: {output_dir / archive_dir}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
