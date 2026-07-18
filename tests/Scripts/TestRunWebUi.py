from __future__ import annotations

import contextlib
import io
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from SubprocessHarness import load_script_module


REPO_ROOT = Path(__file__).resolve().parents[2]
RUN_WEB_UI_SCRIPT = REPO_ROOT / "scripts" / "RunWebUi.py"
TEST_OUTPUT_ROOT = REPO_ROOT / "out" / "tests" / "RunWebUi"


def load_run_web_ui_module() -> object:
    return load_script_module(RUN_WEB_UI_SCRIPT, "run_web_ui_under_test")


class RunWebUiScriptTests(unittest.TestCase):
    def setUp(self) -> None:
        TEST_OUTPUT_ROOT.mkdir(parents=True, exist_ok=True)
        self._temp_dir = tempfile.TemporaryDirectory(
            prefix="inpx-web-reader-run-web-ui-",
            dir=TEST_OUTPUT_ROOT,
        )
        self._repo_root = Path(self._temp_dir.name)
        self._module = load_run_web_ui_module()
        self._web_source = self._repo_root / "web" / "inpx-web-reader"
        self._write_minimal_web_source()

    def tearDown(self) -> None:
        self._temp_dir.cleanup()

    def _write_minimal_web_source(self) -> None:
        for directory_name in self._module.SYNC_DIRECTORIES:
            directory = self._web_source / directory_name
            directory.mkdir(parents=True, exist_ok=True)
            (directory / "fixture.txt").write_text(directory_name, encoding="utf-8")

        for file_name in self._module.SYNC_FILES:
            (self._web_source / file_name).write_text(f"{file_name}\n", encoding="utf-8")

    def test_sync_workspace_copies_sources_under_out_and_preserves_dependencies(self) -> None:
        workspace = self._module.workspace_root(self._repo_root)
        stale_file = workspace / "src" / "stale.txt"
        dependency_marker = workspace / "node_modules" / "marker.txt"
        stale_file.parent.mkdir(parents=True)
        dependency_marker.parent.mkdir(parents=True)
        stale_file.write_text("stale", encoding="utf-8")
        dependency_marker.write_text("installed", encoding="utf-8")

        synced_workspace = self._module.sync_workspace(self._repo_root)

        self.assertEqual(synced_workspace, workspace)
        self.assertFalse(stale_file.exists())
        self.assertTrue((workspace / "public" / "fixture.txt").exists())
        self.assertTrue((workspace / "src" / "fixture.txt").exists())
        self.assertTrue((workspace / "package.json").exists())
        self.assertTrue(dependency_marker.exists())

    def test_remove_workspace_path_refuses_to_modify_outside_workspace(self) -> None:
        workspace = self._module.workspace_root(self._repo_root)
        outside_path = self._repo_root / "web" / "inpx-web-reader" / "src"

        with self.assertRaisesRegex(RuntimeError, "outside"):
            self._module.remove_workspace_path(outside_path, workspace)

    def test_build_command_runs_tools_from_out_workspace(self) -> None:
        commands: list[tuple[list[str], Path, dict[str, str]]] = []

        def fake_run(command: list[str], cwd: Path, env: dict[str, str]) -> None:
            commands.append((command, cwd, env))

        with (
            mock.patch.object(sys, "argv", ["RunWebUi.py", "build"]),
            mock.patch.object(self._module, "repository_root", return_value=self._repo_root),
            mock.patch.object(self._module, "run", side_effect=fake_run),
            contextlib.redirect_stdout(io.StringIO()),
        ):
            exit_code = self._module.main()

        workspace = self._module.workspace_root(self._repo_root)
        artifacts = self._module.out_root(self._repo_root)

        self.assertEqual(exit_code, 0)
        self.assertEqual([cwd for _, cwd, _ in commands], [workspace, workspace, workspace])
        self.assertEqual(commands[0][0], ["npm", "ci"])
        self.assertEqual(commands[1][0], ["npx", "--no-install", "tsc", "--noEmit"])
        self.assertEqual(
            commands[2][0],
            [
                "npx",
                "--no-install",
                "vite",
                "build",
                "--outDir",
                str(artifacts / "dist"),
                "--emptyOutDir",
            ],
        )
        self.assertEqual(commands[0][2]["npm_config_cache"], str(artifacts / "npm-cache"))
        self.assertEqual(commands[1][2]["INPX_WEB_READER_WEB_ARTIFACTS_ROOT"], str(artifacts))
        self.assertEqual(commands[0][2]["TMPDIR"], str(artifacts / "tmp"))
        self.assertEqual(commands[0][2]["XDG_CACHE_HOME"], str(artifacts / "cache"))
        self.assertEqual(commands[0][2]["VCPKG_DOWNLOADS"], str(self._repo_root / "out" / "vcpkg" / "downloads"))

        self.assertFalse((self._web_source / "node_modules").exists())
        self.assertFalse((self._web_source / "dist").exists())
        self.assertFalse((self._web_source / "test-results").exists())
        self.assertFalse((self._web_source / "tsconfig.tsbuildinfo").exists())

    def test_custom_artifacts_root_keeps_tools_under_requested_out_child(self) -> None:
        commands: list[tuple[list[str], Path, dict[str, str]]] = []

        def fake_run(command: list[str], cwd: Path, env: dict[str, str]) -> None:
            commands.append((command, cwd, env))

        artifacts = self._repo_root / "out" / "build" / "linux-release" / "web" / "inpx-web-reader"
        with (
            mock.patch.object(
                sys,
                "argv",
                [
                    "RunWebUi.py",
                    "--artifacts-root",
                    str(artifacts),
                    "build",
                ],
            ),
            mock.patch.object(self._module, "repository_root", return_value=self._repo_root),
            mock.patch.object(self._module, "run", side_effect=fake_run),
            contextlib.redirect_stdout(io.StringIO()),
        ):
            exit_code = self._module.main()

        self.assertEqual(exit_code, 0)
        self.assertEqual([cwd for _, cwd, _ in commands], [artifacts / "workspace"] * 3)
        self.assertEqual(commands[0][2]["npm_config_cache"], str(artifacts / "npm-cache"))
        self.assertEqual(commands[1][2]["INPX_WEB_READER_WEB_ARTIFACTS_ROOT"], str(artifacts))
        self.assertEqual(commands[2][2]["XDG_CACHE_HOME"], str(artifacts / "cache"))
        self.assertEqual(
            commands[2][0],
            [
                "npx",
                "--no-install",
                "vite",
                "build",
                "--outDir",
                str(artifacts / "dist"),
                "--emptyOutDir",
            ],
        )

    def test_artifacts_root_must_stay_under_repository_out(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "outside"):
            self._module.resolve_artifacts_root(self._repo_root, str(self._repo_root / "web-artifacts"))

    def test_audit_uses_out_workspace_without_installing_dependencies(self) -> None:
        commands: list[tuple[list[str], Path]] = []

        def fake_run(command: list[str], cwd: Path, env: dict[str, str]) -> None:
            _ = env
            commands.append((command, cwd))

        with (
            mock.patch.object(sys, "argv", ["RunWebUi.py", "audit"]),
            mock.patch.object(self._module, "repository_root", return_value=self._repo_root),
            mock.patch.object(self._module, "run", side_effect=fake_run),
        ):
            exit_code = self._module.main()

        self.assertEqual(exit_code, 0)
        self.assertEqual(
            commands,
            [
                (
                    ["npm", "audit", "--audit-level=high"],
                    self._module.workspace_root(self._repo_root),
                )
            ],
        )

    def test_coverage_command_writes_reports_under_artifacts(self) -> None:
        commands: list[tuple[list[str], Path]] = []

        def fake_run(command: list[str], cwd: Path, env: dict[str, str]) -> None:
            _ = env
            commands.append((command, cwd))

        with (
            mock.patch.object(sys, "argv", ["RunWebUi.py", "test:coverage"]),
            mock.patch.object(self._module, "repository_root", return_value=self._repo_root),
            mock.patch.object(self._module, "run", side_effect=fake_run),
            contextlib.redirect_stdout(io.StringIO()),
        ):
            exit_code = self._module.main()

        self.assertEqual(exit_code, 0)
        self.assertEqual(commands[-1][0], ["npx", "--no-install", "vitest", "run", "--coverage"])
        self.assertEqual(commands[-1][1], self._module.workspace_root(self._repo_root))

    def test_e2e_command_rejects_non_linux_hosts(self) -> None:
        with (
            mock.patch.object(sys, "argv", ["RunWebUi.py", "test:e2e"]),
            mock.patch.object(self._module.sys, "platform", "darwin"),
        ):
            with self.assertRaisesRegex(RuntimeError, "require Linux"):
                self._module.main()

    def test_e2e_command_uses_matching_playwright_container_on_linux(self) -> None:
        commands: list[tuple[list[str], Path, dict[str, str]]] = []
        package_lock = self._web_source / "package-lock.json"
        package_lock.write_text(
            '{"packages":{"node_modules/@playwright/test":{"version":"1.60.0"}}}\n',
            encoding="utf-8",
        )

        def fake_run(command: list[str], cwd: Path, env: dict[str, str]) -> None:
            commands.append((command, cwd, env))

        with (
            mock.patch.object(sys, "argv", ["RunWebUi.py", "test:e2e"]),
            mock.patch.object(self._module, "repository_root", return_value=self._repo_root),
            mock.patch.object(self._module, "run", side_effect=fake_run),
            mock.patch.object(self._module.sys, "platform", "linux"),
            mock.patch.object(self._module.os, "getuid", return_value=1000, create=True),
            mock.patch.object(self._module.os, "getgid", return_value=1000, create=True),
            mock.patch.object(self._module, "default_parallel_jobs", return_value=24),
            mock.patch.object(
                self._module,
                "real_server_session",
                return_value=contextlib.nullcontext("http://127.0.0.1:49152"),
            ),
            contextlib.redirect_stdout(io.StringIO()),
        ):
            exit_code = self._module.main()

        artifacts = self._module.out_root(self._repo_root)
        self.assertEqual(exit_code, 0)
        self.assertEqual(len(commands), 4)
        container_command = commands[-1][0]
        self.assertEqual(container_command[:3], ["docker", "run", "--rm"])
        self.assertIn("1000:1000", container_command)
        self.assertIn(f"{self._repo_root}:{self._repo_root}", container_command)
        self.assertIn(
            f"INPX_WEB_READER_WEB_ARTIFACTS_ROOT={artifacts}",
            container_command,
        )
        self.assertIn(
            "INPX_WEB_READER_WEB_E2E_BASE_URL=http://127.0.0.1:49152",
            container_command,
        )
        self.assertIn(
            (
                "INPX_WEB_READER_WEB_E2E_SERVER_ROOT="
                f"{artifacts / self._module.E2E_SERVER_WORKSPACE_NAME}"
            ),
            container_command,
        )
        self.assertIn("mcr.microsoft.com/playwright:v1.60.0-noble", container_command)
        self.assertEqual(
            container_command[-6:],
            ["npx", "--no-install", "playwright", "test", "--workers", "24"],
        )

    def test_e2e_native_build_uses_all_detected_cpu_threads(self) -> None:
        with mock.patch.object(self._module, "default_parallel_jobs", return_value=24):
            self.assertEqual(self._module.e2e_parallel_jobs(), 24)

    def test_wait_for_server_reports_process_log_after_early_exit(self) -> None:
        log_path = self._repo_root / "out" / "server-process.log"
        log_path.parent.mkdir(parents=True, exist_ok=True)
        log_path.write_text("fatal startup failure\n", encoding="utf-8")
        process = mock.Mock()
        process.poll.return_value = 23
        process.returncode = 23

        with self.assertRaises(RuntimeError) as raised:
            self._module.wait_for_server(
                "http://127.0.0.1:49152",
                process,
                log_path,
                timeout_seconds=0.1,
            )

        message = str(raised.exception)
        self.assertIn("code 23", message)
        self.assertIn(str(log_path), message)
        self.assertIn("fatal startup failure", message)

    def test_stop_server_requires_clean_sigterm_exit(self) -> None:
        process = mock.Mock()
        process.poll.return_value = None
        process.returncode = 0

        self._module.stop_server(process)

        process.terminate.assert_called_once_with()
        process.wait.assert_called_once_with(timeout=10)

        process.returncode = 143
        with self.assertRaisesRegex(RuntimeError, "code 143"):
            self._module.stop_server(process)

        process.poll.return_value = 1
        with self.assertRaisesRegex(RuntimeError, "before SIGTERM"):
            self._module.stop_server(process)


if __name__ == "__main__":
    unittest.main(verbosity=2)
