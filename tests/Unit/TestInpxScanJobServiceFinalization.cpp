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

TEST_CASE("CInpxCatalogApplication persists INPX scan warnings", "[application-jobs][inpx]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-scan-warnings");
    const auto config = MakeInpxConfig(sandbox, 1);
    WriteInpxArchive(
        config.InpxSource->InpxPath,
        MakeInpxRecord("book-1", "1", "ru", "", false, "txt"));

    InpxWebReader::Application::CInpxCatalogApplication app(config);
    const auto jobId = app.StartInpxScan({.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::InitialScan});
    REQUIRE(WaitForApplicationScan(app, jobId, InpxScanWaitTimeout()));

    const auto result = app.GetInpxScanJobResult(jobId);
    REQUIRE(result.has_value());
    REQUIRE(result->ScanResult.has_value());
    REQUIRE(result->Snapshot.AddedRecords == 0);
    REQUIRE(result->ScanResult->WarningCount >= 1);
    REQUIRE(CountPersistedWarnings(config.CacheRoot / "Database" / "inpx-web-reader.db") >= 1);
}

TEST_CASE("CInpxCatalogApplication caps persisted warnings for one scan", "[application-jobs][inpx]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-scan-warning-cap");
    auto config = MakeInpxConfig(sandbox, 1);
    std::string records;
    for (std::size_t index = 0; index < 1'005; ++index)
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

    InpxWebReader::Application::CInpxCatalogApplication app(config);
    const auto jobId = app.StartInpxScan({.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::InitialScan});
    REQUIRE(WaitForApplicationScan(app, jobId, InpxScanWaitTimeout()));
    const auto result = app.GetInpxScanJobResult(jobId);
    REQUIRE(result.has_value());
    REQUIRE(result->Snapshot.Status == InpxWebReader::ApplicationJobs::EInpxScanJobStatus::Completed);
    REQUIRE(result->ScanResult->WarningCount == 1'005);
    REQUIRE(result->Snapshot.SkippedRecords == 1'005);
    REQUIRE(CountPersistedWarnings(config.CacheRoot / "Database" / "inpx-web-reader.db") == 1'000);
}

TEST_CASE("CInpxCatalogApplication retains warnings from only ten recent warning scans", "[application-jobs][inpx]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-scan-warning-retention");
    auto config = MakeInpxConfig(sandbox, 1);
    WriteInpxArchive(
        config.InpxSource->InpxPath,
        MakeInpxRecord("unsupported", "1", "ru", "scan-0", false, "txt"));
    InpxWebReader::Application::CInpxCatalogApplication app(config);

    for (std::size_t scanIndex = 0; scanIndex < 12; ++scanIndex)
    {
        if (scanIndex != 0)
        {
            WriteInpxArchive(
                config.InpxSource->InpxPath,
                MakeInpxRecord(
                    "unsupported",
                    "1",
                    "ru",
                    "scan-" + std::to_string(scanIndex),
                    false,
                    "txt"));
        }
        const auto jobId = app.StartInpxScan({
            .Mode = scanIndex == 0
                ? InpxWebReader::ApplicationJobs::EInpxScanMode::InitialScan
                : InpxWebReader::ApplicationJobs::EInpxScanMode::Rescan
        });
        REQUIRE(WaitForApplicationScan(app, jobId, InpxScanWaitTimeout()));
        REQUIRE(app.GetInpxScanJobResult(jobId)->Snapshot.Status
            == InpxWebReader::ApplicationJobs::EInpxScanJobStatus::Completed);
    }

    const auto databasePath = config.CacheRoot / "Database" / "inpx-web-reader.db";
    REQUIRE(CountPersistedWarningScans(databasePath) == 10);
    REQUIRE(CountPersistedWarnings(databasePath) == 10);
}

TEST_CASE("INPX scan job service rolls back every catalog change when finalization fails", "[application-jobs][inpx]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-scan-streaming-finalize-failure");
    const auto config = MakeInpxConfig(sandbox, 1);
    const auto databasePath = config.CacheRoot / "Database" / "inpx-web-reader.db";

    {
        InpxWebReader::Application::CInpxCatalogApplication app(config);
        const auto initialJobId = app.StartInpxScan({.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::InitialScan});
        REQUIRE(WaitForApplicationScan(app, initialJobId, InpxScanWaitTimeout()));
        const auto initialList = app.ListBooks({.Limit = 10});
        REQUIRE(initialList.Items.size() == 1);
        REQUIRE(initialList.Items.front().TitleUtf8 == "Payload Title 1");
    }

    WriteInpxArchive(
        config.InpxSource->ArchiveRoot / "fb2-main.zip",
        std::vector<std::string>{"Rolled Back Title"});
    WriteInpxArchive(
        config.InpxSource->InpxPath,
        MakeInpxRecord("book-1", "1", "ru", "", false, "fb2", 0));

    bool injectedFailureReached = false;
    InpxWebReader::ApplicationJobs::CInpxScanJobService service(
        sandbox.GetPath() / "runtime-failing",
        {
            .BeforeCatalogCommit = [&injectedFailureReached]() {
                injectedFailureReached = true;
                throw std::runtime_error("injected catalog apply failure");
            }
        },
        databasePath,
        config.CacheRoot);
    const auto failingJobId = service.Start(
        *config.InpxSource,
        {.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::Rescan});
    REQUIRE(service.Wait(failingJobId, InpxScanWaitTimeout()));

    const auto failedResult = service.TryGetResult(failingJobId);
    REQUIRE(failedResult.has_value());
    REQUIRE(failedResult->Snapshot.Status == InpxWebReader::ApplicationJobs::EInpxScanJobStatus::Failed);
    REQUIRE(injectedFailureReached);

    auto openConfig = MakeBaseConfig(sandbox);
    openConfig.CacheOpenMode = InpxWebReader::Application::ECacheOpenMode::Open;
    InpxWebReader::Application::CInpxCatalogApplication reopened(openConfig);
    REQUIRE(CountBookRows(databasePath) == 1);

    const auto listAfterFailure = reopened.ListBooks({.Limit = 10});
    REQUIRE(listAfterFailure.Items.size() == 1);
    REQUIRE(listAfterFailure.Items.front().TitleUtf8 == "Payload Title 1");
    REQUIRE(reopened.ListBooks({.TextUtf8 = "Rolled Back", .Limit = 10}).Items.empty());
    REQUIRE(reopened.ListBooks({.TextUtf8 = "Payload Title", .Limit = 10}).Items.size() == 1);
}

TEST_CASE("INPX scan job service rejects an INPX change during finalization", "[application-jobs][inpx][fingerprint]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-scan-source-change-during-finalization");
    const auto config = MakeInpxConfig(sandbox, 1);
    const auto databasePath = config.CacheRoot / "Database" / "inpx-web-reader.db";

    {
        InpxWebReader::Application::CInpxCatalogApplication app(config);
        const auto initialJobId =
            app.StartInpxScan({.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::InitialScan});
        REQUIRE(WaitForApplicationScan(app, initialJobId, InpxScanWaitTimeout()));
    }
    const std::string initialFingerprintUtf8 = ReadSourceFingerprint(databasePath);
    const std::string initialDisplayNameUtf8 = ReadSourceDisplayName(databasePath);
    auto relocatedSource = *config.InpxSource;
    relocatedSource.InpxPath = relocatedSource.InpxPath.parent_path() / "relocated.inpx";
    std::filesystem::rename(config.InpxSource->InpxPath, relocatedSource.InpxPath);

    bool sourceChanged = false;
    InpxWebReader::ApplicationJobs::CInpxScanJobService service(
        sandbox.GetPath() / "runtime-source-change",
        {
            .BeforeCatalogCommit = [&]() {
                WriteInpxArchive(
                    relocatedSource.InpxPath,
                    MakeInpxRecord("book-1", "1", "ru", "", true));
                sourceChanged = true;
            }
        },
        databasePath,
        config.CacheRoot);
    const auto rescanJobId = service.Start(
        relocatedSource,
        {.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::Rescan});
    REQUIRE(service.Wait(rescanJobId, InpxScanWaitTimeout()));

    const auto result = service.TryGetResult(rescanJobId);
    REQUIRE(result.has_value());
    REQUIRE(result->Snapshot.Status == InpxWebReader::ApplicationJobs::EInpxScanJobStatus::Failed);
    REQUIRE(result->Snapshot.Message.find("changed while it was being scanned") != std::string::npos);
    REQUIRE(sourceChanged);
    REQUIRE(ReadSourceFingerprint(databasePath) == initialFingerprintUtf8);
    REQUIRE(ReadSourceDisplayName(databasePath) == initialDisplayNameUtf8);
    REQUIRE(InpxWebReader::Foundation::CSha256Fingerprint::ComputeFile(relocatedSource.InpxPath)
        != initialFingerprintUtf8);
}

