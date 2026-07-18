#include <catch2/catch_test_macros.hpp>

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

TEST_CASE("CInpxCatalogApplication skips unchanged INPX rows during rescan", "[application-jobs][inpx]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-scan-unchanged-incremental");
    const auto config = MakeInpxConfig(sandbox, 2);
    InpxWebReader::Application::CInpxCatalogApplication app(config);

    const auto initialJobId = app.StartInpxScan({.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::InitialScan});
    REQUIRE(WaitForApplicationScan(app, initialJobId, InpxScanWaitTimeout()));

    const auto initialResult = app.GetInpxScanJobResult(initialJobId);
    REQUIRE(initialResult.has_value());
    REQUIRE(initialResult->ScanResult.has_value());
    REQUIRE(initialResult->Snapshot.ParsedFb2Records == 2);
    REQUIRE(initialResult->Snapshot.AddedRecords == 2);

    const auto rescanJobId = app.StartInpxScan({.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::Rescan});
    REQUIRE(WaitForApplicationScan(app, rescanJobId, InpxScanWaitTimeout()));

    const auto rescanResult = app.GetInpxScanJobResult(rescanJobId);
    REQUIRE(rescanResult.has_value());
    REQUIRE(rescanResult->Snapshot.Status == InpxWebReader::ApplicationJobs::EInpxScanJobStatus::Completed);
    REQUIRE(rescanResult->ScanResult.has_value());
    REQUIRE(rescanResult->Snapshot.TotalRecords == 2);
    REQUIRE(rescanResult->Snapshot.ScannedRecords == 2);
    REQUIRE(rescanResult->Snapshot.ParsedFb2Records == 0);
    REQUIRE(rescanResult->Snapshot.AddedRecords == 0);
    REQUIRE(rescanResult->Snapshot.UpdatedRecords == 0);
    REQUIRE(rescanResult->Snapshot.MarkedUnavailableRecords == 0);
    REQUIRE(rescanResult->Snapshot.ReusedRecords == 2);
    REQUIRE(rescanResult->Snapshot.SegmentsUnchanged == 1);
    REQUIRE(rescanResult->Snapshot.ArchivesSkipped == 1);
    REQUIRE(rescanResult->Snapshot.ArchivesOpened == 0);
    REQUIRE(rescanResult->Snapshot.ArchiveBytesRead == 0);

    const auto listAfterRescan = app.ListBooks({.Limit = 10});
    REQUIRE(listAfterRescan.TotalCount == 2);
    REQUIRE(listAfterRescan.Items.size() == 2);
    REQUIRE(listAfterRescan.Items[0].TitleUtf8 == "Payload Title 1");
    REQUIRE(listAfterRescan.Items[1].TitleUtf8 == "Payload Title 2");
}

TEST_CASE("CInpxCatalogApplication validates payloads when a repacked archive has the same manifest", "[application-jobs][inpx]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-scan-repacked-archive");
    const auto config = MakeInpxConfig(sandbox, 1);
    InpxWebReader::Application::CInpxCatalogApplication app(config);
    const auto initialJobId = app.StartInpxScan({.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::InitialScan});
    REQUIRE(WaitForApplicationScan(app, initialJobId, InpxScanWaitTimeout()));

    const auto archivePath = config.InpxSource->ArchiveRoot / "fb2-main.zip";
    const auto oldMtime = std::filesystem::last_write_time(archivePath);
    WriteInpxArchive(archivePath, 1);
    std::filesystem::last_write_time(archivePath, oldMtime + std::chrono::seconds(2));

    const auto rescanJobId = app.StartInpxScan({.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::Rescan});
    REQUIRE(WaitForApplicationScan(app, rescanJobId, InpxScanWaitTimeout()));
    const auto result = app.GetInpxScanJobResult(rescanJobId);
    REQUIRE(result.has_value());
    REQUIRE(result->Snapshot.Status == InpxWebReader::ApplicationJobs::EInpxScanJobStatus::Completed);
    REQUIRE(result->Snapshot.ParsedFb2Records == 0);
    REQUIRE(result->Snapshot.ReusedRecords == 1);
    REQUIRE(result->Snapshot.SegmentsUnchanged == 1);
    REQUIRE(result->Snapshot.ArchivesSkipped == 1);
    REQUIRE(result->Snapshot.ArchivesOpened == 1);
    REQUIRE(result->Snapshot.ArchiveBytesRead > 0);
}

TEST_CASE("CInpxCatalogApplication validates one resolved archive once for aliased INP segments", "[application-jobs][inpx][perf]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-scan-shared-resolved-archive");
    auto config = MakeBaseConfig(sandbox);
    config.InpxSource = InpxWebReader::Application::SInpxSourceInfo{
        .InpxPath = WriteInpxArchiveEntries(
            sandbox.GetPath() / "source" / "catalog.inpx",
            {
                {"fb2-main.inp", MakeInpxRecord("book-1", "1")},
                {"fb2-main.zip.inp", MakeInpxRecord("book-2", "2")}
            }),
        .ArchiveRoot = sandbox.GetPath() / "source" / "archives"
    };
    const auto archivePath = config.InpxSource->ArchiveRoot / "fb2-main.zip";
    WriteInpxArchive(archivePath, 2);

    InpxWebReader::Application::CInpxCatalogApplication app(config);
    const auto initialJobId = app.StartInpxScan({.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::InitialScan});
    REQUIRE(WaitForApplicationScan(app, initialJobId, InpxScanWaitTimeout()));
    const auto initialResult = app.GetInpxScanJobResult(initialJobId);
    REQUIRE(initialResult.has_value());
    REQUIRE(initialResult->Snapshot.Status == InpxWebReader::ApplicationJobs::EInpxScanJobStatus::Completed);
    REQUIRE(initialResult->Snapshot.SegmentsAdded == 2);
    REQUIRE(initialResult->Snapshot.ArchivesOpened == 1);

    const auto oldMtime = std::filesystem::last_write_time(archivePath);
    WriteInpxArchive(archivePath, 2);
    std::filesystem::last_write_time(archivePath, oldMtime + std::chrono::seconds(2));

    const auto rescanJobId = app.StartInpxScan({.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::Rescan});
    REQUIRE(WaitForApplicationScan(app, rescanJobId, InpxScanWaitTimeout()));
    const auto result = app.GetInpxScanJobResult(rescanJobId);
    REQUIRE(result.has_value());
    REQUIRE(result->Snapshot.Status == InpxWebReader::ApplicationJobs::EInpxScanJobStatus::Completed);
    REQUIRE(result->Snapshot.SegmentsUnchanged == 2);
    REQUIRE(result->Snapshot.ReusedRecords == 2);
    REQUIRE(result->Snapshot.ArchivesOpened == 1);
    REQUIRE(result->Snapshot.ArchiveBytesRead
        == MakeFb2Payload("Payload Title 1", false).size()
            + MakeFb2Payload("Payload Title 2", false).size());
}

