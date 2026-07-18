#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <stop_token>
#include <type_traits>
#include <utility>
#include <vector>

#include "App/IInpxCatalogApplication.hpp"
#include "Domain/BookId.hpp"
#include "Server/BackendExecutor.hpp"
#include "Server/ServerConfig.hpp"

namespace InpxWebReader::Server {

struct SServerInpxSourceStatus
{
    std::string InpxPathUtf8;
    std::string ArchiveRootUtf8;
    bool IsSourceAvailable = true;
    bool RequiresRescan = false;
    std::string SourceWarningUtf8;
    std::size_t TotalBookCount = 0;
    std::size_t AvailableBookCount = 0;
    std::size_t UnavailableBookCount = 0;
    std::size_t WarningCount = 0;
};

struct SServerHttpRuntimeStatus
{
    std::size_t ActiveWorkers = 0;
    std::size_t QueuedRequests = 0;
    std::size_t MaxWorkers = 0;
    std::size_t MaxQueuedRequests = 0;
};

struct SServerDownloadRuntimeStatus
{
    std::size_t Active = 0;
    std::size_t MaxConcurrent = 0;
};

struct SServerScanRuntimeStatus
{
    bool Active = false;
    std::size_t ActiveJobs = 0;
    std::size_t MaxConcurrentJobs = 1;
    std::size_t ActiveWorkers = 0;
    std::size_t MaxWorkers = 1;
};

struct SServerStorageRuntimeStatus
{
    bool CacheRootPresent = false;
    bool CacheDatabasePresent = false;
    bool RuntimeWorkspacePresent = false;
    std::optional<std::uint64_t> CoverCacheBytes = std::nullopt;
    std::optional<std::uint64_t> InpxScanWorkspaceBytes = std::nullopt;
    std::optional<std::uint64_t> DownloadWorkspaceBytes = std::nullopt;
};

struct SServerResourceRuntimeStatus
{
    std::optional<std::uint64_t> ResidentMemoryBytes = std::nullopt;
    std::optional<std::uint64_t> PeakResidentMemoryBytes = std::nullopt;
    std::uint64_t MaxCoverCacheBytes = 0;
    std::uint64_t MaxSteadyStateMemoryBytes = 0;
};

struct SServerRuntimeStatus
{
    std::uint64_t UptimeSeconds = 0;
    SServerHttpRuntimeStatus Http;
    SBackendExecutorMetrics Backend;
    SServerScanRuntimeStatus Scan;
    SServerDownloadRuntimeStatus Downloads;
    SServerStorageRuntimeStatus Storage;
    SServerResourceRuntimeStatus Resources;
};

struct SServerStatus
{
    std::string VersionUtf8;
    bool IsOpen = false;
    bool CreatedCacheOnOpen = false;
    Application::SCatalogCapabilities Capabilities;
    std::optional<SServerInpxSourceStatus> InpxSource = std::nullopt;
    std::optional<ApplicationJobs::SInpxScanJobSnapshot> ActiveScan = std::nullopt;
    SServerRuntimeStatus Runtime;
};

[[nodiscard]] std::optional<ApplicationJobs::EInpxScanMode> ResolveConfiguredStartupScanMode(
    const SServerStartupConfig& startup,
    const SServerStatus& status) noexcept;

enum class EServerDownloadFormat
{
    Original,
    Epub
};

struct SServerFileResponse
{
    std::filesystem::path Path;
    std::optional<std::filesystem::path> CleanupRoot = std::nullopt;
    std::string FileNameUtf8;
    std::string ContentTypeUtf8;
};

class IServerApplicationHost
{
public:
    virtual ~IServerApplicationHost() = default;

