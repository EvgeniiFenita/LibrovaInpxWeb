#include "Converter/LinuxExternalConverterProcessRunner.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <csignal>
#include <fcntl.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "Converter/LinuxProcessUtils.hpp"
#include "Foundation/UnicodeConversion.hpp"

namespace InpxWebReader::ConverterRuntime {
namespace {

constexpr auto GShutdownWaitTimeout = std::chrono::milliseconds{2000};

[[nodiscard]] std::vector<std::string> BuildArgumentStorage(
    const InpxWebReader::ConverterCommand::SResolvedConverterCommand& command)
{
    std::vector<std::string> arguments;
    arguments.reserve(command.Arguments.size() + 1);
    arguments.push_back(InpxWebReader::Unicode::PathToUtf8(command.ExecutablePath));
    arguments.insert(arguments.end(), command.Arguments.begin(), command.Arguments.end());
    return arguments;
}

[[nodiscard]] std::uint32_t ExitCodeFromStatus(const int status) noexcept
{
    if (WIFEXITED(status))
    {
        return static_cast<std::uint32_t>(WEXITSTATUS(status));
    }

    if (WIFSIGNALED(status))
    {
        return static_cast<std::uint32_t>(128 + WTERMSIG(status));
    }

    return 1;
}

void KillProcessGroupOrProcess(const pid_t processId, const int signalNumber) noexcept
{
    if (kill(-processId, signalNumber) != 0 && errno == ESRCH)
    {
        kill(processId, signalNumber);
    }
}

[[nodiscard]] bool ProcessGroupExists(const pid_t processId) noexcept
{
    if (kill(-processId, 0) == 0)
    {
        return true;
    }
    return errno == EPERM;
}

void ReapProcessGroupNoHang(const pid_t processId, bool& leaderReaped) noexcept
{
    for (;;)
    {
        int status = 0;
        const pid_t waitResult = ConverterLinux::WaitForProcessNoInterrupt(-processId, status, WNOHANG);
        if (waitResult <= 0)
        {
            return;
        }
        if (waitResult == processId)
        {
            leaderReaped = true;
        }
    }
}

class CScopedFileDescriptor final
{
public:
    explicit CScopedFileDescriptor(const int value = -1) noexcept
        : m_value(value)
    {
    }

    ~CScopedFileDescriptor()
    {
        if (m_value >= 0)
        {
            close(m_value);
        }
    }

    CScopedFileDescriptor(const CScopedFileDescriptor&) = delete;
    CScopedFileDescriptor& operator=(const CScopedFileDescriptor&) = delete;
    CScopedFileDescriptor(CScopedFileDescriptor&& other) noexcept
        : m_value(std::exchange(other.m_value, -1))
    {
    }
    CScopedFileDescriptor& operator=(CScopedFileDescriptor&& other) noexcept
    {
        if (this != &other)
        {
            if (m_value >= 0)
            {
                close(m_value);
            }
            m_value = std::exchange(other.m_value, -1);
        }
        return *this;
    }

