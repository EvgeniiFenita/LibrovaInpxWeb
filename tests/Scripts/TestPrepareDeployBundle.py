from __future__ import annotations

import contextlib
import io
import os
import stat
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from SubprocessHarness import load_script_module, run_entry_script_help


REPO_ROOT = Path(__file__).resolve().parents[2]
PREPARE_DEPLOY_BUNDLE_SCRIPT = REPO_ROOT / "scripts" / "PrepareDeployBundle.py"
DEPLOY_ENV_TEMPLATE = REPO_ROOT / "deploy" / "inpx-web-reader" / ".env.template"
DEPLOY_COMPOSE_TEMPLATE = REPO_ROOT / "deploy" / "inpx-web-reader" / "docker-compose.yml"
TEST_OUTPUT_ROOT = REPO_ROOT / "out" / "tests" / "PrepareDeployBundle"


def load_prepare_deploy_bundle_module() -> object:
    return load_script_module(PREPARE_DEPLOY_BUNDLE_SCRIPT, "prepare_deploy_bundle_under_test")


class PrepareDeployBundleScriptTests(unittest.TestCase):
    def setUp(self) -> None:
        TEST_OUTPUT_ROOT.mkdir(parents=True, exist_ok=True)
        self._temp_dir = tempfile.TemporaryDirectory(
            prefix="inpx-web-reader-prepare-host-deploy-",
            dir=TEST_OUTPUT_ROOT,
        )
        self._repo_root = Path(self._temp_dir.name)
        compose_root = self._repo_root / "deploy" / "inpx-web-reader"
        compose_root.mkdir(parents=True)
        (compose_root / "docker-compose.yml").write_text(
            "services:\n"
            "  inpx-web-reader:\n"
            "    image: ${INPX_WEB_READER_IMAGE:-inpx-web-reader:latest}\n"
            '    user: "${INPX_WEB_READER_CONTAINER_UID:-10001}:${INPX_WEB_READER_CONTAINER_GID:-10001}"\n'
            "    restart: unless-stopped\n",
            encoding="utf-8",
        )
        self._module = load_prepare_deploy_bundle_module()
        self._platform_patch = mock.patch.object(self._module.sys, "platform", "linux")
        self._machine_patch = mock.patch.object(self._module.platform, "machine", return_value="x86_64")
        self._platform_patch.start()
        self._machine_patch.start()

    def tearDown(self) -> None:
        self._machine_patch.stop()
        self._platform_patch.stop()
        self._temp_dir.cleanup()

    def _prepare_runnable_generated_script(self) -> tuple[Path, Path]:
        source_root = self._repo_root / "host-source"
        source_root.mkdir()
        bundle_root = self._repo_root / "out" / "deploy" / "inpx-web-reader"
        with (
            mock.patch.object(self._module, "repository_root", return_value=self._repo_root),
            mock.patch.object(
                sys,
                "argv",
                [
                    "PrepareDeployBundle.py",
                    "--host-source-root",
                    str(source_root),
                    "--host-app-root",
                    str(bundle_root),
                    "--skip-image-build",
                    "--skip-converter-download",
                ],
            ),
            contextlib.redirect_stdout(io.StringIO()),
        ):
            self.assertEqual(self._module.main(), 0)

        archive_path = bundle_root / self._module.IMAGE_ARCHIVE_NAME
        archive_path.write_bytes(b"fake image archive")
        archive_sha256 = self._module.sha256_file(archive_path)
        (bundle_root / self._module.IMAGE_ARCHIVE_CHECKSUM_NAME).write_text(
            f"{archive_sha256}  {self._module.IMAGE_ARCHIVE_NAME}\n",
            encoding="utf-8",
        )
        (bundle_root / self._module.BUNDLE_MANIFEST_NAME).write_text("{}\n", encoding="utf-8")

        fake_bin = self._repo_root / "out" / "fake-host-bin"
        fake_bin.mkdir(parents=True)
        fake_commands = {
            "docker": """#!/bin/sh
set -eu
printf '%s\n' "$*" >> "$FAKE_DOCKER_LOG"

if [ "${1:-}" = "compose" ]; then
    shift
    if [ "${1:-}" = "version" ]; then
        exit 0
    fi
    while [ "$#" -gt 0 ]; do
        case "$1" in
            --env-file|-f) shift 2 ;;
            *) break ;;
        esac
    done
    compose_action="${1:-}"
    case "$compose_action" in
        ps)
            if [ -f "$FAKE_DOCKER_STATE" ]; then
                command cat "$FAKE_DOCKER_STATE"
            else
                printf 'existing-old\n'
            fi
            ;;
        up)
            if [ "$FAKE_DEPLOY_MODE" = "rollback" ] && [ -f "$FAKE_DOCKER_STATE" ]; then
                printf 'rollback-container\n' > "$FAKE_DOCKER_STATE"
            else
                printf 'current-new\n' > "$FAKE_DOCKER_STATE"
            fi
            ;;
        logs) ;;
        *) exit 2 ;;
    esac
    exit 0
fi

case "${1:-}" in
    load|run|tag|rm)
        exit 0
        ;;
    inspect)
        format="${3:-}"
        target="${4:-}"
        case "$format" in
            *'.Image'*) printf 'sha256:old\n' ;;
            *'RestartPolicy.Name'*) printf 'unless-stopped\n' ;;
            *'State.Health.Status'*)
                if [ "$FAKE_DEPLOY_MODE" = "rollback" ] && [ "$target" = "current-new" ]; then
                    printf 'starting\n'
                else
                    printf 'healthy\n'
                fi
                ;;
            *'.State.Running'*) printf 'false\n' ;;
            *) exit 2 ;;
        esac
        exit 0
        ;;
    image)
        image_action="${2:-}"
        case "$image_action" in
            inspect)
                format="${4:-}"
                target="${5:-}"
                case "$format" in
                    *'.Id'*) printf 'sha256:new\n' ;;
                    *'.Os'*) printf 'linux/amd64\n' ;;
                    *'org.opencontainers.image.title'*)
                        case "$target" in
                            sha256:old|sha256:ancient|sha256:protected) printf 'InpxWebReader\n' ;;
                            *) printf 'OtherImage\n' ;;
                        esac
                        ;;
                    *'.Config.Entrypoint'*) printf 'null\n' ;;
                    *) exit 2 ;;
                esac
                ;;
            ls)
                case "$*" in
                    *'dangling=true'*) printf 'sha256:foreign\n' ;;
                    *'reference='*)
                        printf 'sha256:new\nsha256:old\nsha256:ancient\nsha256:protected\n'
                        ;;
                esac
                ;;
            rm) ;;
            *) exit 2 ;;
        esac
        exit 0
        ;;
    ps)
        case "$*" in
            *'ancestor=sha256:protected'*) printf 'retained-container\n' ;;
            *'ancestor='*) ;;
            *'com.docker.compose.project=inpx-web-reader'*)
                printf 'current-new\nold-stopped\n'
                ;;
        esac
        exit 0
        ;;
esac

exit 2
""",
            "stat": """#!/bin/sh
set -eu
case "${2:-}" in
    %u|%g) printf '1000\n' ;;
    *) exit 2 ;;
esac
""",
            "ip": """#!/bin/sh
printf '1.1.1.1 via 192.0.2.1 dev eth0 src 192.0.2.25\n'
""",
            "systemctl": "#!/bin/sh\nexit 0\n",
            "chown": "#!/bin/sh\nexit 0\n",
            "chmod": "#!/bin/sh\nexit 0\n",
            "sleep": "#!/bin/sh\nexit 0\n",
        }
        for name, content in fake_commands.items():
            command_path = fake_bin / name
            command_path.write_text(content, encoding="utf-8", newline="\n")
            command_path.chmod(0o755)
        return bundle_root, fake_bin

    def _run_generated_host_script(self, mode: str) -> tuple[subprocess.CompletedProcess[str], list[str]]:
        bundle_root, fake_bin = self._prepare_runnable_generated_script()
        docker_log = self._repo_root / "out" / "fake-docker.log"
        docker_state = self._repo_root / "out" / "fake-docker.state"
        environment = dict(os.environ)
        environment.update(
            {
                "PATH": os.pathsep.join([str(fake_bin), "/usr/bin", "/bin"]),
                "FAKE_DEPLOY_MODE": mode,
                "FAKE_DOCKER_LOG": str(docker_log),
                "FAKE_DOCKER_STATE": str(docker_state),
            }
        )
        result = subprocess.run(
            ["sh", str(bundle_root / "RUN_ON_HOST.sh")],
            cwd=bundle_root,
            env=environment,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        return result, docker_log.read_text(encoding="utf-8").splitlines()

    def test_help_runs_without_side_effects(self) -> None:
        with tempfile.TemporaryDirectory(
            prefix="inpx-web-reader-prepare-host-help-",
            dir=TEST_OUTPUT_ROOT,
        ) as temp_dir:
            result = run_entry_script_help(PREPARE_DEPLOY_BUNDLE_SCRIPT, Path(temp_dir))

        self.assertIn("Prepare a Linux Docker deployment bundle", result.stdout)
        self.assertIn("--host-source-root", result.stdout)
        self.assertIn("--host-app-root", result.stdout)

    def test_deploy_env_template_uses_the_compose_source_path_contract(self) -> None:
        env_template = DEPLOY_ENV_TEMPLATE.read_text(encoding="utf-8")

        self.assertIn("INPX_WEB_READER_SOURCE_PATH=", env_template)
        self.assertNotIn("INPX_WEB_READER_LIBRARY_PATH=", env_template)

    def test_base_compose_applies_the_selected_non_root_identity(self) -> None:
        compose_template = DEPLOY_COMPOSE_TEMPLATE.read_text(encoding="utf-8")

        self.assertIn(
            'user: "${INPX_WEB_READER_CONTAINER_UID:-10001}:${INPX_WEB_READER_CONTAINER_GID:-10001}"',
            compose_template,
        )

    def test_prepare_bundle_writes_linux_docker_files_and_generated_token(self) -> None:
        with (
            mock.patch.object(self._module, "repository_root", return_value=self._repo_root),
            mock.patch.object(
                sys,
                "argv",
                [
                    "PrepareDeployBundle.py",
                    "--host-source-root",
                    "/srv/books/flibusta",
                    "--host-app-root",
                    "/srv/inpx-web-reader",
                    "--host-port",
                    "8091",
                    "--skip-image-build",
                    "--skip-converter-download",
                ],
            ),
            contextlib.redirect_stdout(io.StringIO()),
        ):
            exit_code = self._module.main()

        self.assertEqual(exit_code, 0)
        bundle_root = self._repo_root / "out" / "deploy" / "inpx-web-reader"

        env_text = (bundle_root / ".env").read_text(encoding="utf-8")
        self.assertIn("COMPOSE_PROJECT_NAME=inpx-web-reader", env_text)
        self.assertIn("INPX_WEB_READER_IMAGE=inpx-web-reader:linux-host-local", env_text)
        self.assertIn("INPX_WEB_READER_HOST_PORT=8091", env_text)
        self.assertIn("INPX_WEB_READER_SOURCE_PATH=/srv/books/flibusta", env_text)
        self.assertIn("INPX_WEB_READER_DATA_PATH=/srv/inpx-web-reader/data", env_text)
        self.assertIn("INPX_WEB_READER_CONVERTER_PATH=\n", env_text)
        self.assertNotIn("INPX_WEB_READER_CONVERTER_HOST_PATH", env_text)
        self.assertIn("INPX_WEB_READER_MAX_SCAN_WORKERS=4", env_text)
        self.assertIn("INPX_WEB_READER_LOG_LEVEL=info", env_text)
        self.assertIn("INPX_WEB_READER_LOG_MAX_FILE_SIZE_MIB=20", env_text)
        self.assertIn("INPX_WEB_READER_LOG_MAX_ROTATED_FILES=4", env_text)
        self.assertIn(
            "INPX_WEB_READER_AUTH_TOKEN_FILE=/srv/inpx-web-reader/secrets/inpx-web-reader-auth-token.txt",
            env_text,
        )

        token = (bundle_root / "secrets" / "inpx-web-reader-auth-token.txt").read_text(encoding="utf-8").strip()
        self.assertRegex(
            token,
            r"^[23456789abcdefghjkmnpqrstuvwxyz]{4}(?:-[23456789abcdefghjkmnpqrstuvwxyz]{4}){3}$",
        )

        runner_text = (bundle_root / "RUN_ON_HOST.sh").read_text(encoding="utf-8")
        subprocess.run(["sh", "-n", str(bundle_root / "RUN_ON_HOST.sh")], check=True)
        self.assertIn("export COMPOSE_PROJECT_NAME=inpx-web-reader", runner_text)
        self.assertIn("CONVERTER_ENABLED=0", runner_text)
        self.assertNotIn(". ./.env", runner_text)
        self.assertIn("awk -F=", runner_text)
        self.assertIn("INPX_WEB_READER_IMAGE in .env contains unsupported characters", runner_text)
        self.assertIn("docker load -i", runner_text)
        self.assertIn("previous_image_id=\"$(docker inspect --format '{{.Image}}'", runner_text)
        self.assertIn('loaded_image_id="$(image_id_for_tag "$INPX_WEB_READER_IMAGE")"', runner_text)
        self.assertIn("Docker image archive did not provide image tag $INPX_WEB_READER_IMAGE", runner_text)
        self.assertIn('cleanup_old_project_containers "$container_id"', runner_text)
        self.assertIn('cleanup_superseded_images "$loaded_image_id" "$previous_image_id"', runner_text)
        self.assertIn('docker image rm -f "$candidate_image_id"', runner_text)
        self.assertIn("com.docker.compose.project=inpx-web-reader", runner_text)
        self.assertIn("org.opencontainers.image.title", runner_text)
        self.assertIn("verify_image_archive_checksum", runner_text)
        self.assertIn("rollback_previous_deployment", runner_text)
        self.assertIn("no distinct rollback image is available", runner_text)
        self.assertIn("CONTAINER_UID=10001", runner_text)
        self.assertIn("choose_container_identity", runner_text)
        self.assertIn("stat -c '%u' \"$HOST_SOURCE_ROOT\"", runner_text)
        self.assertIn('[ "$source_uid" != "0" ] && [ "$source_gid" != "0" ]', runner_text)
        self.assertIn("root container identity is forbidden", runner_text)
        self.assertIn('container_can_read_source "10001" "10001"', runner_text)
        self.assertIn("Container uid/gid selected from safe fallback", runner_text)
        self.assertIn("Container uid/gid selected from source directory owner", runner_text)
        self.assertIn("export INPX_WEB_READER_CONTAINER_UID", runner_text)
        self.assertIn("converter_enabled()", runner_text)
        self.assertIn("bundle_compose()", runner_text)
        self.assertIn("if converter_enabled; then", runner_text)
        self.assertIn("verify_converter_executable_by_container", runner_text)
        self.assertIn('chown "$CONTAINER_UID:$CONTAINER_GID" "$token_file"', runner_text)
        self.assertIn("verify_token_readable_by_container", runner_text)
        self.assertIn('--user "$CONTAINER_UID:$CONTAINER_GID"', runner_text)
        self.assertIn("bundle_compose up -d --force-recreate --remove-orphans", runner_text)
        self.assertIn("image_platform_for_tag", runner_text)
        self.assertIn('loaded_image_platform" = "linux/amd64', runner_text)
        self.assertIn("EPUB conversion is disabled for this bundle", runner_text)
        self.assertIn("Host port $HOST_PORT is already in use", runner_text)
        self.assertIn("ip route get 1.1.1.1", runner_text)
        self.assertIn("field_index", runner_text)
        self.assertIn("http://$lan_ip:$HOST_PORT/", runner_text)
        self.assertIn("RestartPolicy.Name", runner_text)
        self.assertIn("unless-stopped", runner_text)
        self.assertIn("systemctl is-enabled docker", runner_text)
        self.assertIn("State.Health.Status", runner_text)

        self.assertFalse((bundle_root / "docker-compose.converter.yml").exists())
        compose_text = (bundle_root / "docker-compose.yml").read_text(encoding="utf-8")
        self.assertIn("INPX_WEB_READER_CONTAINER_UID", compose_text)
        self.assertIn("INPX_WEB_READER_CONTAINER_GID", compose_text)

        stop_text = (bundle_root / "STOP_ON_HOST.sh").read_text(encoding="utf-8")
        subprocess.run(["sh", "-n", str(bundle_root / "STOP_ON_HOST.sh")], check=True)
        self.assertIn("HOST_APP_ROOT='/srv/inpx-web-reader'", stop_text)
        self.assertIn("export COMPOSE_PROJECT_NAME=inpx-web-reader", stop_text)
        self.assertIn("CONVERTER_ENABLED=0", stop_text)
        self.assertIn('cd "$HOST_APP_ROOT"', stop_text)
        self.assertIn(
            'if [ "$CONVERTER_ENABLED" -eq 1 ] && [ -f docker-compose.converter.yml ]; then',
            stop_text,
        )

        readme_text = (bundle_root / "README_HOST_DEPLOY.md").read_text(encoding="utf-8")
        self.assertIn("sh RUN_ON_HOST.sh", readme_text)
        self.assertIn("http://<HOSTNAME-OR-IP>:8091/", readme_text)
        self.assertIn("existing password is reused", readme_text)
        self.assertIn("EPUB conversion is disabled", readme_text)
        self.assertIn("private password", readme_text)
        self.assertIn("It never runs a global Docker prune", readme_text)

    @unittest.skipUnless(os.name == "posix", "The generated host script requires POSIX sh.")
    def test_generated_host_script_cleans_only_unused_scoped_artifacts_after_health(self) -> None:
        result, docker_calls = self._run_generated_host_script("success")

        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("rm old-stopped", docker_calls)
        self.assertIn("image rm -f sha256:old", docker_calls)
        self.assertIn("image rm -f sha256:ancient", docker_calls)
        self.assertNotIn("image rm -f sha256:protected", docker_calls)
        self.assertNotIn("image rm -f sha256:foreign", docker_calls)
        self.assertIn("because a container still references it", result.stdout)

    @unittest.skipUnless(os.name == "posix", "The generated host script requires POSIX sh.")
    def test_generated_host_script_rolls_back_before_cleanup_when_health_fails(self) -> None:
        result, docker_calls = self._run_generated_host_script("rollback")

        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("tag sha256:old inpx-web-reader:linux-host-local", docker_calls)
        self.assertIn("image rm -f sha256:new", docker_calls)
        self.assertNotIn("rm old-stopped", docker_calls)
        self.assertNotIn("image rm -f sha256:ancient", docker_calls)
        self.assertIn("Rollback completed", result.stdout)
        self.assertIn("the previous healthy image was restored", result.stdout)

    def test_prepare_bundle_enables_converter_when_download_is_requested(self) -> None:
        with (
            mock.patch.object(self._module, "repository_root", return_value=self._repo_root),
            mock.patch.object(self._module, "download_converter") as download_converter,
            mock.patch.object(
                sys,
                "argv",
                [
                    "PrepareDeployBundle.py",
                    "--host-source-root",
                    "/srv/books/flibusta",
                    "--host-app-root",
                    "/srv/inpx-web-reader",
                    "--skip-image-build",
                ],
            ),
            contextlib.redirect_stdout(io.StringIO()),
        ):
            exit_code = self._module.main()

        self.assertEqual(exit_code, 0)
        download_converter.assert_called_once()
        bundle_root = self._repo_root / "out" / "deploy" / "inpx-web-reader"
        env_text = (bundle_root / ".env").read_text(encoding="utf-8")
        self.assertIn("INPX_WEB_READER_CONVERTER_PATH=/converter/fbc", env_text)
        self.assertIn(
            "INPX_WEB_READER_CONVERTER_HOST_PATH=/srv/inpx-web-reader/converter",
            env_text,
        )
        converter_compose = (bundle_root / "docker-compose.converter.yml").read_text(encoding="utf-8")
        self.assertIn("${INPX_WEB_READER_CONVERTER_HOST_PATH", converter_compose)
        self.assertIn(":/converter:ro", converter_compose)
        self.assertIn("INPX_WEB_READER_CONVERTER_PATH: /converter/fbc", converter_compose)
        self.assertNotIn("user:", converter_compose)
        runner_text = (bundle_root / "RUN_ON_HOST.sh").read_text(encoding="utf-8")
        self.assertIn("CONVERTER_ENABLED=1", runner_text)
        readme_text = (bundle_root / "README_HOST_DEPLOY.md").read_text(encoding="utf-8")
        self.assertIn("EPUB conversion is enabled", readme_text)

    def test_prepare_bundle_uses_token_file(self) -> None:
        token_file = self._repo_root / "local-token.txt"
        token_file.write_text("example-token\n", encoding="utf-8")
        with (
            mock.patch.object(self._module, "repository_root", return_value=self._repo_root),
            mock.patch.object(
                sys,
                "argv",
                [
                    "PrepareDeployBundle.py",
                    "--host-source-root",
                    "/srv/books",
                    "--host-app-root",
                    "/srv/inpx-web-reader",
                    "--token-file",
                    str(token_file),
                    "--skip-image-build",
                    "--skip-converter-download",
                ],
            ),
            contextlib.redirect_stdout(io.StringIO()),
        ):
            exit_code = self._module.main()

        self.assertEqual(exit_code, 0)
        token = (
            self._repo_root / "out" / "deploy" / "inpx-web-reader" / "secrets" / "inpx-web-reader-auth-token.txt"
        ).read_text(encoding="utf-8")
        self.assertEqual(token, "example-token\n")

    def test_env_value_rejects_shell_substitution_bytes(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "backticks or backslashes"):
            self._module.env_value("/srv/books/`touch pwned`")

        with self.assertRaisesRegex(RuntimeError, "backticks or backslashes"):
            self._module.env_value(r"/srv/books\library")

    @unittest.skipUnless(os.name == "posix", "POSIX mode bits are meaningful only on POSIX hosts.")
    def test_prepare_bundle_writes_owner_only_token_file(self) -> None:
        with (
            mock.patch.object(self._module, "repository_root", return_value=self._repo_root),
            mock.patch.object(
                sys,
                "argv",
                [
                    "PrepareDeployBundle.py",
                    "--host-source-root",
                    "/srv/books",
                    "--host-app-root",
                    "/srv/inpx-web-reader",
                    "--skip-image-build",
                    "--skip-converter-download",
                ],
            ),
            contextlib.redirect_stdout(io.StringIO()),
        ):
            exit_code = self._module.main()

        self.assertEqual(exit_code, 0)
        token_path = (
            self._repo_root / "out" / "deploy" / "inpx-web-reader" / "secrets" / "inpx-web-reader-auth-token.txt"
        )
        self.assertEqual(stat.S_IMODE(token_path.stat().st_mode), 0o600)
        self.assertEqual(stat.S_IMODE(token_path.parent.stat().st_mode), 0o700)

    def test_prepare_bundle_reuses_existing_generated_token_by_default(self) -> None:
        existing_token_path = (
            self._repo_root / "out" / "deploy" / "inpx-web-reader" / "secrets" / "inpx-web-reader-auth-token.txt"
        )
        existing_token_path.parent.mkdir(parents=True)
        existing_token_path.write_text("stable-token\n", encoding="utf-8")

        with (
            mock.patch.object(self._module, "repository_root", return_value=self._repo_root),
            mock.patch.object(
                sys,
                "argv",
                [
                    "PrepareDeployBundle.py",
                    "--host-source-root",
                    "/srv/books",
                    "--host-app-root",
                    "/srv/inpx-web-reader",
                    "--skip-image-build",
                    "--skip-converter-download",
                ],
            ),
            contextlib.redirect_stdout(io.StringIO()),
        ):
            exit_code = self._module.main()

        self.assertEqual(exit_code, 0)
        self.assertEqual(existing_token_path.read_text(encoding="utf-8"), "stable-token\n")

    def test_prepare_bundle_rejects_multiline_token_file(self) -> None:
        token_file = self._repo_root / "local-token.txt"
        token_file.write_text("first\nsecond\n", encoding="utf-8")
        with (
            mock.patch.object(self._module, "repository_root", return_value=self._repo_root),
            mock.patch.object(
                sys,
                "argv",
                [
                    "PrepareDeployBundle.py",
                    "--host-source-root",
                    "/srv/books",
                    "--host-app-root",
                    "/srv/inpx-web-reader",
                    "--token-file",
                    str(token_file),
                    "--skip-image-build",
                    "--skip-converter-download",
                ],
            ),
            contextlib.redirect_stderr(io.StringIO()) as stderr,
        ):
            exit_code = self._module.main()

        self.assertEqual(exit_code, 1)
        self.assertIn("--token-file must contain exactly one access-password line", stderr.getvalue())

    def test_prepare_bundle_rejects_short_access_password(self) -> None:
        token_file = self._repo_root / "local-token.txt"
        token_file.write_text("too-short\n", encoding="utf-8")
        with (
            mock.patch.object(self._module, "repository_root", return_value=self._repo_root),
            mock.patch.object(
                sys,
                "argv",
                [
                    "PrepareDeployBundle.py",
                    "--host-source-root",
                    "/srv/books",
                    "--host-app-root",
                    "/srv/inpx-web-reader",
                    "--token-file",
                    str(token_file),
                    "--skip-image-build",
                    "--skip-converter-download",
                ],
            ),
            contextlib.redirect_stderr(io.StringIO()) as stderr,
        ):
            exit_code = self._module.main()

        self.assertEqual(exit_code, 1)
        self.assertIn("at least 12 characters", stderr.getvalue())

    def test_prepare_bundle_rejects_passwords_unsafe_for_http_headers(self) -> None:
        for password in ("пароль-доступа", "access password"):
            with self.subTest(password=password):
                token_file = self._repo_root / "local-token.txt"
                token_file.write_text(password + "\n", encoding="utf-8")
                with self.assertRaisesRegex(RuntimeError, "printable ASCII characters without spaces"):
                    self._module.read_one_line_token_file(token_file, "--token-file")

    def test_prepare_bundle_rejects_non_absolute_host_paths(self) -> None:
        with (
            mock.patch.object(self._module, "repository_root", return_value=self._repo_root),
            mock.patch.object(
                sys,
                "argv",
                [
                    "PrepareDeployBundle.py",
                    "--host-source-root",
                    "relative/books",
                    "--host-app-root",
                    "/srv/inpx-web-reader",
                    "--skip-image-build",
                    "--skip-converter-download",
                ],
            ),
            contextlib.redirect_stderr(io.StringIO()) as stderr,
        ):
            exit_code = self._module.main()

        self.assertEqual(exit_code, 1)
        self.assertIn("must be an absolute target Linux host path", stderr.getvalue())

    def test_prepare_bundle_rejects_root_parent_and_overlapping_host_paths(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "must not be the filesystem root"):
            self._module.validate_absolute_host_path("/", "--host-app-root")
        with self.assertRaisesRegex(RuntimeError, "parent-directory components"):
            self._module.validate_absolute_host_path("/srv/books/../docker", "--host-app-root")
        with self.assertRaisesRegex(RuntimeError, "backticks, or backslashes"):
            self._module.validate_absolute_host_path("/srv/books/`unsafe`", "--host-app-root")
        with self.assertRaisesRegex(RuntimeError, "must not overlap"):
            self._module.validate_non_overlapping_host_roots(
                "/srv/books/inpx",
                "/srv/books/inpx/deployment",
            )
        with self.assertRaisesRegex(RuntimeError, "must not overlap"):
            self._module.validate_non_overlapping_host_roots(
                "/srv/inpx-web-reader/source",
                "/srv/inpx-web-reader",
            )

    def test_release_asset_helpers_select_linux_amd64_converter(self) -> None:
        release = {
            "tag_name": "v1.3.8",
            "assets": [
                {"name": "fbc-source.zip", "browser_download_url": "https://example.invalid/source.zip"},
                {
                    "name": "fbc-linux-amd64.zip",
                    "browser_download_url": "https://example.invalid/linux.zip",
                    "digest": "sha256:abc123",
                },
            ],
        }

        asset = self._module.find_release_asset(release, "fbc-linux-amd64.zip")

        self.assertEqual(asset["browser_download_url"], "https://example.invalid/linux.zip")
        self.assertEqual(self._module.digest_sha256(asset), "abc123")

    def test_converter_release_asset_without_digest_is_rejected(self) -> None:
        asset = {
            "name": "fbc-linux-amd64.zip",
            "browser_download_url": "https://example.invalid/linux.zip",
        }

        with self.assertRaisesRegex(RuntimeError, "does not provide a sha256 digest"):
            self._module.digest_sha256(asset)

    def test_download_file_rejects_checksum_mismatch(self) -> None:
        target = self._repo_root / "out" / "download.zip"
        response = io.BytesIO(b"payload")

        with mock.patch.object(self._module.urllib.request, "urlopen", return_value=response):
            with self.assertRaisesRegex(RuntimeError, "checksum mismatch"):
                self._module.download_file(
                    "https://example.invalid/fbc-linux-amd64.zip",
                    target,
                    "0" * 64,
                )

    def test_extract_converter_binary_rejects_corrupt_or_ambiguous_archives(self) -> None:
        corrupt_archive = self._repo_root / "corrupt.zip"
        corrupt_archive.write_bytes(b"not a zip")
        with self.assertRaises(Exception):
            self._module.extract_converter_binary(corrupt_archive, self._repo_root / "converter-corrupt")

        missing_archive = self._repo_root / "missing-fbc.zip"
        with self._module.zipfile.ZipFile(missing_archive, "w") as archive:
            archive.writestr("README.txt", "missing")
        with self.assertRaisesRegex(RuntimeError, "does not contain an fbc executable"):
            self._module.extract_converter_binary(missing_archive, self._repo_root / "converter-missing")

        duplicate_archive = self._repo_root / "duplicate-fbc.zip"
        with self._module.zipfile.ZipFile(duplicate_archive, "w") as archive:
            archive.writestr("bin/fbc", "one")
            archive.writestr("nested/fbc", "two")
        with self.assertRaisesRegex(RuntimeError, "multiple fbc executables"):
            self._module.extract_converter_binary(duplicate_archive, self._repo_root / "converter-duplicate")

    def test_build_image_reports_missing_docker_cli(self) -> None:
        with (
            mock.patch.object(
                self._module,
                "ensure_docker_engine",
                side_effect=RuntimeError("Docker CLI was not found on PATH."),
            ),
            mock.patch.object(self._module.sys, "platform", "linux"),
        ):
            with self.assertRaisesRegex(RuntimeError, "Docker CLI was not found"):
                self._module.build_and_save_image(
                    self._repo_root,
                    self._repo_root / "out" / "deploy" / "inpx-web-reader",
                    "inpx-web-reader:test",
                    "linux/amd64",
                    2,
                )

    def test_build_image_reports_unreachable_docker_engine(self) -> None:
        with (
            mock.patch.object(
                self._module,
                "ensure_docker_engine",
                side_effect=RuntimeError("Docker CLI is installed, but the Docker engine is not reachable."),
            ),
            mock.patch.object(self._module.sys, "platform", "linux"),
        ):
            with self.assertRaisesRegex(RuntimeError, "Docker engine is not reachable"):
                self._module.build_and_save_image(
                    self._repo_root,
                    self._repo_root / "out" / "deploy" / "inpx-web-reader",
                    "inpx-web-reader:test",
                    "linux/amd64",
                    2,
                )

    def test_build_image_uses_linux_docker_cli_with_platform(self) -> None:
        commands: list[list[str]] = []
        bundle_root = self._repo_root / "out" / "deploy" / "inpx-web-reader"
        with (
            mock.patch.object(self._module.sys, "platform", "linux"),
            mock.patch.object(self._module, "ensure_docker_engine") as ensure_docker,
            mock.patch.object(
                self._module.subprocess,
                "run",
                return_value=mock.Mock(stdout="linux/amd64\n"),
            ) as inspect_image,
            mock.patch.object(
                self._module,
                "run",
                side_effect=lambda command, *_args, **_kwargs: commands.append(command),
            ),
        ):
            self._module.build_and_save_image(
                self._repo_root,
                bundle_root,
                "inpx-web-reader:test",
                "linux/amd64",
                2,
            )

        ensure_docker.assert_called_once()
        inspect_image.assert_called_once()
        self.assertEqual(
            commands,
            [
                [
                    "docker",
                    "build",
                    "--platform",
                    "linux/amd64",
                    "--build-arg",
                    "INPX_WEB_READER_BUILD_JOBS=2",
                    "-f",
                    str(self._repo_root / self._module.SERVER_DOCKERFILE),
                    "-t",
                    "inpx-web-reader:test",
                    ".",
                ],
                [
                    "docker",
                    "save",
                    "-o",
                    str(bundle_root / self._module.IMAGE_ARCHIVE_NAME),
                    "inpx-web-reader:test",
                ],
            ],
        )

    def test_build_image_rejects_non_amd64_runtime_image(self) -> None:
        bundle_root = self._repo_root / "out" / "deploy" / "inpx-web-reader"
        with (
            mock.patch.object(self._module, "ensure_docker_engine"),
            mock.patch.object(self._module, "run"),
            mock.patch.object(
                self._module.subprocess,
                "run",
                return_value=mock.Mock(stdout="linux/arm64\n"),
            ),
            self.assertRaisesRegex(RuntimeError, "expected linux/amd64"),
        ):
            self._module.build_and_save_image(
                self._repo_root,
                bundle_root,
                "inpx-web-reader:test",
                "linux/amd64",
                2,
            )

    def test_save_existing_image_verifies_platform_and_writes_archive(self) -> None:
        bundle_root = self._repo_root / "out" / "deploy" / "inpx-web-reader"
        commands: list[list[str]] = []
        with (
            mock.patch.object(self._module, "ensure_docker_engine"),
            mock.patch.object(self._module, "verify_image_platform") as verify_platform,
            mock.patch.object(
                self._module,
                "run",
                side_effect=lambda command, *_args, **_kwargs: commands.append(command),
            ),
        ):
            self._module.save_existing_image(
                self._repo_root,
                bundle_root,
                "inpx-web-reader:verified",
            )

        verify_platform.assert_called_once()
        self.assertEqual(
            commands,
            [
                [
                    "docker",
                    "save",
                    "-o",
                    str(bundle_root / self._module.IMAGE_ARCHIVE_NAME),
                    "inpx-web-reader:verified",
                ]
            ],
        )

    def test_bundle_manifest_records_versioned_image_and_archive_checksum(self) -> None:
        bundle_root = self._repo_root / "out" / "deploy" / "inpx-web-reader"
        bundle_root.mkdir(parents=True)
        (self._repo_root / "VERSION.txt").write_text("1.2.0\n", encoding="utf-8")
        archive = bundle_root / self._module.IMAGE_ARCHIVE_NAME
        archive.write_bytes(b"verified-image-archive")

        with mock.patch.object(
            self._module,
            "inspect_image_property",
            side_effect=["sha256:image-id", "linux/amd64"],
        ):
            self._module.write_bundle_manifest(
                self._repo_root,
                bundle_root,
                "inpx-web-reader:1.2.0-abcdef123456",
                "abcdef1234567890",
                True,
                False,
            )

        manifest = self._module.json.loads(
            (bundle_root / self._module.BUNDLE_MANIFEST_NAME).read_text(encoding="utf-8")
        )
        expected_sha256 = self._module.hashlib.sha256(archive.read_bytes()).hexdigest()
        self.assertEqual(manifest["productVersion"], "1.2.0")
        self.assertEqual(manifest["gitCommit"], "abcdef1234567890")
        self.assertTrue(manifest["gitDirty"])
        self.assertEqual(manifest["imagePlatform"], "linux/amd64")
        self.assertEqual(manifest["imageArchiveSha256"], expected_sha256)
        self.assertEqual(
            (bundle_root / self._module.IMAGE_ARCHIVE_CHECKSUM_NAME).read_text(encoding="utf-8"),
            f"{expected_sha256}  {self._module.IMAGE_ARCHIVE_NAME}\n",
        )

    def test_image_build_points_non_linux_users_to_remote_runner(self) -> None:
        with mock.patch.object(self._module.sys, "platform", "non-linux"):
            with self.assertRaisesRegex(RuntimeError, "RunRemoteLinux.py bundle"):
                self._module.build_and_save_image(
                    self._repo_root,
                    self._repo_root / "out" / "deploy" / "inpx-web-reader",
                    "inpx-web-reader:test",
                    "linux/amd64",
                    2,
                )

    def test_skip_image_build_still_rejects_non_linux_host(self) -> None:
        with (
            mock.patch.object(self._module.sys, "platform", "non-linux"),
            mock.patch.object(
                sys,
                "argv",
                [
                    "PrepareDeployBundle.py",
                    "--host-source-root",
                    "/srv/books",
                    "--host-app-root",
                    "/srv/inpx-web-reader",
                    "--skip-image-build",
                    "--skip-converter-download",
                ],
            ),
            contextlib.redirect_stderr(io.StringIO()) as stderr,
        ):
            exit_code = self._module.main()

        self.assertEqual(exit_code, 1)
        self.assertIn("RunRemoteLinux.py bundle", stderr.getvalue())


if __name__ == "__main__":
    unittest.main()
