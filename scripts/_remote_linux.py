from __future__ import annotations

from contextlib import contextmanager
from dataclasses import dataclass
import os
import platform
import re
import shlex
import shutil
import subprocess
import time
import uuid
from pathlib import Path, PurePosixPath
from typing import Iterator


DEFAULT_ENV_FILE = Path(".env")
DEFAULT_SSH_PORT = 22
REMOTE_SOURCE_DIR_NAME = "source"
REMOTE_VCPKG_DIR_NAME = "vcpkg"
REMOTE_JOB_DIR_NAME = "jobs"
REMOTE_JOB_NAME_PATTERN = re.compile(r"^[A-Za-z0-9_.-]+$")
REMOTE_JOB_START_MARKER = "__INPX_WEB_READER_JOB_START__"
REMOTE_JOB_STATE_MARKER = "__INPX_WEB_READER_JOB_STATE__"
REMOTE_ACTIVE_JOB_MARKER = "__INPX_WEB_READER_ACTIVE_JOB__"
SYNC_SOURCE_DIR_PREFIX = "sync-source-"


@dataclass(frozen=True)
class CRemoteLinuxConfig:
    host: str
    user: str
    workdir: PurePosixPath
    port: int = DEFAULT_SSH_PORT
    password: str = ""

    @property
    def target(self) -> str:
        return f"{self.user}@{self.host}"

    @property
    def source_root(self) -> PurePosixPath:
        return self.workdir / REMOTE_SOURCE_DIR_NAME

    @property
    def vcpkg_root(self) -> PurePosixPath:
        return self.workdir / REMOTE_VCPKG_DIR_NAME

def _unquote_env_value(value: str) -> str:
    stripped = value.strip()
    if len(stripped) >= 2 and stripped[0] == stripped[-1] and stripped[0] in {"'", '"'}:
        return stripped[1:-1]
    return stripped


def load_env_values(envFile: Path) -> dict[str, str]:
    if not envFile.is_file():
        raise RuntimeError(
            f"Remote build configuration was not found: {envFile}. "
            "Copy .env.example to .env and fill in the build-machine settings."
        )

    values: dict[str, str] = {}
    for line_number, raw_line in enumerate(envFile.read_text(encoding="utf-8").splitlines(), start=1):
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("export "):
            line = line[7:].lstrip()
        if "=" not in line:
            raise RuntimeError(f"Invalid .env entry at {envFile}:{line_number}: expected KEY=VALUE.")
        key, value = line.split("=", 1)
        key = key.strip()
        if not key:
            raise RuntimeError(f"Invalid .env entry at {envFile}:{line_number}: key is empty.")
        values[key] = _unquote_env_value(value)
    return values


def _required_env(values: dict[str, str], name: str) -> str:
    value = values.get(name, "").strip()
    if not value:
        raise RuntimeError(f"{name} must be set in the remote build .env file.")
    return value


def load_remote_config(envFile: Path, repoRoot: Path) -> CRemoteLinuxConfig:
    resolved_env_file = envFile if envFile.is_absolute() else repoRoot / envFile
    values = load_env_values(resolved_env_file)
    allowed_keys = {
        "INPX_WEB_READER_BUILD_HOST",
        "INPX_WEB_READER_BUILD_USER",
        "INPX_WEB_READER_BUILD_PASSWORD",
        "INPX_WEB_READER_BUILD_WORKDIR",
    }
    unknown_keys = sorted(
        key for key in values if key.startswith("INPX_WEB_READER_BUILD_") and key not in allowed_keys
    )
    if unknown_keys:
        raise RuntimeError(
            "Unsupported remote-build .env fields: " + ", ".join(unknown_keys) + ". "
            "Only host, user, password, and workdir are supported."
        )

    workdir_text = _required_env(values, "INPX_WEB_READER_BUILD_WORKDIR")
    workdir = PurePosixPath(workdir_text)
    if not workdir.is_absolute():
        raise RuntimeError("INPX_WEB_READER_BUILD_WORKDIR must be an absolute Linux path.")

    return CRemoteLinuxConfig(
        host=_required_env(values, "INPX_WEB_READER_BUILD_HOST"),
        user=_required_env(values, "INPX_WEB_READER_BUILD_USER"),
        workdir=workdir,
        password=values.get("INPX_WEB_READER_BUILD_PASSWORD", ""),
    )


