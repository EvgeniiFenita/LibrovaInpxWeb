#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include <httplib.h>
#include <nlohmann/json.hpp>
#include <zip.h>

#include "Domain/DomainError.hpp"
#include "Foundation/UnicodeConversion.hpp"
#include "Server/HttpServer.hpp"
#include "Server/ServerApplicationHost.hpp"
#include "Server/ServerConfig.hpp"
#include "TestServerFixtures.hpp"
#include "TestWorkspace.hpp"

namespace {

using namespace std::chrono_literals;

namespace Application = InpxWebReader::Application;
namespace ApplicationJobs = InpxWebReader::ApplicationJobs;
namespace Domain = InpxWebReader::Domain;
namespace Server = InpxWebReader::Server;

class CFakeScanServerHost final : public Server::IServerApplicationHost
{
public:
    [[nodiscard]] bool IsOpen() const noexcept override
    {
        return Open;
    }

    [[nodiscard]] Server::SServerStatus GetStatus() override
    {
        return Status;
    }

    [[nodiscard]] Application::SBookListResult ListBooks(const Application::SBookListRequest&) override
    {
        return {};
    }

    [[nodiscard]] std::optional<Application::SBookDetails> GetBookDetails(Domain::SBookId) override
    {
        return std::nullopt;
    }

    [[nodiscard]] Application::SCatalogStatistics GetCatalogStatistics() override
    {
        return {};
    }

    [[nodiscard]] std::optional<Server::SServerFileResponse> ResolveBookCover(Domain::SBookId) override
    {
        return std::nullopt;
    }

    [[nodiscard]] std::optional<Server::SServerFileResponse> PrepareBookDownload(
        Domain::SBookId,
        Server::EServerDownloadFormat) override
    {
        return std::nullopt;
    }

    [[nodiscard]] std::optional<Application::SInpxSourceOverview> GetInpxSourceOverview() override
    {
        ++SourceOverviewCalls;
        return SourceOverview;
    }

    [[nodiscard]] ApplicationJobs::TInpxScanJobId StartInpxScan(
        const ApplicationJobs::SInpxScanRequest& request) override
    {
        ++StartCalls;
        LastRequest = request;
        if (Snapshot.has_value() && !Snapshot->IsTerminal())
        {
            throw std::runtime_error("An INPX scan is already running.");
        }

        const auto jobId = NextJobId++;
        ActiveJobId = jobId;
        CancelRequested = false;
        CancelSnapshotPublished = false;
        Result = std::nullopt;
        Snapshot = ApplicationJobs::SInpxScanJobSnapshot{
            .JobId = jobId,
            .Status = ApplicationJobs::EInpxScanJobStatus::Running,
            .Percent = 37,
            .Message = "Scanning INPX source.",
            .Warnings = {"Skipped one damaged record."},
            .TotalRecords = 12,
            .ScannedRecords = 4,
            .ParsedFb2Records = 3,
            .AddedRecords = 2,
            .UpdatedRecords = 1,
            .MarkedUnavailableRecords = 1,
            .SkippedRecords = 1,
            .ReusedRecords = 7,
            .SegmentsTotal = 3,
            .SegmentsUnchanged = 2,
            .SegmentsAdded = 1,
            .ArchivesSkipped = 2,
            .ArchivesOpened = 1,
            .ArchiveBytesRead = 4096,
            .CurrentArchiveNameUtf8 = "fb2-main.zip",
            .CurrentEntryNameUtf8 = "book.fb2"
        };
        return jobId;
    }

    [[nodiscard]] std::optional<ApplicationJobs::SInpxScanJobSnapshot> GetInpxScanJobSnapshot(
        const ApplicationJobs::TInpxScanJobId jobId) override
    {
        ++SnapshotCalls;
        if (!Snapshot.has_value() || Snapshot->JobId != jobId)
        {
            return std::nullopt;
        }

        if (!CancelRequested)
        {
            return Snapshot;
        }

        if (!CancelSnapshotPublished)
        {
            Snapshot->Status = ApplicationJobs::EInpxScanJobStatus::Cancelling;
            Snapshot->Message = "Cancelling INPX source scan.";
            CancelSnapshotPublished = true;
            return Snapshot;
        }

        Snapshot->Status = ApplicationJobs::EInpxScanJobStatus::Cancelled;
        Snapshot->Message = "INPX source scan cancelled.";
        Result = ApplicationJobs::SInpxScanJobResult{
            .Snapshot = *Snapshot,
            .ScanResult = std::nullopt,
            .Error = Domain::SDomainError{
                .Code = Domain::EDomainErrorCode::Cancellation,
                .Message = "INPX source scan cancelled."
            }
        };
        return Snapshot;
    }

