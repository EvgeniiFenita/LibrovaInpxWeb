from __future__ import annotations

import contextlib
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from SubprocessHarness import load_script_module, run_entry_script_help


REPO_ROOT = Path(__file__).resolve().parents[2]
RUN_SERVER_TESTS_SCRIPT = REPO_ROOT / "scripts" / "RunLinuxTests.py"
COMPOSE_TEMPLATE = REPO_ROOT / "deploy" / "inpx-web-reader" / "docker-compose.yml"
TEST_OUTPUT_ROOT = REPO_ROOT / "out" / "tests" / "RunLinuxTests"


def load_run_server_tests_module() -> object:
    return load_script_module(RUN_SERVER_TESTS_SCRIPT, "run_server_tests_under_test")


def make_container_inspect_json(
    *,
    readonly_rootfs: bool = True,
    tmpfs_options: str = "rw,noexec,nosuid,nodev,size=16m",
    cap_drop: list[str] | None = None,
    security_opt: list[str] | None = None,
) -> str:
    return json.dumps(
        [
            {
                "Mounts": [
                    {"Destination": "/source", "RW": False},
                    {"Destination": "/data", "RW": True},
                ],
                "Config": {
                    "Env": [
                        "INPX_WEB_READER_AUTH_TOKEN_FILE=/run/secrets/inpx_web_reader_auth_token",
                    ],
                },
                "HostConfig": {
                    "ReadonlyRootfs": readonly_rootfs,
                    "Tmpfs": {
                        "/tmp": tmpfs_options,
                    },
                    "CapDrop": cap_drop if cap_drop is not None else ["ALL"],
                    "SecurityOpt": security_opt if security_opt is not None else ["no-new-privileges:true"],
                },
            },
        ]
    )


