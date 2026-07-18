#include <catch2/catch_test_macros.hpp>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "Converter/ConverterCommandBuilder.hpp"
#include "Converter/ExternalBookConverter.hpp"
#include "Converter/LinuxExternalConverterProcessRunner.hpp"
#include "TestExternalBookConverterProcessRunner.hpp"
#include "TestWorkspace.hpp"

namespace {

class CScopedEnvironmentVariable final
{
public:
    CScopedEnvironmentVariable(std::string name, const std::string_view value)
        : m_name(std::move(name))
    {
        if (const char* const previous = std::getenv(m_name.c_str()))
        {
            m_previousValue = previous;
        }
        if (setenv(m_name.c_str(), std::string{value}.c_str(), 1) != 0)
        {
            throw std::runtime_error("Could not set test environment variable.");
        }
    }

    ~CScopedEnvironmentVariable()
    {
        if (m_previousValue.has_value())
        {
            static_cast<void>(setenv(m_name.c_str(), m_previousValue->c_str(), 1));
        }
        else
        {
            static_cast<void>(unsetenv(m_name.c_str()));
        }
    }

    CScopedEnvironmentVariable(const CScopedEnvironmentVariable&) = delete;
    CScopedEnvironmentVariable& operator=(const CScopedEnvironmentVariable&) = delete;

private:
    std::string m_name;
    std::optional<std::string> m_previousValue;
};

class CScopedFileDescriptor final
{
public:
    explicit CScopedFileDescriptor(const int value)
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

    [[nodiscard]] int Get() const noexcept
    {
        return m_value;
    }

private:
    int m_value = -1;
};

class CTestProgressSink final : public InpxWebReader::Domain::IProgressSink
{
public:
    void ReportValue(const int percent, std::string_view message) override
    {
        LastPercent = percent;
        LastMessage.assign(message);
    }

    [[nodiscard]] bool IsCancellationRequested() const override
    {
        return CancellationRequested;
    }

    int LastPercent = 0;
    std::string LastMessage;
    bool CancellationRequested = false;
};

void WriteTextFile(const std::filesystem::path& path, const std::string_view text)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output << text;
}

std::string ReadTextFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return std::string{
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{}
    };
}

std::optional<pid_t> TryReadProcessId(const std::filesystem::path& path)
{
    if (!std::filesystem::exists(path))
    {
        return std::nullopt;
    }

    const std::string text = ReadTextFile(path);
    if (text.empty())
    {
        return std::nullopt;
    }

    return static_cast<pid_t>(std::stol(text));
}

std::optional<pid_t> WaitForProcessId(
    const std::filesystem::path& path,
    const std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        const auto processId = TryReadProcessId(path);
        if (processId.has_value())
        {
            return processId;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds{25});
    }

    return TryReadProcessId(path);
}

bool IsProcessAlive(const pid_t processId)
{
    if (processId <= 0)
    {
        return false;
    }

    const std::filesystem::path linuxProcessStatPath =
        std::filesystem::path{"/proc"} / std::to_string(processId) / "stat";
    std::ifstream linuxProcessStat(linuxProcessStatPath);
    std::string statText;
    if (std::getline(linuxProcessStat, statText))
    {
        const auto processNameEnd = statText.rfind(") ");
        if (processNameEnd != std::string::npos && processNameEnd + 2 < statText.size())
        {
            const char processState = statText[processNameEnd + 2];
            if (processState == 'Z')
            {
                return false;
            }
        }
    }

    return kill(processId, 0) == 0;
}

[[nodiscard]] bool HasLinuxProcessEntry(const pid_t processId)
{
    return std::filesystem::exists(
        std::filesystem::path{"/proc"} / std::to_string(processId));
}

bool WaitUntilProcessStops(
    const pid_t processId,
    const std::chrono::milliseconds timeout = std::chrono::seconds{10})
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (!IsProcessAlive(processId))
        {
            return true;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }

    return !IsProcessAlive(processId);
}

