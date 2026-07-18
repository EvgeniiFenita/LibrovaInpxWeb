#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include "App/CInpxCatalogApplication.hpp"
#include "App/InpxScanJobService.hpp"
#include "Database/CatalogStatisticsMaintenance.hpp"
#include "Database/SqliteConnection.hpp"
#include "Database/SqliteStatement.hpp"
#include "Database/SqliteTransaction.hpp"
#include "Foundation/Logging.hpp"
#include "Foundation/Sha256Fingerprint.hpp"
#include "Foundation/UnicodeConversion.hpp"
#include "TestInpxScanJobServiceSupport.hpp"
#include "TestWorkspace.hpp"

using namespace InpxWebReader::Tests::InpxScanJobServiceSupport;

TEST_CASE("CInpxCatalogApplication rejects download concurrency above its manifest memory reserve", "[application-jobs][inpx][limits]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-download-manifest-memory-budget");
    auto config = MakeBaseConfig(sandbox);
    config.MaxSteadyStateMemoryBytes = 16;
    config.MaxConcurrentDownloads = 2;

    REQUIRE_THROWS_WITH(
        InpxWebReader::Application::CInpxCatalogApplication(config),
        Catch::Matchers::ContainsSubstring(
            "concurrent downloads exceeds the reserved download manifest memory budget"));
}

TEST_CASE("CInpxCatalogApplication caches INPX covers without copying payloads", "[application-jobs][inpx][covers]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-scan-cover-cache");
    auto config = MakeInpxConfig(sandbox, 1);
    WriteInpxArchive(config.InpxSource->ArchiveRoot / "fb2-main.zip", 1, true);
    InpxWebReader::Application::CInpxCatalogApplication app(config);

    const auto jobId = app.StartInpxScan({.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::InitialScan});
    REQUIRE(WaitForApplicationScan(app, jobId, InpxScanWaitTimeout()));

    const auto result = app.GetInpxScanJobResult(jobId);
    REQUIRE(result.has_value());
    REQUIRE(result->Snapshot.Status == InpxWebReader::ApplicationJobs::EInpxScanJobStatus::Completed);

    const auto databasePath = config.CacheRoot / "Database" / "inpx-web-reader.db";
    const auto coverPath = ReadCoverPath(databasePath, "1");
    REQUIRE(coverPath.has_value());
    REQUIRE(coverPath->starts_with("Covers/"));
    REQUIRE(std::filesystem::is_regular_file(config.CacheRoot / InpxWebReader::Unicode::PathFromUtf8(*coverPath)));
    for (const auto& entry : std::filesystem::recursive_directory_iterator(config.CacheRoot / "Covers"))
    {
        REQUIRE(entry.path().filename().string().find(".tmp-") == std::string::npos);
    }

    const auto list = app.ListBooks({.Limit = 10});
    REQUIRE(list.Items.size() == 1);
    const auto details = app.GetBookDetails(list.Items.front().Id);
    REQUIRE(details.has_value());
    REQUIRE(details->CoverPath.has_value());
    REQUIRE(InpxWebReader::Unicode::PathToUtf8(*details->CoverPath) == *coverPath);

    const auto statistics = app.GetCatalogStatistics();
    const auto inpxSourceSize =
        static_cast<std::uint64_t>(std::filesystem::file_size(config.InpxSource->InpxPath)
                                   + std::filesystem::file_size(config.InpxSource->ArchiveRoot / "fb2-main.zip"));
    const auto databaseSize =
        InpxWebReader::BookDatabase::CCatalogStatisticsMaintenance::GetDatabaseFootprintSize(databasePath);
    REQUIRE(statistics.BookCount == 1);
    REQUIRE(statistics.UnavailableBookCount == 0);
    REQUIRE(statistics.InpxSourceSizeBytes == inpxSourceSize);
    REQUIRE(statistics.TotalCatalogSizeBytes == inpxSourceSize + SumCoverFileBytes(config.CacheRoot) + databaseSize);
}

