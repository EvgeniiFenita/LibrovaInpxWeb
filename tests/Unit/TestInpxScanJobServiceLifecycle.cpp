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

TEST_CASE("INPX scan job service completes a planning pass over INPX records", "[application-jobs][inpx]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-scan-service-success");
    const auto config = MakeInpxConfig(sandbox, 3);

    InpxWebReader::ApplicationJobs::CInpxScanJobService service(sandbox.GetPath() / "runtime");
    const auto jobId = service.Start(
        *config.InpxSource,
        {.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::InitialScan});

    REQUIRE(jobId != 0);
    REQUIRE(service.Wait(jobId, InpxScanWaitTimeout()));

    const auto result = service.TryGetResult(jobId);
    REQUIRE(result.has_value());
    REQUIRE(result->Snapshot.Status == InpxWebReader::ApplicationJobs::EInpxScanJobStatus::Completed);
    REQUIRE(result->ScanResult.has_value());
    REQUIRE(result->Snapshot.TotalRecords == 3);
    REQUIRE(result->Snapshot.ScannedRecords == 3);
    REQUIRE(result->Snapshot.ParsedFb2Records == 3);
}

TEST_CASE("INPX scan job service rolls back job record and workspace when worker start fails", "[application-jobs][inpx]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-scan-service-worker-start-failure");
    const auto config = MakeInpxConfig(sandbox, 1);
    const auto runtimeRoot = sandbox.GetPath() / "runtime";
    bool failNextStart = true;
    InpxWebReader::ApplicationJobs::CInpxScanJobService service(runtimeRoot, {
        .BeforeWorkerStart = [&failNextStart] {
            if (failNextStart)
            {
                failNextStart = false;
                throw std::runtime_error("injected worker start failure");
            }
        }
    });

    REQUIRE_THROWS_WITH(
        service.Start(
            *config.InpxSource,
            {.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::InitialScan}),
        Catch::Matchers::ContainsSubstring("injected worker start failure"));

    REQUIRE_FALSE(service.TryGetSnapshot(1).has_value());
    REQUIRE_FALSE(std::filesystem::exists(runtimeRoot / "1"));

    const auto jobId = service.Start(
        *config.InpxSource,
        {.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::InitialScan});

    REQUIRE(jobId == 2);
    REQUIRE(service.Wait(jobId, InpxScanWaitTimeout()));
    const auto result = service.TryGetResult(jobId);
    REQUIRE(result.has_value());
    REQUIRE(result->Snapshot.Status == InpxWebReader::ApplicationJobs::EInpxScanJobStatus::Completed);
}

TEST_CASE("INPX scan job service contains non-standard worker exceptions", "[application-jobs][inpx]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-scan-service-non-standard-worker-failure");
    const auto config = MakeInpxConfig(sandbox, 1);
    InpxWebReader::ApplicationJobs::CInpxScanJobService service(
        sandbox.GetPath() / "runtime",
        {
            .OnSourceFingerprintCheckpoint = []() {
                throw 42;
            }
        });

    const auto jobId = service.Start(
        *config.InpxSource,
        {.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::InitialScan});

    REQUIRE(service.Wait(jobId, InpxScanWaitTimeout()));
    const auto result = service.TryGetResult(jobId);
    REQUIRE(result.has_value());
    REQUIRE(result->Snapshot.Status == InpxWebReader::ApplicationJobs::EInpxScanJobStatus::Failed);
    REQUIRE(result->Snapshot.Message == "INPX scan failed with a non-standard exception.");
    REQUIRE(result->Error.has_value());
    REQUIRE(result->Error->Code == InpxWebReader::Domain::EDomainErrorCode::ParserFailure);
}