class CScopedProcessCleanup final
{
public:
    explicit CScopedProcessCleanup(const pid_t processId)
        : m_processId(processId)
    {
    }

    ~CScopedProcessCleanup()
    {
        if (m_processId <= 0)
        {
            return;
        }

        (void)kill(m_processId, SIGKILL);
        int status = 0;
        (void)waitpid(m_processId, &status, 0);
    }

    void Dismiss() noexcept
    {
        m_processId = -1;
    }

private:
    pid_t m_processId = -1;
};

[[nodiscard]] bool WasAlreadyReapedByRunner(const pid_t processId)
{
    int status = 0;
    errno = 0;
    const pid_t waitResult = waitpid(processId, &status, WNOHANG);
    return waitResult == -1 && errno == ECHILD;
}

InpxWebReader::ConverterCommand::SResolvedConverterCommand CreateResolvedShellCommand(
    const std::filesystem::path& scriptPath,
    const std::vector<std::string>& arguments)
{
    return {
        .ExecutablePath = "/bin/sh",
        .Arguments = [&scriptPath, &arguments]()
        {
            std::vector<std::string> resolvedArguments{scriptPath.generic_string()};
            resolvedArguments.insert(resolvedArguments.end(), arguments.begin(), arguments.end());
            return resolvedArguments;
        }(),
        .OutputMode = InpxWebReader::ConverterCommand::EConverterOutputMode::ExactDestinationPath,
        .ExpectedOutputPath = {},
        .ExpectedOutputDirectory = {}
    };
}

InpxWebReader::ConverterCommand::SConverterCommandProfile CreateShellProfile(
    const std::filesystem::path& scriptPath,
    const InpxWebReader::ConverterCommand::EConverterOutputMode outputMode,
    const std::vector<std::string>& trailingArguments)
{
    InpxWebReader::ConverterCommand::SConverterCommandProfile profile = {
        .ExecutablePath = "/bin/sh",
        .ArgumentTemplate = {scriptPath.generic_string()},
        .OutputMode = outputMode
    };

    profile.ArgumentTemplate.insert(
        profile.ArgumentTemplate.end(),
        trailingArguments.begin(),
        trailingArguments.end());
    return profile;
}

} // namespace

TEST_CASE("External book converter supports exact destination path converters on Linux", "[converter-runtime]")
{
    CTestWorkspace sandbox("inpx-web-reader-external-converter-linux-exact");
    const std::filesystem::path scriptPath = sandbox.GetPath() / "copy-converter.sh";
    const std::filesystem::path sourcePath = sandbox.GetPath() / "source.fb2";
    const std::filesystem::path destinationPath = sandbox.GetPath() / "output" / "book.epub";

    WriteTextFile(
        scriptPath,
        "#!/bin/sh\n"
        "source_path=\"$1\"\n"
        "destination_path=\"$2\"\n"
        "mkdir -p \"$(dirname \"$destination_path\")\"\n"
        "cp \"$source_path\" \"$destination_path\"\n");
    WriteTextFile(sourcePath, "converted-content");

    const InpxWebReader::ConverterRuntime::CExternalBookConverter converter({
        .CommandProfile = CreateShellProfile(
            scriptPath,
            InpxWebReader::ConverterCommand::EConverterOutputMode::ExactDestinationPath,
            {"{source}", "{destination}"})
    });
    CTestProgressSink progressSink;

    const InpxWebReader::Domain::SConversionResult result = converter.Convert({
        .SourcePath = sourcePath,
        .DestinationPath = destinationPath,
        .SourceFormat = InpxWebReader::Domain::EBookFormat::Fb2,
        .DestinationFormat = InpxWebReader::Domain::EBookFormat::Epub
    }, progressSink, {});

    REQUIRE(result.IsSuccess());
    REQUIRE(result.Status == InpxWebReader::Domain::EConversionStatus::Succeeded);
    REQUIRE(result.OutputPath == destinationPath);
    REQUIRE(std::filesystem::exists(destinationPath));
    REQUIRE(ReadTextFile(destinationPath) == "converted-content");
    REQUIRE(progressSink.LastPercent == 100);
}