TEST_CASE("INPX source statistics stay bound to the archive path selected by "
          "the scan",
          "[application-jobs][inpx][statistics]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-source-statistics-persisted-archive");
    auto config = MakeInpxConfig(sandbox, 1);
    const auto directArchivePath = config.InpxSource->ArchiveRoot / "fb2-main.zip";
    const auto selectedArchivePath = config.InpxSource->ArchiveRoot / "z-selected" / "fb2-main.zip";
    std::filesystem::create_directories(selectedArchivePath.parent_path());
    std::filesystem::rename(directArchivePath, selectedArchivePath);

    InpxWebReader::Application::CInpxCatalogApplication app(config);
    const auto jobId = app.StartInpxScan({.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::InitialScan});
    REQUIRE(WaitForApplicationScan(app, jobId, InpxScanWaitTimeout()));
    const auto result = app.GetInpxScanJobResult(jobId);
    REQUIRE(result.has_value());
    REQUIRE(result->Snapshot.Status == InpxWebReader::ApplicationJobs::EInpxScanJobStatus::Completed);

    const auto databasePath = config.CacheRoot / "Database" / "inpx-web-reader.db";
    InpxWebReader::Sqlite::CSqliteConnection connection(databasePath);
    InpxWebReader::Sqlite::CSqliteStatement selectedPathStatement(
        connection.GetNativeHandle(),
        "SELECT resolved_archive_path FROM inpx_segments WHERE requires_archive "
        "= 1;");
    REQUIRE(selectedPathStatement.Step());
    REQUIRE(selectedPathStatement.GetColumnText(0) == "z-selected/fb2-main.zip");

    const auto earlierArchivePath = config.InpxSource->ArchiveRoot / "a-earlier" / "fb2-main.zip";
    std::filesystem::create_directories(earlierArchivePath.parent_path());
    std::ofstream(earlierArchivePath, std::ios::binary | std::ios::trunc) << "earlier";
    InpxWebReader::BookDatabase::CCatalogStatisticsMaintenance::RecomputeInpxSourceSize(
        connection, config.InpxSource->InpxPath, config.InpxSource->ArchiveRoot);

    const auto statistics = app.GetCatalogStatistics();
    const auto expectedSourceSize = static_cast<std::uint64_t>(std::filesystem::file_size(config.InpxSource->InpxPath)
                                                               + std::filesystem::file_size(selectedArchivePath));
    REQUIRE(statistics.InpxSourceSizeBytes == expectedSourceSize);
}

TEST_CASE("CInpxCatalogApplication counts INPX cover diagnostics separately "
          "from scan warnings",
          "[application-jobs][inpx][covers]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-scan-cover-diagnostics");
    const auto config = MakeInpxConfig(sandbox, 1);
    WriteSinglePayloadInpxArchive(config.InpxSource->ArchiveRoot / "fb2-main.zip",
                                  R"(<?xml version="1.0" encoding="UTF-8"?>
<FictionBook xmlns:l="http://www.w3.org/1999/xlink">
  <description>
    <title-info>
      <genre>science_fiction</genre>
      <book-title>Payload Title</book-title>
      <author>
        <first-name>Payload</first-name>
        <last-name>Author</last-name>
      </author>
      <lang>ru</lang>
      <coverpage><image l:href="#missing.png"/></coverpage>
    </title-info>
  </description>
</FictionBook>)");

    InpxWebReader::Application::CInpxCatalogApplication app(config);
    const auto jobId = app.StartInpxScan({.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::InitialScan});
    REQUIRE(WaitForApplicationScan(app, jobId, InpxScanWaitTimeout()));

    const auto result = app.GetInpxScanJobResult(jobId);
    REQUIRE(result.has_value());
    REQUIRE(result->ScanResult.has_value());
    REQUIRE(result->ScanResult->WarningCount == 0);
    REQUIRE(result->ScanResult->ParserWarningCount == 0);
    REQUIRE(result->ScanResult->CoverWarningCount == 1);
}

TEST_CASE("CInpxCatalogApplication enforces the configured cover cache budget atomically", "[application-jobs][inpx][covers][limits]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-scan-cover-cache-budget");
    auto config = MakeInpxConfig(sandbox, 1);
    config.MaxCoverCacheBytes = 1;
    WriteInpxArchive(config.InpxSource->ArchiveRoot / "fb2-main.zip", 1, true);
    InpxWebReader::Application::CInpxCatalogApplication app(config);

    const auto jobId = app.StartInpxScan({.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::InitialScan});
    REQUIRE(WaitForApplicationScan(app, jobId, InpxScanWaitTimeout()));
    const auto result = app.GetInpxScanJobResult(jobId);

    REQUIRE(result.has_value());
    REQUIRE(result->Snapshot.Status == InpxWebReader::ApplicationJobs::EInpxScanJobStatus::Failed);
    REQUIRE(result->Snapshot.Message.find("cover cache budget exceeded") != std::string::npos);
    REQUIRE(CountBookRows(config.CacheRoot / "Database" / "inpx-web-reader.db") == 0);
    REQUIRE(CountCoverFiles(config.CacheRoot) == 0);
}

TEST_CASE("CInpxCatalogApplication rejects INP metadata above the coarse scan planning limit", "[application-jobs][inpx][limits]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-scan-memory-budget");
    auto config = MakeInpxConfig(sandbox, 1);
    config.MaxSteadyStateMemoryBytes = 4;
    InpxWebReader::Application::CInpxCatalogApplication app(config);

    const auto jobId = app.StartInpxScan({.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::InitialScan});
    REQUIRE(WaitForApplicationScan(app, jobId, InpxScanWaitTimeout()));
    const auto result = app.GetInpxScanJobResult(jobId);

    REQUIRE(result.has_value());
    REQUIRE(result->Snapshot.Status == InpxWebReader::ApplicationJobs::EInpxScanJobStatus::Failed);
    REQUIRE(result->Snapshot.Message.find("scan planning limit") != std::string::npos);
    REQUIRE(CountBookRows(config.CacheRoot / "Database" / "inpx-web-reader.db") == 0);
}