TEST_CASE("INPX scan job service exposes cancellation", "[application-jobs][inpx]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-scan-service-cancel");
    const auto config = MakeInpxConfig(sandbox, 200);

    InpxWebReader::ApplicationJobs::CInpxScanJobService service(sandbox.GetPath() / "runtime", {
        .AfterRecordProcessed = []() {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });

    const auto jobId = service.Start(
        *config.InpxSource,
        {.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::Rescan});

    REQUIRE(jobId != 0);

    const auto deadline = std::chrono::steady_clock::now() + InpxScanWaitTimeout();
    while (std::chrono::steady_clock::now() < deadline)
    {
        const auto snapshot = service.TryGetSnapshot(jobId);
        if (snapshot.has_value() && snapshot->ScannedRecords > 0)
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    REQUIRE(service.Cancel(jobId));
    REQUIRE(service.Wait(jobId, InpxScanWaitTimeout()));

    const auto result = service.TryGetResult(jobId);
    REQUIRE(result.has_value());
    REQUIRE(result->Snapshot.Status == InpxWebReader::ApplicationJobs::EInpxScanJobStatus::Cancelled);
    REQUIRE(result->Error.has_value());
    REQUIRE(result->Error->Code == InpxWebReader::Domain::EDomainErrorCode::Cancellation);
}

TEST_CASE("INPX scan maps source INP and archive checkpoint cancellation", "[application-jobs][inpx][cancellation][fingerprint]")
{
    enum class ECheckpointKind
    {
        SourceFingerprint,
        InpParser,
        ArchiveManifest
    };

    const auto runCheckpointCancellation = [](const ECheckpointKind checkpointKind) {
        const std::string workspaceName = checkpointKind == ECheckpointKind::SourceFingerprint
            ? "inpx-web-reader-inpx-scan-source-fingerprint-checkpoint-cancel"
            : checkpointKind == ECheckpointKind::InpParser
                ? "inpx-web-reader-inpx-scan-inp-parser-checkpoint-cancel"
                : "inpx-web-reader-inpx-scan-manifest-checkpoint-cancel";
        CTestWorkspace sandbox(workspaceName);
        const auto config = MakeInpxConfig(sandbox, 1);
        std::mutex checkpointMutex;
        std::condition_variable checkpointCondition;
        bool checkpointReached = false;
        bool releaseCheckpoint = false;
        const auto blockAtCheckpoint = [&]() {
            std::unique_lock lock(checkpointMutex);
            if (checkpointReached)
            {
                return;
            }
            checkpointReached = true;
            checkpointCondition.notify_all();
            checkpointCondition.wait(lock, [&]() {
                return releaseCheckpoint;
            });
        };

        InpxWebReader::ApplicationJobs::CInpxScanJobService::SHooks hooks;
        if (checkpointKind == ECheckpointKind::SourceFingerprint)
        {
            hooks.OnSourceFingerprintCheckpoint = blockAtCheckpoint;
        }
        else if (checkpointKind == ECheckpointKind::InpParser)
        {
            hooks.OnInpParserCheckpoint = blockAtCheckpoint;
        }
        else
        {
            hooks.OnArchiveManifestCheckpoint = blockAtCheckpoint;
        }
        InpxWebReader::ApplicationJobs::CInpxScanJobService service(
            sandbox.GetPath() / "runtime",
            std::move(hooks));
        const auto jobId = service.Start(
            *config.InpxSource,
            {.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::InitialScan});

        std::unique_lock checkpointLock(checkpointMutex);
        const bool reached = checkpointCondition.wait_for(
            checkpointLock,
            InpxScanWaitTimeout(),
            [&]() {
                return checkpointReached;
            });
        checkpointLock.unlock();
        bool cancellationRequested = false;
        if (reached)
        {
            cancellationRequested = service.Cancel(jobId);
        }
        else
        {
            std::lock_guard lock(checkpointMutex);
            releaseCheckpoint = true;
            checkpointCondition.notify_all();
        }
        {
            std::lock_guard lock(checkpointMutex);
            releaseCheckpoint = true;
        }
        checkpointCondition.notify_all();
        REQUIRE(reached);
        REQUIRE(cancellationRequested);
        REQUIRE(service.Wait(jobId, InpxScanWaitTimeout()));

        const auto result = service.TryGetResult(jobId);
        REQUIRE(result.has_value());
        REQUIRE(result->Snapshot.Status == InpxWebReader::ApplicationJobs::EInpxScanJobStatus::Cancelled);
        REQUIRE(result->Error.has_value());
        REQUIRE(result->Error->Code == InpxWebReader::Domain::EDomainErrorCode::Cancellation);
    };

    SECTION("source SHA-256")
    {
        runCheckpointCancellation(ECheckpointKind::SourceFingerprint);
    }
    SECTION("INP parsing")
    {
        runCheckpointCancellation(ECheckpointKind::InpParser);
    }
    SECTION("archive manifest")
    {
        runCheckpointCancellation(ECheckpointKind::ArchiveManifest);
    }
}

TEST_CASE("INPX scan job service waits for in-flight payload work when cancelled", "[application-jobs][inpx]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-scan-service-cancel-in-flight");
    auto config = MakeInpxConfig(sandbox, 1);
    WriteInpxArchive(
        config.InpxSource->InpxPath,
        MakeInpxRecord("book-1", "1") + MakeInpxRecord("book-2", "2", "ru", "", true));
    WriteInpxArchive(config.InpxSource->ArchiveRoot / "fb2-main.zip", 1, true);
    const auto databasePath = config.CacheRoot / "Database" / "inpx-web-reader.db";
    const auto logPath = sandbox.GetPath() / "inpx-scan-cancel-in-flight.log";

    {
        InpxWebReader::Application::CInpxCatalogApplication app(config);
    }

    CBlockingCoverImageProcessor coverProcessor;
    std::atomic<InpxWebReader::ApplicationJobs::TInpxScanJobId> startedJobId{0};
    InpxWebReader::ApplicationJobs::CInpxScanJobService* servicePtr = nullptr;
    std::mutex cancellationMutex;
    std::condition_variable cancellationCondition;
    std::size_t processedRecords = 0;
    bool cancellationRequestedFromHook = false;
    InpxWebReader::Logging::CLogging::InitializeHostLogger(logPath);
    try
    {
        InpxWebReader::ApplicationJobs::CInpxScanJobService service(
            sandbox.GetPath() / "runtime",
            {
                .AfterRecordProcessed = [&]() {
                    ++processedRecords;
                    if (processedRecords != 2)
                    {
                        return;
                    }

                    while (startedJobId.load() == 0)
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                    if (!coverProcessor.WaitUntilStarted(1, InpxScanWaitTimeout()))
                    {
                        cancellationCondition.notify_all();
                        return;
                    }
                    const bool cancelled = servicePtr != nullptr && servicePtr->Cancel(startedJobId.load());
                    {
                        std::lock_guard lock(cancellationMutex);
                        cancellationRequestedFromHook = cancelled;
                    }
                    cancellationCondition.notify_all();
                }
            },
            databasePath,
            config.CacheRoot,
            &coverProcessor);
        servicePtr = &service;

        const auto jobId = service.Start(
            *config.InpxSource,
            {.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::InitialScan});
        startedJobId.store(jobId);

        const bool payloadWorkerStarted = coverProcessor.WaitUntilStarted(1, InpxScanWaitTimeout());
        if (!payloadWorkerStarted)
        {
            coverProcessor.Release();
        }
        REQUIRE(payloadWorkerStarted);

        {
            std::unique_lock lock(cancellationMutex);
            const bool cancellationObserved = cancellationCondition.wait_for(lock, InpxScanWaitTimeout(), [&]() {
                return cancellationRequestedFromHook;
            });
            if (!cancellationObserved)
            {
                coverProcessor.Release();
            }
            REQUIRE(cancellationObserved);
        }

        if (!cancellationRequestedFromHook)
        {
            coverProcessor.Release();
        }
        REQUIRE(cancellationRequestedFromHook);

        const bool finishedBeforePayloadWorkerReleased = service.Wait(jobId, std::chrono::milliseconds(100));
        coverProcessor.Release();
        REQUIRE_FALSE(finishedBeforePayloadWorkerReleased);

        REQUIRE(service.Wait(jobId, InpxScanWaitTimeout()));
        REQUIRE(coverProcessor.WaitUntilFinished(1, InpxScanWaitTimeout()));

        const auto result = service.TryGetResult(jobId);
        REQUIRE(result.has_value());
        REQUIRE(result->Snapshot.Status == InpxWebReader::ApplicationJobs::EInpxScanJobStatus::Cancelled);
        REQUIRE(result->Error.has_value());
        REQUIRE(result->Error->Code == InpxWebReader::Domain::EDomainErrorCode::Cancellation);
        InpxWebReader::Logging::CLogging::Shutdown();
    }
    catch (...)
    {
        coverProcessor.Release();
        InpxWebReader::Logging::CLogging::Shutdown();
        throw;
    }

    const std::string logText = ReadAllText(logPath);
    REQUIRE(logText.find("payload-load-failed") == std::string::npos);
}