TEST_CASE("External book converter reports cancellation on Linux", "[converter-runtime]")
{
    CTestWorkspace sandbox("inpx-web-reader-external-converter-linux-cancel");
    const std::filesystem::path scriptPath = sandbox.GetPath() / "sleep-converter.sh";
    const std::filesystem::path sourcePath = sandbox.GetPath() / "source.fb2";
    const std::filesystem::path destinationPath = sandbox.GetPath() / "output" / "book.epub";

    WriteTextFile(
        scriptPath,
        "#!/bin/sh\n"
        "while true; do\n"
        "  sleep 1\n"
        "done\n");
    WriteTextFile(sourcePath, "content");

    const InpxWebReader::ConverterRuntime::CExternalBookConverter converter({
        .CommandProfile = CreateShellProfile(
            scriptPath,
            InpxWebReader::ConverterCommand::EConverterOutputMode::ExactDestinationPath,
            {"{source}", "{destination}"})
    });
    CTestProgressSink progressSink;
    std::stop_source stopSource;
    std::jthread cancellationThread([&stopSource]() {
        std::this_thread::sleep_for(std::chrono::milliseconds{150});
        stopSource.request_stop();
    });

    const InpxWebReader::Domain::SConversionResult result = converter.Convert({
        .SourcePath = sourcePath,
        .DestinationPath = destinationPath,
        .SourceFormat = InpxWebReader::Domain::EBookFormat::Fb2,
        .DestinationFormat = InpxWebReader::Domain::EBookFormat::Epub
    }, progressSink, stopSource.get_token());

    REQUIRE(result.Status == InpxWebReader::Domain::EConversionStatus::Cancelled);
    REQUIRE_FALSE(std::filesystem::exists(destinationPath));
}

TEST_CASE("External book converter relocates generated single-file output discovered in the output directory", "[converter-runtime]")
{
    CTestWorkspace sandbox("inpx-web-reader-external-converter-generated-output");
    const std::filesystem::path executablePath = sandbox.GetPath() / "fake-converter";
    const std::filesystem::path sourcePath = sandbox.GetPath() / "source.fb2";
    const std::filesystem::path destinationPath = sandbox.GetPath() / "output" / "book.epub";
    const std::filesystem::path workingDirectory = sandbox.GetPath() / "converter-work";
    WriteTextFile(sourcePath, "source-content");

    CFakeProcessRunner processRunner;
    processRunner.OnRun = [](const InpxWebReader::ConverterRuntime::SExternalConverterProcessRequest& request) {
        const auto producedPath = request.Command.ExpectedOutputDirectory / "generated-by-runner.epub";
        WriteTextFile(producedPath, "converted-content");
    };

    InpxWebReader::ConverterCommand::SConverterCommandProfile profile = {
        .ExecutablePath = executablePath,
        .ArgumentTemplate = {"--source", "{source}", "--destination-dir", "{destination_dir}"},
        .OutputMode = InpxWebReader::ConverterCommand::EConverterOutputMode::SingleFileInDestinationDirectory
    };

    const InpxWebReader::ConverterRuntime::CExternalBookConverter converter({
        .CommandProfile = profile,
        .WorkingDirectory = workingDirectory,
        .ProcessRunner = &processRunner
    });
    CTestProgressSink progressSink;

    const InpxWebReader::Domain::SConversionResult result = converter.Convert({
        .SourcePath = sourcePath,
        .DestinationPath = destinationPath,
        .SourceFormat = InpxWebReader::Domain::EBookFormat::Fb2,
        .DestinationFormat = InpxWebReader::Domain::EBookFormat::Epub
    }, progressSink, {});

    REQUIRE(result.IsSuccess());
    REQUIRE(result.OutputPath == destinationPath);
    REQUIRE(std::filesystem::exists(destinationPath));
    REQUIRE(ReadTextFile(destinationPath) == "converted-content");
    REQUIRE(processRunner.LastRequest.has_value());
    REQUIRE_FALSE(std::filesystem::exists(
        processRunner.LastRequest->Command.ExpectedOutputDirectory / "generated-by-runner.epub"));
    REQUIRE_FALSE(std::filesystem::exists(processRunner.LastRequest->WorkingDirectory));
}