    [[nodiscard]] virtual bool IsOpen() const noexcept = 0;
    [[nodiscard]] virtual SServerStatus GetStatus() = 0;
    [[nodiscard]] virtual Application::SBookListResult ListBooks(
        const Application::SBookListRequest& request) = 0;
    [[nodiscard]] virtual std::optional<Application::SBookDetails> GetBookDetails(
        Domain::SBookId bookId) = 0;
    [[nodiscard]] virtual Application::SCatalogStatistics GetCatalogStatistics() = 0;
    [[nodiscard]] virtual std::optional<SServerFileResponse> ResolveBookCover(
        Domain::SBookId bookId) = 0;
    [[nodiscard]] virtual std::optional<SServerFileResponse> PrepareBookDownload(
        Domain::SBookId bookId,
        EServerDownloadFormat format) = 0;
    [[nodiscard]] virtual std::optional<Application::SInpxSourceOverview> GetInpxSourceOverview() = 0;
    [[nodiscard]] virtual ApplicationJobs::TInpxScanJobId StartInpxScan(
        const ApplicationJobs::SInpxScanRequest& request) = 0;
    [[nodiscard]] virtual std::optional<ApplicationJobs::SInpxScanJobSnapshot> GetInpxScanJobSnapshot(
        ApplicationJobs::TInpxScanJobId jobId) = 0;
    [[nodiscard]] virtual std::optional<ApplicationJobs::SInpxScanJobResult> GetInpxScanJobResult(
        ApplicationJobs::TInpxScanJobId jobId) = 0;
    [[nodiscard]] virtual bool CancelInpxScanJob(ApplicationJobs::TInpxScanJobId jobId) = 0;
    [[nodiscard]] virtual bool RemoveInpxScanJob(ApplicationJobs::TInpxScanJobId jobId) = 0;
};

class CServerApplicationHost final : public IServerApplicationHost
{
public:
    using FApplicationFactory = std::function<std::unique_ptr<Application::IInpxCatalogApplication>(
        const Application::SInpxCatalogApplicationConfig&)>;

    explicit CServerApplicationHost(
        SServerConfig config,
        FApplicationFactory applicationFactory = {});
    ~CServerApplicationHost() override;

    CServerApplicationHost(const CServerApplicationHost&) = delete;
    CServerApplicationHost& operator=(const CServerApplicationHost&) = delete;
    CServerApplicationHost(CServerApplicationHost&&) = delete;
    CServerApplicationHost& operator=(CServerApplicationHost&&) = delete;

    void Open();
    void Close() noexcept;

    [[nodiscard]] bool IsOpen() const noexcept override;
    [[nodiscard]] SServerStatus GetStatus() override;
    [[nodiscard]] Application::SBookListResult ListBooks(
        const Application::SBookListRequest& request) override;
    [[nodiscard]] std::optional<Application::SBookDetails> GetBookDetails(
        Domain::SBookId bookId) override;
    [[nodiscard]] Application::SCatalogStatistics GetCatalogStatistics() override;
    [[nodiscard]] std::optional<SServerFileResponse> ResolveBookCover(
        Domain::SBookId bookId) override;
    [[nodiscard]] std::optional<SServerFileResponse> PrepareBookDownload(
        Domain::SBookId bookId,
        EServerDownloadFormat format) override;
    [[nodiscard]] std::optional<Application::SInpxSourceOverview> GetInpxSourceOverview() override;
    [[nodiscard]] ApplicationJobs::TInpxScanJobId StartInpxScan(
        const ApplicationJobs::SInpxScanRequest& request) override;
    [[nodiscard]] std::optional<ApplicationJobs::SInpxScanJobSnapshot> GetInpxScanJobSnapshot(
        ApplicationJobs::TInpxScanJobId jobId) override;
    [[nodiscard]] std::optional<ApplicationJobs::SInpxScanJobResult> GetInpxScanJobResult(
        ApplicationJobs::TInpxScanJobId jobId) override;
    [[nodiscard]] bool CancelInpxScanJob(ApplicationJobs::TInpxScanJobId jobId) override;
    [[nodiscard]] bool RemoveInpxScanJob(ApplicationJobs::TInpxScanJobId jobId) override;

private:
    struct SDownloadOperationContext
    {
        Application::IInpxCatalogApplication* Application = nullptr;
        std::stop_token ShutdownStopToken;
    };

    void EnsureOpen() const;
    [[nodiscard]] SDownloadOperationContext BeginDownloadOperation();
    void EndDownloadOperation() noexcept;

    template <typename TOperation>
    [[nodiscard]] auto SubmitWithOpenApplication(TOperation&& operation)
        -> std::invoke_result_t<std::decay_t<TOperation>&, Application::IInpxCatalogApplication&>
    {
        EnsureOpen();
        auto future = m_executor.Submit(
            [this, operation = std::forward<TOperation>(operation)]() mutable {
                if (m_application == nullptr)
                {
                    throw std::runtime_error("Server application host is not open.");
                }
                return std::invoke(operation, *m_application);
            });
        return future.get();
    }

    SServerConfig m_config;
    FApplicationFactory m_applicationFactory;
    std::unique_ptr<Application::IInpxCatalogApplication> m_application;
    CBackendExecutor m_executor;
    std::atomic_bool m_isOpen = false;
    std::atomic_bool m_createdCacheOnOpen = false;
    std::atomic_uint64_t m_downloadSequence = 0;
    std::mutex m_downloadMutex;
    std::condition_variable m_downloadsFinished;
    std::stop_source m_downloadStopSource;
    std::size_t m_activeDownloadOperations = 0;
};

} // namespace InpxWebReader::Server