TEST_CASE("INPX scan job service rejects a second active scan", "[application-jobs][inpx]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-scan-service-single-active");
    const auto config = MakeInpxConfig(sandbox, 200);

    InpxWebReader::ApplicationJobs::CInpxScanJobService service(sandbox.GetPath() / "runtime", {
        .AfterRecordProcessed = []() {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });

    const auto jobId = service.Start(
        *config.InpxSource,
        {.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::InitialScan});
    REQUIRE(jobId != 0);

    const auto deadline = std::chrono::steady_clock::now() + InpxScanWaitTimeout();
    while (std::chrono::steady_clock::now() < deadline)
    {
        const auto snapshot = service.TryGetSnapshot(jobId);
        if (snapshot.has_value() && snapshot->ScannedRecords > 0)
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    REQUIRE_THROWS_WITH(
        service.Start(
            *config.InpxSource,
            {.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::Rescan}),
        Catch::Matchers::ContainsSubstring("already running"));
    REQUIRE(service.Cancel(jobId));
    REQUIRE(service.Wait(jobId, InpxScanWaitTimeout()));
}

TEST_CASE("INPX scan job service writes performance summary when cancelled", "[application-jobs][inpx][logging]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-scan-service-cancel-perf");
    const auto config = MakeInpxConfig(sandbox, 200);
    const auto logPath = sandbox.GetPath() / "inpx-scan-cancel.log";

    InpxWebReader::Logging::CLogging::InitializeHostLogger(logPath);
    try
    {
        InpxWebReader::ApplicationJobs::CInpxScanJobService service(sandbox.GetPath() / "runtime", {
            .AfterRecordProcessed = []() {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        });

        const auto jobId = service.Start(
            *config.InpxSource,
            {.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::Rescan});
        REQUIRE(jobId != 0);

        const auto deadline = std::chrono::steady_clock::now() + InpxScanWaitTimeout();
        while (std::chrono::steady_clock::now() < deadline)
        {
            const auto snapshot = service.TryGetSnapshot(jobId);
            if (snapshot.has_value() && snapshot->ScannedRecords > 0)
            {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        REQUIRE(service.Cancel(jobId));
        REQUIRE(service.Wait(jobId, InpxScanWaitTimeout()));
        InpxWebReader::Logging::CLogging::Shutdown();
    }
    catch (...)
    {
        InpxWebReader::Logging::CLogging::Shutdown();
        throw;
    }

    const std::string logText = ReadAllText(logPath);
    REQUIRE_THAT(logText, Catch::Matchers::ContainsSubstring("INPX scan cancelled"));
    REQUIRE_THAT(logText, Catch::Matchers::ContainsSubstring("[scan-perf] SUMMARY"));
}

TEST_CASE("INPX scan job service removes completed job workspaces", "[application-jobs][inpx]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-scan-service-workspace-cleanup");
    const auto config = MakeInpxConfig(sandbox, 1);
    const auto runtimeRoot = sandbox.GetPath() / "runtime";
    const auto staleWorkspace = runtimeRoot / "42";
    std::filesystem::create_directories(staleWorkspace);
    std::ofstream(staleWorkspace / "stale.tmp", std::ios::binary) << "stale";

    InpxWebReader::ApplicationJobs::CInpxScanJobService service(runtimeRoot);
    REQUIRE_FALSE(std::filesystem::exists(staleWorkspace));

    const auto jobId = service.Start(
        *config.InpxSource,
        {.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::InitialScan});
    REQUIRE(service.Wait(jobId, InpxScanWaitTimeout()));
    const auto jobWorkspace = runtimeRoot / std::to_string(jobId);
    REQUIRE(std::filesystem::is_directory(jobWorkspace));

    REQUIRE(service.Remove(jobId));
    REQUIRE_FALSE(std::filesystem::exists(jobWorkspace));
}

TEST_CASE("CInpxCatalogApplication starts INPX scan jobs for the INPX catalog", "[application-jobs][inpx]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-scan-app-success");
    const auto config = MakeInpxConfig(sandbox, 4);
    InpxWebReader::Application::CInpxCatalogApplication app(config);

    const auto jobId = app.StartInpxScan({.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::InitialScan});

    REQUIRE(jobId != 0);
    REQUIRE(WaitForApplicationScan(app, jobId, InpxScanWaitTimeout()));

    const auto result = app.GetInpxScanJobResult(jobId);
    REQUIRE(result.has_value());
    REQUIRE(result->Snapshot.Status == InpxWebReader::ApplicationJobs::EInpxScanJobStatus::Completed);
    REQUIRE(result->ScanResult.has_value());
    REQUIRE(result->Snapshot.TotalRecords == 4);
    REQUIRE(result->Snapshot.ScannedRecords == 4);
    REQUIRE(result->Snapshot.ParsedFb2Records == 4);
    REQUIRE(result->Snapshot.AddedRecords == 4);

    const auto list = app.ListBooks({.Limit = 10});
    REQUIRE(list.TotalCount == 4);
    REQUIRE(list.Items.size() == 4);
    REQUIRE(list.Items.front().CanDownloadOriginal);
    REQUIRE_FALSE(list.Items.front().CanDownloadAsEpub);
    REQUIRE(list.Items.front().IsAvailable);
    REQUIRE(list.Items.front().AvailabilityLabelUtf8.empty());

    const auto details = app.GetBookDetails(list.Items.front().Id);
    REQUIRE(details.has_value());
    REQUIRE(details->CanDownloadOriginal);
    REQUIRE_FALSE(details->CanDownloadAsEpub);
    REQUIRE(details->IsAvailable);
    REQUIRE(details->AvailabilityLabelUtf8.empty());

    const auto statistics = app.GetCatalogStatistics();
    const auto databasePath = config.CacheRoot / "Database" / "inpx-web-reader.db";
    const auto inpxSourceSize = static_cast<std::uint64_t>(
        std::filesystem::file_size(config.InpxSource->InpxPath)
        + std::filesystem::file_size(config.InpxSource->ArchiveRoot / "fb2-main.zip"));
    const auto databaseSize =
        InpxWebReader::BookDatabase::CCatalogStatisticsMaintenance::GetDatabaseFootprintSize(databasePath);
    REQUIRE(statistics.BookCount == 4);
    REQUIRE(statistics.UnavailableBookCount == 0);
    REQUIRE(statistics.InpxSourceSizeBytes == inpxSourceSize);
    REQUIRE(statistics.TotalCatalogSizeBytes == inpxSourceSize + databaseSize);
}

TEST_CASE("CInpxCatalogApplication normalizes INPX scan language metadata", "[application-jobs][inpx][language-normalization]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-scan-language-normalization");
    auto config = MakeBaseConfig(sandbox);
    config.InpxSource = InpxWebReader::Application::SInpxSourceInfo{
        .InpxPath = WriteInpxArchive(
            sandbox.GetPath() / "source" / "catalog.inpx",
            MakeInpxRecord("book-1", "1", "RU")),
        .ArchiveRoot = sandbox.GetPath() / "source" / "archives"
    };
    std::filesystem::create_directories(config.InpxSource->ArchiveRoot);
    WriteSinglePayloadInpxArchive(
        config.InpxSource->ArchiveRoot / "fb2-main.zip",
        MakeFb2Payload("Payload Title", false, "EN-US"));

    InpxWebReader::Application::CInpxCatalogApplication app(config);
    const auto jobId = app.StartInpxScan({.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::InitialScan});

    REQUIRE(WaitForApplicationScan(app, jobId, InpxScanWaitTimeout()));
    const auto result = app.GetInpxScanJobResult(jobId);
    REQUIRE(result.has_value());
    REQUIRE(result->Snapshot.Status == InpxWebReader::ApplicationJobs::EInpxScanJobStatus::Completed);

    const auto list = app.ListBooks({.Limit = 10});
    REQUIRE(list.Items.size() == 1);
    REQUIRE(list.AvailableLanguages == std::vector<InpxWebReader::Domain::SFacetItem>({{"en", 1}}));

    const auto details = app.GetBookDetails(list.Items.front().Id);
    REQUIRE(details.has_value());
    REQUIRE(details->Language == "en");
}