TEST_CASE("External book converter cancellation stops Linux child process group", "[converter-runtime]")
{
    CTestWorkspace sandbox("inpx-web-reader-external-converter-linux-cancel-group");
    const std::filesystem::path scriptPath = sandbox.GetPath() / "spawn-child-converter.sh";
    const std::filesystem::path childPidPath = sandbox.GetPath() / "child.pid";
    const std::filesystem::path sourcePath = sandbox.GetPath() / "source.fb2";
    const std::filesystem::path destinationPath = sandbox.GetPath() / "output" / "book.epub";

    WriteTextFile(
        scriptPath,
        "#!/bin/sh\n"
        "(sleep 30) &\n"
        "echo $! > \"$3\"\n"
        "while true; do\n"
        "  sleep 1\n"
        "done\n");
    WriteTextFile(sourcePath, "content");

    const InpxWebReader::ConverterRuntime::CExternalBookConverter converter({
        .CommandProfile = CreateShellProfile(
            scriptPath,
            InpxWebReader::ConverterCommand::EConverterOutputMode::ExactDestinationPath,
            {"{source}", "{destination}", childPidPath.generic_string()})
    });
    CTestProgressSink progressSink;
    std::stop_source stopSource;
    std::jthread cancellationThread([&stopSource, &childPidPath]() {
        (void)WaitForProcessId(childPidPath, std::chrono::seconds{10});
        stopSource.request_stop();
    });

    const InpxWebReader::Domain::SConversionResult result = converter.Convert({
        .SourcePath = sourcePath,
        .DestinationPath = destinationPath,
        .SourceFormat = InpxWebReader::Domain::EBookFormat::Fb2,
        .DestinationFormat = InpxWebReader::Domain::EBookFormat::Epub
    }, progressSink, stopSource.get_token());

    REQUIRE(result.Status == InpxWebReader::Domain::EConversionStatus::Cancelled);
    const std::optional<pid_t> childProcessId = WaitForProcessId(childPidPath, std::chrono::seconds{10});
    REQUIRE(childProcessId.has_value());
    CScopedProcessCleanup childCleanup(*childProcessId);
    REQUIRE(WaitUntilProcessStops(*childProcessId));
    REQUIRE(WasAlreadyReapedByRunner(*childProcessId));
    REQUIRE_FALSE(HasLinuxProcessEntry(*childProcessId));
    childCleanup.Dismiss();
}