TEST_CASE("INPX scan groups non-adjacent segments by resolved archive", "[application-jobs][inpx][perf][complexity]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-scan-non-adjacent-shared-archive");
    auto config = MakeBaseConfig(sandbox);
    config.InpxSource = InpxWebReader::Application::SInpxSourceInfo{
        .InpxPath = WriteInpxArchiveEntries(sandbox.GetPath() / "source" / "catalog.inpx",
                                            {{"fb2-main.inp", MakeInpxRecord("book-1", "1")},
                                             {"fb2-extra.zip.inp", MakeInpxRecord("book-1", "2")},
                                             {"fb2-main.zip.inp", MakeInpxRecord("book-2", "3")}}),
        .ArchiveRoot = sandbox.GetPath() / "source" / "archives"};
    WriteInpxArchive(config.InpxSource->ArchiveRoot / "fb2-main.zip", std::vector<std::string>{"Main one", "Main two"});
    WriteInpxArchive(config.InpxSource->ArchiveRoot / "fb2-extra.zip", std::vector<std::string>{"Extra"});

    std::atomic_size_t executionArchiveOpenCount = 0;
    InpxWebReader::ApplicationJobs::CInpxScanJobService service(
        sandbox.GetPath() / "runtime",
        {.AfterExecutionArchiveOpened = [&]() { executionArchiveOpenCount.fetch_add(1, std::memory_order_relaxed); }});
    const auto jobId =
        service.Start(*config.InpxSource, {.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::InitialScan});

    REQUIRE(service.Wait(jobId, InpxScanWaitTimeout()));
    const auto result = service.TryGetResult(jobId);
    REQUIRE(result.has_value());
    REQUIRE(result->Snapshot.Status == InpxWebReader::ApplicationJobs::EInpxScanJobStatus::Completed);
    REQUIRE(result->Snapshot.ParsedFb2Records == 3);
    REQUIRE(executionArchiveOpenCount.load(std::memory_order_relaxed) == 2);
}

TEST_CASE("INPX scan builds one archive fallback snapshot for many segments",
          "[application-jobs][inpx][perf][complexity]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-scan-shared-archive-root-index");
    constexpr std::size_t segmentCount = 24;
    std::vector<std::pair<std::string, std::string>> inpxEntries;
    inpxEntries.reserve(segmentCount);
    for (std::size_t index = 0; index < segmentCount; ++index)
    {
        inpxEntries.emplace_back("archive-" + std::to_string(index) + ".inp",
                                 MakeInpxRecord("book-1", std::to_string(index + 1)));
    }

    auto config = MakeBaseConfig(sandbox);
    config.InpxSource = InpxWebReader::Application::SInpxSourceInfo{
        .InpxPath = WriteInpxArchiveEntries(sandbox.GetPath() / "source" / "catalog.inpx", inpxEntries),
        .ArchiveRoot = sandbox.GetPath() / "source" / "archives"};
    for (std::size_t index = 0; index < segmentCount; ++index)
    {
        WriteInpxArchive(config.InpxSource->ArchiveRoot / ("payload-" + std::to_string(index))
                             / ("archive-" + std::to_string(index) + ".zip"),
                         std::vector<std::string>{"Payload " + std::to_string(index)});
    }

    std::atomic_size_t fallbackSnapshotBuildCount = 0;
    std::atomic_size_t archiveRootEntryVisitCount = 0;
    InpxWebReader::ApplicationJobs::CInpxScanJobService service(
        sandbox.GetPath() / "runtime",
        {.OnArchiveFallbackSnapshotBuild =
             [&]() { fallbackSnapshotBuildCount.fetch_add(1, std::memory_order_relaxed); },
         .OnArchiveRootEntryVisited = [&]() { archiveRootEntryVisitCount.fetch_add(1, std::memory_order_relaxed); }});
    const auto jobId =
        service.Start(*config.InpxSource, {.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::InitialScan});

    REQUIRE(service.Wait(jobId, InpxScanWaitTimeout()));
    const auto result = service.TryGetResult(jobId);
    REQUIRE(result.has_value());
    REQUIRE(result->Snapshot.Status == InpxWebReader::ApplicationJobs::EInpxScanJobStatus::Completed);
    REQUIRE(result->Snapshot.ParsedFb2Records == segmentCount);
    REQUIRE(fallbackSnapshotBuildCount.load(std::memory_order_relaxed) == 1);
    REQUIRE(archiveRootEntryVisitCount.load(std::memory_order_relaxed) == 2 * segmentCount);
}

TEST_CASE("CInpxCatalogApplication rejects conflicting guards for one resolved "
          "archive",
          "[application-jobs][inpx][fingerprint]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-scan-conflicting-shared-archive-guards");
    auto config = MakeBaseConfig(sandbox);
    config.InpxSource = InpxWebReader::Application::SInpxSourceInfo{
        .InpxPath = WriteInpxArchiveEntries(
            sandbox.GetPath() / "source" / "catalog.inpx",
            {{"fb2-main.inp", MakeInpxRecord("book-1", "1")}, {"fb2-main.zip.inp", MakeInpxRecord("book-2", "2")}}),
        .ArchiveRoot = sandbox.GetPath() / "source" / "archives"};
    WriteInpxArchive(config.InpxSource->ArchiveRoot / "fb2-main.zip", 2);

    InpxWebReader::Application::CInpxCatalogApplication app(config);
    const auto initialJobId = app.StartInpxScan({.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::InitialScan});
    REQUIRE(WaitForApplicationScan(app, initialJobId, InpxScanWaitTimeout()));

    const auto databasePath = config.CacheRoot / "Database" / "inpx-web-reader.db";
    {
        InpxWebReader::Sqlite::CSqliteConnection connection(databasePath);
        InpxWebReader::Sqlite::CSqliteStatement statement(
            connection.GetNativeHandle(),
            "UPDATE inpx_segments SET archive_manifest_fingerprint = ? WHERE "
            "inp_entry_name = ?;");
        statement.BindText(1, std::string(64, '0'));
        statement.BindText(2, "fb2-main.zip.inp");
        static_cast<void>(statement.Step());
    }

    const auto rescanJobId = app.StartInpxScan({.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::Rescan});
    REQUIRE(WaitForApplicationScan(app, rescanJobId, InpxScanWaitTimeout()));
    const auto result = app.GetInpxScanJobResult(rescanJobId);
    REQUIRE(result.has_value());
    REQUIRE(result->Snapshot.Status == InpxWebReader::ApplicationJobs::EInpxScanJobStatus::Failed);
    REQUIRE(result->Snapshot.Message.find("Conflicting cached archive manifests") != std::string::npos);
}

TEST_CASE("CInpxCatalogApplication reuses unchanged INP segments after the "
          "INPX container changes",
          "[application-jobs][inpx]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-scan-repacked-inpx");
    const auto config = MakeInpxConfig(sandbox, 1);
    InpxWebReader::Application::CInpxCatalogApplication app(config);
    const auto initialJobId = app.StartInpxScan({.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::InitialScan});
    REQUIRE(WaitForApplicationScan(app, initialJobId, InpxScanWaitTimeout()));

    const auto databasePath = config.CacheRoot / "Database" / "inpx-web-reader.db";
    const std::string initialFingerprint = ReadSourceFingerprint(databasePath);
    WriteInpxArchiveEntries(
        config.InpxSource->InpxPath,
        {{"fb2-main.zip.inp", MakeInpxRecord("book-1", "1")}, {"collection.info", "container metadata changed"}});

    const auto rescanJobId = app.StartInpxScan({.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::Rescan});
    REQUIRE(WaitForApplicationScan(app, rescanJobId, InpxScanWaitTimeout()));
    const auto result = app.GetInpxScanJobResult(rescanJobId);
    REQUIRE(result.has_value());
    REQUIRE(result->Snapshot.Status == InpxWebReader::ApplicationJobs::EInpxScanJobStatus::Completed);
    REQUIRE(result->Snapshot.ParsedFb2Records == 0);
    REQUIRE(result->Snapshot.ReusedRecords == 1);
    REQUIRE(result->Snapshot.SegmentsUnchanged == 1);
    REQUIRE(result->Snapshot.ArchivesSkipped == 1);
    REQUIRE(result->Snapshot.ArchivesOpened == 0);
    REQUIRE(result->Snapshot.ArchiveBytesRead == 0);
    REQUIRE(ReadSourceFingerprint(databasePath) != initialFingerprint);
}

