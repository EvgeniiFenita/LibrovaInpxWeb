from __future__ import annotations

import contextlib
import io
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path, PurePosixPath
from unittest import mock

from SubprocessHarness import load_script_module, run_entry_script_help


REPO_ROOT = Path(__file__).resolve().parents[2]
REMOTE_HELPER_SCRIPT = REPO_ROOT / "scripts" / "_remote_linux.py"
REMOTE_RUNNER_SCRIPT = REPO_ROOT / "scripts" / "RunRemoteLinux.py"
REMOTE_BOOTSTRAP_SCRIPT = REPO_ROOT / "scripts" / "BootstrapRemoteLinux.py"
TEST_OUTPUT_ROOT = REPO_ROOT / "out" / "tests" / "RemoteLinux"


def load_remote_helper_module() -> object:
    return load_script_module(REMOTE_HELPER_SCRIPT, "remote_linux_under_test")


def load_remote_runner_module() -> object:
    return load_script_module(REMOTE_RUNNER_SCRIPT, "run_remote_linux_under_test")


def load_remote_bootstrap_module() -> object:
    return load_script_module(REMOTE_BOOTSTRAP_SCRIPT, "bootstrap_remote_linux_under_test")


class RemoteLinuxConfigTests(unittest.TestCase):
    def setUp(self) -> None:
        TEST_OUTPUT_ROOT.mkdir(parents=True, exist_ok=True)
        self._temp_dir = tempfile.TemporaryDirectory(prefix="inpx-web-reader-remote-config-", dir=TEST_OUTPUT_ROOT)
        self._repo_root = Path(self._temp_dir.name)
        self._module = load_remote_helper_module()

    def tearDown(self) -> None:
        self._temp_dir.cleanup()

    def test_loads_remote_machine_env_values(self) -> None:
        env_path = self._repo_root / ".env"
        env_path.write_text(
            "\n".join(
                [
                    "INPX_WEB_READER_BUILD_HOST=192.0.2.10",
                    "INPX_WEB_READER_BUILD_USER=builder",
                    "INPX_WEB_READER_BUILD_PASSWORD='secret value'",
                    "INPX_WEB_READER_BUILD_WORKDIR=/srv/inpx-web-reader build",
                    "",
                ]
            ),
            encoding="utf-8",
        )

        config = self._module.load_remote_config(Path(".env"), self._repo_root)

        self.assertEqual(config.target, "builder@192.0.2.10")
        self.assertEqual(config.port, 22)
        self.assertEqual(config.password, "secret value")
        self.assertEqual(config.workdir, PurePosixPath("/srv/inpx-web-reader build"))
        self.assertEqual(config.source_root, PurePosixPath("/srv/inpx-web-reader build/source"))

    def test_rejects_relative_remote_workdir(self) -> None:
        env_path = self._repo_root / ".env"
        env_path.write_text(
            "INPX_WEB_READER_BUILD_HOST=host\n"
            "INPX_WEB_READER_BUILD_USER=builder\n"
            "INPX_WEB_READER_BUILD_WORKDIR=relative/path\n",
            encoding="utf-8",
        )

        with self.assertRaisesRegex(RuntimeError, "absolute Linux path"):
            self._module.load_remote_config(env_path, self._repo_root)

    def test_rejects_retired_remote_build_fields(self) -> None:
        env_path = self._repo_root / ".env"
        env_path.write_text(
            "INPX_WEB_READER_BUILD_HOST=host\n"
            "INPX_WEB_READER_BUILD_USER=builder\n"
            "INPX_WEB_READER_BUILD_PASSWORD=secret\n"
            "INPX_WEB_READER_BUILD_WORKDIR=/srv/inpx-web-reader\n"
            "INPX_WEB_READER_BUILD_TRANSPORT=auto\n",
            encoding="utf-8",
        )

        with self.assertRaisesRegex(RuntimeError, "INPX_WEB_READER_BUILD_TRANSPORT"):
            self._module.load_remote_config(env_path, self._repo_root)

    def test_missing_env_file_points_to_template(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "Copy .env.example to .env"):
            self._module.load_remote_config(Path(".env"), self._repo_root)

    def test_tracked_template_contains_only_remote_machine_connection_fields(self) -> None:
        keys = {
            line.split("=", 1)[0]
            for line in (REPO_ROOT / ".env.example").read_text(encoding="utf-8").splitlines()
            if line.startswith("INPX_WEB_READER_BUILD_")
        }

        self.assertEqual(
            keys,
            {
                "INPX_WEB_READER_BUILD_HOST",
                "INPX_WEB_READER_BUILD_USER",
                "INPX_WEB_READER_BUILD_PASSWORD",
                "INPX_WEB_READER_BUILD_WORKDIR",
            },
        )