TEST_CASE("CInpxCatalogApplication opens a portable cache without source configuration", "[application-jobs][inpx]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-scan-portable-cache-offline");
    const auto createConfig = MakeInpxConfig(sandbox, 1);
    {
        InpxWebReader::Application::CInpxCatalogApplication app(createConfig);
        const auto jobId = app.StartInpxScan({
            .Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::InitialScan
        });
        REQUIRE(WaitForApplicationScan(app, jobId, InpxScanWaitTimeout()));
        REQUIRE(app.GetInpxScanJobResult(jobId)->Snapshot.Status
            == InpxWebReader::ApplicationJobs::EInpxScanJobStatus::Completed);
    }

    auto openConfig = createConfig;
    openConfig.CacheOpenMode = InpxWebReader::Application::ECacheOpenMode::Open;
    openConfig.RuntimeWorkspaceRoot = sandbox.GetPath() / "RuntimeOffline";
    openConfig.InpxSource = std::nullopt;
    InpxWebReader::Application::CInpxCatalogApplication app(openConfig);

    const auto session = app.GetCatalogSessionInfo();
    REQUIRE_FALSE(session.InpxSource.has_value());
    REQUIRE_FALSE(session.Capabilities.CanRescanInpxSource);
    REQUIRE_FALSE(session.Capabilities.CanDownloadOriginal);

    const auto list = app.ListBooks({.Limit = 10});
    REQUIRE(list.TotalCount == 1);
    REQUIRE(list.Items.size() == 1);
    REQUIRE_FALSE(list.Items.front().CanDownloadOriginal);

    const auto overview = app.GetInpxSourceOverview();
    REQUIRE(overview.has_value());
    REQUIRE_FALSE(overview->IsSourceAvailable);
    REQUIRE(overview->Source.InpxPath.empty());
    REQUIRE(overview->Source.ArchiveRoot.empty());
    REQUIRE(overview->SourceWarningUtf8.find("not configured") != std::string::npos);

    REQUIRE_THROWS_AS(
        app.StartInpxScan({.Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::Rescan}),
        InpxWebReader::Domain::CDomainException);
}