TEST_CASE("External book converter timeout kills and reaps a TERM-resistant process group", "[converter-runtime]")
{
    CTestWorkspace sandbox("inpx-web-reader-external-converter-linux-timeout-group");
    const std::filesystem::path scriptPath = sandbox.GetPath() / "ignore-term-converter.sh";
    const std::filesystem::path childPidPath = sandbox.GetPath() / "child.pid";
    const std::filesystem::path sourcePath = sandbox.GetPath() / "source.fb2";
    const std::filesystem::path destinationPath = sandbox.GetPath() / "output" / "book.epub";

    WriteTextFile(
        scriptPath,
        "#!/bin/sh\n"
        "trap '' TERM\n"
        "(trap '' TERM; while true; do sleep 1; done) &\n"
        "echo $! > \"$3\"\n"
        "while true; do sleep 1; done\n");
    WriteTextFile(sourcePath, "content");

    const InpxWebReader::ConverterRuntime::CExternalBookConverter converter({
        .CommandProfile = CreateShellProfile(
            scriptPath,
            InpxWebReader::ConverterCommand::EConverterOutputMode::ExactDestinationPath,
            {"{source}", "{destination}", childPidPath.generic_string()}),
        .PollInterval = std::chrono::milliseconds{10},
        .Timeout = std::chrono::milliseconds{100},
        .ProcessHooks = {
            .AfterProcessCreatedBeforeJob = [&childPidPath](std::uint32_t) {
                REQUIRE(WaitForProcessId(childPidPath, std::chrono::seconds{10}).has_value());
            }
        }
    });
    CTestProgressSink progressSink;

    const InpxWebReader::Domain::SConversionResult result = converter.Convert({
        .SourcePath = sourcePath,
        .DestinationPath = destinationPath,
        .SourceFormat = InpxWebReader::Domain::EBookFormat::Fb2,
        .DestinationFormat = InpxWebReader::Domain::EBookFormat::Epub
    }, progressSink, {});

    REQUIRE(result.Status == InpxWebReader::Domain::EConversionStatus::TimedOut);
    REQUIRE(result.IsTimedOut());
    const auto childProcessId = WaitForProcessId(childPidPath, std::chrono::seconds{10});
    REQUIRE(childProcessId.has_value());
    CScopedProcessCleanup childCleanup(*childProcessId);
    REQUIRE(WaitUntilProcessStops(*childProcessId));
    REQUIRE(WasAlreadyReapedByRunner(*childProcessId));
    REQUIRE_FALSE(HasLinuxProcessEntry(*childProcessId));
    childCleanup.Dismiss();
    REQUIRE_FALSE(std::filesystem::exists(destinationPath));
}

TEST_CASE("Linux external converter runner waits for descendants after a successful leader exit", "[converter-runtime][concurrency]")
{
    CTestWorkspace sandbox("inpx-web-reader-external-converter-linux-lingering-descendant");
    const auto scriptPath = sandbox.GetPath() / "exit-with-child.sh";
    const auto childPidPath = sandbox.GetPath() / "child.pid";
    WriteTextFile(
        scriptPath,
        "#!/bin/sh\n"
        "(sleep 30) &\n"
        "echo $! > \"$1\"\n"
        "exit 0\n");

    InpxWebReader::ConverterRuntime::CLinuxExternalConverterProcessRunner runner;
    CTestProgressSink progressSink;
    const auto startedAt = std::chrono::steady_clock::now();
    const auto result = runner.Run(
        {
            .Command = CreateResolvedShellCommand(scriptPath, {childPidPath.generic_string()}),
            .WorkingDirectory = sandbox.GetPath(),
            .PollInterval = std::chrono::milliseconds{10},
            .Timeout = std::chrono::milliseconds{150}
        },
        progressSink,
        {});
    const auto elapsed = std::chrono::steady_clock::now() - startedAt;

    const auto childProcessId = WaitForProcessId(childPidPath, std::chrono::seconds{10});
    REQUIRE(childProcessId.has_value());
    CScopedProcessCleanup childCleanup(*childProcessId);
    REQUIRE(result.WasTimedOut);
    REQUIRE_FALSE(result.WasCancelled);
    REQUIRE(elapsed >= std::chrono::milliseconds{100});
    REQUIRE(WaitUntilProcessStops(*childProcessId));
    REQUIRE(WasAlreadyReapedByRunner(*childProcessId));
    REQUIRE_FALSE(HasLinuxProcessEntry(*childProcessId));
    childCleanup.Dismiss();
}

TEST_CASE("Linux external converter runner harvests completion before classifying the deadline", "[converter-runtime]")
{
    CTestWorkspace sandbox("inpx-web-reader-external-converter-linux-deadline-harvest");
    const auto scriptPath = sandbox.GetPath() / "complete-before-deadline.sh";
    WriteTextFile(
        scriptPath,
        "#!/bin/sh\n"
        "sleep 0.05\n"
        "exit 0\n");

    InpxWebReader::ConverterRuntime::CLinuxExternalConverterProcessRunner runner;
    CTestProgressSink progressSink;
    const auto result = runner.Run(
        {
            .Command = CreateResolvedShellCommand(scriptPath, {}),
            .WorkingDirectory = sandbox.GetPath(),
            .PollInterval = std::chrono::seconds{1},
            .Timeout = std::chrono::milliseconds{250}
        },
        progressSink,
        {});

    REQUIRE(result.ExitCode == 0);
    REQUIRE_FALSE(result.WasCancelled);
    REQUIRE_FALSE(result.WasTimedOut);
}