TEST_CASE("CInpxCatalogApplication detects same-size archive payload changes with an unchanged INP", "[application-jobs][inpx]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-scan-same-size-payload-change");
    const auto config = MakeInpxConfig(sandbox, 1);
    InpxWebReader::Application::CInpxCatalogApplication app(config);
    const auto initialJobId = app.StartInpxScan({.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::InitialScan});
    REQUIRE(WaitForApplicationScan(app, initialJobId, InpxScanWaitTimeout()));

    const auto archivePath = config.InpxSource->ArchiveRoot / "fb2-main.zip";
    const auto oldMtime = std::filesystem::last_write_time(archivePath);
    WriteInpxArchive(archivePath, std::vector<std::string>{"Payload TItle 1"});
    std::filesystem::last_write_time(archivePath, oldMtime + std::chrono::seconds(2));

    const auto rescanJobId = app.StartInpxScan({.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::Rescan});
    REQUIRE(WaitForApplicationScan(app, rescanJobId, InpxScanWaitTimeout()));
    const auto result = app.GetInpxScanJobResult(rescanJobId);
    REQUIRE(result.has_value());
    REQUIRE(result->Snapshot.Status == InpxWebReader::ApplicationJobs::EInpxScanJobStatus::Completed);
    REQUIRE(result->Snapshot.ParsedFb2Records == 1);
    REQUIRE(result->Snapshot.UpdatedRecords == 1);
    REQUIRE(result->Snapshot.SegmentsChanged == 1);
    REQUIRE(result->Snapshot.ArchivesOpened == 1);
    REQUIRE(result->Snapshot.ArchiveBytesRead > 0);
    REQUIRE(app.ListBooks({.Limit = 10}).Items.front().TitleUtf8 == "Payload TItle 1");
}

TEST_CASE("CInpxCatalogApplication marks every book from a removed INP segment unavailable", "[application-jobs][inpx]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-scan-removed-segment");
    auto config = MakeBaseConfig(sandbox);
    config.InpxSource = InpxWebReader::Application::SInpxSourceInfo{
        .InpxPath = WriteInpxArchiveEntries(
            sandbox.GetPath() / "source" / "catalog.inpx",
            {
                {"fb2-main.zip.inp", MakeInpxRecord("book-1", "1")},
                {"fb2-extra.zip.inp", MakeInpxRecord("book-1", "2")}
            }),
        .ArchiveRoot = sandbox.GetPath() / "source" / "archives"
    };
    WriteInpxArchive(config.InpxSource->ArchiveRoot / "fb2-main.zip", std::vector<std::string>{"Main"});
    WriteInpxArchive(config.InpxSource->ArchiveRoot / "fb2-extra.zip", std::vector<std::string>{"Extra"});
    InpxWebReader::Application::CInpxCatalogApplication app(config);
    const auto initialJobId = app.StartInpxScan({.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::InitialScan});
    REQUIRE(WaitForApplicationScan(app, initialJobId, InpxScanWaitTimeout()));

    WriteInpxArchiveEntries(
        config.InpxSource->InpxPath,
        {{"fb2-main.zip.inp", MakeInpxRecord("book-1", "1")}});
    std::filesystem::remove(config.InpxSource->ArchiveRoot / "fb2-extra.zip");
    const auto rescanJobId = app.StartInpxScan({.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::Rescan});
    REQUIRE(WaitForApplicationScan(app, rescanJobId, InpxScanWaitTimeout()));

    const auto result = app.GetInpxScanJobResult(rescanJobId);
    REQUIRE(result.has_value());
    REQUIRE(result->Snapshot.Status == InpxWebReader::ApplicationJobs::EInpxScanJobStatus::Completed);
    REQUIRE(result->Snapshot.SegmentsUnchanged == 1);
    REQUIRE(result->Snapshot.SegmentsRemoved == 1);
    REQUIRE(result->Snapshot.ParsedFb2Records == 0);
    REQUIRE(result->Snapshot.MarkedUnavailableRecords == 1);
    const auto databasePath = config.CacheRoot / "Database" / "inpx-web-reader.db";
    REQUIRE(ReadInpxBookAvailability(databasePath, "1") == "available");
    REQUIRE(ReadInpxBookAvailability(databasePath, "2") == "missing_from_index");

    WriteInpxArchiveEntries(
        config.InpxSource->InpxPath,
        {
            {"fb2-main.zip.inp", MakeInpxRecord("book-1", "1")},
            {"fb2-extra.zip.inp", MakeInpxRecord("book-1", "2")}
        });
    WriteInpxArchive(config.InpxSource->ArchiveRoot / "fb2-extra.zip", std::vector<std::string>{"Extra"});
    const auto restoreJobId = app.StartInpxScan({.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::Rescan});
    REQUIRE(WaitForApplicationScan(app, restoreJobId, InpxScanWaitTimeout()));

    const auto restoreResult = app.GetInpxScanJobResult(restoreJobId);
    REQUIRE(restoreResult.has_value());
    REQUIRE(restoreResult->Snapshot.Status == InpxWebReader::ApplicationJobs::EInpxScanJobStatus::Completed);
    REQUIRE(restoreResult->Snapshot.SegmentsUnchanged == 1);
    REQUIRE(restoreResult->Snapshot.SegmentsChanged == 1);
    REQUIRE(restoreResult->Snapshot.ParsedFb2Records == 1);
    REQUIRE(restoreResult->Snapshot.UpdatedRecords == 1);
    REQUIRE(ReadInpxBookAvailability(databasePath, "2") == "available");
}

TEST_CASE("CInpxCatalogApplication treats an explicit initial scan over an existing cache as authoritative", "[application-jobs][inpx]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-initial-over-existing");
    auto config = MakeBaseConfig(sandbox);
    config.InpxSource = InpxWebReader::Application::SInpxSourceInfo{
        .InpxPath = WriteInpxArchiveEntries(
            sandbox.GetPath() / "source" / "catalog.inpx",
            {
                {"fb2-main.zip.inp", MakeInpxRecord("book-1", "1")},
                {"fb2-extra.zip.inp", MakeInpxRecord("book-1", "2")}
            }),
        .ArchiveRoot = sandbox.GetPath() / "source" / "archives"
    };
    WriteInpxArchive(config.InpxSource->ArchiveRoot / "fb2-main.zip", std::vector<std::string>{"Main"});
    WriteInpxArchive(config.InpxSource->ArchiveRoot / "fb2-extra.zip", std::vector<std::string>{"Extra"});
    InpxWebReader::Application::CInpxCatalogApplication app(config);
    const auto firstJobId = app.StartInpxScan({.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::InitialScan});
    REQUIRE(WaitForApplicationScan(app, firstJobId, InpxScanWaitTimeout()));

    WriteInpxArchiveEntries(
        config.InpxSource->InpxPath,
        {{"fb2-main.zip.inp", MakeInpxRecord("book-1", "1")}});
    std::filesystem::remove(config.InpxSource->ArchiveRoot / "fb2-extra.zip");
    const auto replacementJobId = app.StartInpxScan({.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::InitialScan});
    REQUIRE(WaitForApplicationScan(app, replacementJobId, InpxScanWaitTimeout()));
    const auto result = app.GetInpxScanJobResult(replacementJobId);
    REQUIRE(result.has_value());
    REQUIRE(result->Snapshot.Status == InpxWebReader::ApplicationJobs::EInpxScanJobStatus::Completed);
    REQUIRE(result->Snapshot.SegmentsChanged == 1);
    REQUIRE(result->Snapshot.SegmentsRemoved == 1);
    REQUIRE(result->Snapshot.ParsedFb2Records == 1);

    const auto databasePath = config.CacheRoot / "Database" / "inpx-web-reader.db";
    REQUIRE(ReadInpxBookAvailability(databasePath, "1") == "available");
    REQUIRE(ReadInpxBookAvailability(databasePath, "2") == "missing_from_index");
    REQUIRE(app.GetCatalogStatistics().UnavailableBookCount == 1);
}

