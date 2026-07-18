#include <catch2/catch_test_macros.hpp>
#include "TestWorkspace.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include "Foundation/Logging.hpp"
#include "ScanSupport/ScanConcurrency.hpp"
#include "ScanSupport/ScanPerfTracker.hpp"

namespace {

std::string ReadTextFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

} // namespace

TEST_CASE("Scan perf tracker periodic log sorts scan bottlenecks", "[scan][perf]")
{
    const auto sandbox = MakeUniqueTestPath("inpx-web-reader-scan-perf-tracker");
    std::filesystem::remove_all(sandbox);
    const auto logPath = sandbox / "host.log";

    InpxWebReader::Logging::CLogging::InitializeHostLogger(logPath);

    {
        auto tracker = InpxWebReader::ScanSupport::CScanPerfTracker{77};
        {
            auto timer = tracker.MeasureStage(InpxWebReader::ScanSupport::CScanPerfTracker::EStage::Parse);
            (void)timer;
        }
        {
            auto timer = tracker.MeasureStage(InpxWebReader::ScanSupport::CScanPerfTracker::EStage::ZipExtract);
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
            (void)timer;
        }
        {
            auto timer = tracker.MeasureStage(InpxWebReader::ScanSupport::CScanPerfTracker::EStage::ZipScan);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            (void)timer;
        }

        for (std::uint64_t i = 0; i < InpxWebReader::ScanSupport::CScanPerfTracker::kLogEveryN; ++i)
        {
            tracker.OnBookProcessed(1, 0, 0);
        }
        tracker.LogSummary(std::chrono::seconds(3));
    }

    InpxWebReader::Logging::CLogging::Shutdown();

    const auto logText = ReadTextFile(logPath);
    REQUIRE(logText.find("[scan-perf] job=77") != std::string::npos);
    REQUIRE(logText.find("zip_scan=") != std::string::npos);
    REQUIRE(logText.find("zip_extract=") != std::string::npos);
    REQUIRE(logText.find("[scan-perf] SUMMARY job=77") != std::string::npos);

    const auto bottleneckPos = logText.find("bottleneck:");
    REQUIRE(bottleneckPos != std::string::npos);
    const auto zipExtractPos = logText.find("zip_extract=", bottleneckPos);
    const auto zipScanPos = logText.find("zip_scan=", bottleneckPos);
    const auto parsePos = logText.find("parse=", bottleneckPos);
    REQUIRE(zipExtractPos != std::string::npos);
    REQUIRE(zipScanPos != std::string::npos);
    REQUIRE(parsePos != std::string::npos);
    REQUIRE(zipExtractPos < zipScanPos);
    REQUIRE(zipExtractPos < parsePos);

    std::filesystem::remove_all(sandbox);
}

TEST_CASE("Scan perf tracker does not emit periodic log before thresholds are reached", "[scan][perf]")
{
    const auto sandbox = MakeUniqueTestPath("inpx-web-reader-scan-perf-tracker-threshold");
    std::filesystem::remove_all(sandbox);
    const auto logPath = sandbox / "host.log";

    InpxWebReader::Logging::CLogging::InitializeHostLogger(logPath);

    {
        InpxWebReader::ScanSupport::CScanPerfTracker tracker;
        tracker.OnBookProcessed(1, 0, 0);
        tracker.LogSummary(std::chrono::seconds(1));
    }

    InpxWebReader::Logging::CLogging::Shutdown();

    const auto logText = ReadTextFile(logPath);
    REQUIRE(logText.find("[scan-perf] books=") == std::string::npos);
    REQUIRE(logText.find("[scan-perf] SUMMARY") != std::string::npos);

    std::filesystem::remove_all(sandbox);
}

TEST_CASE("Scan perf tracker supports INPX scan outcome labels", "[scan][perf]")
{
    const auto sandbox = MakeUniqueTestPath("inpx-web-reader-scan-perf-tracker-labels");
    std::filesystem::remove_all(sandbox);
    const auto logPath = sandbox / "host.log";

    InpxWebReader::Logging::CLogging::InitializeHostLogger(logPath);

    {
        auto tracker = InpxWebReader::ScanSupport::CScanPerfTracker{
            88,
            {
                .Added = "added",
                .Updated = "updated",
                .Failed = "failed"
            }};
        tracker.OnBookProcessed(1, 1, 0);
        tracker.LogSummary(std::chrono::seconds(1));
    }

    InpxWebReader::Logging::CLogging::Shutdown();

    const auto logText = ReadTextFile(logPath);
    REQUIRE(logText.find("added=1 updated=1 failed=0") != std::string::npos);

    std::filesystem::remove_all(sandbox);
}

TEST_CASE("Scan worker count policy is deterministic for known hardware counts", "[scan][perf]")
{
    using InpxWebReader::ScanSupport::EScanWorkerPolicy;
    using InpxWebReader::ScanSupport::ResolveScanWorkerCountForHardware;

    REQUIRE(ResolveScanWorkerCountForHardware(1, EScanWorkerPolicy::LeaveOneThreadFree, 1) == 1);
    REQUIRE(ResolveScanWorkerCountForHardware(8, EScanWorkerPolicy::LeaveOneThreadFree, 1) == 7);
    REQUIRE(ResolveScanWorkerCountForHardware(16, EScanWorkerPolicy::LeaveOneThreadFree, 1) == 8);
    REQUIRE(ResolveScanWorkerCountForHardware(0, EScanWorkerPolicy::LeaveOneThreadFree, 1) == 1);
    REQUIRE(ResolveScanWorkerCountForHardware(0, EScanWorkerPolicy::UseAllAvailable, 2) == 2);
    REQUIRE(ResolveScanWorkerCountForHardware(32, EScanWorkerPolicy::UseAllAvailable, 2) == 8);
}