class CRemoteLinuxTransport:
    def __init__(self, config: CRemoteLinuxConfig, repoRoot: Path) -> None:
        self.config = config
        self.repo_root = repoRoot.resolve()

    def CheckLocalPrerequisites(self) -> None:
        missing: list[str] = []
        if self._FindSshExecutable() is None:
            missing.append("ssh")
        if shutil.which("rsync") is None:
            missing.append("rsync")
        if shutil.which("git") is None:
            missing.append("git")
        if missing:
            raise RuntimeError(f"Missing local remote-build tools: {', '.join(missing)}.")

    def LocalRsyncPath(self, path: Path) -> str:
        resolved = path.resolve()
        try:
            relative = resolved.relative_to(self.repo_root)
        except ValueError:
            return str(resolved)
        return "./" if relative == Path(".") else f"./{relative.as_posix()}"

    def RunSsh(self, remoteCommand: str, *, inputText: str | None = None) -> None:
        self.CheckLocalPrerequisites()
        command = [*self._SshCommand(), self.config.target, remoteCommand]
        self._RunAuthenticated(command, inputText=inputText)

    def RunSshCapture(self, remoteCommand: str) -> str:
        self.CheckLocalPrerequisites()
        command = [*self._SshCommand(), self.config.target, remoteCommand]
        completed = self._RunAuthenticated(command, captureOutput=True)
        return completed.stdout

    def ActiveSshJob(self) -> str | None:
        job_root = self.config.workdir / REMOTE_JOB_DIR_NAME / "pipeline"
        quoted_root = shlex.quote(str(job_root))
        command = "\n".join(
            [
                f"job_root={quoted_root}",
                self._RemoteJobIsRunningFunction(),
                "if job_is_running; then",
                f"  printf '{REMOTE_ACTIVE_JOB_MARKER} %s\\n' \"$(cat \"$job_root/name\" 2>/dev/null || true)\"",
                "else",
                f"  printf '{REMOTE_ACTIVE_JOB_MARKER} -\\n'",
                "fi",
            ]
        )
        output = self.RunSshCapture(f"bash -lc {shlex.quote(command)}")
        marker = f"{REMOTE_ACTIVE_JOB_MARKER} "
        for line in reversed(output.splitlines()):
            if line.startswith(marker):
                job_name = line[len(marker) :].strip()
                if job_name == "-":
                    return None
                if not REMOTE_JOB_NAME_PATTERN.fullmatch(job_name):
                    raise RuntimeError("Remote build machine returned an invalid active job name.")
                return job_name
        raise RuntimeError("Remote build machine returned an invalid active-job response.")

    def RunSshJob(
        self,
        remoteCommand: str,
        jobName: str,
        *,
        pollIntervalSeconds: float = 5.0,
        resumeExisting: bool = False,
        startCleanupPaths: tuple[PurePosixPath, ...] = (),
    ) -> None:
        if not REMOTE_JOB_NAME_PATTERN.fullmatch(jobName):
            raise ValueError(f"Invalid remote job name: {jobName}")

        try:
            start_output = self.RunSshCapture(
                self._StartRemoteJobCommand(
                    remoteCommand,
                    jobName,
                    resumeExisting=resumeExisting,
                    startCleanupPaths=startCleanupPaths,
                )
            )
        except subprocess.CalledProcessError as error:
            start_output = error.stdout or ""
            if not self._MarkerFields(start_output, REMOTE_JOB_START_MARKER):
                raise
        start_state = self._MarkerFields(start_output, REMOTE_JOB_START_MARKER)
        if not start_state or start_state[0] not in {"started", "resumed"}:
            raise RuntimeError("Remote build machine returned an invalid job-start response.")
        if start_state[0] == "resumed":
            print(f"==> Reconnected to active remote {jobName} job.", flush=True)

        offset = 0
        while True:
            output = self.RunSshCapture(self._PollRemoteJobCommand(offset))
            marker_text = f"\n{REMOTE_JOB_STATE_MARKER} "
            log_text, separator, state_text = output.rpartition(marker_text)
            if not separator:
                raise RuntimeError("Remote build machine returned an invalid job-status response.")
            if log_text:
                print(log_text, end="" if log_text.endswith("\n") else "\n", flush=True)

            fields = state_text.strip().split()
            if len(fields) != 3:
                raise RuntimeError("Remote build machine returned incomplete job status.")
            offset = int(fields[0])
            state = fields[1]
            if state == "running":
                time.sleep(pollIntervalSeconds)
                continue
            if state == "complete":
                exit_code = int(fields[2])
                if exit_code != 0:
                    raise subprocess.CalledProcessError(exit_code, remoteCommand)
                return
            raise RuntimeError(
                f"Remote {jobName} job stopped without recording an exit status. "
                "Run the command again to start a clean job."
            )

    def RsyncTo(self, localPath: Path, remotePath: PurePosixPath, *, delete: bool = False) -> None:
        self.CheckLocalPrerequisites()
        local_value = self.LocalRsyncPath(localPath)
        if localPath.is_dir() and not local_value.endswith("/"):
            local_value += "/"
        command = ["rsync", "-az"]
        if delete:
            command.append("--delete")
        command.extend(["-e", shlex.join(self._SshCommand())])
        command.extend([local_value, f"{self.config.target}:{shlex.quote(str(remotePath))}/"])
        self._RunAuthenticated(command)

    def RsyncRepository(self) -> None:
        sync_root = self._PrepareRepositorySyncRoot()
        try:
            job_dir = self.config.workdir / REMOTE_JOB_DIR_NAME
            self.RunSsh(
                "mkdir -p "
                + shlex.quote(str(self.config.source_root))
                + " "
                + shlex.quote(str(job_dir))
            )
            local_source = self.LocalRsyncPath(sync_root)
            if not local_source.endswith("/"):
                local_source += "/"
            command = [
                "rsync",
                "-az",
                "--delete",
                # Remote build output is deliberately retained between source snapshots.
                "--filter",
                "P /out/***",
                "-e",
                shlex.join(self._SshCommand()),
                "--rsync-path",
                self._SourceLockedRsyncPath(),
                local_source,
                f"{self.config.target}:{shlex.quote(str(self.config.source_root))}/",
            ]
            self._RunAuthenticated(command)
        finally:
            self._RemoveRepositorySyncRoot(sync_root)

    def _GitFileList(self, *arguments: str) -> tuple[Path, ...]:
        completed = self._RunClient(
            ["git", "ls-files", "-z", *arguments],
            capture_output=True,
        )
        return tuple(Path(value) for value in completed.stdout.split("\0") if value)

    def _PrepareRepositorySyncRoot(self) -> Path:
        untracked_paths = self._GitFileList("--others", "--exclude-standard")
        if untracked_paths:
            preview = ", ".join(path.as_posix() for path in untracked_paths[:5])
            if len(untracked_paths) > 5:
                preview += f", and {len(untracked_paths) - 5} more"
            raise RuntimeError(
                "Remote source sync only transfers Git-indexed files. "
                f"Add or ignore untracked paths before verification: {preview}"
            )

        tracked_paths = self._GitFileList("--cached")
        sync_root = self.repo_root / "out" / "remote-linux" / f"{SYNC_SOURCE_DIR_PREFIX}{uuid.uuid4().hex}"
        sync_root.mkdir(parents=True, exist_ok=False)
        try:
            for relative_path in tracked_paths:
                if relative_path.is_absolute() or ".." in relative_path.parts:
                    raise RuntimeError(f"Git returned an unsafe tracked path: {relative_path}")

                source = self.repo_root / relative_path
                if not source.exists() and not source.is_symlink():
                    continue

                destination = sync_root / relative_path
                destination.parent.mkdir(parents=True, exist_ok=True)
                if source.is_symlink():
                    destination.symlink_to(os.readlink(source))
                elif source.is_file():
                    shutil.copy2(source, destination)
                else:
                    raise RuntimeError(f"Unsupported Git-indexed path type: {relative_path}")
        except Exception:
            self._RemoveRepositorySyncRoot(sync_root)
            raise
        return sync_root

    def _RemoveRepositorySyncRoot(self, syncRoot: Path) -> None:
        expected_parent = (self.repo_root / "out" / "remote-linux").resolve(strict=False)
        resolved = syncRoot.resolve(strict=False)
        if resolved.parent != expected_parent or not resolved.name.startswith(SYNC_SOURCE_DIR_PREFIX):
            raise RuntimeError(f"Refusing to remove an invalid repository sync root: {syncRoot}")
        if syncRoot.exists():
            shutil.rmtree(syncRoot)

    def RsyncFrom(
        self,
        remotePath: PurePosixPath,
        localPath: Path,
        *,
        delete: bool = False,
        lockSource: bool = False,
    ) -> None:
        self.CheckLocalPrerequisites()
        localPath.mkdir(parents=True, exist_ok=True)
        command = ["rsync", "-az"]
        if delete:
            command.append("--delete")
        command.extend(["-e", shlex.join(self._SshCommand())])
        if lockSource:
            command.extend(["--rsync-path", self._SourceLockedRsyncPath()])
        command.extend(
            [
                f"{self.config.target}:{shlex.quote(str(remotePath))}/",
                self.LocalRsyncPath(localPath).rstrip("/") + "/",
            ]
        )
        self._RunAuthenticated(command)

    def _SourceLockedRsyncPath(self) -> str:
        job_dir = self.config.workdir / REMOTE_JOB_DIR_NAME
        return " ".join(
            [
                "flock -n",
                shlex.quote(str(job_dir / "start.lock")),
                "flock -n",
                shlex.quote(str(self.config.workdir / "source.lock")),
                "rsync",
            ]
        )

    def _SshCommand(self) -> list[str]:
        executable = self._FindSshExecutable() or "ssh"
        command = [
            executable,
            "-p",
            str(self.config.port),
            "-o",
            "StrictHostKeyChecking=accept-new",
            "-o",
            "ServerAliveInterval=15",
            "-o",
            "ServerAliveCountMax=3",
        ]
        return command

    def _FindSshExecutable(self) -> str | None:
        rsync_executable = shutil.which("rsync")
        if rsync_executable is not None:
            suffix = ".exe" if platform.system() == "Windows" else ""
            sibling = Path(rsync_executable).with_name(f"ssh{suffix}")
            if sibling.is_file():
                return str(sibling)
        return shutil.which("ssh")

    def _StartRemoteJobCommand(
        self,
        remoteCommand: str,
        jobName: str,
        *,
        resumeExisting: bool = False,
        startCleanupPaths: tuple[PurePosixPath, ...] = (),
    ) -> str:
        job_root = self.config.workdir / REMOTE_JOB_DIR_NAME / "pipeline"
        start_lock = self.config.workdir / REMOTE_JOB_DIR_NAME / "start.lock"
        source_lock = self.config.workdir / "source.lock"
        quoted_root = shlex.quote(str(job_root))
        quoted_name = shlex.quote(jobName)
        exit_code_path = shlex.quote(str(job_root / "exit-code"))
        cleanup_command = ""
        if startCleanupPaths:
            cleanup_command = "rm -f -- " + " ".join(
                shlex.quote(str(path)) for path in startCleanupPaths
            )
        runner = "\n".join(
            [
                "exec 9>&-",
                "set +e",
                remoteCommand,
                "status=$?",
                f"printf '%s\\n' \"$status\" > {exit_code_path}",
                "exit \"$status\"",
            ]
        )
        script = "\n".join(
            [
                "set -eu",
                f"job_root={quoted_root}",
                "mkdir -p \"$job_root\"",
                f"exec 9>{shlex.quote(str(start_lock))}",
                "flock 9",
                self._RemoteJobIsRunningFunction(),
                "if job_is_running; then",
                "  active_name=$(cat \"$job_root/name\" 2>/dev/null || true)",
                f"  if [ \"$active_name\" != {quoted_name} ]; then",
                f"    {cleanup_command}" if cleanup_command else "    :",
                "    echo \"Remote pipeline job '$active_name' is already running.\" >&2",
                "    exit 73",
                "  fi",
                f"  {cleanup_command}" if cleanup_command else "  :",
                f"  printf '{REMOTE_JOB_START_MARKER} resumed\\n'",
                "  exit 0",
                "fi",
                "if [ " + ("1" if resumeExisting else "0") + " -eq 1 ]; then",
                "  completed_name=$(cat \"$job_root/name\" 2>/dev/null || true)",
                "  if [ \"$completed_name\" = " + quoted_name + " ] && [ -f \"$job_root/exit-code\" ]; then",
                f"    {cleanup_command}" if cleanup_command else "    :",
                f"    printf '{REMOTE_JOB_START_MARKER} resumed\\n'",
                "    exit 0",
                "  fi",
                f"  {cleanup_command}" if cleanup_command else "  :",
                "  echo 'The remote job finished or disappeared before reconnection.' >&2",
                "  exit 74",
                "fi",
                "rm -f \"$job_root/job.log\" \"$job_root/pid\" \"$job_root/pid-start\" \"$job_root/exit-code\"",
                f"printf '%s\\n' {quoted_name} > \"$job_root/name\"",
                f"exec 8>{shlex.quote(str(source_lock))}",
                "flock -s 8",
                (
                    f"nohup setsid bash -lc {shlex.quote(runner)} > \"$job_root/job.log\" "
                    "2>&1 < /dev/null &"
                ),
                "printf '%s\\n' \"$!\" > \"$job_root/pid\"",
                "awk '{print $22}' \"/proc/$!/stat\" > \"$job_root/pid-start\"",
                f"printf '{REMOTE_JOB_START_MARKER} started\\n'",
            ]
        )
        return f"bash -lc {shlex.quote(script)}"

    def _PollRemoteJobCommand(self, offset: int) -> str:
        job_root = self.config.workdir / REMOTE_JOB_DIR_NAME / "pipeline"
        quoted_root = shlex.quote(str(job_root))
        script = "\n".join(
            [
                f"job_root={quoted_root}",
                self._RemoteJobIsRunningFunction(),
                "size=0",
                "if [ -f \"$job_root/job.log\" ]; then size=$(stat -c %s \"$job_root/job.log\"); fi",
                f"if [ \"$size\" -gt {offset} ]; then tail -c +{offset + 1} \"$job_root/job.log\"; fi",
                "if [ -f \"$job_root/exit-code\" ]; then",
                "  state=complete",
                "  status=$(cat \"$job_root/exit-code\")",
                "elif job_is_running; then",
                "  state=running",
                "  status=-",
                "else",
                "  state=lost",
                "  status=-",
                "fi",
                f"printf '\\n{REMOTE_JOB_STATE_MARKER} %s %s %s\\n' \"$size\" \"$state\" \"$status\"",
            ]
        )
        return f"bash -lc {shlex.quote(script)}"

    @staticmethod
    def _RemoteJobIsRunningFunction() -> str:
        return "\n".join(
            [
                "job_is_running()",
                "{",
                "  [ ! -f \"$job_root/exit-code\" ] || return 1",
                "  [ -f \"$job_root/pid\" ] && [ -f \"$job_root/pid-start\" ] || return 1",
                "  pid=$(cat \"$job_root/pid\")",
                "  expected_start=$(cat \"$job_root/pid-start\")",
                "  kill -0 \"$pid\" 2>/dev/null || return 1",
                "  [ -r \"/proc/$pid/stat\" ] || return 1",
                "  current_start=$(awk '{print $22}' \"/proc/$pid/stat\")",
                "  [ \"$current_start\" = \"$expected_start\" ]",
                "}",
            ]
        )

    @staticmethod
    def _MarkerFields(output: str, marker: str) -> list[str]:
        prefix = marker + " "
        for line in reversed(output.splitlines()):
            if line.startswith(prefix):
                return line[len(prefix) :].split()
        return []

    def _RunClient(
        self,
        command: list[str],
        *,
        check: bool = True,
        capture_output: bool = False,
        input_text: str | None = None,
        environment: dict[str, str] | None = None,
    ) -> subprocess.CompletedProcess[str]:
        executable = shutil.which(command[0], path=(environment or os.environ).get("PATH"))
        if executable is None:
            raise RuntimeError(f"Remote-build client was not found: {command[0]}")
        resolved_command = [executable, *command[1:]]
        return subprocess.run(
            resolved_command,
            cwd=self.repo_root,
            env=environment,
            input=input_text,
            stdout=subprocess.PIPE if capture_output else None,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
            check=check,
        )

    def _RunAuthenticated(
        self,
        command: list[str],
        *,
        inputText: str | None = None,
        captureOutput: bool = False,
    ) -> subprocess.CompletedProcess[str]:
        with self._AuthenticationEnvironment() as (environment, command_prefix):
            return self._RunClient(
                [*command_prefix, *command],
                capture_output=captureOutput,
                input_text=inputText,
                environment=environment,
            )

    @contextmanager
    def _AuthenticationEnvironment(self) -> Iterator[tuple[dict[str, str], list[str]]]:
        environment = dict(os.environ)
        if not self.config.password:
            yield environment, []
            return

        auth_root = self.repo_root / "out" / "remote-linux" / "auth"
        auth_root.mkdir(parents=True, exist_ok=True)
        auth_root.chmod(0o700)
        session_root = auth_root / uuid.uuid4().hex
        session_root.mkdir(mode=0o700)
        password_path = session_root / "password"
        windows_host = platform.system() == "Windows"
        askpass_path = session_root / ("askpass.cmd" if windows_host else "askpass.sh")

        try:
            password_path.write_text(self.config.password + "\n", encoding="utf-8")
            password_path.chmod(0o600)

            password_client_path = str(password_path.resolve())
            if windows_host:
                askpass_path.write_text(
                    f'@echo off\r\ntype "{password_client_path}"\r\n',
                    encoding="utf-8",
                )
            else:
                askpass_path.write_text(
                    "#!/bin/sh\n"
                    f"cat {shlex.quote(password_client_path)}\n",
                    encoding="utf-8",
                    newline="\n",
                )
            askpass_path.chmod(0o700)
            askpass_client_path = str(askpass_path.resolve())
            environment.update(
                {
                    "SSH_ASKPASS": askpass_client_path,
                    "SSH_ASKPASS_REQUIRE": "force",
                    "DISPLAY": environment.get("DISPLAY", "inpx-web-reader:0"),
                }
            )
            yield environment, []
        finally:
            password_path.unlink(missing_ok=True)
            askpass_path.unlink(missing_ok=True)
            session_root.rmdir()


def remote_shell_command(
    config: CRemoteLinuxConfig,
    command: list[str],
    *,
    cleanupPaths: tuple[PurePosixPath, ...] = (),
) -> str:
    lines = ["set -eu"]
    if cleanupPaths:
        cleanup_command = "rm -f -- " + " ".join(shlex.quote(str(path)) for path in cleanupPaths)
        lines.extend([f"cleanup() {{ {cleanup_command}; }}", "trap cleanup EXIT"])
    lines.extend(
        [
            f"cd {shlex.quote(str(config.source_root))}",
            f"export VCPKG_ROOT={shlex.quote(str(config.vcpkg_root))}",
            shlex.join(command),
        ]
    )
    script = "\n".join(lines)
    return f"bash -lc {shlex.quote(script)}"