TEST_CASE("INPX scan bounds aggregate raw FB2 payload bytes in flight", "[application-jobs][inpx][limits]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-scan-payload-window");
    constexpr std::uint64_t scanMemoryBytes = 2ull * 1024ull * 1024ull;
    constexpr std::uint64_t payloadWindowBytes = scanMemoryBytes / 16;
    auto config = MakeInpxConfig(sandbox, 1);

    std::string payload =
        "<FictionBook><description><title-info><book-title>Working Set</book-title>"
        "<author><last-name>Author</last-name></author><lang>en</lang>"
        "</title-info></description><body><section><p>";
    payload.append(payloadWindowBytes, 'x');
    payload.append("</p></section></body></FictionBook>");
    REQUIRE(payload.size() > payloadWindowBytes);
    WriteSinglePayloadInpxArchive(config.InpxSource->ArchiveRoot / "fb2-main.zip", payload);

    InpxWebReader::ApplicationJobs::CInpxScanJobService service(
        sandbox.GetPath() / "runtime-payload-window",
        {},
        {},
        {},
        nullptr,
        1,
        128ull * 1024ull * 1024ull,
        scanMemoryBytes);
    const auto jobId = service.Start(
        *config.InpxSource,
        {.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::InitialScan});
    REQUIRE(service.Wait(jobId, InpxScanWaitTimeout()));

    const auto result = service.TryGetResult(jobId);
    REQUIRE(result.has_value());
    REQUIRE(result->Snapshot.Status == InpxWebReader::ApplicationJobs::EInpxScanJobStatus::Failed);
    REQUIRE(result->Snapshot.Message.find("in-flight payload limit") != std::string::npos);
}

TEST_CASE("INPX scan record limit bounds unchanged catalog planning state", "[application-jobs][inpx][limits]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-scan-record-limit");
    constexpr std::size_t recordCount = 1'024;
    constexpr std::uint64_t lowMemoryBudgetBytes = 512ull * 1024ull;
    constexpr std::uint64_t highMemoryBudgetBytes = 16ull * 1024ull * 1024ull;

    auto config = MakeInpxConfig(sandbox, 1);
    std::string records;
    records.reserve(recordCount * 256);
    std::vector<std::string> payloadTitles;
    payloadTitles.reserve(recordCount);
    for (std::size_t index = 0; index < recordCount; ++index)
    {
        const std::string libId = "id-" + std::to_string(index);
        std::string inpxRecord = MakeInpxRecord("book-" + std::to_string(index + 1), libId);
        const std::string generatedTitle = "Title " + libId;
        const std::size_t titleOffset = inpxRecord.find(generatedTitle);
        REQUIRE(titleOffset != std::string::npos);
        inpxRecord.replace(titleOffset, generatedTitle.size(), "T");
        records += inpxRecord;
        payloadTitles.push_back("Payload " + std::to_string(index + 1));
    }
    WriteInpxArchive(config.InpxSource->InpxPath, records);
    WriteInpxArchive(config.InpxSource->ArchiveRoot / "fb2-main.zip", payloadTitles);

    {
        config.MaxSteadyStateMemoryBytes = highMemoryBudgetBytes;
        InpxWebReader::Application::CInpxCatalogApplication app(config);
        const auto jobId = app.StartInpxScan(
            {.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::InitialScan});
        REQUIRE(WaitForApplicationScan(app, jobId, InpxScanWaitTimeout()));
        const auto result = app.GetInpxScanJobResult(jobId);
        REQUIRE(result.has_value());
        REQUIRE(result->Snapshot.Status == InpxWebReader::ApplicationJobs::EInpxScanJobStatus::Completed);
        REQUIRE(result->Snapshot.ParsedFb2Records == recordCount);
    }

    const auto databasePath = config.CacheRoot / "Database" / "inpx-web-reader.db";
    {
        InpxWebReader::ApplicationJobs::CInpxScanJobService service(
            sandbox.GetPath() / "runtime-low-memory",
            {},
            databasePath,
            config.CacheRoot,
            nullptr,
            1,
            128ull * 1024ull * 1024ull,
            lowMemoryBudgetBytes);
        const auto jobId = service.Start(
            *config.InpxSource,
            {.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::Rescan});
        REQUIRE(service.Wait(jobId, InpxScanWaitTimeout()));
        const auto result = service.TryGetResult(jobId);
        REQUIRE(result.has_value());
        REQUIRE(result->Snapshot.Status == InpxWebReader::ApplicationJobs::EInpxScanJobStatus::Failed);
        REQUIRE(result->Snapshot.Message.find("record count exceeds the configured scan limit")
                != std::string::npos);
    }

    {
        InpxWebReader::ApplicationJobs::CInpxScanJobService service(
            sandbox.GetPath() / "runtime-high-memory",
            {},
            databasePath,
            config.CacheRoot,
            nullptr,
            1,
            128ull * 1024ull * 1024ull,
            highMemoryBudgetBytes);
        const auto jobId = service.Start(
            *config.InpxSource,
            {.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::Rescan});
        REQUIRE(service.Wait(jobId, InpxScanWaitTimeout()));
        const auto result = service.TryGetResult(jobId);
        REQUIRE(result.has_value());
        REQUIRE(result->Snapshot.Status == InpxWebReader::ApplicationJobs::EInpxScanJobStatus::Completed);
        REQUIRE(result->Snapshot.SegmentsUnchanged == 1);
        REQUIRE(result->Snapshot.ReusedRecords == recordCount);
    }
}