TEST_CASE("INPX scan job service rejects an archive change during finalization", "[application-jobs][inpx][fingerprint]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-scan-archive-change-during-finalization");
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
    WriteInpxArchive(config.InpxSource->ArchiveRoot / "fb2-main.zip", 2);
    const auto databasePath = config.CacheRoot / "Database" / "inpx-web-reader.db";

    {
        InpxWebReader::Application::CInpxCatalogApplication app(config);
        const auto initialJobId =
            app.StartInpxScan({.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::InitialScan});
        REQUIRE(WaitForApplicationScan(app, initialJobId, InpxScanWaitTimeout()));
    }
    const std::string initialFingerprintUtf8 = ReadSourceFingerprint(databasePath);
    WriteInpxArchiveEntries(
        config.InpxSource->InpxPath,
        {
            {"fb2-main.inp", MakeInpxRecord("book-1", "1", "ru", "changed")},
            {"fb2-main.zip.inp", MakeInpxRecord("book-2", "2", "ru", "changed")}
        });

    const auto archivePath = config.InpxSource->ArchiveRoot / "fb2-main.zip";
    const auto archiveMtime = std::filesystem::last_write_time(archivePath);
    bool archiveChanged = false;
    InpxWebReader::ApplicationJobs::CInpxScanJobService service(
        sandbox.GetPath() / "runtime-archive-change",
        {
            .BeforeCatalogCommit = [&]() {
                WriteInpxArchive(
                    archivePath,
                    std::vector<std::string>{"Payload TItle 1", "Payload Title 2"});
                std::filesystem::last_write_time(archivePath, archiveMtime);
                archiveChanged = true;
            }
        },
        databasePath,
        config.CacheRoot);
    const auto rescanJobId = service.Start(
        *config.InpxSource,
        {.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::Rescan});
    REQUIRE(service.Wait(rescanJobId, InpxScanWaitTimeout()));

    const auto result = service.TryGetResult(rescanJobId);
    REQUIRE(result.has_value());
    REQUIRE(result->Snapshot.Status == InpxWebReader::ApplicationJobs::EInpxScanJobStatus::Failed);
    REQUIRE(result->Snapshot.Message.find("changed while it was being scanned") != std::string::npos);
    REQUIRE(archiveChanged);
    REQUIRE(ReadSourceFingerprint(databasePath) == initialFingerprintUtf8);

    auto openConfig = MakeBaseConfig(sandbox);
    openConfig.CacheOpenMode = InpxWebReader::Application::ECacheOpenMode::Open;
    InpxWebReader::Application::CInpxCatalogApplication reopened(openConfig);
    const auto books = reopened.ListBooks({.Limit = 10});
    REQUIRE(books.TotalCount == 2);
    REQUIRE(books.Items[0].TitleUtf8 == "Payload Title 1");
    REQUIRE(books.Items[1].TitleUtf8 == "Payload Title 2");
}

