#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "App/InpxScanJobService.hpp"
#include "App/InpxCatalogFacade.hpp"
#include "App/BookDownloadFacade.hpp"

namespace InpxWebReader::Application {

enum class ECacheOpenMode
{
    Open,
    CreateNew
};

struct SCatalogCapabilities
{
    bool CanRescanInpxSource = true;
    bool CanDownloadOriginal = true;
    bool CanDownloadAsEpub = false;
};

struct SInpxSourceInfo
{
    std::filesystem::path InpxPath;
    std::filesystem::path ArchiveRoot;
};

struct SCatalogSessionInfo
{
    SCatalogCapabilities Capabilities;
    std::optional<SInpxSourceInfo> InpxSource = std::nullopt;
};

struct SInpxSourceOverview
{
    SInpxSourceInfo Source;
    std::string DisplayNameUtf8;
    bool IsSourceAvailable = true;
    bool RequiresRescan = false;
    std::string SourceWarningUtf8;
    std::string LastScanStartedAtUtc;
    std::string LastScanCompletedAtUtc;
    std::string LastSeenSnapshotId;
    std::size_t TotalBookCount = 0;
    std::size_t AvailableBookCount = 0;
    std::size_t UnavailableBookCount = 0;
    std::size_t WarningCount = 0;
    std::vector<std::string> RecentWarningsUtf8;
};

struct SInpxCatalogApplicationConfig
{
    std::filesystem::path CacheRoot;
    std::filesystem::path RuntimeWorkspaceRoot;
    std::optional<std::filesystem::path> ConverterPath = std::nullopt;
    ECacheOpenMode CacheOpenMode = ECacheOpenMode::Open;
    std::optional<SInpxSourceInfo> InpxSource = std::nullopt;
    std::size_t MaxInpxScanWorkers = 0;
    std::size_t MaxConcurrentDownloads = 1;
    std::uint64_t MaxCoverCacheBytes = 128ull * 1024ull * 1024ull;
    std::uint64_t MaxSteadyStateMemoryBytes = 1024ull * 1024ull * 1024ull;
};

class IInpxCatalogApplication
{
public:
    virtual ~IInpxCatalogApplication() = default;

    [[nodiscard]] virtual SCatalogSessionInfo GetCatalogSessionInfo() = 0;
    [[nodiscard]] virtual SBookListResult ListBooks(const SBookListRequest& request) = 0;
    [[nodiscard]] virtual std::optional<SBookDetails> GetBookDetails(Domain::SBookId id) = 0;
    [[nodiscard]] virtual SCatalogStatistics GetCatalogStatistics() = 0;
    [[nodiscard]] virtual std::optional<SInpxSourceOverview> GetInpxSourceOverview() = 0;
    [[nodiscard]] virtual ApplicationJobs::TInpxScanJobId StartInpxScan(
        const ApplicationJobs::SInpxScanRequest& request) = 0;
    [[nodiscard]] virtual std::optional<ApplicationJobs::SInpxScanJobSnapshot> GetInpxScanJobSnapshot(
        ApplicationJobs::TInpxScanJobId jobId) = 0;
    [[nodiscard]] virtual std::optional<ApplicationJobs::SInpxScanJobResult> GetInpxScanJobResult(
        ApplicationJobs::TInpxScanJobId jobId) = 0;
    [[nodiscard]] virtual bool CancelInpxScanJob(ApplicationJobs::TInpxScanJobId jobId) = 0;
    [[nodiscard]] virtual bool RemoveInpxScanJob(ApplicationJobs::TInpxScanJobId jobId) = 0;
    [[nodiscard]] virtual std::optional<SPreparedBookDownload> PrepareDownload(
        const SBookDownloadRequest& request) = 0;
};

} // namespace InpxWebReader::Application