TEST_CASE("INPX scan job service prepares covers on payload worker threads", "[application-jobs][inpx][covers][perf]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-scan-cover-worker-processing");
    constexpr std::size_t bookCount = 24;
    const auto slowExecutorTimeout = InpxScanWaitTimeout();
    auto config = MakeInpxConfig(sandbox, bookCount);
    WriteInpxArchive(config.InpxSource->ArchiveRoot / "fb2-main.zip", bookCount, true);
    {
        InpxWebReader::Application::CInpxCatalogApplication app(config);
    }

    std::mutex scanThreadMutex;
    std::optional<std::thread::id> scanThreadId = std::nullopt;
    const auto recordScanThread = [&]() {
        std::lock_guard lock(scanThreadMutex);
        scanThreadId = std::this_thread::get_id();
    };
    CRecordingCoverImageProcessor coverImageProcessor;

    const auto databasePath = config.CacheRoot / "Database" / "inpx-web-reader.db";
    InpxWebReader::ApplicationJobs::CInpxScanJobService service(
        sandbox.GetPath() / "runtime-cover-worker-processing",
        {
            .AfterRecordProcessed = recordScanThread
        },
        databasePath,
        config.CacheRoot,
        &coverImageProcessor);

    const auto jobId = service.Start(
        *config.InpxSource,
        {.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::InitialScan});
    REQUIRE(jobId != 0);
    REQUIRE(service.Wait(jobId, slowExecutorTimeout));

    const auto result = service.TryGetResult(jobId);
    REQUIRE(result.has_value());
    REQUIRE(result->Snapshot.Status == InpxWebReader::ApplicationJobs::EInpxScanJobStatus::Completed);
    REQUIRE(CountBooksWithCoverPath(databasePath) == bookCount);

    const auto coverProcessingThreadIds = coverImageProcessor.GetThreadIds();
    std::optional<std::thread::id> observedScanThreadId = std::nullopt;
    {
        std::lock_guard lock(scanThreadMutex);
        observedScanThreadId = scanThreadId;
    }

    const std::size_t hardwareConcurrency = std::thread::hardware_concurrency();
    const std::size_t expectedMaxWorkerThreads = (std::max<std::size_t>)(
        1,
        (std::min<std::size_t>)(8, hardwareConcurrency == 0 ? 2 : hardwareConcurrency));
    const std::unordered_set<std::thread::id> uniqueCoverWorkerThreads(
        coverProcessingThreadIds.begin(),
        coverProcessingThreadIds.end());

    REQUIRE(coverProcessingThreadIds.size() == bookCount);
    REQUIRE(uniqueCoverWorkerThreads.size() <= expectedMaxWorkerThreads);
    REQUIRE(observedScanThreadId.has_value());
    REQUIRE(std::all_of(
        coverProcessingThreadIds.begin(),
        coverProcessingThreadIds.end(),
        [observedScanThreadId](const std::thread::id coverThreadId) {
            return coverThreadId != *observedScanThreadId;
        }));
}

TEST_CASE("INPX scan job service honors configured payload worker limit", "[application-jobs][inpx][covers][perf]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-scan-configured-worker-limit");
    constexpr std::size_t bookCount = 12;
    auto config = MakeInpxConfig(sandbox, bookCount);
    WriteInpxArchive(config.InpxSource->ArchiveRoot / "fb2-main.zip", bookCount, true);
    {
        InpxWebReader::Application::CInpxCatalogApplication app(config);
    }

    CRecordingCoverImageProcessor coverImageProcessor;
    const auto databasePath = config.CacheRoot / "Database" / "inpx-web-reader.db";
    InpxWebReader::ApplicationJobs::CInpxScanJobService service(
        sandbox.GetPath() / "runtime-configured-worker-limit",
        {},
        databasePath,
        config.CacheRoot,
        &coverImageProcessor,
        1);

    const auto jobId = service.Start(
        *config.InpxSource,
        {.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::InitialScan});
    REQUIRE(jobId != 0);
    REQUIRE(service.Wait(jobId, InpxScanWaitTimeout()));

    const auto result = service.TryGetResult(jobId);
    REQUIRE(result.has_value());
    REQUIRE(result->Snapshot.Status == InpxWebReader::ApplicationJobs::EInpxScanJobStatus::Completed);

    const auto coverProcessingThreadIds = coverImageProcessor.GetThreadIds();
    const std::unordered_set<std::thread::id> uniqueCoverWorkerThreads(
        coverProcessingThreadIds.begin(),
        coverProcessingThreadIds.end());

    REQUIRE(coverProcessingThreadIds.size() == bookCount);
    REQUIRE(uniqueCoverWorkerThreads.size() == 1);
}