TEST_CASE("CInpxCatalogApplication rejects duplicate active LibIds across unchanged and new segments atomically", "[application-jobs][inpx]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-scan-duplicate-segments");
    const auto config = MakeInpxConfig(sandbox, 1);
    InpxWebReader::Application::CInpxCatalogApplication app(config);
    const auto initialJobId = app.StartInpxScan({.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::InitialScan});
    REQUIRE(WaitForApplicationScan(app, initialJobId, InpxScanWaitTimeout()));
    const auto databasePath = config.CacheRoot / "Database" / "inpx-web-reader.db";
    const std::string storedFingerprint = ReadSourceFingerprint(databasePath);

    WriteInpxArchiveEntries(
        config.InpxSource->InpxPath,
        {
            {"fb2-main.zip.inp", MakeInpxRecord("book-1", "1")},
            {"fb2-extra.zip.inp", MakeInpxRecord("book-1", "1")}
        });
    WriteInpxArchive(config.InpxSource->ArchiveRoot / "fb2-extra.zip", std::vector<std::string>{"Duplicate"});
    const auto rescanJobId = app.StartInpxScan({.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::Rescan});
    REQUIRE(WaitForApplicationScan(app, rescanJobId, InpxScanWaitTimeout()));

    const auto result = app.GetInpxScanJobResult(rescanJobId);
    REQUIRE(result.has_value());
    REQUIRE(result->Snapshot.Status == InpxWebReader::ApplicationJobs::EInpxScanJobStatus::Failed);
    REQUIRE(result->Snapshot.Message.find("Duplicate active LibId") != std::string::npos);
    REQUIRE(CountBookRows(databasePath) == 1);
    REQUIRE(ReadInpxBookAvailability(databasePath, "1") == "available");
    REQUIRE(ReadSourceFingerprint(databasePath) == storedFingerprint);
}

TEST_CASE("CInpxCatalogApplication applies a delete marker from a new segment to an unchanged book", "[application-jobs][inpx]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-scan-cross-segment-delete");
    const auto config = MakeInpxConfig(sandbox, 1);
    InpxWebReader::Application::CInpxCatalogApplication app(config);
    const auto initialJobId = app.StartInpxScan({.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::InitialScan});
    REQUIRE(WaitForApplicationScan(app, initialJobId, InpxScanWaitTimeout()));

    WriteInpxArchiveEntries(
        config.InpxSource->InpxPath,
        {
            {"fb2-main.zip.inp", MakeInpxRecord("book-1", "1")},
            {"fb2-delete.zip.inp", MakeInpxRecord("book-1", "1", "ru", "", true)}
        });
    REQUIRE_FALSE(std::filesystem::exists(config.InpxSource->ArchiveRoot / "fb2-delete.zip"));

    const auto rescanJobId = app.StartInpxScan({.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::Rescan});
    REQUIRE(WaitForApplicationScan(app, rescanJobId, InpxScanWaitTimeout()));
    const auto result = app.GetInpxScanJobResult(rescanJobId);
    REQUIRE(result.has_value());
    REQUIRE(result->Snapshot.Status == InpxWebReader::ApplicationJobs::EInpxScanJobStatus::Completed);
    REQUIRE(result->Snapshot.SegmentsUnchanged == 1);
    REQUIRE(result->Snapshot.SegmentsAdded == 1);
    REQUIRE(result->Snapshot.ParsedFb2Records == 0);
    REQUIRE(result->Snapshot.ReusedRecords == 1);
    REQUIRE(result->Snapshot.MarkedUnavailableRecords == 1);
    REQUIRE(result->Snapshot.ArchivesOpened == 0);

    const auto databasePath = config.CacheRoot / "Database" / "inpx-web-reader.db";
    REQUIRE(ReadInpxBookAvailability(databasePath, "1") == "missing_from_index");
    REQUIRE(ReadInpxPresence(databasePath, "1"));
    REQUIRE(CountInpxDeletionMarkers(databasePath, "1") == 1);
    REQUIRE_FALSE(ReadSegmentRequiresArchive(databasePath, "fb2-delete.zip.inp"));
    REQUIRE(app.GetCatalogStatistics().UnavailableBookCount == 1);

    const auto unchangedJobId = app.StartInpxScan({.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::Rescan});
    REQUIRE(WaitForApplicationScan(app, unchangedJobId, InpxScanWaitTimeout()));
    const auto unchangedResult = app.GetInpxScanJobResult(unchangedJobId);
    REQUIRE(unchangedResult.has_value());
    REQUIRE(unchangedResult->Snapshot.Status == InpxWebReader::ApplicationJobs::EInpxScanJobStatus::Completed);
    REQUIRE(unchangedResult->Snapshot.SegmentsUnchanged == 2);
    REQUIRE(unchangedResult->Snapshot.ArchivesOpened == 0);
    REQUIRE(unchangedResult->Snapshot.MarkedUnavailableRecords == 0);
    REQUIRE(ReadInpxBookAvailability(databasePath, "1") == "missing_from_index");
    REQUIRE(CountInpxDeletionMarkers(databasePath, "1") == 1);

    WriteInpxArchiveEntries(
        config.InpxSource->InpxPath,
        {{"fb2-main.zip.inp", MakeInpxRecord("book-1", "1")}});
    const auto removalJobId = app.StartInpxScan({.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::Rescan});
    REQUIRE(WaitForApplicationScan(app, removalJobId, InpxScanWaitTimeout()));
    const auto removalResult = app.GetInpxScanJobResult(removalJobId);
    REQUIRE(removalResult.has_value());
    REQUIRE(removalResult->Snapshot.Status == InpxWebReader::ApplicationJobs::EInpxScanJobStatus::Completed);
    REQUIRE(removalResult->Snapshot.SegmentsUnchanged == 1);
    REQUIRE(removalResult->Snapshot.SegmentsRemoved == 1);
    REQUIRE(removalResult->Snapshot.ParsedFb2Records == 0);
    REQUIRE(removalResult->Snapshot.MarkedUnavailableRecords == 0);
    REQUIRE(ReadInpxBookAvailability(databasePath, "1") == "available");
    REQUIRE(CountInpxDeletionMarkers(databasePath, "1") == 0);
    REQUIRE(app.GetCatalogStatistics().UnavailableBookCount == 0);
}