    [[nodiscard]] std::optional<ApplicationJobs::SInpxScanJobResult> GetInpxScanJobResult(
        const ApplicationJobs::TInpxScanJobId jobId) override
    {
        ++ResultCalls;
        if (!Result.has_value() || Result->Snapshot.JobId != jobId)
        {
            return std::nullopt;
        }

        return Result;
    }

    [[nodiscard]] bool CancelInpxScanJob(const ApplicationJobs::TInpxScanJobId jobId) override
    {
        ++CancelCalls;
        if (!Snapshot.has_value() || Snapshot->JobId != jobId || Snapshot->IsTerminal())
        {
            return false;
        }

        CancelRequested = true;
        return true;
    }

    [[nodiscard]] bool RemoveInpxScanJob(const ApplicationJobs::TInpxScanJobId jobId) override
    {
        ++RemoveCalls;
        RemovedJobId = jobId;
        return true;
    }

    bool Open = true;
    Server::SServerStatus Status{
        .VersionUtf8 = "0.1.0",
        .IsOpen = true,
        .Capabilities = {
            .CanRescanInpxSource = true,
            .CanDownloadOriginal = true,
            .CanDownloadAsEpub = true
        },
        .InpxSource = Server::SServerInpxSourceStatus{
            .InpxPathUtf8 = "/source/catalog.inpx",
            .ArchiveRootUtf8 = "/source/archives",
            .IsSourceAvailable = true,
            .RequiresRescan = false,
            .SourceWarningUtf8 = {},
            .TotalBookCount = 42,
            .AvailableBookCount = 40,
            .UnavailableBookCount = 2,
            .WarningCount = 3
        }
    };
    Application::SInpxSourceOverview SourceOverview{
        .Source = {
            .InpxPath = std::filesystem::path{"/source/catalog.inpx"},
            .ArchiveRoot = std::filesystem::path{"/source/archives"}
        },
        .DisplayNameUtf8 = "catalog.inpx",
        .IsSourceAvailable = true,
        .RequiresRescan = false,
        .SourceWarningUtf8 = {},
        .LastScanStartedAtUtc = "2026-05-20T10:00:00Z",
        .LastScanCompletedAtUtc = "2026-05-20T10:01:00Z",
        .LastSeenSnapshotId = "snapshot-1",
        .TotalBookCount = 42,
        .AvailableBookCount = 40,
        .UnavailableBookCount = 2,
        .WarningCount = 3,
        .RecentWarningsUtf8 = {"Skipped one damaged record."}
    };
    std::optional<ApplicationJobs::SInpxScanRequest> LastRequest = std::nullopt;
    std::optional<ApplicationJobs::SInpxScanJobSnapshot> Snapshot = std::nullopt;
    std::optional<ApplicationJobs::SInpxScanJobResult> Result = std::nullopt;
    std::optional<ApplicationJobs::TInpxScanJobId> ActiveJobId = std::nullopt;
    std::optional<ApplicationJobs::TInpxScanJobId> RemovedJobId = std::nullopt;
    ApplicationJobs::TInpxScanJobId NextJobId = 1;
    bool CancelRequested = false;
    bool CancelSnapshotPublished = false;
    int SourceOverviewCalls = 0;
    int StartCalls = 0;
    int SnapshotCalls = 0;
    int ResultCalls = 0;
    int CancelCalls = 0;
    int RemoveCalls = 0;
};

[[nodiscard]] httplib::Result GetWithRetry(
    httplib::Client& client,
    const char* path,
    const httplib::Headers& headers = {})
{
    for (int attempt = 0; attempt < 50; ++attempt)
    {
        if (auto response = client.Get(path, headers))
        {
            return response;
        }

        std::this_thread::sleep_for(10ms);
    }

    return {};
}

[[nodiscard]] httplib::Result PostWithRetry(
    httplib::Client& client,
    const char* path,
    const std::string& bodyUtf8 = {},
    const httplib::Headers& headers = {})
{
    for (int attempt = 0; attempt < 50; ++attempt)
    {
        if (auto response = client.Post(path, headers, bodyUtf8, "application/json"))
        {
            return response;
        }

        std::this_thread::sleep_for(10ms);
    }

    return {};
}

[[nodiscard]] std::unique_ptr<Server::CHttpServer> StartTestServer(
    Server::IServerApplicationHost& host,
    Server::SServerSecurityConfig security = {})
{
    auto server = std::make_unique<Server::CHttpServer>(
        host,
        Server::SHttpServerOptions{
            .HostUtf8 = "127.0.0.1",
            .Port = 0,
            .Security = std::move(security)
        });
    server->Start();
    return server;
}

[[nodiscard]] std::string MakeFb2Payload()
{
    return R"(<?xml version="1.0" encoding="UTF-8"?>
<FictionBook xmlns:l="http://www.w3.org/1999/xlink">
  <description>
    <title-info>
      <book-title>Payload Title</book-title>
      <author>
        <first-name>Ivan</first-name>
        <last-name>Testov</last-name>
      </author>
      <lang>ru</lang>
    </title-info>
  </description>
  <body>
    <section><p>Payload</p></section>
  </body>
</FictionBook>)";
}