TEST_CASE("INPX scan binds execution payloads to the planned archive manifest", "[application-jobs][inpx][fingerprint][aba]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-scan-archive-execution-aba");
    auto config = MakeInpxConfig(sandbox, 1);
    const auto archivePath = config.InpxSource->ArchiveRoot / "fb2-main.zip";
    const std::string payloadA = MakeFb2Payload("Archive A", false);
    const std::string payloadB = MakeFb2Payload("Archive B", false);
    REQUIRE(payloadA.size() == payloadB.size());
    WriteSinglePayloadInpxArchive(archivePath, payloadA);
    const auto databasePath = config.CacheRoot / "Database" / "inpx-web-reader.db";

    {
        InpxWebReader::Application::CInpxCatalogApplication app(config);
        const auto initialJobId =
            app.StartInpxScan({.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::InitialScan});
        REQUIRE(WaitForApplicationScan(app, initialJobId, InpxScanWaitTimeout()));
        REQUIRE(app.ListBooks({.Limit = 10}).Items.front().TitleUtf8 == "Archive A");
    }
    const std::string initialSourceFingerprint = ReadSourceFingerprint(databasePath);
    const auto plannedArchiveMtime = std::filesystem::last_write_time(archivePath);
    const auto archiveAFixture = sandbox.GetPath() / "archive-a.zip";
    const auto archiveBFixture = sandbox.GetPath() / "archive-b.zip";
    std::filesystem::copy_file(
        archivePath,
        archiveAFixture,
        std::filesystem::copy_options::overwrite_existing);
    WriteSinglePayloadInpxArchive(archiveBFixture, payloadB);
    WriteInpxArchive(
        config.InpxSource->InpxPath,
        MakeInpxRecord("book-1", "1", "ru", "changed"));

    std::size_t replacementIndex = 0;
    const auto replaceArchive = [&](const std::filesystem::path& fixture) {
        const auto preparedPath = archivePath.parent_path()
            / ("archive-replacement-" + std::to_string(++replacementIndex) + ".zip");
        std::filesystem::copy_file(
            fixture,
            preparedPath,
            std::filesystem::copy_options::overwrite_existing);
        std::filesystem::last_write_time(preparedPath, plannedArchiveMtime);
        std::filesystem::rename(preparedPath, archivePath);
    };

    bool archiveSwappedBeforeExecution = false;
    bool archiveRestoredBeforeCommit = false;
    InpxWebReader::ApplicationJobs::CInpxScanJobService service(
        sandbox.GetPath() / "runtime-archive-execution-aba",
        {
            .AfterRecordProcessed = [&]() {
                if (!archiveSwappedBeforeExecution)
                {
                    replaceArchive(archiveBFixture);
                    archiveSwappedBeforeExecution = true;
                }
            },
            .BeforeCatalogCommit = [&]() {
                replaceArchive(archiveAFixture);
                archiveRestoredBeforeCommit = true;
            }
        },
        databasePath,
        config.CacheRoot);
    const auto rescanJobId = service.Start(
        *config.InpxSource,
        {.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::Rescan});
    REQUIRE(service.Wait(rescanJobId, InpxScanWaitTimeout()));

    const auto result = service.TryGetResult(rescanJobId);
    if (!archiveRestoredBeforeCommit)
    {
        replaceArchive(archiveAFixture);
    }
    REQUIRE(result.has_value());
    REQUIRE(result->Snapshot.Status == InpxWebReader::ApplicationJobs::EInpxScanJobStatus::Failed);
    REQUIRE(result->Snapshot.Message.find("changed before its payloads were read") != std::string::npos);
    REQUIRE(archiveSwappedBeforeExecution);
    REQUIRE_FALSE(archiveRestoredBeforeCommit);
    REQUIRE(ReadSourceFingerprint(databasePath) == initialSourceFingerprint);

    auto openConfig = MakeBaseConfig(sandbox);
    openConfig.CacheOpenMode = InpxWebReader::Application::ECacheOpenMode::Open;
    InpxWebReader::Application::CInpxCatalogApplication reopened(openConfig);
    REQUIRE(reopened.ListBooks({.Limit = 10}).Items.front().TitleUtf8 == "Archive A");
}

