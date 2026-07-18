from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from SubprocessHarness import load_script_module, run_entry_script_help


REPO_ROOT = Path(__file__).resolve().parents[2]
RUN_SCRIPT_TESTS_SCRIPT = REPO_ROOT / "scripts" / "RunScriptTests.py"
RUN_STATIC_ANALYSIS_SCRIPT = REPO_ROOT / "scripts" / "RunStaticAnalysis.py"
RUN_SANITIZERS_SCRIPT = REPO_ROOT / "scripts" / "RunSanitizers.py"
RUN_COVERAGE_SCRIPT = REPO_ROOT / "scripts" / "RunCoverage.py"
RUN_FUZZERS_SCRIPT = REPO_ROOT / "scripts" / "RunFuzzers.py"
TEST_OUTPUT_ROOT = REPO_ROOT / "out" / "tests" / "RunScriptTests"


def load_run_script_tests_module() -> object:
    return load_script_module(RUN_SCRIPT_TESTS_SCRIPT, "run_script_tests_under_test")


class RunScriptTestsScriptTests(unittest.TestCase):
    def setUp(self) -> None:
        TEST_OUTPUT_ROOT.mkdir(parents=True, exist_ok=True)
        self._temp_dir = tempfile.TemporaryDirectory(
            prefix="inpx-web-reader-run-script-tests-",
            dir=TEST_OUTPUT_ROOT,
        )
        self._repo_root = Path(self._temp_dir.name)
        self._module = load_run_script_tests_module()

    def tearDown(self) -> None:
        self._temp_dir.cleanup()

    def test_parse_args_accepts_multi_token_python_command(self) -> None:
        with mock.patch.object(sys, "argv", ["RunScriptTests.py", "--python-exe", "python3", "launcher"]):
            args = self._module.parse_args()

        self.assertEqual(args.python_exe, ["python3", "launcher"])

    def test_discovery_pattern_uses_override(self) -> None:
        with mock.patch.object(sys, "argv", ["RunScriptTests.py", "--pattern", "TestRun*.py"]):
            args = self._module.parse_args()

        self.assertEqual(self._module.discovery_pattern(args), "TestRun*.py")

    def test_main_runs_unittest_discovery_with_python_exe_and_bytecode_env(self) -> None:
        commands: list[tuple[list[str], Path, dict[str, str]]] = []

        def fake_run(command: list[str], cwd: Path, env: dict[str, str] | None = None) -> None:
            commands.append((command, cwd, dict(env or {})))

        with (
            mock.patch.object(sys, "argv", ["RunScriptTests.py", "--python-exe", "python3", "launcher"]),
            mock.patch.object(self._module, "repository_root", return_value=self._repo_root),
            mock.patch.object(self._module, "run_command", side_effect=fake_run),
        ):
            exit_code = self._module.main()

        self.assertEqual(exit_code, 0)
        self.assertEqual(
            commands,
            [
                (
                    [
                        "python3",
                        "launcher",
                        "-m",
                        "unittest",
                        "discover",
                        "-s",
                        "tests/Scripts",
                        "-p",
                        "Test*.py",
                        "-v",
                    ],
                    self._repo_root,
                    mock.ANY,
                )
            ],
        )
        self.assertEqual(commands[0][2]["PYTHONDONTWRITEBYTECODE"], "1")
        self.assertEqual(commands[0][2]["PYTHONPYCACHEPREFIX"], str(self._repo_root / "out" / "pycache"))

    def test_help_run_does_not_create_pycache_in_scripts_dir(self) -> None:
        result = run_entry_script_help(RUN_SCRIPT_TESTS_SCRIPT, self._repo_root)
        scripts_dir = self._repo_root / "scripts"

        self.assertIn("Run the repository Python script unittest suite.", result.stdout)
        self.assertFalse((scripts_dir / "__pycache__").exists())

    def test_static_analysis_defaults_to_all_detected_cpu_threads(self) -> None:
        module = load_script_module(RUN_STATIC_ANALYSIS_SCRIPT, "run_static_analysis_under_test")
        with (
            mock.patch.object(sys, "argv", ["RunStaticAnalysis.py"]),
            mock.patch.object(module, "default_parallel_jobs", return_value=24),
        ):
            args = module.parse_args()

        self.assertEqual(args.parallel_jobs, 24)

    def test_coverage_help_and_defaults_are_linux_scoped(self) -> None:
        result = run_entry_script_help(RUN_COVERAGE_SCRIPT, self._repo_root)
        self.assertIn("native Linux line coverage", result.stdout)

        module = load_script_module(RUN_COVERAGE_SCRIPT, "run_coverage_under_test")
        with (
            mock.patch.object(sys, "argv", ["RunCoverage.py"]),
            mock.patch.object(module, "default_parallel_jobs", return_value=24),
        ):
            args = module.parse_args()

        self.assertEqual(args.preset, "linux-coverage")
        self.assertEqual(args.parallel_jobs, 24)

    def test_coverage_tools_keep_intermediate_files_under_out(self) -> None:
        module = load_script_module(RUN_COVERAGE_SCRIPT, "run_coverage_commands_under_test")
        build_root = self._repo_root / "out" / "build" / "linux-coverage"
        report_root = self._repo_root / "out" / "reports" / "coverage" / "native"
        temporary_root = self._repo_root / "out" / "coverage" / "tmp"
        commands: list[list[str]] = []

        with (
            mock.patch.object(module, "require_executable", side_effect=lambda name: name),
            mock.patch.object(
                module,
                "run_command",
                side_effect=lambda command, cwd, env: commands.append(command),
            ),
        ):
            module.capture_coverage(
                self._repo_root,
                build_root,
                report_root,
                {"TMPDIR": str(temporary_root)},
            )

        capture, remove, _summary, html = commands
        self.assertEqual(capture[capture.index("--tempdir") + 1], str(temporary_root))
        self.assertEqual(remove[remove.index("--tempdir") + 1], str(temporary_root))
        self.assertEqual(html[html.index("--tempdir") + 1], str(temporary_root))

    def test_tsan_stops_on_the_first_report(self) -> None:
        module = load_script_module(RUN_SANITIZERS_SCRIPT, "run_sanitizers_under_test")
        commands: list[dict[str, str]] = []
        suppression_path = self._repo_root / "scripts" / "tsan-suppressions.txt"
        suppression_path.parent.mkdir(parents=True)
        suppression_path.write_text("race:walIndexWriteHdr\n", encoding="utf-8")

        with (
            mock.patch.object(
                sys,
                "argv",
                ["RunSanitizers.py", "--preset", "linux-tsan", "--parallel-jobs", "2"],
            ),
            mock.patch.object(module, "ensure_linux_host"),
            mock.patch.object(module, "repository_root", return_value=self._repo_root),
            mock.patch.object(
                module,
                "run_command",
                side_effect=lambda command, cwd, env: commands.append(dict(env)),
            ),
        ):
            exit_code = module.main()

        self.assertEqual(exit_code, 0)
        self.assertTrue(commands)
        expected_options = f"halt_on_error=1:suppressions={suppression_path.resolve()}"
        self.assertTrue(all(env["TSAN_OPTIONS"] == expected_options for env in commands))

    def test_tsan_suppression_is_limited_to_the_sqlite_wal_header_false_positive(self) -> None:
        entries = [
            line.strip()
            for line in (REPO_ROOT / "scripts" / "tsan-suppressions.txt").read_text(encoding="utf-8").splitlines()
            if line.strip() and not line.lstrip().startswith("#")
        ]

        self.assertEqual(entries, ["race:walIndexWriteHdr"])

    def test_tsan_names_the_system_time_zone_only_when_tz_is_unset(self) -> None:
        module = load_script_module(RUN_SANITIZERS_SCRIPT, "run_sanitizers_timezone_under_test")
        suppression_path = self._repo_root / "scripts" / "tsan-suppressions.txt"
        suppression_path.parent.mkdir(parents=True)
        suppression_path.write_text("race:walIndexWriteHdr\n", encoding="utf-8")

        default_environment: dict[str, str] = {}
        module.configure_tsan_environment(default_environment, self._repo_root)
        self.assertEqual(default_environment["TZ"], ":/etc/localtime")

        explicit_environment = {"TZ": "Europe/Prague"}
        module.configure_tsan_environment(explicit_environment, self._repo_root)
        self.assertEqual(explicit_environment["TZ"], "Europe/Prague")

    def test_tsan_rejects_a_missing_suppression_file(self) -> None:
        module = load_script_module(RUN_SANITIZERS_SCRIPT, "run_sanitizers_missing_suppression_under_test")

        with self.assertRaisesRegex(RuntimeError, "TSan suppression file was not found"):
            module.configure_tsan_environment({}, self._repo_root)

    def test_sanitizer_helper_rejects_non_instrumented_presets_before_running_commands(self) -> None:
        module = load_script_module(RUN_SANITIZERS_SCRIPT, "run_sanitizers_preset_under_test")

        for preset in ("linux-debug", "linux-coverage", "unknown"):
            with (
                self.subTest(preset=preset),
                mock.patch.object(sys, "argv", ["RunSanitizers.py", "--preset", preset]),
                mock.patch.object(module, "ensure_linux_host"),
                mock.patch.object(module, "run_command") as run_command,
            ):
                with self.assertRaisesRegex(RuntimeError, "instrumented sanitizer preset"):
                    module.main()
                run_command.assert_not_called()

    def test_sanitizer_helper_accepts_only_asan_and_tsan_presets(self) -> None:
        module = load_script_module(RUN_SANITIZERS_SCRIPT, "run_sanitizers_allowlist_under_test")

        module.validate_sanitizer_preset("linux-asan")
        module.validate_sanitizer_preset("linux-tsan")

    def test_fuzzer_help_and_defaults_are_reproducible_and_linux_scoped(self) -> None:
        result = run_entry_script_help(RUN_FUZZERS_SCRIPT, self._repo_root)
        self.assertIn("FB2 and INPX/ZIP libFuzzer", result.stdout)

        module = load_script_module(RUN_FUZZERS_SCRIPT, "run_fuzzers_under_test")
        with (
            mock.patch.object(sys, "argv", ["RunFuzzers.py"]),
            mock.patch.object(module, "default_parallel_jobs", return_value=24),
        ):
            args = module.parse_args()

        self.assertEqual(args.preset, "linux-fuzz")
        self.assertEqual(args.parallel_jobs, 24)
        self.assertEqual(args.seed, 1)
        self.assertEqual(args.max_total_time, 30)

    def test_fuzzer_workspace_starts_from_the_deterministic_seed_corpus(self) -> None:
        module = load_script_module(RUN_FUZZERS_SCRIPT, "run_fuzzers_workspace_under_test")
        stale_artifact = self._repo_root / "out" / "fuzz" / "fb2" / "artifacts" / "crash-stale"
        stale_artifact.parent.mkdir(parents=True)
        stale_artifact.write_bytes(b"stale")

        corpus, artifacts = module.prepare_fuzz_workspace(self._repo_root)

        self.assertFalse(stale_artifact.exists())
        self.assertEqual(
            sorted(path.name for path in corpus.iterdir()),
            ["malformed.fb2", "minimal-valid.fb2"],
        )
        self.assertEqual(list(artifacts.iterdir()), [])

    def test_inpx_archive_fuzzer_workspace_has_deterministic_boundary_seeds(self) -> None:
        module = load_script_module(RUN_FUZZERS_SCRIPT, "run_inpx_fuzzers_workspace_under_test")

        corpus, artifacts, runtime = module.prepare_inpx_archive_fuzz_workspace(self._repo_root)

        self.assertEqual(
            sorted(path.name for path in corpus.iterdir()),
            [
                "corrupt-stored-payload.zip",
                "duplicate-names.zip",
                "legacy-cp866.inpx",
                "malformed.zip",
                "minimal-valid.inpx",
            ],
        )
        self.assertEqual(list(artifacts.iterdir()), [])
        self.assertEqual(list(runtime.iterdir()), [])


if __name__ == "__main__":
    unittest.main(verbosity=2)