std::filesystem::path WriteInpxPayloadArchive(const std::filesystem::path& archivePath)
{
    std::filesystem::create_directories(archivePath.parent_path());

    int errorCode = ZIP_ER_OK;
    const auto archivePathUtf8 = InpxWebReader::Unicode::PathToUtf8(archivePath);
    zip_t* archive = zip_open(archivePathUtf8.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &errorCode);
    if (archive == nullptr)
    {
        throw std::runtime_error("Failed to create INPX payload archive.");
    }

    InpxWebReader::Tests::ServerFixtures::AddZipEntry(archive, "book.fb2", MakeFb2Payload());

    if (zip_close(archive) != 0)
    {
        zip_discard(archive);
        throw std::runtime_error("Failed to finalize INPX payload archive.");
    }

    return archivePath;
}

[[nodiscard]] nlohmann::json WaitForTerminalScan(httplib::Client& client)
{
    for (int attempt = 0; attempt < 100; ++attempt)
    {
        const auto response = GetWithRetry(client, "/api/scan/progress");
        REQUIRE(response);
        REQUIRE(response->status == 200);
        auto body = nlohmann::json::parse(response->body);
        if (!body.at("active").get<bool>())
        {
            return body;
        }

        std::this_thread::sleep_for(20ms);
    }

    FAIL("Timed out waiting for terminal INPX scan progress.");
    return {};
}

} // namespace

TEST_CASE("HTTP source reports INPX overview", "[server][http][scan]")
{
    CFakeScanServerHost host;
    auto server = StartTestServer(host);
    httplib::Client client("127.0.0.1", server->GetBoundPort());

    const auto response = GetWithRetry(client, "/api/source");

    REQUIRE(response);
    REQUIRE(response->status == 200);
    const auto body = nlohmann::json::parse(response->body);
    const auto& source = body.at("source");
    REQUIRE(source.at("inpxPath").get<std::string>().find("catalog.inpx") != std::string::npos);
    REQUIRE(source.at("archiveRoot").get<std::string>().find("archives") != std::string::npos);
    REQUIRE(source.at("displayName").get<std::string>() == "catalog.inpx");
    REQUIRE(source.at("available").get<bool>());
    REQUIRE_FALSE(source.at("requiresRescan").get<bool>());
    REQUIRE(source.at("totalBookCount").get<std::size_t>() == 42);
    REQUIRE(source.at("availableBookCount").get<std::size_t>() == 40);
    REQUIRE(source.at("unavailableBookCount").get<std::size_t>() == 2);
    REQUIRE(source.at("warningCount").get<std::size_t>() == 3);
    REQUIRE(source.at("recentWarnings").at(0).get<std::string>() == "Skipped one damaged record.");
    REQUIRE(host.SourceOverviewCalls == 1);
}

