#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <mutex>
#include <stop_token>
#include <string>
#include <vector>

#include <sys/stat.h>

#include "Converter/ExternalBookConverter.hpp"
#include "TestExternalBookConverterProcessRunner.hpp"
#include "TestWorkspace.hpp"

namespace {

class CTestProgressSink final : public InpxWebReader::Domain::IProgressSink
{
public:
    void ReportValue(const int percent, std::string_view message) override
    {
        LastPercent = percent;
        LastMessage.assign(message);
    }

    bool IsCancellationRequested() const override
    {
        return CancellationRequested;
    }

    int LastPercent = 0;
    std::string LastMessage;
    bool CancellationRequested = false;
};

void WriteTextFile(const std::filesystem::path& path, const std::string& text)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output << text;
}

class CConcurrentProcessRunner final : public InpxWebReader::ConverterRuntime::IExternalConverterProcessRunner
{
public:
    [[nodiscard]] InpxWebReader::ConverterRuntime::SExternalConverterProcessResult Run(
        const InpxWebReader::ConverterRuntime::SExternalConverterProcessRequest& request,
        InpxWebReader::Domain::IProgressSink&,
        std::stop_token) const override
    {
        {
            std::unique_lock lock(m_mutex);
            m_requests.push_back(request);
            m_condition.notify_all();
            if (!m_condition.wait_for(
                    lock,
                    std::chrono::seconds{5},
                    [this]() { return m_requests.size() >= 2; }))
            {
                return {.ExitCode = 98};
            }
        }

        WriteTextFile(request.WorkingDirectory / "fb2cng.log", "invocation-log");
        WriteTextFile(request.Command.ExpectedOutputPath, "converted-content");
        return {};
    }

    [[nodiscard]] std::vector<InpxWebReader::ConverterRuntime::SExternalConverterProcessRequest> GetRequests() const
    {
        const std::scoped_lock lock(m_mutex);
        return m_requests;
    }

private:
    mutable std::mutex m_mutex;
    mutable std::condition_variable m_condition;
    mutable std::vector<InpxWebReader::ConverterRuntime::SExternalConverterProcessRequest> m_requests;
};

std::string ReadTextFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

InpxWebReader::ConverterCommand::SConverterCommandProfile CreateFakeProfile(
    const std::filesystem::path& executablePath,
    const InpxWebReader::ConverterCommand::EConverterOutputMode outputMode)
{
    return {
        .ExecutablePath = executablePath,
        .ArgumentTemplate = {"--source", "{source}", "--destination", "{destination}"},
        .OutputMode = outputMode
    };
}

} // namespace

