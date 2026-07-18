from __future__ import annotations

import io
import sys
import tempfile
import unittest
import zipfile
from contextlib import redirect_stdout
from pathlib import Path
from unittest import mock

from SubprocessHarness import load_script_module


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT_PATH = REPO_ROOT / "scripts" / "GenerateInpxFixtureCatalog.py"
TEST_OUTPUT_ROOT = REPO_ROOT / "out" / "tests" / "GenerateInpxFixtureCatalog"


def load_generator_module() -> object:
    return load_script_module(SCRIPT_PATH, "generate_inpx_fixture_catalog_under_test")


def write_fb2(
    path: Path,
    *,
    title: str,
    first_name: str,
    last_name: str,
    genres: list[str] | None = None,
    series_name: str = "",
    series_number: str = "",
    language: str = "ru",
) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    genre_xml = "".join(f"<genre>{genre}</genre>" for genre in (genres or []))
    sequence_xml = ""
    if series_name:
        sequence_xml = f'<sequence name="{series_name}" number="{series_number}" />'
    path.write_text(
        (
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
            "<FictionBook xmlns=\"http://www.gribuser.ru/xml/fictionbook/2.0\">\n"
            "  <description>\n"
            "    <title-info>\n"
            f"      {genre_xml}\n"
            f"      <book-title>{title}</book-title>\n"
            "      <author>\n"
            f"        <first-name>{first_name}</first-name>\n"
            f"        <last-name>{last_name}</last-name>\n"
            "      </author>\n"
            f"      <lang>{language}</lang>\n"
            f"      {sequence_xml}\n"
            "    </title-info>\n"
            "  </description>\n"
            "  <body>\n"
            f"    <section><title><p>{title}</p></title><p>Body</p></section>\n"
            "  </body>\n"
            "</FictionBook>\n"
        ),
        encoding="utf-8",
    )