TEST_CASE("HTTP scan start returns accepted job progress and rejects a concurrent start", "[server][http][scan]")
{
    CFakeScanServerHost host;
    auto server = StartTestServer(host);
    httplib::Client client("127.0.0.1", server->GetBoundPort());

    const auto started = PostWithRetry(client, "/api/scan/start", R"({"mode":"rescan","warningLimit":7})");

    REQUIRE(started);
    REQUIRE(started->status == 202);
    const auto startBody = nlohmann::json::parse(started->body);
    REQUIRE(startBody.at("jobId").get<std::uint64_t>() == 1);
    const auto& scan = startBody.at("scan");
    REQUIRE(scan.at("active").get<bool>());
    REQUIRE(scan.at("status").get<std::string>() == "running");
    REQUIRE(scan.at("percent").get<int>() == 37);
    REQUIRE(scan.at("totalRecords").get<std::size_t>() == 12);
    REQUIRE(scan.at("scannedRecords").get<std::size_t>() == 4);
    REQUIRE(scan.at("parsedFb2Records").get<std::size_t>() == 3);
    REQUIRE(scan.at("addedRecords").get<std::size_t>() == 2);
    REQUIRE(scan.at("updatedRecords").get<std::size_t>() == 1);
    REQUIRE(scan.at("markedUnavailableRecords").get<std::size_t>() == 1);
    REQUIRE(scan.at("unavailableRecords").get<std::size_t>() == 1);
    REQUIRE(scan.at("skippedRecords").get<std::size_t>() == 1);
    REQUIRE(scan.at("reusedRecords").get<std::size_t>() == 7);
    REQUIRE(scan.at("segmentsTotal").get<std::size_t>() == 3);
    REQUIRE(scan.at("segmentsUnchanged").get<std::size_t>() == 2);
    REQUIRE(scan.at("segmentsAdded").get<std::size_t>() == 1);
    REQUIRE(scan.at("archivesSkipped").get<std::size_t>() == 2);
    REQUIRE(scan.at("archivesOpened").get<std::size_t>() == 1);
    REQUIRE(scan.at("archiveBytesRead").get<std::uint64_t>() == 4096);
    REQUIRE(scan.at("current").at("archiveName").get<std::string>() == "fb2-main.zip");
    REQUIRE(scan.at("current").at("entryName").get<std::string>() == "book.fb2");
    REQUIRE(scan.at("warnings").at(0).get<std::string>() == "Skipped one damaged record.");
    REQUIRE(host.LastRequest.has_value());
    REQUIRE(host.LastRequest->Mode == ApplicationJobs::EInpxScanMode::Rescan);
    REQUIRE(host.LastRequest->WarningLimit == 7);

    const auto status = GetWithRetry(client, "/api/status");
    REQUIRE(status);
    REQUIRE(status->status == 200);
    const auto statusBody = nlohmann::json::parse(status->body);
    REQUIRE(statusBody.at("scan").at("active").get<bool>());
    REQUIRE_FALSE(statusBody.at("inpxSource").at("requiresRescan").get<bool>());
    REQUIRE(statusBody.at("scan").at("status").get<std::string>() == "running");
    REQUIRE(statusBody.at("runtime").at("scan").at("active").get<bool>());
    REQUIRE(statusBody.at("runtime").at("scan").at("activeJobs").get<int>() == 1);
    REQUIRE(statusBody.at("runtime").at("scan").at("activeWorkers").get<int>() == 4);

    const auto concurrent = PostWithRetry(client, "/api/scan/start");

    REQUIRE(concurrent);
    REQUIRE(concurrent->status == 409);
    const auto concurrentBody = nlohmann::json::parse(concurrent->body);
    REQUIRE(concurrentBody.at("error").at("code").get<std::string>() == "conflict");
    REQUIRE(host.StartCalls == 1);
}

TEST_CASE("HTTP scan progress is idle before a scan has started", "[server][http][scan]")
{
    CFakeScanServerHost host;
    auto server = StartTestServer(host);
    httplib::Client client("127.0.0.1", server->GetBoundPort());

    const auto response = GetWithRetry(client, "/api/scan/progress");

    REQUIRE(response);
    REQUIRE(response->status == 200);
    const auto body = nlohmann::json::parse(response->body);
    REQUIRE_FALSE(body.at("active").get<bool>());
    REQUIRE(body.at("status").get<std::string>() == "idle");
}