TEST_CASE("External book converter delegates process execution to the configured runner", "[converter-runtime]")
{
    CTestWorkspace sandbox("inpx-web-reader-external-converter-fake-runner");
    const std::filesystem::path executablePath = sandbox.GetPath() / "fake-converter";
    const std::filesystem::path sourcePath = sandbox.GetPath() / "source.fb2";
    const std::filesystem::path destinationPath = sandbox.GetPath() / "output" / "book.epub";
    const std::filesystem::path workingDirectory = sandbox.GetPath() / "converter-work";
    WriteTextFile(sourcePath, "source-content");

    CFakeProcessRunner processRunner;
    processRunner.OnRun = [](const InpxWebReader::ConverterRuntime::SExternalConverterProcessRequest& request) {
        WriteTextFile(request.Command.ExpectedOutputPath, "converted-content");
    };

    const InpxWebReader::ConverterRuntime::CExternalBookConverter converter({
        .CommandProfile = CreateFakeProfile(
            executablePath,
            InpxWebReader::ConverterCommand::EConverterOutputMode::ExactDestinationPath),
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
    REQUIRE(ReadTextFile(destinationPath) == "converted-content");
    REQUIRE(processRunner.Calls == 1);
    REQUIRE(processRunner.LastRequest.has_value());
    REQUIRE(processRunner.LastRequest->Command.ExecutablePath == executablePath);
    REQUIRE(processRunner.LastRequest->Command.ExpectedOutputPath == destinationPath);
    REQUIRE(processRunner.LastRequest->WorkingDirectory.parent_path() == workingDirectory);
    REQUIRE(processRunner.LastRequest->WorkingDirectory.filename().string().starts_with("conversion-"));
    REQUIRE_FALSE(std::filesystem::exists(processRunner.LastRequest->WorkingDirectory));
    REQUIRE_FALSE(processRunner.LastProgressSinkCancellationState);
    REQUIRE_FALSE(processRunner.LastStopRequested);
    REQUIRE(progressSink.LastPercent == 100);
}

TEST_CASE("External book converter isolates concurrent same-stem working directories", "[converter-runtime][concurrency]")
{
    CTestWorkspace sandbox("inpx-web-reader-external-converter-concurrent-working-directories");
    const auto executablePath = sandbox.GetPath() / "fake-converter";
    const auto sourcePath = sandbox.GetPath() / "source.fb2";
    const auto workingDirectory = sandbox.GetPath() / "converter-work";
    const auto firstDestination = sandbox.GetPath() / "first" / "book.epub";
    const auto secondDestination = sandbox.GetPath() / "second" / "book.epub";
    WriteTextFile(sourcePath, "source-content");

    CConcurrentProcessRunner processRunner;
    const InpxWebReader::ConverterRuntime::CExternalBookConverter converter({
        .CommandProfile = CreateFakeProfile(
            executablePath,
            InpxWebReader::ConverterCommand::EConverterOutputMode::ExactDestinationPath),
        .WorkingDirectory = workingDirectory,
        .ProcessRunner = &processRunner
    });

    const auto convert = [&converter, &sourcePath](const std::filesystem::path& destinationPath) {
        CTestProgressSink progressSink;
        return converter.Convert({
            .SourcePath = sourcePath,
            .DestinationPath = destinationPath,
            .SourceFormat = InpxWebReader::Domain::EBookFormat::Fb2,
            .DestinationFormat = InpxWebReader::Domain::EBookFormat::Epub
        }, progressSink, {});
    };
    auto first = std::async(std::launch::async, convert, firstDestination);
    auto second = std::async(std::launch::async, convert, secondDestination);

    const auto firstResult = first.get();
    const auto secondResult = second.get();
    const auto requests = processRunner.GetRequests();

    REQUIRE(firstResult.IsSuccess());
    REQUIRE(secondResult.IsSuccess());
    REQUIRE(requests.size() == 2);
    REQUIRE(requests[0].WorkingDirectory != requests[1].WorkingDirectory);
    REQUIRE(requests[0].WorkingDirectory.parent_path() == workingDirectory);
    REQUIRE(requests[1].WorkingDirectory.parent_path() == workingDirectory);
    REQUIRE_FALSE(std::filesystem::exists(requests[0].WorkingDirectory));
    REQUIRE_FALSE(std::filesystem::exists(requests[1].WorkingDirectory));
    REQUIRE(ReadTextFile(firstDestination) == "converted-content");
    REQUIRE(ReadTextFile(secondDestination) == "converted-content");
}

TEST_CASE("External book converter cleans produced files when injected runner reports cancellation", "[converter-runtime]")
{
    CTestWorkspace sandbox("inpx-web-reader-external-converter-fake-cancel");
    const std::filesystem::path executablePath = sandbox.GetPath() / "fake-converter";
    const std::filesystem::path sourcePath = sandbox.GetPath() / "source.fb2";
    const std::filesystem::path destinationPath = sandbox.GetPath() / "output" / "book.epub";
    WriteTextFile(sourcePath, "source-content");

    CFakeProcessRunner processRunner;
    processRunner.Result = {
        .ExitCode = 1,
        .WasCancelled = true
    };
    processRunner.OnRun = [](const InpxWebReader::ConverterRuntime::SExternalConverterProcessRequest& request) {
        WriteTextFile(request.Command.ExpectedOutputPath, "partial-content");
    };

    const InpxWebReader::ConverterRuntime::CExternalBookConverter converter({
        .CommandProfile = CreateFakeProfile(
            executablePath,
            InpxWebReader::ConverterCommand::EConverterOutputMode::ExactDestinationPath),
        .ProcessRunner = &processRunner
    });
    CTestProgressSink progressSink;

    const InpxWebReader::Domain::SConversionResult result = converter.Convert({
        .SourcePath = sourcePath,
        .DestinationPath = destinationPath,
        .SourceFormat = InpxWebReader::Domain::EBookFormat::Fb2,
        .DestinationFormat = InpxWebReader::Domain::EBookFormat::Epub
    }, progressSink, {});

    REQUIRE_FALSE(result.IsSuccess());
    REQUIRE(result.IsCancelled());
    REQUIRE(result.Warnings == std::vector<std::string>({"Conversion cancelled."}));
    REQUIRE(processRunner.Calls == 1);
    REQUIRE_FALSE(std::filesystem::exists(destinationPath));
    REQUIRE(progressSink.LastPercent == 0);
}

TEST_CASE("External book converter reports timeout distinctly and cleans partial output", "[converter-runtime]")
{
    CTestWorkspace sandbox("inpx-web-reader-external-converter-fake-timeout");
    const auto executablePath = sandbox.GetPath() / "fake-converter";
    const auto sourcePath = sandbox.GetPath() / "source.fb2";
    const auto destinationPath = sandbox.GetPath() / "output" / "book.epub";
    WriteTextFile(sourcePath, "source-content");

    CFakeProcessRunner processRunner;
    processRunner.Result = {
        .ExitCode = 0,
        .WasCancelled = false,
        .WasTimedOut = true
    };
    processRunner.OnRun = [](const InpxWebReader::ConverterRuntime::SExternalConverterProcessRequest& request) {
        WriteTextFile(request.Command.ExpectedOutputPath, "partial-content");
    };

    const InpxWebReader::ConverterRuntime::CExternalBookConverter converter({
        .CommandProfile = CreateFakeProfile(
            executablePath,
            InpxWebReader::ConverterCommand::EConverterOutputMode::ExactDestinationPath),
        .Timeout = std::chrono::milliseconds{250},
        .ProcessRunner = &processRunner
    });
    CTestProgressSink progressSink;

    const auto result = converter.Convert({
        .SourcePath = sourcePath,
        .DestinationPath = destinationPath,
        .SourceFormat = InpxWebReader::Domain::EBookFormat::Fb2,
        .DestinationFormat = InpxWebReader::Domain::EBookFormat::Epub
    }, progressSink, {});

    REQUIRE(result.IsTimedOut());
    REQUIRE(result.Warnings == std::vector<std::string>({"Conversion timed out."}));
    REQUIRE(processRunner.LastRequest.has_value());
    REQUIRE(processRunner.LastRequest->Timeout == std::chrono::milliseconds{250});
    REQUIRE_FALSE(std::filesystem::exists(destinationPath));
}

TEST_CASE("External book converter preserves the process exit code in failure diagnostics", "[converter-runtime]")
{
    CTestWorkspace sandbox("inpx-web-reader-external-converter-exit-code");
    const std::filesystem::path executablePath = sandbox.GetPath() / "fake-converter";
    const std::filesystem::path sourcePath = sandbox.GetPath() / "source.fb2";
    const std::filesystem::path destinationPath = sandbox.GetPath() / "output" / "book.epub";
    WriteTextFile(sourcePath, "source-content");

    CFakeProcessRunner processRunner;
    processRunner.Result.ExitCode = 42;

    const InpxWebReader::ConverterRuntime::CExternalBookConverter converter({
        .CommandProfile = CreateFakeProfile(
            executablePath,
            InpxWebReader::ConverterCommand::EConverterOutputMode::ExactDestinationPath),
        .ProcessRunner = &processRunner
    });
    CTestProgressSink progressSink;

    const InpxWebReader::Domain::SConversionResult result = converter.Convert({
        .SourcePath = sourcePath,
        .DestinationPath = destinationPath,
        .SourceFormat = InpxWebReader::Domain::EBookFormat::Fb2,
        .DestinationFormat = InpxWebReader::Domain::EBookFormat::Epub
    }, progressSink, {});

    REQUIRE_FALSE(result.IsSuccess());
    REQUIRE_FALSE(result.IsCancelled());
    REQUIRE(result.Warnings == std::vector<std::string>({"Converter process exited with code 42."}));
    REQUIRE_FALSE(std::filesystem::exists(destinationPath));
}

TEST_CASE("External book converter pins the revalidated built-in executable identity", "[converter-runtime]")
{
    CTestWorkspace sandbox("inpx-web-reader-external-converter-identity");
    const auto executablePath = sandbox.GetPath() / "fbc";
    const auto sourcePath = sandbox.GetPath() / "source.fb2";
    const auto destinationPath = sandbox.GetPath() / "output" / "book.epub";
    WriteTextFile(
        executablePath,
        "#!/bin/sh\n"
        "echo 'Usage: fbc convert FB2 files'\n");
    REQUIRE(chmod(executablePath.c_str(), 0755) == 0);
    WriteTextFile(sourcePath, "source-content");

    CFakeProcessRunner processRunner;
    processRunner.OnRun = [](const InpxWebReader::ConverterRuntime::SExternalConverterProcessRequest& request) {
        WriteTextFile(request.Command.ExpectedOutputPath, "converted-content");
    };
    const InpxWebReader::ConverterRuntime::CExternalBookConverter converter({
        .CommandProfile = CreateFakeProfile(
            executablePath,
            InpxWebReader::ConverterCommand::EConverterOutputMode::ExactDestinationPath),
        .RevalidateBuiltInFbcBeforeRun = true,
        .ProcessRunner = &processRunner
    });
    CTestProgressSink progressSink;

    const auto result = converter.Convert({
        .SourcePath = sourcePath,
        .DestinationPath = destinationPath,
        .SourceFormat = InpxWebReader::Domain::EBookFormat::Fb2,
        .DestinationFormat = InpxWebReader::Domain::EBookFormat::Epub
    }, progressSink, {});

    REQUIRE(result.IsSuccess());
    REQUIRE(processRunner.LastRequest.has_value());
    REQUIRE(processRunner.LastRequest->ExpectedExecutableIdentity.has_value());
    REQUIRE(processRunner.LastRequest->ExpectedExecutableIdentity->IsValid());
    REQUIRE(processRunner.LastRequest->Command.ExecutablePath
        == processRunner.LastRequest->ExpectedExecutableIdentity->CanonicalPath);
}

TEST_CASE("External book converter rejects and removes a symlink EPUB output", "[converter-runtime][files]")
{
    CTestWorkspace sandbox("inpx-web-reader-external-converter-symlink-output");
    const auto executablePath = sandbox.GetPath() / "fake-converter";
    const auto sourcePath = sandbox.GetPath() / "source.fb2";
    const auto destinationPath = sandbox.GetPath() / "output" / "book.epub";
    const auto protectedPath = sandbox.GetPath() / "protected.epub";
    WriteTextFile(sourcePath, "source-content");
    WriteTextFile(protectedPath, "must-not-be-published-or-removed");

    std::filesystem::path generatedSymlinkPath;
    CFakeProcessRunner processRunner;
    processRunner.OnRun = [&](const InpxWebReader::ConverterRuntime::SExternalConverterProcessRequest& request) {
        generatedSymlinkPath = request.Command.ExpectedOutputDirectory / "generated.epub";
        std::filesystem::create_symlink(protectedPath, generatedSymlinkPath);
    };

    const InpxWebReader::ConverterRuntime::CExternalBookConverter converter({
        .CommandProfile = CreateFakeProfile(
            executablePath,
            InpxWebReader::ConverterCommand::EConverterOutputMode::SingleFileInDestinationDirectory),
        .ProcessRunner = &processRunner
    });
    CTestProgressSink progressSink;

    const auto result = converter.Convert({
        .SourcePath = sourcePath,
        .DestinationPath = destinationPath,
        .SourceFormat = InpxWebReader::Domain::EBookFormat::Fb2,
        .DestinationFormat = InpxWebReader::Domain::EBookFormat::Epub
    }, progressSink, {});

    REQUIRE_FALSE(result.IsSuccess());
    REQUIRE(result.Warnings == std::vector<std::string>{
        "Converter process produced a symlink or non-regular EPUB output."});
    REQUIRE_FALSE(std::filesystem::exists(std::filesystem::symlink_status(generatedSymlinkPath)));
    REQUIRE_FALSE(std::filesystem::exists(destinationPath));
    REQUIRE(ReadTextFile(protectedPath) == "must-not-be-published-or-removed");
}

TEST_CASE("External book converter rejects ambiguous EPUB outputs and removes every new entry", "[converter-runtime][files]")
{
    CTestWorkspace sandbox("inpx-web-reader-external-converter-ambiguous-output");
    const auto executablePath = sandbox.GetPath() / "fake-converter";
    const auto sourcePath = sandbox.GetPath() / "source.fb2";
    const auto destinationPath = sandbox.GetPath() / "output" / "book.epub";
    WriteTextFile(sourcePath, "source-content");

    std::filesystem::path firstOutputPath;
    std::filesystem::path secondOutputPath;
    std::filesystem::path extraDirectoryPath;
    CFakeProcessRunner processRunner;
    processRunner.OnRun = [&](const InpxWebReader::ConverterRuntime::SExternalConverterProcessRequest& request) {
        firstOutputPath = request.Command.ExpectedOutputDirectory / "first.epub";
        secondOutputPath = request.Command.ExpectedOutputDirectory / "second.epub";
        extraDirectoryPath = request.Command.ExpectedOutputDirectory / "converter-scratch";
        WriteTextFile(firstOutputPath, "first");
        WriteTextFile(secondOutputPath, "second");
        std::filesystem::create_directory(extraDirectoryPath);
        WriteTextFile(extraDirectoryPath / "temporary.txt", "temporary");
    };

    const InpxWebReader::ConverterRuntime::CExternalBookConverter converter({
        .CommandProfile = CreateFakeProfile(
            executablePath,
            InpxWebReader::ConverterCommand::EConverterOutputMode::SingleFileInDestinationDirectory),
        .ProcessRunner = &processRunner
    });
    CTestProgressSink progressSink;

    const auto result = converter.Convert({
        .SourcePath = sourcePath,
        .DestinationPath = destinationPath,
        .SourceFormat = InpxWebReader::Domain::EBookFormat::Fb2,
        .DestinationFormat = InpxWebReader::Domain::EBookFormat::Epub
    }, progressSink, {});

    REQUIRE_FALSE(result.IsSuccess());
    REQUIRE(result.Warnings == std::vector<std::string>{
        "Converter process did not produce one unambiguous EPUB output."});
    REQUIRE_FALSE(std::filesystem::exists(firstOutputPath));
    REQUIRE_FALSE(std::filesystem::exists(secondOutputPath));
    REQUIRE_FALSE(std::filesystem::exists(extraDirectoryPath));
    REQUIRE_FALSE(std::filesystem::exists(destinationPath));
}

TEST_CASE("External book converter rejects an exact EPUB accompanied by another EPUB", "[converter-runtime][files]")
{
    CTestWorkspace sandbox("inpx-web-reader-external-converter-exact-and-fallback-output");
    const auto executablePath = sandbox.GetPath() / "fake-converter";
    const auto sourcePath = sandbox.GetPath() / "source.fb2";
    const auto destinationPath = sandbox.GetPath() / "output" / "book.epub";
    WriteTextFile(sourcePath, "source-content");

    std::filesystem::path fallbackOutputPath;
    CFakeProcessRunner processRunner;
    processRunner.OnRun = [&](const InpxWebReader::ConverterRuntime::SExternalConverterProcessRequest& request) {
        WriteTextFile(request.Command.ExpectedOutputPath, "exact");
        fallbackOutputPath = request.Command.ExpectedOutputDirectory / "fallback.epub";
        WriteTextFile(fallbackOutputPath, "fallback");
    };

    const InpxWebReader::ConverterRuntime::CExternalBookConverter converter({
        .CommandProfile = CreateFakeProfile(
            executablePath,
            InpxWebReader::ConverterCommand::EConverterOutputMode::SingleFileInDestinationDirectory),
        .ProcessRunner = &processRunner
    });
    CTestProgressSink progressSink;

    const auto result = converter.Convert({
        .SourcePath = sourcePath,
        .DestinationPath = destinationPath,
        .SourceFormat = InpxWebReader::Domain::EBookFormat::Fb2,
        .DestinationFormat = InpxWebReader::Domain::EBookFormat::Epub
    }, progressSink, {});

    REQUIRE_FALSE(result.IsSuccess());
    REQUIRE(result.Warnings == std::vector<std::string>{
        "Converter process did not produce one unambiguous EPUB output."});
    REQUIRE_FALSE(std::filesystem::exists(destinationPath));
    REQUIRE_FALSE(std::filesystem::exists(fallbackOutputPath));
}

TEST_CASE("External book converter rejects an exact-path symlink output", "[converter-runtime][files]")
{
    CTestWorkspace sandbox("inpx-web-reader-external-converter-exact-symlink-output");
    const auto executablePath = sandbox.GetPath() / "fake-converter";
    const auto sourcePath = sandbox.GetPath() / "source.fb2";
    const auto destinationPath = sandbox.GetPath() / "output" / "book.epub";
    const auto protectedPath = sandbox.GetPath() / "protected.epub";
    WriteTextFile(sourcePath, "source-content");
    WriteTextFile(protectedPath, "must-stay-private");

    CFakeProcessRunner processRunner;
    processRunner.OnRun = [&](const InpxWebReader::ConverterRuntime::SExternalConverterProcessRequest& request) {
        std::filesystem::create_symlink(protectedPath, request.Command.ExpectedOutputPath);
    };
    const InpxWebReader::ConverterRuntime::CExternalBookConverter converter({
        .CommandProfile = CreateFakeProfile(
            executablePath,
            InpxWebReader::ConverterCommand::EConverterOutputMode::ExactDestinationPath),
        .ProcessRunner = &processRunner
    });
    CTestProgressSink progressSink;

    const auto result = converter.Convert({
        .SourcePath = sourcePath,
        .DestinationPath = destinationPath,
        .SourceFormat = InpxWebReader::Domain::EBookFormat::Fb2,
        .DestinationFormat = InpxWebReader::Domain::EBookFormat::Epub
    }, progressSink, {});

    REQUIRE_FALSE(result.IsSuccess());
    REQUIRE(result.Warnings == std::vector<std::string>{
        "Converter process completed, but the expected output is missing, a symlink, or not a regular file."});
    REQUIRE_FALSE(std::filesystem::exists(std::filesystem::symlink_status(destinationPath)));
    REQUIRE(ReadTextFile(protectedPath) == "must-stay-private");
}