TEST_CASE("CInpxCatalogApplication reconciles deletion precedence when unchanged INP segments are reordered", "[application-jobs][inpx]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-scan-reordered-delete-segment");
    auto config = MakeBaseConfig(sandbox);
    const std::string activeRecord = MakeInpxRecord("book-1", "1");
    const std::string deleteRecord = MakeInpxRecord("book-1", "1", "ru", "", true);
    config.InpxSource = InpxWebReader::Application::SInpxSourceInfo{
        .InpxPath = WriteInpxArchiveEntries(
            sandbox.GetPath() / "source" / "catalog.inpx",
            {
                {"fb2-main.zip.inp", activeRecord},
                {"fb2-delete.zip.inp", deleteRecord}
            }),
        .ArchiveRoot = sandbox.GetPath() / "source" / "archives"
    };
    WriteInpxArchive(config.InpxSource->ArchiveRoot / "fb2-main.zip", 1);
    InpxWebReader::Application::CInpxCatalogApplication app(config);

    const auto initialJobId = app.StartInpxScan({.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::InitialScan});
    REQUIRE(WaitForApplicationScan(app, initialJobId, InpxScanWaitTimeout()));
    const auto databasePath = config.CacheRoot / "Database" / "inpx-web-reader.db";
    REQUIRE(ReadInpxBookAvailability(databasePath, "1") == "missing_from_index");
    REQUIRE(app.GetCatalogStatistics().UnavailableBookCount == 1);

    WriteInpxArchiveEntries(
        config.InpxSource->InpxPath,
        {
            {"fb2-delete.zip.inp", deleteRecord},
            {"fb2-main.zip.inp", activeRecord}
        });
    const auto restoreJobId = app.StartInpxScan({.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::Rescan});
    REQUIRE(WaitForApplicationScan(app, restoreJobId, InpxScanWaitTimeout()));
    const auto restoreResult = app.GetInpxScanJobResult(restoreJobId);
    REQUIRE(restoreResult.has_value());
    REQUIRE(restoreResult->Snapshot.Status == InpxWebReader::ApplicationJobs::EInpxScanJobStatus::Completed);
    REQUIRE(restoreResult->Snapshot.SegmentsUnchanged == 2);
    REQUIRE(restoreResult->Snapshot.ParsedFb2Records == 0);
    REQUIRE(restoreResult->Snapshot.ArchivesOpened == 0);
    REQUIRE(ReadInpxBookAvailability(databasePath, "1") == "available");
    REQUIRE(app.GetCatalogStatistics().UnavailableBookCount == 0);

    WriteInpxArchiveEntries(
        config.InpxSource->InpxPath,
        {
            {"fb2-main.zip.inp", activeRecord},
            {"fb2-delete.zip.inp", deleteRecord}
        });
    const auto deleteJobId = app.StartInpxScan({.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::Rescan});
    REQUIRE(WaitForApplicationScan(app, deleteJobId, InpxScanWaitTimeout()));
    const auto deleteResult = app.GetInpxScanJobResult(deleteJobId);
    REQUIRE(deleteResult.has_value());
    REQUIRE(deleteResult->Snapshot.Status == InpxWebReader::ApplicationJobs::EInpxScanJobStatus::Completed);
    REQUIRE(deleteResult->Snapshot.SegmentsUnchanged == 2);
    REQUIRE(deleteResult->Snapshot.MarkedUnavailableRecords == 1);
    REQUIRE(ReadInpxBookAvailability(databasePath, "1") == "missing_from_index");
    REQUIRE(app.GetCatalogStatistics().UnavailableBookCount == 1);
}

TEST_CASE("CInpxCatalogApplication rejects duplicate active LibIds even when the cached row is tombstoned", "[application-jobs][inpx]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-scan-tombstoned-duplicate");
    auto config = MakeBaseConfig(sandbox);
    const std::string activeRecord = MakeInpxRecord("book-1", "1");
    const std::string deleteRecord = MakeInpxRecord("book-1", "1", "ru", "", true);
    config.InpxSource = InpxWebReader::Application::SInpxSourceInfo{
        .InpxPath = WriteInpxArchiveEntries(
            sandbox.GetPath() / "source" / "catalog.inpx",
            {
                {"fb2-main.zip.inp", activeRecord},
                {"fb2-delete.zip.inp", deleteRecord}
            }),
        .ArchiveRoot = sandbox.GetPath() / "source" / "archives"
    };
    WriteInpxArchive(config.InpxSource->ArchiveRoot / "fb2-main.zip", 1);
    InpxWebReader::Application::CInpxCatalogApplication app(config);
    const auto initialJobId = app.StartInpxScan({.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::InitialScan});
    REQUIRE(WaitForApplicationScan(app, initialJobId, InpxScanWaitTimeout()));
    const auto databasePath = config.CacheRoot / "Database" / "inpx-web-reader.db";
    const std::string storedFingerprint = ReadSourceFingerprint(databasePath);
    REQUIRE(ReadInpxBookAvailability(databasePath, "1") == "missing_from_index");

    WriteInpxArchiveEntries(
        config.InpxSource->InpxPath,
        {
            {"fb2-main.zip.inp", activeRecord},
            {"fb2-delete.zip.inp", deleteRecord},
            {"fb2-extra.zip.inp", activeRecord}
        });
    WriteInpxArchive(config.InpxSource->ArchiveRoot / "fb2-extra.zip", 1);
    const auto rescanJobId = app.StartInpxScan({.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::Rescan});
    REQUIRE(WaitForApplicationScan(app, rescanJobId, InpxScanWaitTimeout()));
    const auto result = app.GetInpxScanJobResult(rescanJobId);
    REQUIRE(result.has_value());
    REQUIRE(result->Snapshot.Status == InpxWebReader::ApplicationJobs::EInpxScanJobStatus::Failed);
    REQUIRE(result->Snapshot.Message.find("Duplicate active LibId") != std::string::npos);
    REQUIRE(ReadInpxBookAvailability(databasePath, "1") == "missing_from_index");
    REQUIRE(ReadSourceFingerprint(databasePath) == storedFingerprint);
}

TEST_CASE("CInpxCatalogApplication rolls back a changed segment when its ZIP entry is missing", "[application-jobs][inpx]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-scan-missing-entry-rollback");
    const auto config = MakeInpxConfig(sandbox, 1);
    InpxWebReader::Application::CInpxCatalogApplication app(config);
    const auto initialJobId = app.StartInpxScan({.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::InitialScan});
    REQUIRE(WaitForApplicationScan(app, initialJobId, InpxScanWaitTimeout()));
    const auto databasePath = config.CacheRoot / "Database" / "inpx-web-reader.db";
    const std::string storedFingerprint = ReadSourceFingerprint(databasePath);

    WriteInpxArchive(config.InpxSource->InpxPath, MakeInpxRecord("missing-book", "1"));
    const auto rescanJobId = app.StartInpxScan({.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::Rescan});
    REQUIRE(WaitForApplicationScan(app, rescanJobId, InpxScanWaitTimeout()));
    const auto result = app.GetInpxScanJobResult(rescanJobId);
    REQUIRE(result.has_value());
    REQUIRE(result->Snapshot.Status == InpxWebReader::ApplicationJobs::EInpxScanJobStatus::Failed);
    REQUIRE(app.ListBooks({.Limit = 10}).Items.front().TitleUtf8 == "Payload Title 1");
    REQUIRE(ReadInpxBookAvailability(databasePath, "1") == "available");
    REQUIRE(ReadSourceFingerprint(databasePath) == storedFingerprint);
}

TEST_CASE("CInpxCatalogApplication rejects a replacement INPX without inp segments atomically", "[application-jobs][inpx]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-scan-no-inp-segments");
    const auto config = MakeInpxConfig(sandbox, 1);
    InpxWebReader::Application::CInpxCatalogApplication app(config);
    const auto initialJobId = app.StartInpxScan({.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::InitialScan});
    REQUIRE(WaitForApplicationScan(app, initialJobId, InpxScanWaitTimeout()));
    const auto databasePath = config.CacheRoot / "Database" / "inpx-web-reader.db";
    const std::string storedFingerprint = ReadSourceFingerprint(databasePath);

    WriteInpxArchiveEntries(
        config.InpxSource->InpxPath,
        {{"README.txt", "This ZIP is not an INPX catalog."}});
    const auto rescanJobId = app.StartInpxScan({.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::Rescan});
    REQUIRE(WaitForApplicationScan(app, rescanJobId, InpxScanWaitTimeout()));
    const auto result = app.GetInpxScanJobResult(rescanJobId);
    REQUIRE(result.has_value());
    REQUIRE(result->Snapshot.Status == InpxWebReader::ApplicationJobs::EInpxScanJobStatus::Failed);
    REQUIRE(result->Snapshot.Message.find("does not contain any .inp segments") != std::string::npos);
    REQUIRE(ReadInpxBookAvailability(databasePath, "1") == "available");
    REQUIRE(ReadSourceFingerprint(databasePath) == storedFingerprint);
    REQUIRE(app.ListBooks({.Limit = 10}).Items.front().TitleUtf8 == "Payload Title 1");
}