TEST_CASE("INPX scan job service rolls back finalization when cancelled before commit", "[application-jobs][inpx]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-scan-finalize-cancel");
    const auto config = MakeInpxConfig(sandbox, 2);
    const auto databasePath = config.CacheRoot / "Database" / "inpx-web-reader.db";
    const auto slowExecutorTimeout = InpxScanWaitTimeout();

    {
        InpxWebReader::Application::CInpxCatalogApplication app(config);
        const auto initialJobId =
            app.StartInpxScan({.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::InitialScan});
        REQUIRE(WaitForApplicationScan(app, initialJobId, slowExecutorTimeout));
        REQUIRE(ReadInpxBookAvailability(databasePath, "1") == "available");
        REQUIRE(ReadInpxBookAvailability(databasePath, "2") == "available");
    }

    WriteInpxArchive(
        config.InpxSource->InpxPath,
        MakeInpxRecord("book-1", "1", "ru", "", false, "fb2", 0)
            + MakeInpxRecord("book-2", "2", "ru", "", true));
    WriteInpxArchive(
        config.InpxSource->ArchiveRoot / "fb2-main.zip",
        std::vector<std::string>{"Updated Payload Title", "Payload Title 2"});

    std::mutex finalizationMutex;
    std::condition_variable finalizationCondition;
    bool finalizationReached = false;
    bool releaseCommit = false;

    InpxWebReader::ApplicationJobs::CInpxScanJobService service(
        sandbox.GetPath() / "runtime-finalize-cancel",
        {
            .BeforeCatalogCommit = [&]() {
                std::unique_lock lock(finalizationMutex);
                finalizationReached = true;
                finalizationCondition.notify_all();
                finalizationCondition.wait(lock, [&]() {
                    return releaseCommit;
                });
            }
        },
        databasePath,
        config.CacheRoot);

    const auto rescanJobId = service.Start(
        *config.InpxSource,
        {.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::Rescan});

    std::unique_lock finalizationLock(finalizationMutex);
    const bool reachedFinalization = finalizationCondition.wait_for(finalizationLock, slowExecutorTimeout, [&]() {
        return finalizationReached;
    });
    finalizationLock.unlock();
    if (!reachedFinalization)
    {
        static_cast<void>(service.Cancel(rescanJobId));
        std::lock_guard lock(finalizationMutex);
        releaseCommit = true;
        finalizationCondition.notify_all();
    }
    REQUIRE(reachedFinalization);

    const bool cancellationRequested = service.Cancel(rescanJobId);
    {
        std::lock_guard lock(finalizationMutex);
        releaseCommit = true;
    }
    finalizationCondition.notify_all();
    REQUIRE(cancellationRequested);

    REQUIRE(service.Wait(rescanJobId, slowExecutorTimeout));
    const auto cancelledResult = service.TryGetResult(rescanJobId);
    REQUIRE(cancelledResult.has_value());
    REQUIRE(cancelledResult->Snapshot.Status == InpxWebReader::ApplicationJobs::EInpxScanJobStatus::Cancelled);
    REQUIRE(cancelledResult->Error.has_value());
    REQUIRE(cancelledResult->Error->Code == InpxWebReader::Domain::EDomainErrorCode::Cancellation);

    auto openConfig = MakeBaseConfig(sandbox);
    openConfig.CacheOpenMode = InpxWebReader::Application::ECacheOpenMode::Open;
    InpxWebReader::Application::CInpxCatalogApplication reopened(openConfig);
    REQUIRE(ReadInpxBookAvailability(databasePath, "1") == "available");
    REQUIRE(ReadInpxBookAvailability(databasePath, "2") == "available");
    REQUIRE(reopened.ListBooks({.TextUtf8 = "Updated Payload", .Limit = 10}).Items.empty());
    REQUIRE(reopened.GetCatalogStatistics().UnavailableBookCount == 0);
}