TEST_CASE("INPX scan job service does not publish prepared covers before commit", "[application-jobs][inpx][covers]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-scan-cover-streaming");
    auto config = MakeInpxConfig(sandbox, 3);
    WriteInpxArchive(config.InpxSource->ArchiveRoot / "fb2-main.zip", 3, true);
    {
        InpxWebReader::Application::CInpxCatalogApplication app(config);
    }

    const auto databasePath = config.CacheRoot / "Database" / "inpx-web-reader.db";
    std::mutex finalizationMutex;
    std::condition_variable finalizationCondition;
    bool finalizationReached = false;
    bool releaseFinalization = false;
    InpxWebReader::ApplicationJobs::CInpxScanJobService service(
        sandbox.GetPath() / "runtime-streaming",
        {
            .BeforeCatalogCommit = [&]() {
                std::unique_lock lock(finalizationMutex);
                finalizationReached = true;
                finalizationCondition.notify_all();
                finalizationCondition.wait(lock, [&]() {
                    return releaseFinalization;
                });
            }
        },
        databasePath,
        config.CacheRoot);

    const auto jobId = service.Start(
        *config.InpxSource,
        {.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::InitialScan});
    REQUIRE(jobId != 0);

    std::unique_lock finalizationLock(finalizationMutex);
    const bool reachedFinalization = finalizationCondition.wait_for(
        finalizationLock,
        InpxScanWaitTimeout(),
        [&]() {
            return finalizationReached;
        });
    finalizationLock.unlock();

    std::optional<InpxWebReader::ApplicationJobs::SInpxScanJobSnapshot> snapshot = std::nullopt;
    std::int64_t visibleCoverRows = 0;
    std::size_t visibleCoverFiles = 0;
    try
    {
        if (reachedFinalization)
        {
            snapshot = service.TryGetSnapshot(jobId);
            visibleCoverRows = CountBooksWithCoverPath(databasePath);
            visibleCoverFiles = CountCoverFiles(config.CacheRoot);
        }
    }
    catch (...)
    {
        std::lock_guard lock(finalizationMutex);
        releaseFinalization = true;
        finalizationCondition.notify_all();
        throw;
    }

    {
        std::lock_guard lock(finalizationMutex);
        releaseFinalization = true;
    }
    finalizationCondition.notify_all();

    REQUIRE(reachedFinalization);
    REQUIRE(snapshot.has_value());
    REQUIRE_FALSE(snapshot->IsTerminal());
    REQUIRE(visibleCoverRows == 0);
    REQUIRE(visibleCoverFiles == 1);
    REQUIRE(service.Wait(jobId, InpxScanWaitTimeout()));

    const auto result = service.TryGetResult(jobId);
    REQUIRE(result.has_value());
    REQUIRE(result->Snapshot.Status == InpxWebReader::ApplicationJobs::EInpxScanJobStatus::Completed);
    REQUIRE(CountBooksWithCoverPath(databasePath) == 3);
}