TEST_CASE("CInpxCatalogApplication rolls back a changed archive when FB2 parsing fails", "[application-jobs][inpx]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-scan-corrupt-fb2-rollback");
    const auto config = MakeInpxConfig(sandbox, 1);
    InpxWebReader::Application::CInpxCatalogApplication app(config);
    const auto initialJobId = app.StartInpxScan({.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::InitialScan});
    REQUIRE(WaitForApplicationScan(app, initialJobId, InpxScanWaitTimeout()));
    const auto databasePath = config.CacheRoot / "Database" / "inpx-web-reader.db";

    WriteSinglePayloadInpxArchive(
        config.InpxSource->ArchiveRoot / "fb2-main.zip",
        "not-an-fb2-document");
    const auto rescanJobId = app.StartInpxScan({.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::Rescan});
    REQUIRE(WaitForApplicationScan(app, rescanJobId, InpxScanWaitTimeout()));
    const auto result = app.GetInpxScanJobResult(rescanJobId);
    REQUIRE(result.has_value());
    REQUIRE(result->Snapshot.Status == InpxWebReader::ApplicationJobs::EInpxScanJobStatus::Failed);
    REQUIRE(CountBookRows(databasePath) == 1);
    REQUIRE(app.ListBooks({.Limit = 10}).Items.front().TitleUtf8 == "Payload Title 1");
}

TEST_CASE("CInpxCatalogApplication rejects an INPX and ZIP entry size mismatch", "[application-jobs][inpx]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-scan-size-mismatch");
    auto config = MakeInpxConfig(sandbox, 1);
    WriteInpxArchive(
        config.InpxSource->InpxPath,
        MakeInpxRecord("book-1", "1", "ru", "", false, "fb2", 1));
    InpxWebReader::Application::CInpxCatalogApplication app(config);
    const auto jobId = app.StartInpxScan({.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::InitialScan});
    REQUIRE(WaitForApplicationScan(app, jobId, InpxScanWaitTimeout()));
    const auto result = app.GetInpxScanJobResult(jobId);
    REQUIRE(result.has_value());
    REQUIRE(result->Snapshot.Status == InpxWebReader::ApplicationJobs::EInpxScanJobStatus::Failed);
    REQUIRE(result->Snapshot.Message.find("size mismatch") != std::string::npos);
    REQUIRE(CountBookRows(config.CacheRoot / "Database" / "inpx-web-reader.db") == 0);
}

TEST_CASE("CInpxCatalogApplication does not require archives for segments without active FB2 records", "[application-jobs][inpx]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-scan-metadata-only-segment");
    auto config = MakeBaseConfig(sandbox);
    config.InpxSource = InpxWebReader::Application::SInpxSourceInfo{
        .InpxPath = WriteInpxArchive(
            sandbox.GetPath() / "source" / "catalog.inpx",
            MakeInpxRecord("unsupported", "1", "ru", "", false, "epub")
                + MakeInpxRecord("deleted", "2", "ru", "", true)),
        .ArchiveRoot = sandbox.GetPath() / "source" / "archives"
    };
    std::filesystem::create_directories(config.InpxSource->ArchiveRoot);
    InpxWebReader::Application::CInpxCatalogApplication app(config);

    const auto jobId = app.StartInpxScan({.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::InitialScan});
    REQUIRE(WaitForApplicationScan(app, jobId, InpxScanWaitTimeout()));
    const auto result = app.GetInpxScanJobResult(jobId);
    REQUIRE(result.has_value());
    REQUIRE(result->Snapshot.Status == InpxWebReader::ApplicationJobs::EInpxScanJobStatus::Completed);
    REQUIRE(result->Snapshot.ParsedFb2Records == 0);
    REQUIRE(result->Snapshot.SkippedRecords == 1);
    REQUIRE(result->Snapshot.ArchivesOpened == 0);
    REQUIRE(result->Snapshot.ArchiveBytesRead == 0);
    REQUIRE_FALSE(ReadSegmentRequiresArchive(
        config.CacheRoot / "Database" / "inpx-web-reader.db",
        "fb2-main.zip.inp"));

    const auto rescanJobId = app.StartInpxScan({.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::Rescan});
    REQUIRE(WaitForApplicationScan(app, rescanJobId, InpxScanWaitTimeout()));
    const auto rescanResult = app.GetInpxScanJobResult(rescanJobId);
    REQUIRE(rescanResult.has_value());
    REQUIRE(rescanResult->Snapshot.Status == InpxWebReader::ApplicationJobs::EInpxScanJobStatus::Completed);
    REQUIRE(rescanResult->Snapshot.SegmentsUnchanged == 1);
    REQUIRE(rescanResult->Snapshot.ArchivesSkipped == 0);
    REQUIRE(rescanResult->Snapshot.ArchivesOpened == 0);
    REQUIRE(rescanResult->Snapshot.ArchiveBytesRead == 0);
}

TEST_CASE("CInpxCatalogApplication stores the full-file INPX fingerprint after rescan", "[application-jobs][inpx][fingerprint]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-scan-source-fingerprint");
    const auto config = MakeInpxConfig(sandbox, 1);
    InpxWebReader::Application::CInpxCatalogApplication app(config);

    const auto initialJobId = app.StartInpxScan({.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::InitialScan});
    REQUIRE(WaitForApplicationScan(app, initialJobId, InpxScanWaitTimeout()));

    const auto databasePath = config.CacheRoot / "Database" / "inpx-web-reader.db";
    const std::string initialFingerprintUtf8 = ReadSourceFingerprint(databasePath);

    WriteInpxArchive(
        config.InpxSource->InpxPath,
        MakeInpxRecord("book-1", "1", "ru", "", true));

    const auto rescanJobId = app.StartInpxScan({.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::Rescan});
    REQUIRE(WaitForApplicationScan(app, rescanJobId, InpxScanWaitTimeout()));

    const std::string rescanFingerprintUtf8 = ReadSourceFingerprint(databasePath);
    REQUIRE(rescanFingerprintUtf8 != initialFingerprintUtf8);
    REQUIRE(rescanFingerprintUtf8
        == InpxWebReader::Foundation::CSha256Fingerprint::ComputeFile(config.InpxSource->InpxPath));
}

TEST_CASE("CInpxCatalogApplication uses INPX title when source FB2 payload lacks book-title", "[application-jobs][inpx]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-scan-missing-payload-title");
    const auto config = MakeInpxConfig(sandbox, 1);
    WriteSinglePayloadInpxArchive(
        config.InpxSource->ArchiveRoot / "fb2-main.zip",
        R"(<?xml version="1.0" encoding="UTF-8"?>
<FictionBook>
  <description>
    <title-info>
      <genre>science_fiction</genre>
      <author>
        <first-name>Payload</first-name>
        <last-name>Author</last-name>
      </author>
      <lang>ru</lang>
    </title-info>
  </description>
</FictionBook>)");

    InpxWebReader::Application::CInpxCatalogApplication app(config);
    const auto jobId = app.StartInpxScan({.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::InitialScan});
    REQUIRE(WaitForApplicationScan(app, jobId, InpxScanWaitTimeout()));

    const auto result = app.GetInpxScanJobResult(jobId);
    REQUIRE(result.has_value());
    REQUIRE(result->ScanResult.has_value());
    REQUIRE(result->Snapshot.AddedRecords == 1);
    REQUIRE(result->Snapshot.ParsedFb2Records == 1);
    REQUIRE(result->ScanResult->WarningCount == 0);
    REQUIRE(CountPersistedWarnings(config.CacheRoot / "Database" / "inpx-web-reader.db") == 0);

    const auto list = app.ListBooks({.Limit = 10});
    REQUIRE(list.TotalCount == 1);
    REQUIRE(list.Items.size() == 1);
    REQUIRE(list.Items.front().TitleUtf8 == "Title 1");

    const auto details = app.GetBookDetails(list.Items.front().Id);
    REQUIRE(details.has_value());
    REQUIRE(details->TitleUtf8 == "Title 1");
    REQUIRE(details->AuthorsUtf8 == std::vector<std::string>{"Payload Author"});
}

