from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from SubprocessHarness import load_script_module, run_entry_script_help


REPO_ROOT = Path(__file__).resolve().parents[2]
RUN_RELEASE_SCRIPT = REPO_ROOT / "scripts" / "RunRelease.py"
TEST_OUTPUT_ROOT = REPO_ROOT / "out" / "tests" / "RunRelease"


def load_run_release_module() -> object:
    return load_script_module(RUN_RELEASE_SCRIPT, "run_release_under_test")


class RunReleaseScriptTests(unittest.TestCase):
    def setUp(self) -> None:
        TEST_OUTPUT_ROOT.mkdir(parents=True, exist_ok=True)
        self._temp_dir = tempfile.TemporaryDirectory(
            prefix="inpx-web-reader-run-release-",
            dir=TEST_OUTPUT_ROOT,
        )
        self._repo_root = Path(self._temp_dir.name)
        self._module = load_run_release_module()

    def tearDown(self) -> None:
        self._temp_dir.cleanup()

    def test_help_runs_without_side_effects(self) -> None:
        result = run_entry_script_help(RUN_RELEASE_SCRIPT, self._repo_root)

        self.assertIn("Verify InpxWebReader", result.stdout)
        self.assertIn("--image-tag", result.stdout)
        self.assertIn("--skip-e2e", result.stdout)

    def test_rejects_non_linux_host(self) -> None:
        with mock.patch.object(self._module.sys, "platform", "darwin"):
            with self.assertRaisesRegex(RuntimeError, "RunRemoteLinux.py release"):
                self._module.ensure_linux_host()

    def test_rejects_unsafe_deploy_arguments_before_running_stages(self) -> None:
        with (
            mock.patch.object(
                sys,
                "argv",
                [
                    "RunRelease.py",
                    "--host-source-root",
                    "/srv/books/inpx",
                    "--host-app-root",
                    "/srv/books/inpx/deployment",
                    "--image-tag",
                    "inpx-web-reader:test",
                ],
            ),
            mock.patch.object(self._module.sys, "platform", "linux"),
            mock.patch.object(self._module.platform, "machine", return_value="x86_64"),
            mock.patch.object(self._module, "run_timed_stages") as run_stages,
            self.assertRaisesRegex(RuntimeError, "must not overlap"),
        ):
            self._module.main()

        run_stages.assert_not_called()

    def test_runs_verification_then_packages_the_same_image(self) -> None:
        image_tag = "inpx-web-reader:1.2.0-abcdef123456"
        token_file = self._repo_root / "access-password.txt"
        token_file.write_text("example-password\n", encoding="utf-8")
        commands: list[list[str]] = []

        def run_stages(stages: list[object], **_kwargs: object) -> None:
            for stage in stages:
                stage.action()

        with (
            mock.patch.object(
                sys,
                "argv",
                [
                    "RunRelease.py",
                    "--host-source-root",
                    "/srv/books/inpx",
                    "--host-app-root",
                    "/srv/inpx-web-reader",
                    "--image-tag",
                    image_tag,
                    "--parallel-jobs",
                    "8",
                    "--build-jobs",
                    "7",
                    "--test-jobs",
                    "6",
                    "--token-file",
                    str(token_file),
                    "--skip-converter-download",
                    "--git-commit",
                    "abcdef1234567890",
                ],
            ),
            mock.patch.object(self._module.sys, "platform", "linux"),
            mock.patch.object(self._module.platform, "machine", return_value="x86_64"),
            mock.patch.object(self._module, "repository_root", return_value=self._repo_root),
            mock.patch.object(
                self._module,
                "run_command",
                side_effect=lambda command, *_args, **_kwargs: commands.append(command),
            ),
            mock.patch.object(self._module, "run_timed_stages", side_effect=run_stages),
        ):
            exit_code = self._module.main()

        self.assertEqual(exit_code, 0)
        self.assertEqual(commands[0][1:], ["scripts/RunScriptTests.py"])
        self.assertEqual(
            commands[1][1:],
            ["scripts/RunStaticAnalysis.py", "--skip-clang-tidy"],
        )
        self.assertEqual(commands[2][1:], ["scripts/RunWebUi.py", "build"])
        self.assertEqual(commands[3][1:], ["scripts/RunWebUi.py", "test"])
        self.assertEqual(commands[4][1:], ["scripts/RunWebUi.py", "test:e2e"])
        self.assertIn("scripts/RunLinuxTests.py", commands[5])
        self.assertIn(image_tag, commands[5])
        self.assertIn("scripts/PrepareDeployBundle.py", commands[6])
        self.assertIn(image_tag, commands[6])
        self.assertIn("--reuse-image", commands[6])
        self.assertIn("--skip-converter-download", commands[6])


if __name__ == "__main__":
    unittest.main()