TEST_CASE("CInpxCatalogApplication removes stale INPX cover cache on rescan", "[application-jobs][inpx][covers]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-scan-cover-cache-stale");
    auto config = MakeInpxConfig(sandbox, 1);
    WriteInpxArchive(config.InpxSource->ArchiveRoot / "fb2-main.zip", 1, true);
    InpxWebReader::Application::CInpxCatalogApplication app(config);

    const auto initialJobId = app.StartInpxScan({.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::InitialScan});
    REQUIRE(WaitForApplicationScan(app, initialJobId, InpxScanWaitTimeout()));

    const auto databasePath = config.CacheRoot / "Database" / "inpx-web-reader.db";
    const auto initialCoverPath = ReadCoverPath(databasePath, "1");
    REQUIRE(initialCoverPath.has_value());
    const auto initialCoverAbsolutePath = config.CacheRoot / InpxWebReader::Unicode::PathFromUtf8(*initialCoverPath);
    REQUIRE(std::filesystem::is_regular_file(initialCoverAbsolutePath));
    const auto initialStatistics = app.GetCatalogStatistics();
    const auto initialInpxSourceSize = static_cast<std::uint64_t>(
        std::filesystem::file_size(config.InpxSource->InpxPath)
        + std::filesystem::file_size(config.InpxSource->ArchiveRoot / "fb2-main.zip"));
    const auto initialDatabaseSize =
        InpxWebReader::BookDatabase::CCatalogStatisticsMaintenance::GetDatabaseFootprintSize(databasePath);
    REQUIRE(initialStatistics.BookCount == 1);
    REQUIRE(initialStatistics.InpxSourceSizeBytes == initialInpxSourceSize);
    REQUIRE(
        initialStatistics.TotalCatalogSizeBytes
        == initialInpxSourceSize + SumCoverFileBytes(config.CacheRoot) + initialDatabaseSize);

    WriteInpxArchive(config.InpxSource->ArchiveRoot / "fb2-main.zip", 1, false);
    WriteInpxArchive(
        config.InpxSource->InpxPath,
        MakeInpxRecord("book-1", "1", "ru", "changed", false, "fb2", 0));
    const auto rescanJobId = app.StartInpxScan({.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::Rescan});
    REQUIRE(WaitForApplicationScan(app, rescanJobId, InpxScanWaitTimeout()));

    const auto coverPathAfterRescan = ReadCoverPath(databasePath, "1");
    REQUIRE_FALSE(coverPathAfterRescan.has_value());
    REQUIRE_FALSE(std::filesystem::exists(initialCoverAbsolutePath));

    const auto statisticsAfterRescan = app.GetCatalogStatistics();
    const auto inpxSourceSizeAfterRescan = static_cast<std::uint64_t>(
        std::filesystem::file_size(config.InpxSource->InpxPath)
        + std::filesystem::file_size(config.InpxSource->ArchiveRoot / "fb2-main.zip"));
    const auto databaseSizeAfterRescan =
        InpxWebReader::BookDatabase::CCatalogStatisticsMaintenance::GetDatabaseFootprintSize(databasePath);
    REQUIRE(statisticsAfterRescan.BookCount == 1);
    REQUIRE(statisticsAfterRescan.UnavailableBookCount == 0);
    REQUIRE(statisticsAfterRescan.InpxSourceSizeBytes == inpxSourceSizeAfterRescan);
    REQUIRE(SumCoverFileBytes(config.CacheRoot) == 0);
    REQUIRE(statisticsAfterRescan.TotalCatalogSizeBytes == inpxSourceSizeAfterRescan + databaseSizeAfterRescan);
}

TEST_CASE("CInpxCatalogApplication rejects a corrupted content-addressed cover instead of trusting its path", "[application-jobs][inpx][covers]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-scan-corrupt-content-cover");
    auto config = MakeInpxConfig(sandbox, 1);
    WriteInpxArchive(config.InpxSource->ArchiveRoot / "fb2-main.zip", 1, true);
    InpxWebReader::Application::CInpxCatalogApplication app(config);
    const auto initialJobId = app.StartInpxScan({.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::InitialScan});
    REQUIRE(WaitForApplicationScan(app, initialJobId, InpxScanWaitTimeout()));

    const auto databasePath = config.CacheRoot / "Database" / "inpx-web-reader.db";
    const auto storedCoverPath = ReadCoverPath(databasePath, "1");
    REQUIRE(storedCoverPath.has_value());
    const auto absoluteCoverPath = config.CacheRoot / InpxWebReader::Unicode::PathFromUtf8(*storedCoverPath);
    {
        std::ofstream output(absoluteCoverPath, std::ios::binary | std::ios::trunc);
        REQUIRE(output.good());
        output << "corrupt";
        REQUIRE(output.good());
    }
    const std::string storedSourceFingerprint = ReadSourceFingerprint(databasePath);
    WriteInpxArchive(
        config.InpxSource->InpxPath,
        MakeInpxRecord("book-1", "1", "ru", "changed"));

    const auto rescanJobId = app.StartInpxScan({.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::Rescan});
    REQUIRE(WaitForApplicationScan(app, rescanJobId, InpxScanWaitTimeout()));
    const auto result = app.GetInpxScanJobResult(rescanJobId);
    REQUIRE(result.has_value());
    REQUIRE(result->Snapshot.Status == InpxWebReader::ApplicationJobs::EInpxScanJobStatus::Failed);
    REQUIRE(result->Snapshot.Message.find("does not match its SHA-256 path") != std::string::npos);
    REQUIRE(ReadSourceFingerprint(databasePath) == storedSourceFingerprint);
}

TEST_CASE("CInpxCatalogApplication does not delete unsafe stored INPX cover paths", "[application-jobs][inpx][covers]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-scan-cover-cache-unsafe");
    const auto config = MakeInpxConfig(sandbox, 1);
    InpxWebReader::Application::CInpxCatalogApplication app(config);

    const auto initialJobId = app.StartInpxScan({.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::InitialScan});
    REQUIRE(WaitForApplicationScan(app, initialJobId, InpxScanWaitTimeout()));

    const auto databasePath = config.CacheRoot / "Database" / "inpx-web-reader.db";
    const auto outsidePath = sandbox.GetPath() / "outside-cover.bin";
    {
        std::ofstream output(outsidePath, std::ios::binary);
        output << "outside";
    }
    REQUIRE(std::filesystem::is_regular_file(outsidePath));
    UpdateCoverPath(databasePath, "1", "../outside-cover.bin");
    WriteInpxArchive(
        config.InpxSource->InpxPath,
        MakeInpxRecord("book-1", "1", "ru", "changed", false, "fb2", 0));

    const auto rescanJobId = app.StartInpxScan({.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::Rescan});
    REQUIRE(WaitForApplicationScan(app, rescanJobId, InpxScanWaitTimeout()));

    REQUIRE_FALSE(ReadCoverPath(databasePath, "1").has_value());
    REQUIRE(std::filesystem::is_regular_file(outsidePath));
}