TEST_CASE("CInpxCatalogApplication repairs replacement-character source metadata from INPX rows", "[application-jobs][inpx]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-scan-replacement-metadata");
    const auto config = MakeInpxConfig(sandbox, 1);
    const std::string replacement = "\xEF\xBF\xBD";
    WriteSinglePayloadInpxArchive(
        config.InpxSource->ArchiveRoot / "fb2-main.zip",
        std::string(R"(<?xml version="1.0" encoding="UTF-8"?>
<FictionBook>
  <description>
    <title-info>
      <book-title>)")
            + replacement + replacement
            + R"(</book-title>
      <author>
        <first-name>)"
            + replacement + replacement
            + R"(</first-name>
        <last-name>)"
            + replacement + replacement
            + R"(</last-name>
      </author>
      <lang>ru</lang>
    </title-info>
  </description>
</FictionBook>)");

    InpxWebReader::Application::CInpxCatalogApplication app(config);
    const auto jobId = app.StartInpxScan({.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::InitialScan});
    REQUIRE(WaitForApplicationScan(app, jobId, InpxScanWaitTimeout()));

    const auto result = app.GetInpxScanJobResult(jobId);
    REQUIRE(result.has_value());
    REQUIRE(result->ScanResult.has_value());
    REQUIRE(result->Snapshot.AddedRecords == 1);
    REQUIRE(result->Snapshot.ParsedFb2Records == 1);

    const auto list = app.ListBooks({.Limit = 10});
    REQUIRE(list.TotalCount == 1);
    REQUIRE(list.Items.size() == 1);
    REQUIRE(list.Items.front().TitleUtf8 == "Title 1");

    const auto details = app.GetBookDetails(list.Items.front().Id);
    REQUIRE(details.has_value());
    REQUIRE(details->TitleUtf8 == "Title 1");
    REQUIRE(details->AuthorsUtf8 == std::vector<std::string>{"Author"});
}

TEST_CASE("CInpxCatalogApplication marks records omitted from a changed segment unavailable", "[application-jobs][inpx]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-scan-omitted-preserved");
    const auto config = MakeInpxConfig(sandbox, 2);
    InpxWebReader::Application::CInpxCatalogApplication app(config);

    const auto initialJobId = app.StartInpxScan({.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::InitialScan});
    REQUIRE(WaitForApplicationScan(app, initialJobId, InpxScanWaitTimeout()));

    WriteInpxArchive(
        sandbox.GetPath() / "source" / "catalog.inpx",
        MakeInpxRecord("book-1", "1"));

    const auto rescanJobId = app.StartInpxScan({.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::Rescan});
    REQUIRE(WaitForApplicationScan(app, rescanJobId, InpxScanWaitTimeout()));

    const auto rescanResult = app.GetInpxScanJobResult(rescanJobId);
    REQUIRE(rescanResult.has_value());
    REQUIRE(rescanResult->ScanResult.has_value());
    REQUIRE(rescanResult->Snapshot.ParsedFb2Records == 1);
    REQUIRE(rescanResult->Snapshot.UpdatedRecords == 1);
    REQUIRE(rescanResult->Snapshot.MarkedUnavailableRecords == 1);

    const auto databasePath = config.CacheRoot / "Database" / "inpx-web-reader.db";
    REQUIRE(ReadInpxBookAvailability(databasePath, "1") == "available");
    REQUIRE(ReadInpxBookAvailability(databasePath, "2") == "missing_from_index");

    const auto statistics = app.GetCatalogStatistics();
    REQUIRE(statistics.BookCount == 2);
    REQUIRE(statistics.UnavailableBookCount == 1);
}

TEST_CASE("CInpxCatalogApplication marks deleted INPX books unavailable on rescan", "[application-jobs][inpx]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-scan-deleted-unavailable");
    const auto config = MakeInpxConfig(sandbox, 2);
    InpxWebReader::Application::CInpxCatalogApplication app(config);

    const auto initialJobId = app.StartInpxScan({.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::InitialScan});
    REQUIRE(WaitForApplicationScan(app, initialJobId, InpxScanWaitTimeout()));

    WriteInpxArchive(
        sandbox.GetPath() / "source" / "catalog.inpx",
        MakeInpxRecord("book-1", "1") + MakeInpxRecord("book-2", "2", "ru", "", true));

    const auto rescanJobId = app.StartInpxScan({.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::Rescan});
    REQUIRE(WaitForApplicationScan(app, rescanJobId, InpxScanWaitTimeout()));

    const auto rescanResult = app.GetInpxScanJobResult(rescanJobId);
    REQUIRE(rescanResult.has_value());
    REQUIRE(rescanResult->ScanResult.has_value());
    REQUIRE(rescanResult->Snapshot.ParsedFb2Records == 1);
    REQUIRE(rescanResult->Snapshot.UpdatedRecords == 1);
    REQUIRE(rescanResult->Snapshot.MarkedUnavailableRecords == 1);

    const auto databasePath = config.CacheRoot / "Database" / "inpx-web-reader.db";
    REQUIRE(ReadInpxBookAvailability(databasePath, "1") == "available");
    REQUIRE(ReadInpxBookAvailability(databasePath, "2") == "missing_from_index");

    const auto list = app.ListBooks({.Limit = 10});
    REQUIRE(list.TotalCount == 2);
    const auto unavailableIterator = std::find_if(list.Items.begin(), list.Items.end(), [](const auto& item) {
        return !item.IsAvailable;
    });
    REQUIRE(unavailableIterator != list.Items.end());
    REQUIRE_FALSE(unavailableIterator->CanDownloadOriginal);
    REQUIRE_FALSE(unavailableIterator->CanDownloadAsEpub);
    REQUIRE(unavailableIterator->AvailabilityLabelUtf8 == "Unavailable in current INPX source");

    const auto unavailableDetails = app.GetBookDetails(unavailableIterator->Id);
    REQUIRE(unavailableDetails.has_value());
    REQUIRE_FALSE(unavailableDetails->CanDownloadOriginal);
    REQUIRE_FALSE(unavailableDetails->CanDownloadAsEpub);
    REQUIRE_FALSE(unavailableDetails->IsAvailable);
    REQUIRE(unavailableDetails->AvailabilityLabelUtf8 == "Unavailable in current INPX source");

    const auto statistics = app.GetCatalogStatistics();
    REQUIRE(statistics.BookCount == 2);
    REQUIRE(statistics.UnavailableBookCount == 1);
    REQUIRE(statistics.InpxSourceSizeBytes > 0);
}

TEST_CASE("CInpxCatalogApplication keeps deleted marker when later active row is unsupported", "[application-jobs][inpx]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-scan-deleted-unsupported-conflict");
    const auto config = MakeInpxConfig(sandbox, 1);
    InpxWebReader::Application::CInpxCatalogApplication app(config);

    const auto initialJobId = app.StartInpxScan({.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::InitialScan});
    REQUIRE(WaitForApplicationScan(app, initialJobId, InpxScanWaitTimeout()));

    WriteInpxArchive(
        config.InpxSource->InpxPath,
        MakeInpxRecord("book-1", "1", "ru", "", true)
            + MakeInpxRecord("book-1", "1", "ru", "", false, "epub"));

    const auto rescanJobId = app.StartInpxScan({.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::Rescan});
    REQUIRE(WaitForApplicationScan(app, rescanJobId, InpxScanWaitTimeout()));

    const auto rescanResult = app.GetInpxScanJobResult(rescanJobId);
    REQUIRE(rescanResult.has_value());
    REQUIRE(rescanResult->ScanResult.has_value());
    REQUIRE(rescanResult->Snapshot.ParsedFb2Records == 0);
    REQUIRE(rescanResult->Snapshot.SkippedRecords == 1);
    REQUIRE(rescanResult->Snapshot.MarkedUnavailableRecords == 1);

    const auto databasePath = config.CacheRoot / "Database" / "inpx-web-reader.db";
    REQUIRE(ReadInpxBookAvailability(databasePath, "1") == "missing_from_index");
    REQUIRE_FALSE(app.ListBooks({.Limit = 10}).Items.front().CanDownloadOriginal);
    REQUIRE(app.GetCatalogStatistics().UnavailableBookCount == 1);
}