TEST_CASE("CInpxCatalogApplication rehashes the INPX source before download", "[application-jobs][inpx][fingerprint]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-download-source-fingerprint-refresh");
    const auto config = MakeInpxConfig(sandbox, 1);
    InpxWebReader::Application::CInpxCatalogApplication app(config);
    const auto scanJobId = app.StartInpxScan({
        .Mode = InpxWebReader::ApplicationJobs::EInpxScanMode::InitialScan
    });
    REQUIRE(WaitForApplicationScan(app, scanJobId, InpxScanWaitTimeout()));
    REQUIRE(app.GetCatalogSessionInfo().Capabilities.CanDownloadOriginal);

    const auto inpxPath = config.InpxSource->InpxPath;
    const auto originalSize = std::filesystem::file_size(inpxPath);
    const auto originalWriteTime = std::filesystem::last_write_time(inpxPath);
    {
        std::fstream inpx(inpxPath, std::ios::binary | std::ios::in | std::ios::out);
        REQUIRE(inpx.is_open());
        char firstByte = 0;
        inpx.read(&firstByte, 1);
        REQUIRE(inpx.gcount() == 1);
        inpx.clear();
        inpx.seekp(0);
        inpx.put(firstByte == 'X' ? 'Y' : 'X');
        REQUIRE(inpx.good());
    }
    std::filesystem::last_write_time(inpxPath, originalWriteTime);
    REQUIRE(std::filesystem::file_size(inpxPath) == originalSize);
    REQUIRE(std::filesystem::last_write_time(inpxPath) == originalWriteTime);

    const auto list = app.ListBooks({.Limit = 10});
    REQUIRE(list.Items.size() == 1);
    REQUIRE_THROWS_WITH(
        static_cast<void>(app.PrepareDownload({
            .BookId = list.Items.front().Id,
            .DestinationPath = sandbox.GetPath() / "download.fb2"
        })),
        Catch::Matchers::ContainsSubstring("rescan is required"));
}

