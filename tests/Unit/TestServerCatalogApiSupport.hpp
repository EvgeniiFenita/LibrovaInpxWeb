#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "Domain/DomainError.hpp"
#include "Server/HttpServer.hpp"
#include "Server/ServerApplicationHost.hpp"

namespace InpxWebReader::Tests::ServerCatalogApiSupport {

namespace Application = InpxWebReader::Application;
namespace ApplicationJobs = InpxWebReader::ApplicationJobs;
namespace Domain = InpxWebReader::Domain;
namespace Server = InpxWebReader::Server;

class CFakeCatalogServerHost final : public Server::IServerApplicationHost
{
public:
    CFakeCatalogServerHost();

    [[nodiscard]] bool IsOpen() const noexcept override;
    [[nodiscard]] Server::SServerStatus GetStatus() override;
    [[nodiscard]] Application::SBookListResult ListBooks(const Application::SBookListRequest& request) override;
    [[nodiscard]] std::optional<Application::SBookDetails> GetBookDetails(Domain::SBookId bookId) override;
    [[nodiscard]] Application::SCatalogStatistics GetCatalogStatistics() override;
    [[nodiscard]] std::optional<Server::SServerFileResponse> ResolveBookCover(Domain::SBookId bookId) override;
    [[nodiscard]] std::optional<Server::SServerFileResponse> PrepareBookDownload(
        Domain::SBookId bookId,
        Server::EServerDownloadFormat format) override;
    [[nodiscard]] std::optional<Application::SInpxSourceOverview> GetInpxSourceOverview() override;
    [[nodiscard]] ApplicationJobs::TInpxScanJobId StartInpxScan(
        const ApplicationJobs::SInpxScanRequest& request) override;
    [[nodiscard]] std::optional<ApplicationJobs::SInpxScanJobSnapshot> GetInpxScanJobSnapshot(
        ApplicationJobs::TInpxScanJobId jobId) override;
    [[nodiscard]] std::optional<ApplicationJobs::SInpxScanJobResult> GetInpxScanJobResult(
        ApplicationJobs::TInpxScanJobId jobId) override;
    [[nodiscard]] bool CancelInpxScanJob(ApplicationJobs::TInpxScanJobId jobId) override;
    [[nodiscard]] bool RemoveInpxScanJob(ApplicationJobs::TInpxScanJobId jobId) override;

    [[nodiscard]] bool WaitForBlockedDownload(std::chrono::milliseconds timeout);
    void ReleaseBlockedDownloads();

    std::atomic_bool Open = true;
    Server::SServerStatus Status{
        .VersionUtf8 = "0.1.0",
        .IsOpen = true,
        .Capabilities = {
            .CanRescanInpxSource = true,
            .CanDownloadOriginal = true,
            .CanDownloadAsEpub = true
        }
    };

    mutable std::mutex Mutex;
    Application::SBookListResult ListResult;
    std::optional<Application::SBookDetails> Details;
    Application::SCatalogStatistics Statistics;
    std::optional<Server::SServerFileResponse> CoverResponse;
    std::optional<Server::SServerFileResponse> DownloadResponse;
    std::optional<Application::SBookListRequest> LastListRequest;
    std::optional<Domain::EDomainErrorCode> ListErrorCode;
    std::optional<Domain::SBookId> LastDetailsBookId;
    std::optional<Domain::SBookId> LastCoverBookId;
    std::optional<Domain::SBookId> LastDownloadBookId;
    std::optional<Server::EServerDownloadFormat> LastDownloadFormat;
    bool BlockDownloads = false;
    bool ReleaseDownloads = false;
    int BlockedDownloadEntries = 0;
    int ListCalls = 0;
    int DetailsCalls = 0;
    int StatisticsCalls = 0;
    int CoverCalls = 0;
    int DownloadCalls = 0;
    std::condition_variable DownloadEntered;
    std::condition_variable DownloadReleased;
};

class CBlockedDownloadReleaseGuard final
{
public:
    explicit CBlockedDownloadReleaseGuard(CFakeCatalogServerHost& host) noexcept;
    ~CBlockedDownloadReleaseGuard();

    CBlockedDownloadReleaseGuard(const CBlockedDownloadReleaseGuard&) = delete;
    CBlockedDownloadReleaseGuard& operator=(const CBlockedDownloadReleaseGuard&) = delete;

    void ReleaseNow() noexcept;

private:
    CFakeCatalogServerHost& m_host;
    bool m_active = true;
};

[[nodiscard]] httplib::Result GetWithRetry(
    httplib::Client& client,
    const std::string& path,
    const httplib::Headers& headers = {});

[[nodiscard]] httplib::Result PostWithRetry(
    httplib::Client& client,
    const std::string& path,
    const std::string& bodyUtf8 = {},
    const httplib::Headers& headers = {});

[[nodiscard]] std::unique_ptr<Server::CHttpServer> StartTestServer(
    Server::IServerApplicationHost& host,
    Server::SHttpServerOptions options = {});

void WriteBinaryFile(const std::filesystem::path& path, const std::string& content);
[[nodiscard]] bool WaitUntilRemoved(const std::filesystem::path& path);
std::filesystem::path WriteInpxArchive(const std::filesystem::path& archivePath);
std::filesystem::path WriteInpxPayloadArchive(const std::filesystem::path& archivePath);
[[nodiscard]] nlohmann::json WaitForTerminalScan(httplib::Client& client);

} // namespace InpxWebReader::Tests::ServerCatalogApiSupport