    [[nodiscard]] int Get() const noexcept { return m_value; }

private:
    int m_value = -1;
};

[[nodiscard]] SExternalConverterExecutableIdentity BuildExecutableIdentity(
    const std::filesystem::path& canonicalPath,
    const struct stat& status)
{
    return {
        .CanonicalPath = canonicalPath,
        .Device = static_cast<std::uint64_t>(status.st_dev),
        .Inode = static_cast<std::uint64_t>(status.st_ino),
        .SizeBytes = static_cast<std::uint64_t>(status.st_size),
        .ModifiedSeconds = status.st_mtim.tv_sec,
        .ModifiedNanoseconds = status.st_mtim.tv_nsec,
        .ChangedSeconds = status.st_ctim.tv_sec,
        .ChangedNanoseconds = status.st_ctim.tv_nsec
    };
}

void TerminateAndWait(const pid_t processId, bool leaderReaped = false) noexcept
{
    if (processId <= 0)
    {
        return;
    }

    KillProcessGroupOrProcess(processId, SIGTERM);
    const auto deadline = std::chrono::steady_clock::now() + GShutdownWaitTimeout;

    while (std::chrono::steady_clock::now() < deadline)
    {
        ReapProcessGroupNoHang(processId, leaderReaped);
        if (!ProcessGroupExists(processId))
        {
            return;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds{25});
    }

    KillProcessGroupOrProcess(processId, SIGKILL);
    if (!leaderReaped)
    {
        int status = 0;
        (void)ConverterLinux::WaitForProcessNoInterrupt(processId, status, 0);
        leaderReaped = true;
    }
    const auto reapDeadline = std::chrono::steady_clock::now() + GShutdownWaitTimeout;
    while (std::chrono::steady_clock::now() < reapDeadline)
    {
        ReapProcessGroupNoHang(processId, leaderReaped);
        if (!ProcessGroupExists(processId))
        {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    ReapProcessGroupNoHang(processId, leaderReaped);
}

class CScopedChildProcess final
{
public:
    explicit CScopedChildProcess(const pid_t processId = -1) noexcept
        : m_processId(processId)
    {
    }

    ~CScopedChildProcess() noexcept
    {
        if (m_processId > 0)
        {
            TerminateAndWait(m_processId);
        }
    }

    CScopedChildProcess(const CScopedChildProcess&) = delete;
    CScopedChildProcess& operator=(const CScopedChildProcess&) = delete;

    [[nodiscard]] pid_t Get() const noexcept { return m_processId; }

    [[nodiscard]] pid_t Release() noexcept { return std::exchange(m_processId, -1); }

private:
    pid_t m_processId = -1;
};

} // namespace

SExternalConverterExecutableIdentity ReadExternalConverterExecutableIdentity(
    const std::filesystem::path& executablePath)
{
    std::error_code errorCode;
    const auto canonicalPath = std::filesystem::canonical(executablePath, errorCode);
    if (errorCode)
    {
        throw std::runtime_error("External converter executable could not be canonicalized.");
    }

    struct stat status = {};
    const std::string pathUtf8 = InpxWebReader::Unicode::PathToUtf8(canonicalPath);
    if (stat(pathUtf8.c_str(), &status) != 0 || !S_ISREG(status.st_mode))
    {
        throw std::runtime_error("External converter executable identity could not be read.");
    }
    return BuildExecutableIdentity(canonicalPath, status);
}

SExternalConverterProcessResult CLinuxExternalConverterProcessRunner::Run(
    const SExternalConverterProcessRequest& request,
    InpxWebReader::Domain::IProgressSink& progressSink,
    const std::stop_token stopToken) const
{
    if (!request.Command.IsValid())
    {
        throw std::invalid_argument("External converter command must be valid before execution.");
    }
    if (request.PollInterval.count() <= 0 || request.Timeout.count() <= 0)
    {
        throw std::invalid_argument("External converter process timing settings must be positive.");
    }
    if (prctl(PR_SET_CHILD_SUBREAPER, 1) != 0)
    {
        throw std::runtime_error("Failed to configure converter descendant reaping.");
    }

    std::vector<std::string> arguments = BuildArgumentStorage(request.Command);
    std::vector<char*> argv = ConverterLinux::BuildArgv(arguments);
    std::vector<std::string> environment = ConverterLinux::BuildSanitizedEnvironment();
    std::vector<char*> envp = ConverterLinux::BuildEnvp(environment);
    const std::string workingDirectory = InpxWebReader::Unicode::PathToUtf8(request.WorkingDirectory);
    const long fileDescriptorCloseLimit = ConverterLinux::ResolveFileDescriptorCloseLimit();

    CScopedFileDescriptor executableDescriptor;
    if (request.ExpectedExecutableIdentity.has_value())
    {
        const auto& expectedIdentity = *request.ExpectedExecutableIdentity;
        if (!expectedIdentity.IsValid()
            || request.Command.ExecutablePath != expectedIdentity.CanonicalPath)
        {
            throw std::runtime_error("External converter executable identity is invalid.");
        }

        const std::string executablePathUtf8 = InpxWebReader::Unicode::PathToUtf8(
            expectedIdentity.CanonicalPath);
        executableDescriptor = CScopedFileDescriptor{
            open(executablePathUtf8.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW)};
        if (executableDescriptor.Get() < 0)
        {
            throw std::runtime_error("External converter executable could not be opened safely.");
        }

        struct stat status = {};
        if (fstat(executableDescriptor.Get(), &status) != 0
            || BuildExecutableIdentity(expectedIdentity.CanonicalPath, status) != expectedIdentity)
        {
            throw std::runtime_error("External converter executable changed after validation.");
        }
    }

    CScopedChildProcess process(fork());
    if (process.Get() < 0)
    {
        throw std::runtime_error("Failed to create external converter process.");
    }

    if (process.Get() == 0)
    {
        if (setpgid(0, 0) != 0)
        {
            _exit(127);
        }

        if (!workingDirectory.empty() && chdir(workingDirectory.c_str()) != 0)
        {
            _exit(127);
        }

        if (executableDescriptor.Get() >= 0)
        {
            const int descriptorFlags = fcntl(executableDescriptor.Get(), F_GETFD);
            if (descriptorFlags < 0
                || fcntl(
                       executableDescriptor.Get(),
                       F_SETFD,
                       descriptorFlags & ~FD_CLOEXEC)
                    != 0)
            {
                _exit(127);
            }
        }

        // Keep the post-fork child path limited to async-signal-safe syscalls.
        ConverterLinux::CloseInheritedFileDescriptors(
            fileDescriptorCloseLimit,
            executableDescriptor.Get());
        if (executableDescriptor.Get() >= 0)
        {
            fexecve(executableDescriptor.Get(), argv.data(), envp.data());
        }
        else
        {
            execve(argv.front(), argv.data(), envp.data());
        }
        _exit(127);
    }

    setpgid(process.Get(), process.Get());

    const pid_t processId = process.Get();
    const auto deadline = std::chrono::steady_clock::now() + request.Timeout;

    if (request.ProcessHooks.AfterProcessCreatedBeforeJob)
    {
        request.ProcessHooks.AfterProcessCreatedBeforeJob(
            static_cast<std::uint32_t>(processId));
    }

    bool leaderReaped = false;
    std::uint32_t leaderExitCode = 0;

    while (true)
    {
        if (stopToken.stop_requested() || progressSink.IsCancellationRequested())
        {
            TerminateAndWait(processId, leaderReaped);
            (void)process.Release();
            return {
                .WasCancelled = true
            };
        }

        if (!leaderReaped)
        {
            int status = 0;
            const pid_t waitResult = ConverterLinux::WaitForProcessNoInterrupt(
                processId,
                status,
                WNOHANG);
            if (waitResult == processId)
            {
                leaderReaped = true;
                leaderExitCode = ExitCodeFromStatus(status);
                (void)process.Release();
            }
            else if (waitResult < 0)
            {
                throw std::runtime_error("Failed while waiting for external converter process.");
            }
        }

        if (leaderReaped)
        {
            ReapProcessGroupNoHang(processId, leaderReaped);
        }
        if (leaderReaped && !ProcessGroupExists(processId))
        {
            return {
                .ExitCode = leaderExitCode
            };
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
        {
            TerminateAndWait(processId, leaderReaped);
            (void)process.Release();
            return {
                .ExitCode = 0,
                .WasCancelled = false,
                .WasTimedOut = true
            };
        }

        const auto remaining = deadline - now;
        const auto pollInterval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            request.PollInterval);
        std::this_thread::sleep_for((std::min)(pollInterval, remaining));
    }
}

} // namespace InpxWebReader::ConverterRuntime
