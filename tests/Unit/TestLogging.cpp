#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "Foundation/Logging.hpp"

#include "TestWorkspace.hpp"

namespace {


[[nodiscard]] std::string ReadAllText(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

} // namespace

TEST_CASE("Logging initializes host logger and writes records into file", "[logging]")
{
    CTestWorkspace sandbox("inpx-web-reader-logging-кириллица");
    const auto logFilePath = sandbox.GetPath() / "host.log";

    InpxWebReader::Logging::CLogging::InitializeHostLogger(logFilePath);
    InpxWebReader::Logging::Info("Hello {}.", "logger");
    InpxWebReader::Logging::Error("Something {}.", "happened");
    InpxWebReader::Logging::CLogging::Shutdown();

    REQUIRE(std::filesystem::exists(logFilePath));
    REQUIRE(std::filesystem::is_regular_file(logFilePath));

    const auto fileContents = ReadAllText(logFilePath);
    REQUIRE(fileContents.find("Z] [info] [Server]") != std::string::npos);
    REQUIRE(fileContents.find("[Server] Hello logger.") != std::string::npos);
    REQUIRE(fileContents.find("[Server] Something happened.") != std::string::npos);
}

TEST_CASE("Logging filters debug records at info and enables them explicitly", "[logging]")
{
    CTestWorkspace sandbox("inpx-web-reader-logging-level");
    const auto infoLogPath = sandbox.GetPath() / "info.log";
    const auto debugLogPath = sandbox.GetPath() / "debug.log";

    InpxWebReader::Logging::CLogging::InitializeHostLogger(infoLogPath);
    InpxWebReader::Logging::Debug("hidden debug record");
    InpxWebReader::Logging::Info("visible info record");
    InpxWebReader::Logging::CLogging::Shutdown();

    InpxWebReader::Logging::CLogging::InitializeHostLogger(
        debugLogPath,
        {.Level = InpxWebReader::Logging::ELogLevel::Debug});
    InpxWebReader::Logging::Debug("visible debug record");
    InpxWebReader::Logging::CLogging::Shutdown();

    REQUIRE(ReadAllText(infoLogPath).find("hidden debug record") == std::string::npos);
    REQUIRE(ReadAllText(infoLogPath).find("visible info record") != std::string::npos);
    REQUIRE(ReadAllText(debugLogPath).find("visible debug record") != std::string::npos);
}

TEST_CASE("Logging rotates persistent files within the configured retention", "[logging]")
{
    CTestWorkspace sandbox("inpx-web-reader-logging-rotation");
    const auto logFilePath = sandbox.GetPath() / "rotating.log";

    InpxWebReader::Logging::CLogging::InitializeHostLogger(
        logFilePath,
        {
            .MaxFileSizeBytes = 512,
            .MaxRotatedFiles = 2
        });
    const std::string payload(300, 'x');
    for (int index = 0; index < 20; ++index)
    {
        InpxWebReader::Logging::Info("rotation record {} {}", index, payload);
    }
    InpxWebReader::Logging::CLogging::Shutdown();

    std::size_t logFileCount = 0;
    for (const auto& entry : std::filesystem::directory_iterator(sandbox.GetPath()))
    {
        if (entry.is_regular_file())
        {
            ++logFileCount;
        }
    }
    REQUIRE(logFileCount >= 2);
    REQUIRE(logFileCount <= 3);
}

TEST_CASE("Logging IfInitialized helpers ignore unavailable logger lifecycle states", "[logging]")
{
    InpxWebReader::Logging::CLogging::Shutdown();

    REQUIRE_NOTHROW(InpxWebReader::Logging::InfoIfInitialized("ignored {}", "before-init"));

    CTestWorkspace sandbox("inpx-web-reader-logging-shutdown-safe");
    const auto logFilePath = sandbox.GetPath() / "host.log";

    InpxWebReader::Logging::CLogging::InitializeHostLogger(logFilePath);
    REQUIRE_NOTHROW(InpxWebReader::Logging::InfoIfInitialized("written {}", "during-init"));

    InpxWebReader::Logging::CLogging::Shutdown();

    REQUIRE_NOTHROW(InpxWebReader::Logging::InfoIfInitialized("ignored {}", "after-shutdown"));
}

TEST_CASE("Logging IfInitialized helpers are synchronized with shutdown", "[logging]")
{
    CTestWorkspace sandbox("inpx-web-reader-logging-concurrent-shutdown");
    const auto logFilePath = sandbox.GetPath() / "host.log";

    InpxWebReader::Logging::CLogging::InitializeHostLogger(logFilePath);

    std::atomic<bool> stopRequested{false};
    std::atomic<int> failureCount{0};
    std::vector<std::thread> threads;
    threads.reserve(4);

    for (int threadIndex = 0; threadIndex < 4; ++threadIndex)
    {
        threads.emplace_back([threadIndex, &failureCount, &stopRequested]() {
            while (!stopRequested.load(std::memory_order_acquire))
            {
                try
                {
                    InpxWebReader::Logging::InfoIfInitialized("worker {}", threadIndex);
                }
                catch (...)
                {
                    failureCount.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    for (int iteration = 0; iteration < 20; ++iteration)
    {
        InpxWebReader::Logging::CLogging::Shutdown();
        InpxWebReader::Logging::CLogging::InitializeHostLogger(logFilePath);
    }

    stopRequested.store(true, std::memory_order_release);
    for (std::thread& thread : threads)
    {
        thread.join();
    }

    InpxWebReader::Logging::CLogging::Shutdown();
    REQUIRE(failureCount.load(std::memory_order_relaxed) == 0);
}