TEST_CASE("Linux external converter runner returns non-zero exit status", "[converter-runtime]")
{
    CTestWorkspace sandbox("inpx-web-reader-external-converter-linux-exit-code");
    const std::filesystem::path scriptPath = sandbox.GetPath() / "fail-converter.sh";
    WriteTextFile(
        scriptPath,
        "#!/bin/sh\n"
        "echo 'converter failed' >&2\n"
        "exit 42\n");

    InpxWebReader::ConverterRuntime::CLinuxExternalConverterProcessRunner runner;
    CTestProgressSink progressSink;

    const auto result = runner.Run(
        {
            .Command = CreateResolvedShellCommand(scriptPath, {}),
            .WorkingDirectory = sandbox.GetPath(),
            .PollInterval = std::chrono::milliseconds{10}
        },
        progressSink,
        {});

    REQUIRE_FALSE(result.WasCancelled);
    REQUIRE(result.ExitCode == 42);
}

TEST_CASE("Linux external converter runner does not inherit server secrets", "[converter-runtime][security]")
{
    CTestWorkspace sandbox("inpx-web-reader-external-converter-linux-sanitized-environment");
    const auto scriptPath = sandbox.GetPath() / "inspect-environment.sh";
    const auto inheritedFilePath = sandbox.GetPath() / "parent-only.txt";
    WriteTextFile(inheritedFilePath, "parent-only");
    const CScopedFileDescriptor inheritedDescriptor(
        open(inheritedFilePath.c_str(), O_RDONLY));
    REQUIRE(inheritedDescriptor.Get() > STDERR_FILENO);
    WriteTextFile(
        scriptPath,
        "if [ \"${INPX_WEB_READER_AUTH_TOKEN+x}\" = x ]; then exit 91; fi\n"
        "if [ -z \"${PATH:-}\" ]; then exit 92; fi\n"
        "if [ -e \"/proc/self/fd/$1\" ]; then exit 93; fi\n"
        "exit 0\n");
    CScopedEnvironmentVariable token("INPX_WEB_READER_AUTH_TOKEN", "sentinel-server-secret");

    InpxWebReader::ConverterRuntime::CLinuxExternalConverterProcessRunner runner;
    CTestProgressSink progressSink;
    using TExecutableIdentity = InpxWebReader::ConverterRuntime::SExternalConverterExecutableIdentity;
    const auto identity = InpxWebReader::ConverterRuntime::ReadExternalConverterExecutableIdentity(
        "/bin/sh");
    const auto run = [&](const std::optional<TExecutableIdentity>& expectedIdentity) {
        auto command = CreateResolvedShellCommand(
            scriptPath,
            {std::to_string(inheritedDescriptor.Get())});
        if (expectedIdentity.has_value())
        {
            command.ExecutablePath = expectedIdentity->CanonicalPath;
        }
        return runner.Run(
            {
                .Command = std::move(command),
                .WorkingDirectory = sandbox.GetPath(),
                .PollInterval = std::chrono::milliseconds{10},
                .ExpectedExecutableIdentity = expectedIdentity
            },
            progressSink,
            {});
    };

    for (const auto& result : {run(std::nullopt), run(identity)})
    {
        REQUIRE_FALSE(result.WasCancelled);
        REQUIRE_FALSE(result.WasTimedOut);
        REQUIRE(result.ExitCode == 0);
    }
}