class RunLinuxTestsScriptTests(unittest.TestCase):
    def setUp(self) -> None:
        TEST_OUTPUT_ROOT.mkdir(parents=True, exist_ok=True)
        self._temp_dir = tempfile.TemporaryDirectory(
            prefix="inpx-web-reader-run-server-tests-",
            dir=TEST_OUTPUT_ROOT,
        )
        self._repo_root = Path(self._temp_dir.name)
        self._module = load_run_server_tests_module()

    def tearDown(self) -> None:
        self._temp_dir.cleanup()

    def test_parse_args_defaults_to_linux_server_preset(self) -> None:
        with mock.patch.object(sys, "argv", ["RunLinuxTests.py"]):
            args = self._module.parse_args()

        self.assertEqual(args.preset, self._module.DEFAULT_PRESET)
        self.assertEqual(args.image_tag, self._module.DEFAULT_IMAGE_TAG)
        self.assertIsNone(args.parallel_jobs)
        self.assertIsNone(args.build_jobs)
        self.assertIsNone(args.test_jobs)
        self.assertEqual(args.smoke_book_count, self._module.DEFAULT_SMOKE_BOOK_COUNT)

    def test_main_runs_native_docker_and_runtime_checks_in_order(self) -> None:
        commands: list[tuple[list[str], Path]] = []
        environments: list[dict[str, str] | None] = []

        def fake_run(command: list[str], cwd: Path, env: dict[str, str] | None = None) -> None:
            commands.append((command, cwd))
            environments.append(env)

        with (
            mock.patch.object(
                sys,
                "argv",
                [
                    "RunLinuxTests.py",
                    "--preset",
                    "linux-debug",
                    "--image-tag",
                    "inpx-web-reader:test",
                    "--docker-platform",
                    "linux/amd64",
                    "--parallel-jobs",
                    "3",
                    "--skip-compose",
                ],
            ),
            mock.patch.object(self._module, "repository_root", return_value=self._repo_root),
            mock.patch.object(self._module, "ensure_linux_host"),
            mock.patch.object(self._module, "ensure_docker_engine") as ensure_docker,
            mock.patch.object(self._module, "run", side_effect=fake_run),
            mock.patch.object(self._module, "verify_runtime_image_shape") as verify_runtime_image_shape,
            mock.patch.object(self._module, "verify_runtime_compose_identity") as verify_runtime_compose_identity,
            mock.patch.object(
                self._module,
                "verify_runtime_entrypoint_rejects_partial_source",
            ) as verify_runtime_entrypoint,
            contextlib.redirect_stdout(io.StringIO()),
        ):
            exit_code = self._module.main()

        self.assertEqual(exit_code, 0)
        ensure_docker.assert_called_once_with(self._repo_root)
        self.assertEqual(
            commands,
            [
                (["cmake", "--preset", "linux-debug"], self._repo_root),
                (["cmake", "--build", "--preset", "linux-debug", "--parallel", "3"], self._repo_root),
                (
                    [
                        "ctest",
                        "--test-dir",
                        str(self._repo_root / "out" / "build" / "linux-debug"),
                        "--output-on-failure",
                        "--no-tests=error",
                        "-j",
                        "3",
                    ],
                    self._repo_root,
                ),
                (
                    [
                        "docker",
                        "build",
                        "--platform",
                        "linux/amd64",
                        "--build-arg",
                        "INPX_WEB_READER_BUILD_JOBS=3",
                        "-f",
                        str(self._repo_root / self._module.DEFAULT_DOCKERFILE),
                        "-t",
                        "inpx-web-reader:test",
                        ".",
                    ],
                    self._repo_root,
                ),
            ],
        )
        for environment in environments[:3]:
            self.assertIsNotNone(environment)
            self.assertEqual(environment["VCPKG_MAX_CONCURRENCY"], "3")
        self.assertIsNone(environments[3])
        verify_runtime_image_shape.assert_called_once_with(self._repo_root, "inpx-web-reader:test")
        verify_runtime_compose_identity.assert_called_once_with(
            self._repo_root,
            self._repo_root / self._module.DEFAULT_COMPOSE_FILE,
            "inpx-web-reader:test",
        )
        verify_runtime_entrypoint.assert_called_once_with(self._repo_root, "inpx-web-reader:test")

    def test_main_rejects_all_stages_skipped(self) -> None:
        with (
            mock.patch.object(
                sys,
                "argv",
                [
                    "RunLinuxTests.py",
                    "--skip-native",
                    "--skip-docker",
                ],
            ),
            mock.patch.object(self._module, "repository_root", return_value=self._repo_root),
            mock.patch.object(self._module, "ensure_linux_host"),
            contextlib.redirect_stdout(io.StringIO()),
        ):
            with self.assertRaisesRegex(RuntimeError, "No stages were selected"):
                self._module.main()

    def test_ensure_linux_host_points_non_linux_users_to_remote_runner(self) -> None:
        with mock.patch.object(self._module.sys, "platform", "non-linux"):
            with self.assertRaisesRegex(RuntimeError, "RunRemoteLinux.py"):
                self._module.ensure_linux_host()

    def test_ensure_linux_host_rejects_non_x86_64_linux(self) -> None:
        with (
            mock.patch.object(self._module.sys, "platform", "linux"),
            mock.patch.object(self._module.platform, "machine", return_value="aarch64"),
            self.assertRaisesRegex(RuntimeError, "x86_64"),
        ):
            self._module.ensure_linux_host()

    def test_status_wait_uses_current_inpx_status_contract(self) -> None:
        responses = [
            (200, {"status": "open"}),
            (200, {"status": "open", "inpxSource": {"available": True}}),
        ]

        with (
            mock.patch.object(self._module, "request_json", side_effect=responses),
            mock.patch.object(self._module.time, "sleep"),
        ):
            status = self._module.wait_for_status("http://127.0.0.1:49152")

        self.assertEqual(status["inpxSource"], {"available": True})

    def test_runtime_image_shape_check_rejects_node_and_build_tools(self) -> None:
        commands: list[list[str]] = []

        def fake_run(command: list[str], cwd: Path, env: dict[str, str] | None = None) -> None:
            commands.append(command)

        with (
            mock.patch.object(self._module, "run", side_effect=fake_run),
            mock.patch.object(
                self._module,
                "run_capture",
                side_effect=["linux/amd64\n", "InpxWebReader\n"],
            ),
        ):
            self._module.verify_runtime_image_shape(self._repo_root, "inpx-web-reader:test")

        command_text = " ".join(commands[0])
        self.assertIn("test -x /opt/inpx-web-reader/inpx-web-reader", command_text)
        self.assertIn("! command -v node", command_text)
        self.assertIn("! command -v npm", command_text)
        self.assertIn("! command -v cmake", command_text)
        self.assertIn("! command -v g++", command_text)
        self.assertIn("! test -d /src", command_text)

    def test_runtime_image_shape_rejects_non_amd64_platform(self) -> None:
        with (
            mock.patch.object(self._module, "run_capture", return_value="linux/arm64\n"),
            mock.patch.object(self._module, "run") as run,
            self.assertRaisesRegex(RuntimeError, "expected linux/amd64"),
        ):
            self._module.verify_runtime_image_shape(self._repo_root, "inpx-web-reader:test")

        run.assert_not_called()

    def test_runtime_image_shape_requires_cleanup_identity_label(self) -> None:
        with (
            mock.patch.object(
                self._module,
                "run_capture",
                side_effect=["linux/amd64\n", "\n"],
            ),
            mock.patch.object(self._module, "run") as run,
            self.assertRaisesRegex(RuntimeError, "org.opencontainers.image.title"),
        ):
            self._module.verify_runtime_image_shape(self._repo_root, "inpx-web-reader:test")

        run.assert_not_called()

    def test_runtime_entrypoint_rejects_both_partial_source_shapes(self) -> None:
        rejected = self._module.subprocess.CompletedProcess(
            ["docker", "run"],
            1,
            stdout="INPX source configuration is partial after auto-detection\n",
        )
        with mock.patch.object(
            self._module.subprocess,
            "run",
            side_effect=[
                rejected,
                rejected,
                self._module.subprocess.CompletedProcess(
                    ["docker", "run"],
                    1,
                    stdout="INPX source config was not detected; cache is absent\n",
                ),
            ],
        ) as run:
            self._module.verify_runtime_entrypoint_rejects_partial_source(
                self._repo_root,
                "inpx-web-reader:test",
            )

        self.assertEqual(run.call_count, 3)
        commands = [call.args[0] for call in run.call_args_list]
        self.assertIn("INPX_WEB_READER_INPX_PATH=/source/catalog.inpx", commands[0])
        self.assertIn("INPX_WEB_READER_ARCHIVE_ROOT=/source", commands[1])
        for command in commands[:2]:
            mount_index = command.index("--mount")
            mount_value = command[mount_index + 1]
            self.assertIn("dst=/source", mount_value)
            self.assertIn("readonly", mount_value)
        incomplete_mount = commands[2][commands[2].index("--mount") + 1]
        self.assertIn("inpx-only-source", incomplete_mount)

        fixture_source = self._repo_root / "out" / "server-docker-partial-source" / "source"
        self.assertTrue((fixture_source / "catalog.inpx").is_file())
        self.assertTrue((fixture_source / "books.zip").is_file())
        incomplete_source = (
            self._repo_root
            / "out"
            / "server-docker-partial-source"
            / "inpx-only-source"
            / "catalog.inpx"
        )
        self.assertTrue(incomplete_source.is_file())

    def test_runtime_compose_uses_the_selected_container_identity(self) -> None:
        with (
            mock.patch.object(
                self._module,
                "resolve_docker_compose_command",
                return_value=["docker", "compose"],
            ),
            mock.patch.object(
                self._module,
                "run_capture",
                return_value="services:\n  inpx-web-reader:\n    user: '2000:2001'\n",
            ) as run_capture,
        ):
            self._module.verify_runtime_compose_identity(
                self._repo_root,
                self._repo_root / self._module.DEFAULT_COMPOSE_FILE,
                "inpx-web-reader:test",
            )

        environment = run_capture.call_args.kwargs["env"]
        self.assertEqual(environment["INPX_WEB_READER_CONTAINER_UID"], "2000")
        self.assertEqual(environment["INPX_WEB_READER_CONTAINER_GID"], "2001")

    def test_compose_command_falls_back_to_standalone_docker_compose(self) -> None:
        completed = self._module.subprocess.CompletedProcess(
            ["docker", "compose", "version"],
            1,
            stdout="docker: unknown command: docker compose",
        )
        with (
            mock.patch.object(self._module.subprocess, "run", return_value=completed),
            mock.patch.object(self._module.shutil, "which", return_value="/opt/homebrew/bin/docker-compose"),
        ):
            command = self._module.resolve_docker_compose_command(self._repo_root)

        self.assertEqual(command, ["docker-compose"])

    def test_container_mount_check_requires_compose_security_hardening(self) -> None:
        responses = iter(
            [
                "container-id\n",
                make_container_inspect_json(),
                "10001\n",
                "600\n",
                "absent\n",
            ]
        )

        with mock.patch.object(self._module, "run_capture", side_effect=lambda *_args: next(responses)):
            container_id = self._module.verify_container_mounts(
                self._repo_root,
                COMPOSE_TEMPLATE,
                self._repo_root / "compose.env",
                "inpx-web-reader-smoke-test",
            )

        self.assertEqual(container_id, "container-id")

    def test_runtime_logs_are_checked_inside_the_container_and_docker_console(self) -> None:
        commands: list[list[str]] = []
        log_text = (
            "[2026-07-14T10:20:30.123Z] [info] [Server] InpxWebReader startup: "
            "logLevel=info logMaxFileSizeMiB=20 logMaxRotatedFiles=4\n"
            "[2026-07-14T10:20:30.456Z] [info] [Server] INPX initial scan started. "
            "source='/source/catalog.inpx' archiveRoot='/source/lib.rus.ec'\n"
            "[2026-07-14T10:20:31.123Z] [info] [Server] INPX initial scan completed. jobId=1\n"
            "[2026-07-14T10:20:31.456Z] [info] [Server] INPX rescan completed. jobId=2\n"
            "[2026-07-14T10:20:32.123Z] [info] [Server] "
            "HTTP request: requestId=req-1 method=GET path=/ status=200 durationMs=1\n"
        )
        responses = iter(["present\n", log_text, log_text])

        def fake_run_capture(command: list[str], _cwd: Path) -> str:
            commands.append(command)
            return next(responses)

        with (
            mock.patch.object(self._module, "run_capture", side_effect=fake_run_capture),
            mock.patch.object(self._module.time, "sleep"),
        ):
            self._module.verify_container_runtime_logs(self._repo_root, "container-id")

        self.assertEqual(commands[0][:3], ["docker", "exec", "container-id"])
        self.assertIn("/data/cache/Database/inpx-web-reader.db", commands[0][-1])
        self.assertIn("/data/runtime/Logs/inpx-web-reader.log", commands[0][-1])
        self.assertEqual(
            commands[1],
            [
                "docker",
                "exec",
                "container-id",
                "cat",
                "/data/runtime/Logs/inpx-web-reader.log",
            ],
        )
        self.assertEqual(commands[2], ["docker", "logs", "container-id"])

    def test_runtime_log_check_waits_for_periodic_file_flush(self) -> None:
        log_text = (
            "[2026-07-14T10:20:30.123Z] [info] [Server] InpxWebReader startup: "
            "logLevel=info logMaxFileSizeMiB=20 logMaxRotatedFiles=4\n"
            "[2026-07-14T10:20:30.456Z] [info] [Server] INPX initial scan started. "
            "source='/source/catalog.inpx' archiveRoot='/source/lib.rus.ec'\n"
            "[2026-07-14T10:20:31.123Z] [info] [Server] INPX initial scan completed. jobId=1\n"
            "[2026-07-14T10:20:31.456Z] [info] [Server] INPX rescan completed. jobId=2\n"
            "[2026-07-14T10:20:32.123Z] [info] [Server] "
            "HTTP request: requestId=req-1 method=GET path=/ status=200 durationMs=1\n"
        )
        responses = iter(["startup record only\n", log_text])

        with (
            mock.patch.object(self._module, "run_capture", side_effect=lambda *_args: next(responses)),
            mock.patch.object(self._module.time, "monotonic", side_effect=[10.0, 10.0]),
            mock.patch.object(self._module.time, "sleep") as sleep,
        ):
            result = self._module.wait_for_persistent_runtime_log(self._repo_root, "container-id")

        self.assertEqual(result, log_text)
        sleep.assert_called_once_with(0.25)

    def test_container_mount_check_rejects_writable_root_filesystem(self) -> None:
        responses = iter(
            [
                "container-id\n",
                make_container_inspect_json(readonly_rootfs=False),
            ]
        )

        with mock.patch.object(self._module, "run_capture", side_effect=lambda *_args: next(responses)):
            with self.assertRaisesRegex(RuntimeError, "root filesystem"):
                self._module.verify_container_mounts(
                    self._repo_root,
                    COMPOSE_TEMPLATE,
                    self._repo_root / "compose.env",
                    "inpx-web-reader-smoke-test",
                )

    def test_scan_wait_accepts_server_started_initial_scan(self) -> None:
        calls: list[tuple[str, str, str, object | None]] = []
        responses = [
            (200, {"active": True, "status": "running"}),
            (200, {"active": False, "status": "completed", "result": {"totalRecords": 64}}),
        ]

        def fake_request_json(
            base_url: str,
            path: str,
            method: str = "GET",
            body: object | None = None,
        ) -> tuple[int, dict]:
            calls.append((base_url, path, method, body))
            return responses.pop(0)

        with (
            mock.patch.object(self._module, "request_json", side_effect=fake_request_json),
            mock.patch.object(self._module.time, "sleep"),
        ):
            progress = self._module.wait_for_scan_completion(
                "http://127.0.0.1:49152",
                "initial",
                accept_existing_completed=True,
            )

        self.assertEqual(progress["result"]["totalRecords"], 64)
        self.assertEqual(
            calls,
            [
                ("http://127.0.0.1:49152", "/api/scan/progress", "GET", None),
                ("http://127.0.0.1:49152", "/api/scan/progress", "GET", None),
            ],
        )

    def test_scan_wait_starts_a_requested_rescan_and_returns_terminal_metrics(self) -> None:
        responses = [
            (202, {"jobId": 9}),
            (200, {"active": True, "status": "running"}),
            (200, {"active": False, "status": "completed", "result": {"reusedRecords": 55}}),
        ]
        with (
            mock.patch.object(self._module, "request_json", side_effect=responses) as request_json,
            mock.patch.object(self._module.time, "sleep"),
        ):
            progress = self._module.wait_for_scan_completion(
                "http://127.0.0.1:49152",
                "rescan",
            )

        self.assertEqual(progress["result"]["reusedRecords"], 55)
        self.assertEqual(
            request_json.call_args_list[0].kwargs,
            {"method": "POST", "body": {"mode": "rescan", "warningLimit": 5}},
        )

    def test_initial_import_verification_requires_every_generated_book_and_segment(self) -> None:
        progress = {
            "active": False,
            "status": "completed",
            "result": {
                "totalRecords": 10,
                "scannedRecords": 10,
                "parsedFb2Records": 10,
                "addedRecords": 10,
                "updatedRecords": 0,
                "markedUnavailableRecords": 0,
                "unavailableRecords": 0,
                "skippedRecords": 0,
                "reusedRecords": 0,
                "segmentsTotal": 2,
                "segmentsUnchanged": 0,
                "segmentsAdded": 2,
                "segmentsChanged": 0,
                "segmentsRemoved": 0,
                "archivesSkipped": 0,
                "archivesOpened": 2,
                "archiveBytesRead": 4096,
                "warningCount": 0,
            },
        }

        self._module.verify_initial_scan_result(progress, 10)

        incomplete = {**progress, "result": {**progress["result"], "parsedFb2Records": 9}}
        with self.assertRaisesRegex(RuntimeError, "fully parse and publish"):
            self._module.verify_initial_scan_result(incomplete, 10)

    def test_incremental_update_verification_rejects_a_full_catalog_reparse(self) -> None:
        progress = {
            "active": False,
            "status": "completed",
            "result": {
                "totalRecords": 65,
                "scannedRecords": 65,
                "parsedFb2Records": 10,
                "addedRecords": 1,
                "updatedRecords": 9,
                "markedUnavailableRecords": 0,
                "unavailableRecords": 0,
                "skippedRecords": 0,
                "reusedRecords": 55,
                "segmentsTotal": 13,
                "segmentsUnchanged": 11,
                "segmentsAdded": 0,
                "segmentsChanged": 2,
                "segmentsRemoved": 0,
                "archivesSkipped": 11,
                "archivesOpened": 2,
                "archiveBytesRead": 4096,
                "warningCount": 0,
            },
        }

        self._module.verify_incremental_update_result(progress, 64)

        full_reparse = {
            **progress,
            "result": {
                **progress["result"],
                "parsedFb2Records": 65,
                "updatedRecords": 64,
                "reusedRecords": 0,
            },
        }
        with self.assertRaisesRegex(RuntimeError, "reuse unchanged segments"):
            self._module.verify_incremental_update_result(full_reparse, 64)

    def test_incremental_update_verification_covers_archive_boundaries(self) -> None:
        common_result = {
            "addedRecords": 1,
            "markedUnavailableRecords": 0,
            "unavailableRecords": 0,
            "skippedRecords": 0,
            "reusedRecords": 0,
            "segmentsUnchanged": 0,
            "segmentsRemoved": 0,
            "archivesSkipped": 0,
            "archiveBytesRead": 4096,
            "warningCount": 0,
        }
        cases = (
            (
                1,
                {
                    "totalRecords": 2,
                    "scannedRecords": 2,
                    "parsedFb2Records": 2,
                    "updatedRecords": 1,
                    "segmentsTotal": 1,
                    "segmentsAdded": 0,
                    "segmentsChanged": 1,
                    "archivesOpened": 1,
                },
            ),
            (
                5,
                {
                    "totalRecords": 6,
                    "scannedRecords": 6,
                    "parsedFb2Records": 6,
                    "updatedRecords": 5,
                    "segmentsTotal": 2,
                    "segmentsAdded": 1,
                    "segmentsChanged": 1,
                    "archivesOpened": 2,
                },
            ),
        )

        for initial_book_count, expected_result in cases:
            with self.subTest(initial_book_count=initial_book_count):
                self._module.verify_incremental_update_result(
                    {
                        "active": False,
                        "status": "completed",
                        "result": {**common_result, **expected_result},
                    },
                    initial_book_count,
                )

    def test_authentication_check_rejects_missing_and_incorrect_tokens(self) -> None:
        calls: list[str | None] = []

        def fake_request_json(
            _base_url: str,
            _path: str,
            method: str = "GET",
            body: object | None = None,
            auth_token: str | None = self._module.DEFAULT_TOKEN,
        ) -> tuple[int, dict]:
            _ = method, body
            calls.append(auth_token)
            return 401, {"error": {"code": "unauthorized"}}

        with mock.patch.object(self._module, "request_json", side_effect=fake_request_json):
            self._module.verify_authentication("http://127.0.0.1:49152")

        self.assertEqual(calls, [None, "wrong-docker-smoke-token"])

    def test_compose_env_uses_token_file_not_token_value(self) -> None:
        with mock.patch.object(self._module.Path, "chmod") as chmod:
            env_path = self._module.write_compose_env(self._repo_root, "inpx-web-reader:test", 49152)
        env_text = env_path.read_text(encoding="utf-8")

        self.assertIn("INPX_WEB_READER_AUTH_TOKEN_FILE=", env_text)
        self.assertNotIn("INPX_WEB_READER_AUTH_TOKEN=docker-smoke-token", env_text)
        token_path = self._repo_root / "auth-token.txt"
        self.assertEqual(token_path.read_text(encoding="utf-8"), "docker-smoke-token\n")
        chmod.assert_any_call(0o644)

    def test_compose_env_uses_shared_server_limit_mapping(self) -> None:
        env_path = self._module.write_compose_env(self._repo_root, "inpx-web-reader:test", 49152)
        env_text = env_path.read_text(encoding="utf-8")

        for line in self._module.server_limit_env_lines(self._module.SMOKE_LIMITS):
            self.assertIn(line, env_text)
        self.assertIn("INPX_WEB_READER_LOG_LEVEL=info", env_text)
        self.assertIn("INPX_WEB_READER_LOG_MAX_FILE_SIZE_MIB=20", env_text)
        self.assertIn("INPX_WEB_READER_LOG_MAX_ROTATED_FILES=4", env_text)

    def test_web_ui_serving_requires_an_html_entry_point(self) -> None:
        with mock.patch.object(
            self._module,
            "request_bytes",
            return_value=(200, b"<!doctype html><html></html>", {"Content-Type": "text/html; charset=utf-8"}),
        ):
            self._module.verify_web_ui_serving("http://127.0.0.1:49152")

        with mock.patch.object(
            self._module,
            "request_bytes",
            return_value=(200, b"{}", {"Content-Type": "application/json"}),
        ):
            with self.assertRaisesRegex(RuntimeError, "Web UI entry point"):
                self._module.verify_web_ui_serving("http://127.0.0.1:49152")

    def test_smoke_fixture_contains_larger_catalog_probe(self) -> None:
        books_dir = self._repo_root / "books"

        self._module.write_smoke_book(books_dir, 64)

        fb2_files = sorted(books_dir.glob("*.fb2"))
        self.assertEqual(len(fb2_files), 64)
        self.assertIn("Docker Smoke Book", (books_dir / "docker-smoke.fb2").read_text(encoding="utf-8"))
        self.assertIn(
            "Fixture Load Book 063",
            (books_dir / "fixture-load-000063.fb2").read_text(encoding="utf-8"),
        )

    def test_smoke_fixture_supports_a_deterministic_scale_size(self) -> None:
        books_dir = self._repo_root / "scale-books"

        self._module.write_smoke_book(books_dir, 257)

        fb2_files = sorted(books_dir.glob("*.fb2"))
        self.assertEqual(len(fb2_files), 257)
        self.assertEqual(fb2_files[-1].name, "fixture-load-000256.fb2")
        self.assertIn(
            "Fixture Load Book 256",
            (books_dir / "fixture-load-000256.fb2").read_text(encoding="utf-8"),
        )

    def test_catalog_flow_requires_the_exact_generated_catalog_size(self) -> None:
        responses = [
            (
                200,
                {
                    "catalogSnapshotId": "scan-17",
                    "totalCount": 257,
                    "items": [{"id": 6}],
                    "nextCursor": "opaque+/=",
                },
            ),
            (
                200,
                {
                    "catalogSnapshotId": "scan-17",
                    "totalCount": None,
                    "facets": None,
                    "offset": 1,
                    "items": [{"id": 7}],
                },
            ),
            (200, {"bookCount": 257, "unavailableBookCount": 0}),
            (
                200,
                {
                    "source": {
                        "totalBookCount": 257,
                        "availableBookCount": 257,
                        "unavailableBookCount": 0,
                        "lastSeenSnapshotId": "scan-17",
                    }
                },
            ),
            (200, {"totalCount": 1, "items": [{"id": 7, "title": "Docker Smoke Book"}]}),
            (200, {"book": {"title": "Docker Smoke Book"}}),
        ]
        with (
            mock.patch.object(self._module, "request_json", side_effect=responses) as request_json,
            mock.patch.object(
                self._module,
                "request_bytes",
                side_effect=[
                    (200, b"png", {"Content-Type": "image/png"}),
                    (200, b"Docker Smoke Book", {"Content-Disposition": "filename*=UTF-8''book.fb2"}),
                ],
            ),
        ):
            self.assertEqual(
                self._module.verify_catalog_flow(
                    "http://127.0.0.1:49152",
                    257,
                    "Docker Smoke Book",
                    b"Docker Smoke Book",
                ),
                (7, "scan-17"),
            )
            self.assertEqual(
                request_json.call_args_list[1].args[1],
                "/api/books?limit=1&cursor=opaque%2B%2F%3D",
            )

        with mock.patch.object(
            self._module,
            "request_json",
            return_value=(200, {"totalCount": 256, "items": []}),
        ):
            with self.assertRaisesRegex(RuntimeError, "expected 257 books"):
                self._module.verify_catalog_flow(
                    "http://127.0.0.1:49152",
                    257,
                    "Docker Smoke Book",
                )

    def test_runtime_status_check_requires_linux_memory_metrics(self) -> None:
        status = {
            "runtime": {
                "uptimeSeconds": 3,
                "http": {
                    "activeWorkers": 1,
                    "queuedRequests": 0,
                    "maxWorkers": 2,
                    "maxQueuedRequests": 16,
                },
                "backend": {
                    "activeOperations": 0,
                    "queuedOperations": 0,
                    "maxQueueDepth": 16,
                },
                "scan": {
                    "active": False,
                    "activeJobs": 0,
                    "maxConcurrentJobs": 1,
                    "activeWorkers": 0,
                    "maxWorkers": 1,
                },
                "downloads": {
                    "active": 0,
                    "maxConcurrent": 2,
                },
                "storage": {
                    "cacheRootPresent": True,
                    "cacheDatabasePresent": True,
                    "runtimeWorkspacePresent": True,
                    "coverCacheBytes": 1024,
                    "inpxScanWorkspaceBytes": 0,
                    "downloadWorkspaceBytes": 0,
                },
                "resources": {
                    "residentMemoryBytes": 256 * 1024 * 1024,
                    "peakResidentMemoryBytes": 300 * 1024 * 1024,
                    "maxCoverCacheBytes": 32 * 1024 * 1024,
                    "maxSteadyStateMemoryBytes": 1024 * 1024 * 1024,
                },
            }
        }

        self._module.verify_runtime_status(status)

        status["runtime"]["resources"]["residentMemoryBytes"] = None
        with self.assertRaisesRegex(RuntimeError, "resident RSS"):
            self._module.verify_runtime_status(status)

    def test_help_run_does_not_create_pycache_in_scripts_dir(self) -> None:
        result = run_entry_script_help(RUN_SERVER_TESTS_SCRIPT, self._repo_root)
        scripts_dir = self._repo_root / "scripts"

        self.assertIn("Run InpxWebReader native and Docker deployment checks", result.stdout)
        self.assertFalse((scripts_dir / "__pycache__").exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