TEST_CASE("CInpxCatalogApplication writes server logs for INPX scans", "[application-jobs][inpx][logging]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-scan-logging");
    auto config = MakeInpxConfig(sandbox, 2);
    WriteInpxArchive(config.InpxSource->ArchiveRoot / "fb2-main.zip", 2, true);
    const auto logPath = sandbox.GetPath() / "inpx-scan.log";
    InpxWebReader::ApplicationJobs::TInpxScanJobId jobId = 0;

    InpxWebReader::Logging::CLogging::InitializeHostLogger(logPath);
    try
    {
        InpxWebReader::Application::CInpxCatalogApplication app(config);
        jobId = app.StartInpxScan({.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::InitialScan});
        REQUIRE(WaitForApplicationScan(app, jobId, InpxScanWaitTimeout()));
        const auto result = app.GetInpxScanJobResult(jobId);
        REQUIRE(result.has_value());
        REQUIRE(result->Snapshot.Status == InpxWebReader::ApplicationJobs::EInpxScanJobStatus::Completed);
        InpxWebReader::Logging::CLogging::Shutdown();
    }
    catch (...)
    {
        InpxWebReader::Logging::CLogging::Shutdown();
        throw;
    }

    const std::string logText = ReadAllText(logPath);
    REQUIRE_THAT(logText, Catch::Matchers::ContainsSubstring("[Server]"));
    REQUIRE_THAT(logText, Catch::Matchers::ContainsSubstring("INPX scan job queued"));
    REQUIRE_THAT(logText, Catch::Matchers::ContainsSubstring("jobId=" + std::to_string(jobId)));
    REQUIRE_THAT(logText, Catch::Matchers::ContainsSubstring("INPX initial scan started"));
    REQUIRE_THAT(logText, Catch::Matchers::ContainsSubstring("payloadWorkers="));
    REQUIRE_THAT(logText, Catch::Matchers::ContainsSubstring("INPX scan progress"));
    REQUIRE_THAT(logText, Catch::Matchers::ContainsSubstring("INPX scan archive started"));
    REQUIRE_THAT(logText, Catch::Matchers::ContainsSubstring("INPX initial scan completed"));
    REQUIRE_THAT(logText, Catch::Matchers::ContainsSubstring("deleted=0 skipped=0 scanWarnings=0"));
    REQUIRE_THAT(logText, Catch::Matchers::ContainsSubstring("parserWarnings=0 coverWarnings=0"));
    REQUIRE_THAT(logText, Catch::Matchers::ContainsSubstring("metadataFallbacks=0 titleFallbacks=0 authorFallbacks=0 languageFallbacks=0"));
    REQUIRE_THAT(logText, Catch::Matchers::ContainsSubstring("[scan-perf] archive"));
    REQUIRE_THAT(logText, Catch::Matchers::ContainsSubstring("[scan-perf] SUMMARY"));
    REQUIRE_THAT(logText, Catch::Matchers::ContainsSubstring("added=2 updated=0 failed=0"));
    REQUIRE(logText.find(" dup=") == std::string::npos);
    REQUIRE_THAT(logText, Catch::Matchers::ContainsSubstring("covers=2 coverBytes="));
    REQUIRE_THAT(logText, Catch::Matchers::ContainsSubstring("segments=0/1/0/0 archivesSkipped=0 archivesOpened=1"));
    REQUIRE(logText.find("cover: INPX cached") == std::string::npos);
}