TEST_CASE("INPX scan cancellation interrupts blocked transaction acquisition", "[application-jobs][inpx]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-scan-transaction-acquire-cancel");
    const auto config = MakeInpxConfig(sandbox, 1);
    const auto databasePath = config.CacheRoot / "Database" / "inpx-web-reader.db";

    {
        InpxWebReader::Application::CInpxCatalogApplication app(config);
        const auto initialJobId =
            app.StartInpxScan({.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::InitialScan});
        REQUIRE(WaitForApplicationScan(app, initialJobId, InpxScanWaitTimeout()));
        REQUIRE(ReadInpxBookAvailability(databasePath, "1") == "available");
    }

    WriteInpxArchive(
        config.InpxSource->InpxPath,
        MakeInpxRecord("book-1", "1", "ru", "", true));

    InpxWebReader::Sqlite::CSqliteConnection blockingConnection(databasePath);
    InpxWebReader::Sqlite::CSqliteTransaction blockingTransaction(blockingConnection);
    InpxWebReader::ApplicationJobs::CInpxScanJobService service(
        sandbox.GetPath() / "runtime-transaction-acquire-cancel",
        {},
        databasePath,
        config.CacheRoot);
    const auto rescanJobId = service.Start(
        *config.InpxSource,
        {.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::Rescan});

    const auto deadline = std::chrono::steady_clock::now() + InpxScanWaitTimeout();
    bool transactionAcquisitionReached = false;
    while (std::chrono::steady_clock::now() < deadline)
    {
        const auto snapshot = service.TryGetSnapshot(rescanJobId);
        if (snapshot.has_value()
            && snapshot->Message.find("Scanning changed INPX segments") != std::string::npos)
        {
            transactionAcquisitionReached = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(transactionAcquisitionReached);
    REQUIRE(service.Cancel(rescanJobId));
    REQUIRE(service.Wait(rescanJobId, InpxScanWaitTimeout()));
    blockingTransaction.Commit();

    const auto cancelledResult = service.TryGetResult(rescanJobId);
    REQUIRE(cancelledResult.has_value());
    REQUIRE(cancelledResult->Snapshot.Status == InpxWebReader::ApplicationJobs::EInpxScanJobStatus::Cancelled);
    REQUIRE(cancelledResult->Error.has_value());
    REQUIRE(cancelledResult->Error->Code == InpxWebReader::Domain::EDomainErrorCode::Cancellation);
    REQUIRE(ReadInpxBookAvailability(databasePath, "1") == "available");
}