class RemoteLinuxTransportTests(unittest.TestCase):
    def setUp(self) -> None:
        TEST_OUTPUT_ROOT.mkdir(parents=True, exist_ok=True)
        self._temp_dir = tempfile.TemporaryDirectory(prefix="inpx-web-reader-remote-transport-", dir=TEST_OUTPUT_ROOT)
        self._repo_root = Path(self._temp_dir.name)
        self._module = load_remote_helper_module()
        self._config = self._module.CRemoteLinuxConfig(
            host="build.example",
            user="builder",
            workdir=PurePosixPath("/srv/inpx-web-reader"),
            port=2222,
        )

    def tearDown(self) -> None:
        self._temp_dir.cleanup()

    def test_repository_rsync_uses_git_indexed_staging_and_preserves_remote_output(self) -> None:
        transport = self._module.CRemoteLinuxTransport(self._config, self._repo_root)
        sync_root = self._repo_root / "out" / "remote-linux" / f"{self._module.SYNC_SOURCE_DIR_PREFIX}test"
        sync_root.mkdir(parents=True)
        (sync_root / "README.md").write_text("tracked source\n", encoding="utf-8")
        commands: list[list[str]] = []
        with (
            mock.patch.object(transport, "CheckLocalPrerequisites"),
            mock.patch.object(transport, "_PrepareRepositorySyncRoot", return_value=sync_root),
            mock.patch.object(transport, "RunSsh") as run_ssh,
            mock.patch.object(
                transport,
                "_RunAuthenticated",
                side_effect=lambda command, **_kwargs: commands.append(command),
            ),
        ):
            transport.RsyncRepository()

        run_ssh.assert_called_once()
        command = commands[0]
        self.assertEqual(command[:3], ["rsync", "-az", "--delete"])
        self.assertNotIn("--delete-excluded", command)
        self.assertIn("P /out/***", command)
        self.assertTrue(command[-2].endswith(f"/{self._module.SYNC_SOURCE_DIR_PREFIX}test/"))
        self.assertEqual(command[-1], "builder@build.example:/srv/inpx-web-reader/source/")
        remote_shell = command[command.index("-e") + 1]
        self.assertIn("-p 2222", remote_shell)
        self.assertIn("ssh", remote_shell.lower())
        self.assertIn("StrictHostKeyChecking=accept-new", remote_shell)
        rsync_path = command[command.index("--rsync-path") + 1]
        self.assertIn("start.lock", rsync_path)
        self.assertIn("source.lock", rsync_path)
        self.assertIn("flock -n", rsync_path)
        self.assertFalse(sync_root.exists())

    def test_repository_sync_stages_only_git_indexed_files(self) -> None:
        subprocess.run(["git", "init", "--quiet"], cwd=self._repo_root, check=True)
        (self._repo_root / ".gitignore").write_text("private-token.txt\n", encoding="utf-8")
        (self._repo_root / "tracked.txt").write_text("working tree content\n", encoding="utf-8")
        (self._repo_root / "private-token.txt").write_text("secret\n", encoding="utf-8")
        subprocess.run(
            ["git", "add", ".gitignore", "tracked.txt"],
            cwd=self._repo_root,
            check=True,
        )
        transport = self._module.CRemoteLinuxTransport(self._config, self._repo_root)

        sync_root = transport._PrepareRepositorySyncRoot()
        try:
            self.assertEqual((sync_root / "tracked.txt").read_text(encoding="utf-8"), "working tree content\n")
            self.assertTrue((sync_root / ".gitignore").is_file())
            self.assertFalse((sync_root / "private-token.txt").exists())
        finally:
            transport._RemoveRepositorySyncRoot(sync_root)

    def test_repository_sync_rejects_untracked_non_ignored_files(self) -> None:
        subprocess.run(["git", "init", "--quiet"], cwd=self._repo_root, check=True)
        (self._repo_root / "untracked.txt").write_text("not indexed\n", encoding="utf-8")
        transport = self._module.CRemoteLinuxTransport(self._config, self._repo_root)

        with self.assertRaisesRegex(RuntimeError, "untracked.txt"):
            transport._PrepareRepositorySyncRoot()

    def test_bundle_download_locks_remote_source_against_new_jobs(self) -> None:
        transport = self._module.CRemoteLinuxTransport(self._config, self._repo_root)
        commands: list[list[str]] = []
        with (
            mock.patch.object(transport, "CheckLocalPrerequisites"),
            mock.patch.object(
                transport,
                "_RunAuthenticated",
                side_effect=lambda command, **_kwargs: commands.append(command),
            ),
        ):
            transport.RsyncFrom(
                PurePosixPath("/srv/inpx-web-reader/source/out/deploy/inpx-web-reader"),
                self._repo_root / "out" / "deploy" / "inpx-web-reader",
                delete=True,
                lockSource=True,
            )

        command = commands[0]
        rsync_path = command[command.index("--rsync-path") + 1]
        self.assertIn("start.lock", rsync_path)
        self.assertIn("source.lock", rsync_path)

    def test_macos_development_host_uses_posix_askpass_and_cleans_it_up(self) -> None:
        config = self._module.CRemoteLinuxConfig(
            host="build.example",
            user="builder",
            workdir=PurePosixPath("/srv/inpx-web-reader"),
            password="private-password",
        )
        transport = self._module.CRemoteLinuxTransport(config, self._repo_root)

        with mock.patch.object(self._module.platform, "system", return_value="Darwin"):
            with transport._AuthenticationEnvironment() as (environment, prefix):
                askpass_path = Path(environment["SSH_ASKPASS"])
                password_path = askpass_path.parent / "password"
                self.assertEqual(prefix, [])
                self.assertEqual(askpass_path.suffix, ".sh")
                self.assertTrue(askpass_path.is_file())
                self.assertEqual(password_path.read_text(encoding="utf-8"), "private-password\n")
                self.assertTrue(askpass_path.read_text(encoding="utf-8").startswith("#!/bin/sh\n"))
                self.assertNotIn("private-password", environment["SSH_ASKPASS"])

        self.assertFalse(askpass_path.exists())
        self.assertFalse(password_path.exists())

    def test_windows_development_host_uses_cmd_askpass_file(self) -> None:
        config = self._module.CRemoteLinuxConfig(
            host="build.example",
            user="builder",
            workdir=PurePosixPath("/srv/inpx-web-reader"),
            password="private-password",
        )
        transport = self._module.CRemoteLinuxTransport(config, self._repo_root)

        with mock.patch.object(self._module.platform, "system", return_value="Windows"):
            with transport._AuthenticationEnvironment() as (environment, _prefix):
                askpass_path = Path(environment["SSH_ASKPASS"])
                password_path = askpass_path.parent / "password"
                askpass_text = askpass_path.read_text(encoding="utf-8")
                self.assertEqual(askpass_path.suffix, ".cmd")
                self.assertIn("@echo off", askpass_text)
                self.assertIn(f'type "{password_path.resolve()}"', askpass_text)
                self.assertNotIn("private-password", askpass_text)

        self.assertFalse(askpass_path.exists())
        self.assertFalse(password_path.exists())

    def test_parallel_password_sessions_use_independent_askpass_files(self) -> None:
        config = self._module.CRemoteLinuxConfig(
            host="build.example",
            user="builder",
            workdir=PurePosixPath("/srv/inpx-web-reader"),
            password="private-password",
        )
        transport = self._module.CRemoteLinuxTransport(config, self._repo_root)

        with transport._AuthenticationEnvironment() as (first_environment, _first_prefix):
            first_askpass = Path(first_environment["SSH_ASKPASS"])
            with transport._AuthenticationEnvironment() as (second_environment, _second_prefix):
                second_askpass = Path(second_environment["SSH_ASKPASS"])
                self.assertNotEqual(first_askpass, second_askpass)
                self.assertTrue(first_askpass.is_file())
                self.assertTrue(second_askpass.is_file())

            self.assertTrue(first_askpass.is_file())

        self.assertFalse(first_askpass.exists())
        self.assertFalse(second_askpass.exists())

    def test_client_command_runs_without_host_specific_wrapper(self) -> None:
        completed = subprocess.CompletedProcess([], 0, stdout="ok\n")
        with (
            mock.patch.object(self._module.shutil, "which", return_value="/usr/bin/ssh"),
            mock.patch.object(self._module.subprocess, "run", return_value=completed) as run_process,
        ):
            transport = self._module.CRemoteLinuxTransport(self._config, self._repo_root)
            transport._RunClient(["ssh", "host"], capture_output=True)

        self.assertEqual(run_process.call_args.args[0], ["/usr/bin/ssh", "host"])
        self.assertEqual(run_process.call_args.kwargs["encoding"], "utf-8")
        self.assertEqual(run_process.call_args.kwargs["errors"], "replace")

    def test_macos_development_host_prefers_ssh_beside_rsync(self) -> None:
        tool_root = self._repo_root / "usr" / "bin"
        tool_root.mkdir(parents=True)
        rsync_path = tool_root / "rsync"
        ssh_path = tool_root / "ssh"
        rsync_path.touch()
        ssh_path.touch()
        transport = self._module.CRemoteLinuxTransport(self._config, self._repo_root)

        def find_tool(name: str, **_kwargs: object) -> str | None:
            if name == "rsync":
                return str(rsync_path)
            if name == "ssh":
                return "/usr/bin/ssh"
            return None

        with (
            mock.patch.object(self._module.platform, "system", return_value="Darwin"),
            mock.patch.object(self._module.shutil, "which", side_effect=find_tool),
        ):
            command = transport._SshCommand()

        self.assertEqual(Path(command[0]), ssh_path)

    def test_windows_development_host_prefers_ssh_exe_beside_rsync(self) -> None:
        tool_root = self._repo_root / "usr" / "bin"
        tool_root.mkdir(parents=True)
        rsync_path = tool_root / "rsync.exe"
        ssh_path = tool_root / "ssh.exe"
        rsync_path.touch()
        ssh_path.touch()
        transport = self._module.CRemoteLinuxTransport(self._config, self._repo_root)

        def find_tool(name: str, **_kwargs: object) -> str | None:
            if name == "rsync":
                return str(rsync_path)
            if name == "ssh":
                return "C:/Windows/System32/OpenSSH/ssh.exe"
            return None

        with (
            mock.patch.object(self._module.platform, "system", return_value="Windows"),
            mock.patch.object(self._module.shutil, "which", side_effect=find_tool),
        ):
            command = transport._SshCommand()

        self.assertEqual(Path(command[0]), ssh_path)

    def test_remote_shell_uses_persistent_remote_source_build_state(self) -> None:
        command = self._module.remote_shell_command(self._config, ["python3", "scripts/RunLinuxTests.py"])

        self.assertIn("VCPKG_ROOT=/srv/inpx-web-reader/vcpkg", command)
        self.assertNotIn("VCPKG_DEFAULT_BINARY_CACHE", command)
        self.assertIn("python3 scripts/RunLinuxTests.py", command)

    def test_remote_shell_cleans_sensitive_inputs_on_exit(self) -> None:
        command = self._module.remote_shell_command(
            self._config,
            ["python3", "scripts/PrepareDeployBundle.py"],
            cleanupPaths=(PurePosixPath("/srv/inpx-web-reader/inputs/token.txt"),),
        )

        self.assertIn("trap cleanup EXIT", command)
        self.assertIn("rm -f -- /srv/inpx-web-reader/inputs/token.txt", command)

    def test_remote_job_start_serializes_source_sync_and_tracks_process_identity(self) -> None:
        transport = self._module.CRemoteLinuxTransport(self._config, self._repo_root)

        command = transport._StartRemoteJobCommand("true", "test")

        self.assertIn("start.lock", command)
        self.assertIn("source.lock", command)
        self.assertIn("flock -s 8", command)
        self.assertIn("pid-start", command)
        self.assertIn("/proc/$pid/stat", command)

    def test_remote_job_resume_does_not_start_a_replacement_job(self) -> None:
        transport = self._module.CRemoteLinuxTransport(self._config, self._repo_root)

        command = transport._StartRemoteJobCommand("true", "test", resumeExisting=True)

        self.assertIn("The remote job finished or disappeared before reconnection", command)
        self.assertIn("exit 74", command)

    def test_remote_job_streams_output_until_success(self) -> None:
        transport = self._module.CRemoteLinuxTransport(self._config, self._repo_root)
        responses = iter(
            [
                "__INPX_WEB_READER_JOB_START__ started\n",
                "configure output\n\n__INPX_WEB_READER_JOB_STATE__ 17 running -\n",
                "test output\n\n__INPX_WEB_READER_JOB_STATE__ 29 complete 0\n",
            ]
        )
        output = io.StringIO()
        with (
            mock.patch.object(transport, "RunSshCapture", side_effect=lambda _command: next(responses)),
            mock.patch.object(self._module.time, "sleep"),
            contextlib.redirect_stdout(output),
        ):
            transport.RunSshJob("bash -lc 'run tests'", "test")

        self.assertIn("configure output", output.getvalue())
        self.assertIn("test output", output.getvalue())

    def test_remote_job_accepts_resume_marker_when_ssh_reports_failure(self) -> None:
        transport = self._module.CRemoteLinuxTransport(self._config, self._repo_root)
        resume_error = subprocess.CalledProcessError(
            1,
            ["ssh"],
            output="__INPX_WEB_READER_JOB_START__ resumed\n",
        )
        responses = iter(
            [
                resume_error,
                "done\n\n__INPX_WEB_READER_JOB_STATE__ 5 complete 0\n",
            ]
        )

        def remote_response(_command: str) -> str:
            response = next(responses)
            if isinstance(response, Exception):
                raise response
            return response

        with (
            mock.patch.object(transport, "RunSshCapture", side_effect=remote_response) as run_ssh_capture,
            contextlib.redirect_stdout(io.StringIO()),
        ):
            transport.RunSshJob("true", "test")

        self.assertEqual(run_ssh_capture.call_count, 2)

    def test_remote_job_reports_worker_failure(self) -> None:
        transport = self._module.CRemoteLinuxTransport(self._config, self._repo_root)
        responses = iter(
            [
                "__INPX_WEB_READER_JOB_START__ started\n",
                "failed\n\n__INPX_WEB_READER_JOB_STATE__ 7 complete 9\n",
            ]
        )
        with (
            mock.patch.object(
                transport,
                "RunSshCapture",
                side_effect=lambda _command: next(responses),
            ),
            contextlib.redirect_stdout(io.StringIO()),
        ):
            with self.assertRaisesRegex(subprocess.CalledProcessError, "exit status 9"):
                transport.RunSshJob("false", "test")