TEST_CASE("HTTP server-owned startup scan is exposed through scan progress", "[server][http][scan]")
{
    CFakeScanServerHost host;
    auto server = StartTestServer(host);
    httplib::Client client("127.0.0.1", server->GetBoundPort());

    const auto started = server->StartScan({
        .Mode = ApplicationJobs::EInpxScanMode::Rescan,
        .WarningLimit = 9
    });

    REQUIRE(started.JobId == 1);
    REQUIRE(host.StartCalls == 1);
    REQUIRE(host.LastRequest.has_value());
    REQUIRE(host.LastRequest->Mode == ApplicationJobs::EInpxScanMode::Rescan);
    REQUIRE(host.LastRequest->WarningLimit == 9);

    const auto progress = GetWithRetry(client, "/api/scan/progress");
    REQUIRE(progress);
    REQUIRE(progress->status == 200);
    const auto progressBody = nlohmann::json::parse(progress->body);
    REQUIRE(progressBody.at("active").get<bool>());
    REQUIRE(progressBody.at("status").get<std::string>() == "running");

    const auto status = GetWithRetry(client, "/api/status");
    REQUIRE(status);
    REQUIRE(status->status == 200);
    const auto statusBody = nlohmann::json::parse(status->body);
    REQUIRE(statusBody.at("scan").at("active").get<bool>());
    REQUIRE(statusBody.at("runtime").at("scan").at("active").get<bool>());
}

TEST_CASE("HTTP status refreshes terminal scan progress instead of reporting stale active state", "[server][http][scan]")
{
    CFakeScanServerHost host;
    auto server = StartTestServer(host);
    httplib::Client client("127.0.0.1", server->GetBoundPort());

    const auto started = PostWithRetry(client, "/api/scan/start");
    REQUIRE(started);
    REQUIRE(started->status == 202);
    REQUIRE(host.Snapshot.has_value());
    host.Snapshot->Status = ApplicationJobs::EInpxScanJobStatus::Completed;
    host.Snapshot->Percent = 100;
    host.Snapshot->Message = "INPX source scan completed.";
    host.Snapshot->TotalRecords = 12;
    host.Snapshot->ScannedRecords = 12;
    host.Snapshot->ParsedFb2Records = 3;
    host.Snapshot->AddedRecords = 2;
    host.Snapshot->UpdatedRecords = 1;
    host.Snapshot->MarkedUnavailableRecords = 1;
    host.Snapshot->SkippedRecords = 1;
    host.Result = ApplicationJobs::SInpxScanJobResult{
        .Snapshot = *host.Snapshot,
        .ScanResult = ApplicationJobs::SInpxScanResult{}
    };

    const auto status = GetWithRetry(client, "/api/status");

    REQUIRE(status);
    REQUIRE(status->status == 200);
    const auto statusBody = nlohmann::json::parse(status->body);
    REQUIRE_FALSE(statusBody.at("scan").at("active").get<bool>());
    REQUIRE(statusBody.at("scan").at("status").get<std::string>() == "completed");
    REQUIRE_FALSE(statusBody.at("runtime").at("scan").at("active").get<bool>());
    REQUIRE(host.RemoveCalls == 1);
}

TEST_CASE("HTTP scan cancel returns accepted state and later cancelled terminal result", "[server][http][scan]")
{
    CFakeScanServerHost host;
    auto server = StartTestServer(host);
    httplib::Client client("127.0.0.1", server->GetBoundPort());

    const auto started = PostWithRetry(client, "/api/scan/start");
    REQUIRE(started);
    REQUIRE(started->status == 202);

    const auto cancelled = PostWithRetry(client, "/api/scan/cancel");

    REQUIRE(cancelled);
    REQUIRE(cancelled->status == 202);
    const auto cancelBody = nlohmann::json::parse(cancelled->body);
    REQUIRE(cancelBody.at("accepted").get<bool>());
    REQUIRE(cancelBody.at("scan").at("active").get<bool>());
    REQUIRE(cancelBody.at("scan").at("status").get<std::string>() == "cancelling");

    const auto progress = GetWithRetry(client, "/api/scan/progress");

    REQUIRE(progress);
    REQUIRE(progress->status == 200);
    const auto progressBody = nlohmann::json::parse(progress->body);
    REQUIRE_FALSE(progressBody.at("active").get<bool>());
    REQUIRE(progressBody.at("status").get<std::string>() == "cancelled");
    REQUIRE(progressBody.at("error").at("code").get<std::string>() == "cancellation");
    REQUIRE(host.RemoveCalls == 1);
    REQUIRE(host.RemovedJobId == 1);

    const auto source = GetWithRetry(client, "/api/source");
    REQUIRE(source);
    REQUIRE(source->status == 200);
    const auto sourceBody = nlohmann::json::parse(source->body);
    REQUIRE(sourceBody.at("source").at("totalBookCount").get<std::size_t>() == 42);
    REQUIRE(sourceBody.at("source").at("availableBookCount").get<std::size_t>() == 40);
}