TEST_CASE("CInpxCatalogApplication falls back to INPX language when source FB2 lang is missing", "[application-jobs][inpx][language-normalization]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-scan-language-fallback");
    auto config = MakeBaseConfig(sandbox);
    config.InpxSource = InpxWebReader::Application::SInpxSourceInfo{
        .InpxPath = WriteInpxArchive(
            sandbox.GetPath() / "source" / "catalog.inpx",
            MakeInpxRecord("book-1", "1", "ENG")),
        .ArchiveRoot = sandbox.GetPath() / "source" / "archives"
    };
    std::filesystem::create_directories(config.InpxSource->ArchiveRoot);
    WriteSinglePayloadInpxArchive(
        config.InpxSource->ArchiveRoot / "fb2-main.zip",
        R"(<?xml version="1.0" encoding="UTF-8"?>
<FictionBook>
  <description>
    <title-info>
      <genre>science_fiction</genre>
      <book-title>Payload Title</book-title>
      <author>
        <first-name>Payload</first-name>
        <last-name>Author</last-name>
      </author>
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
    REQUIRE(result->ScanResult->MetadataFallbackCount == 1);
    REQUIRE(result->ScanResult->LanguageFallbackCount == 1);

    const auto list = app.ListBooks({.Limit = 10});
    REQUIRE(list.Items.size() == 1);
    REQUIRE(list.AvailableLanguages == std::vector<InpxWebReader::Domain::SFacetItem>({{"en", 1}}));

    const auto details = app.GetBookDetails(list.Items.front().Id);
    REQUIRE(details.has_value());
    REQUIRE(details->Language == "en");
}