TEST_CASE("CInpxCatalogApplication restores a deleted INPX book by reparsing its changed segment", "[application-jobs][inpx]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-scan-deleted-restored-incremental");
    const auto config = MakeInpxConfig(sandbox, 1);
    InpxWebReader::Application::CInpxCatalogApplication app(config);

    const auto initialJobId = app.StartInpxScan({.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::InitialScan});
    REQUIRE(WaitForApplicationScan(app, initialJobId, InpxScanWaitTimeout()));

    WriteInpxArchive(
        config.InpxSource->InpxPath,
        MakeInpxRecord("book-1", "1", "ru", "", true));
    const auto deleteJobId = app.StartInpxScan({.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::Rescan});
    REQUIRE(WaitForApplicationScan(app, deleteJobId, InpxScanWaitTimeout()));

    const auto deleteResult = app.GetInpxScanJobResult(deleteJobId);
    REQUIRE(deleteResult.has_value());
    REQUIRE(deleteResult->ScanResult.has_value());
    REQUIRE(deleteResult->Snapshot.ParsedFb2Records == 0);
    REQUIRE(deleteResult->Snapshot.MarkedUnavailableRecords == 1);

    const auto databasePath = config.CacheRoot / "Database" / "inpx-web-reader.db";
    REQUIRE(ReadInpxBookAvailability(databasePath, "1") == "missing_from_index");
    REQUIRE(app.GetCatalogStatistics().UnavailableBookCount == 1);

    WriteInpxArchive(
        config.InpxSource->InpxPath,
        MakeInpxRecord("book-1", "1"));
    const auto restoreJobId = app.StartInpxScan({.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::Rescan});
    REQUIRE(WaitForApplicationScan(app, restoreJobId, InpxScanWaitTimeout()));

    const auto restoreResult = app.GetInpxScanJobResult(restoreJobId);
    REQUIRE(restoreResult.has_value());
    REQUIRE(restoreResult->ScanResult.has_value());
    REQUIRE(restoreResult->Snapshot.ParsedFb2Records == 1);
    REQUIRE(restoreResult->Snapshot.UpdatedRecords == 1);
    REQUIRE(restoreResult->Snapshot.MarkedUnavailableRecords == 0);
    REQUIRE(ReadInpxBookAvailability(databasePath, "1") == "available");

    const auto list = app.ListBooks({.Limit = 10});
    REQUIRE(list.TotalCount == 1);
    REQUIRE(list.Items.size() == 1);
    REQUIRE(list.Items.front().TitleUtf8 == "Payload Title 1");

    const auto statistics = app.GetCatalogStatistics();
    REQUIRE(statistics.BookCount == 1);
    REQUIRE(statistics.UnavailableBookCount == 0);
}

TEST_CASE("CInpxCatalogApplication fails INPX rescan with unavailable archives without changing cached availability", "[application-jobs][inpx]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-scan-archive-unavailable");
    const auto config = MakeInpxConfig(sandbox, 2);
    InpxWebReader::Application::CInpxCatalogApplication app(config);

    const auto initialJobId = app.StartInpxScan({.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::InitialScan});
    REQUIRE(WaitForApplicationScan(app, initialJobId, InpxScanWaitTimeout()));

    std::filesystem::remove(config.InpxSource->ArchiveRoot / "fb2-main.zip");

    const auto rescanJobId = app.StartInpxScan({.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::Rescan});
    REQUIRE(WaitForApplicationScan(app, rescanJobId, InpxScanWaitTimeout()));

    const auto rescanResult = app.GetInpxScanJobResult(rescanJobId);
    REQUIRE(rescanResult.has_value());
    REQUIRE(rescanResult->Snapshot.Status == InpxWebReader::ApplicationJobs::EInpxScanJobStatus::Failed);
    REQUIRE_FALSE(rescanResult->ScanResult.has_value());
    REQUIRE(rescanResult->Error.has_value());

    const auto databasePath = config.CacheRoot / "Database" / "inpx-web-reader.db";
    REQUIRE(ReadInpxBookAvailability(databasePath, "1") == "available");
    REQUIRE(ReadInpxBookAvailability(databasePath, "2") == "available");

    const auto statistics = app.GetCatalogStatistics();
    REQUIRE(statistics.BookCount == 2);
    REQUIRE(statistics.UnavailableBookCount == 0);
}

TEST_CASE("CInpxCatalogApplication applies INPX catalog upgrades with added archives and delete markers", "[application-jobs][inpx]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-scan-catalog-upgrade");
    const auto config = MakeInpxConfig(sandbox, 2);
    InpxWebReader::Application::CInpxCatalogApplication app(config);

    const auto initialJobId = app.StartInpxScan({.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::InitialScan});
    REQUIRE(WaitForApplicationScan(app, initialJobId, InpxScanWaitTimeout()));

    WriteInpxArchiveEntries(
        config.InpxSource->InpxPath,
        {
            {
                "fb2-main.zip.inp",
                MakeInpxRecord("book-1", "1") + MakeInpxRecord("book-2", "2", "ru", "", true)
            },
            {
                "fb2-extra.zip.inp",
                MakeInpxRecord("book-1", "3") + MakeInpxRecord("book-2", "4")
            }
        });
    WriteInpxArchive(
        config.InpxSource->ArchiveRoot / "fb2-extra.zip",
        std::vector<std::string>{"Added Payload Title 3", "Added Payload Title 4"});

    const auto rescanJobId = app.StartInpxScan({.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::Rescan});
    REQUIRE(WaitForApplicationScan(app, rescanJobId, InpxScanWaitTimeout()));

    const auto rescanResult = app.GetInpxScanJobResult(rescanJobId);
    REQUIRE(rescanResult.has_value());
    REQUIRE(rescanResult->Snapshot.Status == InpxWebReader::ApplicationJobs::EInpxScanJobStatus::Completed);
    REQUIRE(rescanResult->ScanResult.has_value());
    REQUIRE(rescanResult->Snapshot.ParsedFb2Records == 3);
    REQUIRE(rescanResult->Snapshot.AddedRecords == 2);
    REQUIRE(rescanResult->Snapshot.UpdatedRecords == 1);
    REQUIRE(rescanResult->Snapshot.MarkedUnavailableRecords == 1);

    const auto databasePath = config.CacheRoot / "Database" / "inpx-web-reader.db";
    REQUIRE(ReadInpxBookAvailability(databasePath, "1") == "available");
    REQUIRE(ReadInpxBookAvailability(databasePath, "2") == "missing_from_index");
    REQUIRE(ReadInpxBookAvailability(databasePath, "3") == "available");
    REQUIRE(ReadInpxBookAvailability(databasePath, "4") == "available");

    const auto allBooks = app.ListBooks({.Limit = 10});
    REQUIRE(allBooks.TotalCount == 4);
    REQUIRE(app.ListBooks({.TextUtf8 = "Added Payload", .Limit = 10}).Items.size() == 2);

    const auto statistics = app.GetCatalogStatistics();
    REQUIRE(statistics.BookCount == 4);
    REQUIRE(statistics.UnavailableBookCount == 1);
}