class GenerateInpxFixtureCatalogTests(unittest.TestCase):
    def setUp(self) -> None:
        TEST_OUTPUT_ROOT.mkdir(parents=True, exist_ok=True)
        self._temp_dir = tempfile.TemporaryDirectory(
            prefix="inpx-web-reader-generate-inpx-",
            dir=TEST_OUTPUT_ROOT,
        )
        self._workspace = Path(self._temp_dir.name)
        self._module = load_generator_module()

    def tearDown(self) -> None:
        self._temp_dir.cleanup()

    def test_parse_args_defaults_output_to_repository_out(self) -> None:
        with (
            mock.patch.object(self._module, "repository_root", return_value=self._workspace),
            mock.patch.object(sys, "argv", ["GenerateInpxFixtureCatalog.py", str(self._workspace / "books")]),
        ):
            args = self._module.parse_args()

        self.assertEqual(
            Path(args.output_dir),
            self._workspace / "out" / self._module.DEFAULT_OUTPUT_DIR_NAME,
        )
        self.assertEqual(args.books_per_archive, self._module.DEFAULT_BOOKS_PER_ARCHIVE)
        self.assertEqual(args.archive_dir_name, self._module.DEFAULT_ARCHIVE_DIR_NAME)
        self.assertFalse(args.force)

    def test_main_generates_multi_archive_catalog_and_payload_archives(self) -> None:
        input_dir = self._workspace / "books"
        output_dir = self._workspace / "generated"
        write_fb2(
            input_dir / "Булгаков" / "master.fb2",
            title="Мастер и Маргарита",
            first_name="Михаил",
            last_name="Булгаков",
            genres=["novel", "mystic"],
            series_name="Классика",
            series_number="1",
        )
        write_fb2(
            input_dir / "Стругацкие" / "picnic.fb2",
            title="Пикник на обочине",
            first_name="Аркадий",
            last_name="Стругацкий",
            genres=["sf"],
        )
        write_fb2(
            input_dir / "Стругацкие" / "snail.fb2",
            title="Улитка на склоне",
            first_name="Борис",
            last_name="Стругацкий",
            genres=["sf"],
        )
        write_fb2(
            input_dir / "A" / "b" / "c" / "book4.fb2",
            title="Nested Book",
            first_name="Jane",
            last_name="Doe",
            genres=["test"],
            language="en",
        )
        write_fb2(
            input_dir / "book5.FB2",
            title="Case Insensitive",
            first_name="John",
            last_name="Smith",
            genres=["test"],
            language="en",
        )

        stdout = io.StringIO()
        with (
            mock.patch.object(
                sys,
                "argv",
                [
                    "GenerateInpxFixtureCatalog.py",
                    str(input_dir),
                    "--output-dir",
                    str(output_dir),
                    "--books-per-archive",
                    "2",
                    "--archive-dir-name",
                    "payloads",
                ],
            ),
            redirect_stdout(stdout),
        ):
            exit_code = self._module.main()

        self.assertEqual(exit_code, 0)
        self.assertIn("Generated 5 books into 3 archives", stdout.getvalue())
        self.assertIn(f"Archive root: {output_dir / 'payloads'}", stdout.getvalue())
        self.assertTrue((output_dir / "catalog.inpx").exists())
        archive_root = output_dir / "payloads"
        self.assertEqual(
            sorted(path.name for path in archive_root.glob("fb2-*.zip")),
            ["fb2-001.zip", "fb2-002.zip", "fb2-003.zip"],
        )

        with zipfile.ZipFile(output_dir / "catalog.inpx") as catalog:
            self.assertEqual(
                sorted(catalog.namelist()),
                ["fb2-001.inp", "fb2-002.inp", "fb2-003.inp"],
            )
            record_lines: list[str] = []
            for entry_name in sorted(catalog.namelist()):
                payload = catalog.read(entry_name).decode("utf-8")
                record_lines.extend(line for line in payload.splitlines() if line)

        self.assertEqual(len(record_lines), 5)
        parsed_records = [line.split(self._module.FIELD_SEPARATOR) for line in record_lines]
        self.assertTrue(all(len(fields) == self._module.EXPECTED_FIELD_COUNT for fields in parsed_records))
        records_by_title = {fields[2]: fields for fields in parsed_records}
        self.assertEqual(records_by_title["Nested Book"][0], "Doe,Jane")
        self.assertEqual(records_by_title["Nested Book"][11], "en")
        self.assertEqual(records_by_title["Мастер и Маргарита"][0], "Булгаков,Михаил")
        self.assertEqual(records_by_title["Мастер и Маргарита"][3], "Классика")
        self.assertEqual(records_by_title["Мастер и Маргарита"][4], "1")
        self.assertEqual(records_by_title["Мастер и Маргарита"][13], "Булгаков")

        archive_entries: dict[str, set[str]] = {}
        for archive_path in archive_root.glob("fb2-*.zip"):
            with zipfile.ZipFile(archive_path) as archive:
                archive_entries[archive_path.name] = set(archive.namelist())

        for record in parsed_records:
            lib_id = int(record[7])
            archive_name = f"fb2-{((lib_id - 1) // 2) + 1:03d}.zip"
            self.assertIn(f"{record[5]}.fb2", archive_entries[archive_name])

    def test_main_rejects_output_outside_repository_out(self) -> None:
        input_dir = self._workspace / "books"
        write_fb2(
            input_dir / "book.fb2",
            title="Book",
            first_name="A",
            last_name="Author",
        )

        with mock.patch.object(
            sys,
            "argv",
            [
                "GenerateInpxFixtureCatalog.py",
                str(input_dir),
                "--output-dir",
                str(REPO_ROOT / "generated-inpx-outside-out"),
            ],
        ):
            with self.assertRaisesRegex(RuntimeError, "repository out directory"):
                self._module.main()

    def test_main_rejects_non_empty_output_without_force(self) -> None:
        input_dir = self._workspace / "books"
        output_dir = self._workspace / "generated"
        write_fb2(
            input_dir / "book.fb2",
            title="Book",
            first_name="A",
            last_name="Author",
        )
        output_dir.mkdir(parents=True)
        (output_dir / "stale.txt").write_text("stale", encoding="utf-8")

        with mock.patch.object(
            sys,
            "argv",
            [
                "GenerateInpxFixtureCatalog.py",
                str(input_dir),
                "--output-dir",
                str(output_dir),
            ],
        ):
            with self.assertRaisesRegex(RuntimeError, "Output directory already exists"):
                self._module.main()


if __name__ == "__main__":
    unittest.main()