class RemoteLinuxRunnerTests(unittest.TestCase):
    def setUp(self) -> None:
        TEST_OUTPUT_ROOT.mkdir(parents=True, exist_ok=True)
        self._temp_dir = tempfile.TemporaryDirectory(prefix="inpx-web-reader-remote-runner-", dir=TEST_OUTPUT_ROOT)
        self._repo_root = Path(self._temp_dir.name)
        self._module = load_remote_runner_module()
        self._config = self._module.CRemoteLinuxConfig(
            host="build.example",
            user="builder",
            workdir=PurePosixPath("/srv/inpx-web-reader"),
        )

    def tearDown(self) -> None:
        self._temp_dir.cleanup()

    def test_help_runs_without_side_effects(self) -> None:
        with tempfile.TemporaryDirectory(prefix="inpx-web-reader-remote-help-", dir=TEST_OUTPUT_ROOT) as temp_dir:
            result = run_entry_script_help(REMOTE_RUNNER_SCRIPT, Path(temp_dir))

        self.assertIn("Linux Docker deployment bundle", result.stdout)
        self.assertIn("test", result.stdout)
        self.assertIn("e2e", result.stdout)
        self.assertIn("bundle", result.stdout)

    def test_e2e_command_runs_real_server_browser_lane_on_remote_linux(self) -> None:
        args = argparse_namespace(
            artifacts_root="out/web-remote",
            tool_args=["--", "--project=chromium"],
        )

        command = self._module.build_e2e_command(args)

        self.assertEqual(
            command,
            [
                "python3",
                "scripts/RunWebUi.py",
                "--artifacts-root",
                "out/web-remote",
                "test:e2e",
                "--",
                "--project=chromium",
            ],
        )

    def test_main_syncs_then_runs_remote_test_worker(self) -> None:
        transport = mock.Mock()
        transport.ActiveSshJob.return_value = None
        with (
            mock.patch.object(
                sys,
                "argv",
                ["RunRemoteLinux.py", "test", "--parallel-jobs", "3", "--smoke-book-count", "2048"],
            ),
            mock.patch.object(self._module, "repository_root", return_value=self._repo_root),
            mock.patch.object(self._module, "load_remote_config", return_value=self._config),
            mock.patch.object(self._module, "CRemoteLinuxTransport", return_value=transport),
            contextlib.redirect_stdout(io.StringIO()),
        ):
            exit_code = self._module.main()

        self.assertEqual(exit_code, 0)
        transport.CheckLocalPrerequisites.assert_called_once()
        transport.RsyncRepository.assert_called_once()
        remote_command = transport.RunSshJob.call_args.args[0]
        self.assertIn("scripts/RunLinuxTests.py", remote_command)
        self.assertIn("--parallel-jobs 3", remote_command)
        self.assertIn("--smoke-book-count 2048", remote_command)
        self.assertEqual(transport.RunSshJob.call_args.args[1], "test")
        self.assertFalse(transport.RunSshJob.call_args.kwargs["resumeExisting"])

    def test_main_syncs_then_runs_remote_real_server_e2e_worker(self) -> None:
        transport = mock.Mock()
        transport.ActiveSshJob.return_value = None
        with (
            mock.patch.object(
                sys,
                "argv",
                [
                    "RunRemoteLinux.py",
                    "e2e",
                    "--artifacts-root",
                    "out/web-remote",
                    "--",
                    "--project=chromium",
                ],
            ),
            mock.patch.object(self._module, "repository_root", return_value=self._repo_root),
            mock.patch.object(self._module, "load_remote_config", return_value=self._config),
            mock.patch.object(self._module, "CRemoteLinuxTransport", return_value=transport),
            contextlib.redirect_stdout(io.StringIO()),
        ):
            exit_code = self._module.main()

        self.assertEqual(exit_code, 0)
        transport.RsyncRepository.assert_called_once()
        remote_command = transport.RunSshJob.call_args.args[0]
        self.assertIn("scripts/RunWebUi.py", remote_command)
        self.assertIn("--artifacts-root out/web-remote", remote_command)
        self.assertIn("test:e2e -- --project=chromium", remote_command)
        self.assertEqual(transport.RunSshJob.call_args.args[1], "e2e")

    def test_main_configures_utf8_console_before_streaming_remote_logs(self) -> None:
        with (
            mock.patch.object(sys, "argv", ["RunRemoteLinux.py", "test"]),
            mock.patch.object(self._module, "configure_console_streams") as configure_streams,
            mock.patch.object(
                self._module,
                "load_remote_config",
                side_effect=RuntimeError("stop after console setup"),
            ),
            contextlib.redirect_stderr(io.StringIO()),
        ):
            exit_code = self._module.main()

        self.assertEqual(exit_code, 1)
        configure_streams.assert_called_once_with()

    def test_main_resumes_active_test_without_resyncing_sources(self) -> None:
        transport = mock.Mock()
        transport.ActiveSshJob.return_value = "test"
        with (
            mock.patch.object(sys, "argv", ["RunRemoteLinux.py", "test"]),
            mock.patch.object(self._module, "repository_root", return_value=self._repo_root),
            mock.patch.object(self._module, "load_remote_config", return_value=self._config),
            mock.patch.object(self._module, "CRemoteLinuxTransport", return_value=transport),
            contextlib.redirect_stdout(io.StringIO()),
        ):
            exit_code = self._module.main()

        self.assertEqual(exit_code, 0)
        transport.RsyncRepository.assert_not_called()
        transport.RunSshJob.assert_called_once()
        self.assertTrue(transport.RunSshJob.call_args.kwargs["resumeExisting"])

    def test_main_resumes_active_bundle_without_syncing_or_replacing_token(self) -> None:
        token_file = self._repo_root / "token.txt"
        token_file.write_text("secret\n", encoding="utf-8")
        transport = mock.Mock()
        transport.ActiveSshJob.return_value = "bundle"
        with (
            mock.patch.object(
                sys,
                "argv",
                [
                    "RunRemoteLinux.py",
                    "bundle",
                    "--nas-source-root",
                    "/volume/books",
                    "--nas-app-root",
                    "/volume/inpx-web-reader",
                    "--token-file",
                    str(token_file),
                ],
            ),
            mock.patch.object(self._module, "repository_root", return_value=self._repo_root),
            mock.patch.object(self._module, "load_remote_config", return_value=self._config),
            mock.patch.object(self._module, "CRemoteLinuxTransport", return_value=transport),
            mock.patch.object(self._module, "sync_token_file") as sync_token,
            contextlib.redirect_stdout(io.StringIO()),
        ):
            exit_code = self._module.main()

        self.assertEqual(exit_code, 0)
        sync_token.assert_not_called()
        self.assertTrue(transport.RunSshJob.call_args.kwargs["resumeExisting"])
        self.assertEqual(transport.RunSshJob.call_args.kwargs["startCleanupPaths"], ())

    def test_main_rejects_parallel_remote_pipeline_actions(self) -> None:
        transport = mock.Mock()
        transport.ActiveSshJob.return_value = "bundle"
        error_output = io.StringIO()
        with (
            mock.patch.object(sys, "argv", ["RunRemoteLinux.py", "test"]),
            mock.patch.object(self._module, "repository_root", return_value=self._repo_root),
            mock.patch.object(self._module, "load_remote_config", return_value=self._config),
            mock.patch.object(self._module, "CRemoteLinuxTransport", return_value=transport),
            contextlib.redirect_stderr(error_output),
        ):
            exit_code = self._module.main()

        self.assertEqual(exit_code, 1)
        self.assertIn("bundle", error_output.getvalue())
        transport.RsyncRepository.assert_not_called()

    def test_bundle_command_forwards_remote_token_path(self) -> None:
        args = argparse_namespace(
            nas_source_root="/volume/books",
            nas_app_root="/volume/inpx-web-reader",
            host_port=8090,
            image_tag="inpx-web-reader:test",
            docker_platform="linux/amd64",
            build_jobs=2,
            converter_version="latest",
            converter_asset_name="fbc-linux-amd64.zip",
            skip_image_build=False,
            skip_converter_download=True,
        )

        command = self._module.build_bundle_command(args, PurePosixPath("/srv/inpx-web-reader/inputs/token.txt"))

        self.assertIn("--token-file", command)
        self.assertIn("/srv/inpx-web-reader/inputs/token.txt", command)
        self.assertIn("--build-jobs", command)
        self.assertIn("2", command)
        self.assertIn("--skip-converter-download", command)

    def test_bundle_output_defaults_under_repository_out(self) -> None:
        output = self._module.resolve_bundle_output(self._repo_root, None)

        self.assertEqual(output, self._repo_root / "out" / "deploy" / "inpx-web-reader")

    def test_bundle_output_rejects_path_outside_repository_out(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "repository out directory"):
            self._module.resolve_bundle_output(self._repo_root, self._repo_root / "bundle")

    def test_token_file_is_staged_under_out_for_rsync(self) -> None:
        token_file = self._repo_root / "external-token.txt"
        token_file.write_text("secret\n", encoding="utf-8")
        transport = mock.Mock()
        transport.repo_root = self._repo_root
        staged_paths: list[Path] = []

        def capture_staged_path(path: Path, _remote_path: PurePosixPath) -> None:
            self.assertTrue(path.is_file())
            if os.name != "nt":
                self.assertEqual(path.stat().st_mode & 0o777, 0o600)
            staged_paths.append(path)

        transport.RsyncTo.side_effect = capture_staged_path

        remote_path = self._module.sync_token_file(transport, self._config, token_file)

        self.assertRegex(str(remote_path), r"^/srv/inpx-web-reader/inputs/token-[0-9a-f]{32}\.txt$")
        self.assertEqual(len(staged_paths), 1)
        self.assertEqual(staged_paths[0].name, remote_path.name)
        self.assertIn(self._repo_root / "out", staged_paths[0].parents)
        self.assertFalse(staged_paths[0].exists())
        self.assertIn("install -d -m 700", transport.RunSsh.call_args_list[0].args[0])
        self.assertIn("chmod 600", transport.RunSsh.call_args_list[-1].args[0])
        self.assertIn(str(remote_path), transport.RunSsh.call_args_list[-1].args[0])

    def test_token_staging_cleans_local_copy_when_remote_setup_fails(self) -> None:
        token_file = self._repo_root / "external-token.txt"
        token_file.write_text("secret\n", encoding="utf-8")
        transport = mock.Mock()
        transport.repo_root = self._repo_root
        transport.RunSsh.side_effect = RuntimeError("SSH unavailable")

        with self.assertRaisesRegex(RuntimeError, "SSH unavailable"):
            self._module.sync_token_file(transport, self._config, token_file)

        input_root = self._repo_root / "out" / "remote-linux" / "inputs"
        self.assertEqual(list(input_root.glob("*")), [])

    def test_token_staging_cleans_local_copy_when_copy_fails(self) -> None:
        token_file = self._repo_root / "external-token.txt"
        token_file.write_text("secret\n", encoding="utf-8")
        transport = mock.Mock()
        transport.repo_root = self._repo_root

        with (
            mock.patch.object(self._module.shutil, "copyfile", side_effect=OSError("copy failed")),
            self.assertRaisesRegex(OSError, "copy failed"),
        ):
            self._module.sync_token_file(transport, self._config, token_file)

        input_root = self._repo_root / "out" / "remote-linux" / "inputs"
        self.assertEqual(list(input_root.glob("*")), [])
        transport.RunSsh.assert_not_called()


