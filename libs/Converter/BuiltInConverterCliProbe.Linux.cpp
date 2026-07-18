#include "Converter/BuiltInConverterCliProbe.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <csignal>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "Converter/LinuxProcessUtils.hpp"
#include "Foundation/UnicodeConversion.hpp"

namespace InpxWebReader::ConverterValidation {
namespace {

constexpr std::size_t GMaxProbeOutputBytes = 64ull * 1024ull;
constexpr auto GShutdownWaitTimeout = std::chrono::milliseconds{2000};

class CScopedFd final
{
public:
    explicit CScopedFd(const int fd = -1)
        : m_fd(fd)
    {
    }

    ~CScopedFd() noexcept
    {
        if (m_fd >= 0)
        {
            close(m_fd);
        }
    }

    CScopedFd(const CScopedFd&) = delete;
    CScopedFd& operator=(const CScopedFd&) = delete;

    CScopedFd(CScopedFd&& other) noexcept
        : m_fd(std::exchange(other.m_fd, -1))
    {
    }

    CScopedFd& operator=(CScopedFd&& other) noexcept
    {
        if (this != &other)
        {
            if (m_fd >= 0)
            {
                close(m_fd);
            }

            m_fd = std::exchange(other.m_fd, -1);
        }

        return *this;
    }

    [[nodiscard]] int Get() const noexcept
    {
        return m_fd;
    }

    [[nodiscard]] int Release() noexcept
    {
        return std::exchange(m_fd, -1);
    }

private:
    int m_fd = -1;
};

class CScopedChildProcess final
{
public:
    explicit CScopedChildProcess(const pid_t processId = -1)
        : m_processId(processId)
    {
    }

    ~CScopedChildProcess() noexcept
    {
        if (m_processId > 0)
        {
            TerminateAndWait();
        }
    }

    CScopedChildProcess(const CScopedChildProcess&) = delete;
    CScopedChildProcess& operator=(const CScopedChildProcess&) = delete;

    [[nodiscard]] pid_t Get() const noexcept
    {
        return m_processId;
    }

    void MarkLeaderReaped() noexcept
    {
        m_leaderReaped = true;
    }

    void TerminateAndRelease() noexcept
    {
        TerminateAndWait();
        m_processId = -1;
    }

private:
    static void KillProcessGroupOrProcess(
        const pid_t processId,
        const int signalNumber) noexcept
    {
        if (kill(-processId, signalNumber) != 0 && errno == ESRCH)
        {
            kill(processId, signalNumber);
        }
    }

    [[nodiscard]] static bool ProcessGroupExists(const pid_t processId) noexcept
    {
        if (kill(-processId, 0) == 0)
        {
            return true;
        }
        return errno == EPERM;
    }