TEST_CASE("HTTP scan endpoints enforce bearer token when configured", "[server][http][scan][auth]")
{
    CFakeScanServerHost host;
    auto server = StartTestServer(host, {.TokenUtf8 = "secret"});
    httplib::Client client("127.0.0.1", server->GetBoundPort());

    for (const std::string& path : {std::string{"/api/source"}, std::string{"/api/scan/progress"}})
    {
        const auto unauthenticated = GetWithRetry(client, path.c_str());
        REQUIRE(unauthenticated);
        REQUIRE(unauthenticated->status == 401);

        const auto wrongToken = GetWithRetry(client, path.c_str(), {{"Authorization", "Bearer wrong"}});
        REQUIRE(wrongToken);
        REQUIRE(wrongToken->status == 401);
    }

    for (const std::string& path : {std::string{"/api/scan/start"}, std::string{"/api/scan/cancel"}})
    {
        const auto unauthenticated = PostWithRetry(client, path.c_str());
        REQUIRE(unauthenticated);
        REQUIRE(unauthenticated->status == 401);

        const auto wrongToken = PostWithRetry(client, path.c_str(), {}, {{"Authorization", "Bearer wrong"}});
        REQUIRE(wrongToken);
        REQUIRE(wrongToken->status == 401);
    }

    const auto authorized = PostWithRetry(
        client,
        "/api/scan/start",
        R"({"mode":"initial"})",
        {{"Authorization", "Bearer secret"}});
    REQUIRE(authorized);
    REQUIRE(authorized->status == 202);
}

TEST_CASE("HTTP scan API runs a real INPX scan through the server host", "[server][http][scan][integration]")
{
    CTestWorkspace workspace("inpx-web-reader-server-scan-api-real");
    const auto inpxPath = InpxWebReader::Tests::ServerFixtures::WriteInpxArchive(
        workspace.GetPath() / "source" / "catalog.inpx");
    const auto archiveRoot = workspace.GetPath() / "archives";
    const auto cacheRoot = workspace.GetPath() / "cache";
    const auto runtimeRoot = workspace.GetPath() / "runtime";
    WriteInpxPayloadArchive(archiveRoot / "fb2-main.zip");

    const auto config = Server::CServerConfigLoader::LoadFromJsonText(
        InpxWebReader::Tests::ServerFixtures::MakeServerConfigJson(
            cacheRoot,
            runtimeRoot,
            inpxPath,
            archiveRoot));

    Server::CServerApplicationHost host(config);
    host.Open();
    auto server = StartTestServer(host);
    httplib::Client client("127.0.0.1", server->GetBoundPort());

    const auto started = PostWithRetry(client, "/api/scan/start", R"({"mode":"initial","warningLimit":5})");
    REQUIRE(started);
    REQUIRE(started->status == 202);
    REQUIRE(nlohmann::json::parse(started->body).at("jobId").get<std::uint64_t>() == 1);

    const auto terminal = WaitForTerminalScan(client);
    REQUIRE(terminal.at("status").get<std::string>() == "completed");
    REQUIRE(terminal.at("result").at("totalRecords").get<std::size_t>() == 1);
    REQUIRE(terminal.at("result").at("scannedRecords").get<std::size_t>() == 1);
    REQUIRE(terminal.at("result").at("parsedFb2Records").get<std::size_t>() == 1);
    REQUIRE(terminal.at("result").at("addedRecords").get<std::size_t>() == 1);
    REQUIRE(terminal.at("result").at("reusedRecords").get<std::size_t>() == 0);
    REQUIRE(terminal.at("result").at("segmentsTotal").get<std::size_t>() == 1);
    REQUIRE(terminal.at("result").at("segmentsAdded").get<std::size_t>() == 1);
    REQUIRE(terminal.at("result").at("archivesOpened").get<std::size_t>() == 1);
    REQUIRE(terminal.at("result").at("archiveBytesRead").get<std::uint64_t>() > 0);

    const auto source = GetWithRetry(client, "/api/source");
    REQUIRE(source);
    REQUIRE(source->status == 200);
    const auto sourceBody = nlohmann::json::parse(source->body);
    REQUIRE(sourceBody.at("source").at("totalBookCount").get<std::size_t>() == 1);
    REQUIRE(sourceBody.at("source").at("availableBookCount").get<std::size_t>() == 1);

    server->Stop();
    host.Close();
}