TEST_CASE("INPX scan captures compact progress log views without warning vectors", "[application-jobs][inpx][logging][perf]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-scan-progress-log-view-capture");
    auto config = MakeInpxConfig(sandbox, 1);
    std::string records;
    for (std::size_t index = 0; index < 10'005; ++index)
    {
        records += MakeInpxRecord(
            "unsupported-" + std::to_string(index),
            std::to_string(index + 1),
            "ru",
            "",
            false,
            "txt");
    }
    WriteInpxArchive(config.InpxSource->InpxPath, records);

    std::atomic_size_t progressLogViewCaptures = 0;
    std::atomic_size_t sourceWarningsAtLastCapture = 0;
    InpxWebReader::ApplicationJobs::CInpxScanJobService service(
        sandbox.GetPath() / "runtime-progress-log-view-capture",
        {
            .AfterProgressLogViewCaptured = [&](const std::size_t sourceWarningCount) {
                sourceWarningsAtLastCapture.store(sourceWarningCount, std::memory_order_relaxed);
                progressLogViewCaptures.fetch_add(1, std::memory_order_relaxed);
            }
        });
    const auto jobId = service.Start(
        *config.InpxSource,
        {
            .Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::InitialScan,
            .WarningLimit = 1'000
        });
    REQUIRE(service.Wait(jobId, InpxScanWaitTimeout()));

    const auto result = service.TryGetResult(jobId);
    REQUIRE(result.has_value());
    REQUIRE(result->Snapshot.Status == InpxWebReader::ApplicationJobs::EInpxScanJobStatus::Completed);
    REQUIRE(result->Snapshot.ScannedRecords == 10'005);
    REQUIRE(result->Snapshot.SkippedRecords == 10'005);
    REQUIRE(result->Snapshot.Warnings.size() == 1'000);
    REQUIRE(progressLogViewCaptures.load(std::memory_order_relaxed) == 2);
    REQUIRE(sourceWarningsAtLastCapture.load(std::memory_order_relaxed) == 1'000);
}

TEST_CASE("CInpxCatalogApplication rejects malformed INPX records with source context", "[application-jobs][inpx][logging]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-scan-warning-logging");
    auto config = MakeBaseConfig(sandbox);
    config.InpxSource = InpxWebReader::Application::SInpxSourceInfo{
        .InpxPath = WriteInpxArchive(
            sandbox.GetPath() / "source" / "catalog.inpx",
            "malformed-record-without-field-separators\n"),
        .ArchiveRoot = sandbox.GetPath() / "source" / "archives"
    };
    std::filesystem::create_directories(config.InpxSource->ArchiveRoot);
    WriteInpxArchive(config.InpxSource->ArchiveRoot / "fb2-main.zip", 1);

    const auto logPath = sandbox.GetPath() / "inpx-scan-warning.log";
    InpxWebReader::ApplicationJobs::TInpxScanJobId jobId = 0;
    InpxWebReader::Logging::CLogging::InitializeHostLogger(logPath);
    try
    {
        InpxWebReader::Application::CInpxCatalogApplication app(config);
        jobId = app.StartInpxScan({.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::InitialScan});
        REQUIRE(WaitForApplicationScan(app, jobId, InpxScanWaitTimeout()));
        const auto result = app.GetInpxScanJobResult(jobId);
        REQUIRE(result.has_value());
        REQUIRE(result->Snapshot.Status == InpxWebReader::ApplicationJobs::EInpxScanJobStatus::Failed);
        REQUIRE_FALSE(result->ScanResult.has_value());
        InpxWebReader::Logging::CLogging::Shutdown();
    }
    catch (...)
    {
        InpxWebReader::Logging::CLogging::Shutdown();
        throw;
    }

    const std::string logText = ReadAllText(logPath);
    REQUIRE_THAT(logText, Catch::Matchers::ContainsSubstring("INPX scan failed"));
    REQUIRE_THAT(logText, Catch::Matchers::ContainsSubstring("jobId=" + std::to_string(jobId)));
    REQUIRE_THAT(logText, Catch::Matchers::ContainsSubstring("fb2-main.zip.inp"));
    REQUIRE_THAT(logText, Catch::Matchers::ContainsSubstring("expected 15 fields"));
}

TEST_CASE("CInpxCatalogApplication keeps paths and FB2 previews in parser failure logs", "[application-jobs][inpx][logging]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-fb2-preview-logging");
    auto config = MakeInpxConfig(sandbox, 1);
    WriteSinglePayloadInpxArchive(
        config.InpxSource->ArchiveRoot / "fb2-main.zip",
        "<FictionBook><description><title-info><book-title>Диагностический фрагмент");
    const auto logPath = sandbox.GetPath() / "inpx-fb2-preview.log";

    InpxWebReader::Logging::CLogging::InitializeHostLogger(logPath);
    try
    {
        InpxWebReader::Application::CInpxCatalogApplication app(config);
        const auto jobId = app.StartInpxScan({.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::InitialScan});
        REQUIRE(WaitForApplicationScan(app, jobId, InpxScanWaitTimeout()));
        const auto result = app.GetInpxScanJobResult(jobId);
        REQUIRE(result.has_value());
        REQUIRE(result->Snapshot.Status == InpxWebReader::ApplicationJobs::EInpxScanJobStatus::Failed);
        InpxWebReader::Logging::CLogging::Shutdown();
    }
    catch (...)
    {
        InpxWebReader::Logging::CLogging::Shutdown();
        throw;
    }

    const std::string logText = ReadAllText(logPath);
    REQUIRE_THAT(
        logText,
        Catch::Matchers::ContainsSubstring(InpxWebReader::Unicode::PathToUtf8(config.InpxSource->InpxPath)));
    REQUIRE_THAT(logText, Catch::Matchers::ContainsSubstring("fb2-main.zip!book-1.fb2"));
    REQUIRE_THAT(logText, Catch::Matchers::ContainsSubstring("xml_preview=\""));
    REQUIRE_THAT(logText, Catch::Matchers::ContainsSubstring("Диагностический фрагмент"));
}