    static void ReapProcessGroupNoHang(
        const pid_t processId,
        bool& leaderReaped) noexcept
    {
        for (;;)
        {
            int status = 0;
            const pid_t waitResult = ConverterLinux::WaitForProcessNoInterrupt(
                -processId,
                status,
                WNOHANG);
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

    void TerminateAndWait() noexcept
    {
        KillProcessGroupOrProcess(m_processId, SIGTERM);
        const auto terminateDeadline = std::chrono::steady_clock::now()
            + GShutdownWaitTimeout;
        while (std::chrono::steady_clock::now() < terminateDeadline)
        {
            ReapProcessGroupNoHang(m_processId, m_leaderReaped);
            if (!ProcessGroupExists(m_processId))
            {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{25});
        }

        KillProcessGroupOrProcess(m_processId, SIGKILL);
        if (!m_leaderReaped)
        {
            int status = 0;
            static_cast<void>(ConverterLinux::WaitForProcessNoInterrupt(
                m_processId,
                status,
                0));
            m_leaderReaped = true;
        }
        const auto reapDeadline = std::chrono::steady_clock::now()
            + GShutdownWaitTimeout;
        while (std::chrono::steady_clock::now() < reapDeadline)
        {
            ReapProcessGroupNoHang(m_processId, m_leaderReaped);
            if (!ProcessGroupExists(m_processId))
            {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        ReapProcessGroupNoHang(m_processId, m_leaderReaped);
    }

    pid_t m_processId = -1;
    bool m_leaderReaped = false;
};

enum class EPipeDrainResult
{
    Drained,
    EndOfFile,
    OutputLimitExceeded,
    Failed
};

[[nodiscard]] EPipeDrainResult DrainPipe(const int fd, std::string& output)
{
    std::array<char, 4096> buffer{};

    while (true)
    {
        const ssize_t bytesRead = read(fd, buffer.data(), buffer.size());
        if (bytesRead > 0)
        {
            const auto byteCount = static_cast<std::size_t>(bytesRead);
            if (byteCount > GMaxProbeOutputBytes - output.size())
            {
                return EPipeDrainResult::OutputLimitExceeded;
            }
            output.append(buffer.data(), byteCount);
            continue;
        }

        if (bytesRead == 0)
        {
            return EPipeDrainResult::EndOfFile;
        }

        if (errno == EINTR)
        {
            continue;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            return EPipeDrainResult::Drained;
        }

        return EPipeDrainResult::Failed;
    }
}

[[nodiscard]] InpxWebReader::ConverterRuntime::SExternalConverterExecutableIdentity
BuildExecutableIdentity(
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

[[nodiscard]] std::vector<std::string> BuildArgumentStorage(
    const std::filesystem::path& executablePath)
{
    std::vector<std::string> arguments;
    arguments.reserve(2);
    arguments.push_back(InpxWebReader::Unicode::PathToUtf8(executablePath));
    arguments.emplace_back("--help");
    return arguments;
}

} // namespace

std::optional<std::string> ProbeBuiltInConverterHelpOutput(
    const std::filesystem::path& executablePath,
    const std::chrono::milliseconds timeout,
    const std::optional<InpxWebReader::ConverterRuntime::SExternalConverterExecutableIdentity>&
        expectedIdentity,
    const SBuiltInConverterCliProbeHooks& hooks)
{
    if (timeout.count() <= 0 || prctl(PR_SET_CHILD_SUBREAPER, 1) != 0)
    {
        return std::nullopt;
    }
    if (hooks.BeforeExecutableOpen)
    {
        hooks.BeforeExecutableOpen();
    }

    const std::string executablePathUtf8 = InpxWebReader::Unicode::PathToUtf8(
        executablePath);
    CScopedFd executableDescriptor{
        open(executablePathUtf8.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW)};
    if (executableDescriptor.Get() < 0)
    {
        return std::nullopt;
    }

    struct stat executableStatus = {};
    if (fstat(executableDescriptor.Get(), &executableStatus) != 0
        || !S_ISREG(executableStatus.st_mode))
    {
        return std::nullopt;
    }
    if (expectedIdentity.has_value()
        && (!expectedIdentity->IsValid()
            || executablePath.lexically_normal()
                != expectedIdentity->CanonicalPath.lexically_normal()
            || BuildExecutableIdentity(
                   expectedIdentity->CanonicalPath,
                   executableStatus)
                != *expectedIdentity))
    {
        return std::nullopt;
    }

    int pipeFds[2]{-1, -1};
    if (pipe2(pipeFds, O_CLOEXEC) != 0)
    {
        return std::nullopt;
    }

    CScopedFd readPipe(pipeFds[0]);
    CScopedFd writePipe(pipeFds[1]);

    if (fcntl(readPipe.Get(), F_SETFL, O_NONBLOCK) != 0)
    {
        return std::nullopt;
    }

    std::vector<std::string> arguments = BuildArgumentStorage(executablePath);
    std::vector<char*> argv = ConverterLinux::BuildArgv(arguments);
    std::vector<std::string> environment = ConverterLinux::BuildSanitizedEnvironment();
    std::vector<char*> envp = ConverterLinux::BuildEnvp(environment);
    std::error_code workingDirectoryError;
    const std::filesystem::path workingDirectory = executablePath.has_parent_path()
        ? executablePath.parent_path()
        : std::filesystem::current_path(workingDirectoryError);
    if (workingDirectoryError)
    {
        return std::nullopt;
    }
    const std::string workingDirectoryUtf8 = InpxWebReader::Unicode::PathToUtf8(
        workingDirectory);
    const long fileDescriptorCloseLimit = ConverterLinux::ResolveFileDescriptorCloseLimit();

    const pid_t processId = fork();
    if (processId < 0)
    {
        return std::nullopt;
    }

    if (processId == 0)
    {
        if (setpgid(0, 0) != 0)
        {
            _exit(127);
        }

        if (dup2(writePipe.Get(), STDOUT_FILENO) < 0)
        {
            _exit(127);
        }

        if (dup2(writePipe.Get(), STDERR_FILENO) < 0)
        {
            _exit(127);
        }

        if (chdir(workingDirectoryUtf8.c_str()) != 0)
        {
            _exit(127);
        }

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

        // Keep the post-fork child path limited to async-signal-safe syscalls.
        ConverterLinux::CloseInheritedFileDescriptors(
            fileDescriptorCloseLimit,
            executableDescriptor.Get());
        fexecve(executableDescriptor.Get(), argv.data(), envp.data());
        _exit(127);
    }

    CScopedChildProcess child(processId);
    if (setpgid(processId, processId) != 0 && errno != EACCES && errno != ESRCH)
    {
        return std::nullopt;
    }
    close(writePipe.Release());

    std::string output;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    int processStatus = 0;

    while (true)
    {
        const pid_t waitResult = ConverterLinux::WaitForProcessNoInterrupt(processId, processStatus, WNOHANG);
        if (waitResult == processId)
        {
            child.MarkLeaderReaped();
            break;
        }

        if (waitResult < 0)
        {
            return std::nullopt;
        }

        if (std::chrono::steady_clock::now() >= deadline)
        {
            return std::nullopt;
        }

        const auto now = std::chrono::steady_clock::now();
        const auto remaining = now < deadline
            ? std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
            : std::chrono::milliseconds{0};
        const auto pollWait = std::chrono::milliseconds{std::min<std::int64_t>(remaining.count(), 100)};
        pollfd descriptor = {
            .fd = readPipe.Get(),
            .events = static_cast<short>(POLLIN | POLLHUP),
            .revents = 0
        };

        const int pollResult = poll(&descriptor, 1, static_cast<int>(pollWait.count()));
        if (pollResult > 0)
        {
            const EPipeDrainResult drainResult = DrainPipe(readPipe.Get(), output);
            if (drainResult == EPipeDrainResult::OutputLimitExceeded
                || drainResult == EPipeDrainResult::Failed)
            {
                return std::nullopt;
            }
        }
        else if (pollResult < 0 && errno != EINTR)
        {
            return std::nullopt;
        }
    }

    child.TerminateAndRelease();
    const EPipeDrainResult finalDrainResult = DrainPipe(readPipe.Get(), output);
    if (finalDrainResult == EPipeDrainResult::OutputLimitExceeded
        || finalDrainResult == EPipeDrainResult::Failed)
    {
        return std::nullopt;
    }

    if (!WIFEXITED(processStatus) || WEXITSTATUS(processStatus) != 0)
    {
        return std::nullopt;
    }

    return output;
}

} // namespace InpxWebReader::ConverterValidation