class RemoteLinuxBootstrapTests(unittest.TestCase):
    def setUp(self) -> None:
        self._module = load_remote_bootstrap_module()

    def test_package_script_installs_remote_build_and_docker_tools(self) -> None:
        script = self._module.package_install_script(True)

        self.assertIn("Waiting for another apt/dpkg process", script)
        self.assertIn("pgrep -x apt-get", script)
        self.assertIn("pgrep -x dpkg", script)
        self.assertNotIn("unattended-upgr", script)
        self.assertIn("dpkg --configure -a", script)
        self.assertIn("cmake", script)
        self.assertIn("clang-tidy", script)
        self.assertIn("rsync", script)
        self.assertIn("procps", script)
        self.assertIn("util-linux", script)
        self.assertIn("nodejs", script)
        self.assertIn("npm", script)
        self.assertIn("pipx install --force ruff==", script)
        self.assertIn(self._module.RUFF_VERSION, script)
        self.assertIn("docker.io", script)
        self.assertIn("docker-compose-v2", script)
        self.assertIn("usermod -aG docker", script)

    def test_package_script_can_skip_docker(self) -> None:
        script = self._module.package_install_script(False)

        self.assertNotIn("docker.io", script)
        self.assertNotIn("usermod -aG docker", script)

    def test_vcpkg_script_rebootstraps_when_pinned_ref_changes(self) -> None:
        script = self._module.vcpkg_install_script("/srv/inpx-web-reader", "/srv/inpx-web-reader/vcpkg")

        self.assertIn("previous_ref=", script)
        self.assertIn(self._module.VCPKG_REF, script)
        self.assertIn("bootstrap-vcpkg.sh", script)

    def test_readiness_checks_process_tools_and_pinned_vcpkg_ref(self) -> None:
        script = self._module.readiness_script("/srv/inpx-web-reader/vcpkg", True)

        self.assertIn('test "$(uname -m)" = x86_64', script)
        self.assertIn("flock --version", script)
        self.assertIn("clang-tidy --version", script)
        self.assertIn("setsid --version", script)
        self.assertIn("node --version", script)
        self.assertIn("npm --version", script)
        self.assertIn("ruff --version", script)
        self.assertIn(self._module.RUFF_VERSION, script)
        self.assertIn("git -C /srv/inpx-web-reader/vcpkg rev-parse HEAD", script)
        self.assertIn(self._module.VCPKG_REF, script)


def argparse_namespace(**values: object) -> object:
    return type("CArgs", (), values)()


if __name__ == "__main__":
    unittest.main()