TEST_CASE("Linux external converter runner executes an identity-bound shebang", "[converter-runtime][security]")
{
    CTestWorkspace sandbox("inpx-web-reader-external-converter-linux-identity-script");
    const auto scriptPath = sandbox.GetPath() / "identity-bound-converter.sh";
    WriteTextFile(
        scriptPath,
        "#!/bin/sh\n"
        "exit 0\n");
    std::filesystem::permissions(
        scriptPath,
        std::filesystem::perms::owner_read
            | std::filesystem::perms::owner_write
            | std::filesystem::perms::owner_exec,
        std::filesystem::perm_options::replace);

    InpxWebReader::ConverterRuntime::CLinuxExternalConverterProcessRunner runner;
    CTestProgressSink progressSink;
    const auto identity = InpxWebReader::ConverterRuntime::ReadExternalConverterExecutableIdentity(
        scriptPath);
    const auto result = runner.Run(
        {
            .Command = {
                .ExecutablePath = identity.CanonicalPath,
                .Arguments = {"--run"},
                .OutputMode = InpxWebReader::ConverterCommand::EConverterOutputMode::ExactDestinationPath
            },
            .WorkingDirectory = sandbox.GetPath(),
            .PollInterval = std::chrono::milliseconds{10},
            .ExpectedExecutableIdentity = identity
        },
        progressSink,
        {});

    REQUIRE_FALSE(result.WasCancelled);
    REQUIRE_FALSE(result.WasTimedOut);
    REQUIRE(result.ExitCode == 0);
}

TEST_CASE("Linux external converter runner maps signal termination to exit status", "[converter-runtime]")
{
    CTestWorkspace sandbox("inpx-web-reader-external-converter-linux-signal");
    const std::filesystem::path scriptPath = sandbox.GetPath() / "signal-converter.sh";
    WriteTextFile(
        scriptPath,
        "#!/bin/sh\n"
        "kill -TERM $$\n");

    InpxWebReader::ConverterRuntime::CLinuxExternalConverterProcessRunner runner;
    CTestProgressSink progressSink;

    const auto result = runner.Run(
        {
            .Command = CreateResolvedShellCommand(scriptPath, {}),
            .WorkingDirectory = sandbox.GetPath(),
            .PollInterval = std::chrono::milliseconds{10}
        },
        progressSink,
        {});

    REQUIRE_FALSE(result.WasCancelled);
    REQUIRE(result.ExitCode == 128 + SIGTERM);
}

TEST_CASE("Linux external converter runner reports exec failure", "[converter-runtime]")
{
    CTestWorkspace sandbox("inpx-web-reader-external-converter-linux-exec-failure");

    InpxWebReader::ConverterRuntime::CLinuxExternalConverterProcessRunner runner;
    CTestProgressSink progressSink;

    const auto result = runner.Run(
        {
            .Command = {
                .ExecutablePath = sandbox.GetPath() / "missing-converter",
                .Arguments = {"--version"},
                .OutputMode = InpxWebReader::ConverterCommand::EConverterOutputMode::ExactDestinationPath
            },
            .WorkingDirectory = sandbox.GetPath(),
            .PollInterval = std::chrono::milliseconds{10}
        },
        progressSink,
        {});

    REQUIRE_FALSE(result.WasCancelled);
    REQUIRE(result.ExitCode == 127);
}

TEST_CASE("Linux external converter runner rejects a changed validated executable identity", "[converter-runtime]")
{
    CTestWorkspace sandbox("inpx-web-reader-external-converter-linux-identity");
    InpxWebReader::ConverterRuntime::CLinuxExternalConverterProcessRunner runner;
    CTestProgressSink progressSink;
    auto identity = InpxWebReader::ConverterRuntime::ReadExternalConverterExecutableIdentity("/bin/sh");
    ++identity.Inode;

    REQUIRE_THROWS_AS(
        runner.Run(
            {
                .Command = {
                    .ExecutablePath = identity.CanonicalPath,
                    .Arguments = {"-c", "exit 0"},
                    .OutputMode = InpxWebReader::ConverterCommand::EConverterOutputMode::ExactDestinationPath
                },
                .WorkingDirectory = sandbox.GetPath(),
                .PollInterval = std::chrono::milliseconds{10},
                .ExpectedExecutableIdentity = identity
            },
            progressSink,
            {}),
        std::runtime_error);
}
