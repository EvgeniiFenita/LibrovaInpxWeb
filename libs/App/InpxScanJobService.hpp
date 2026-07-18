#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "Domain/DomainError.hpp"
#include "Domain/ServiceContracts.hpp"

namespace InpxWebReader::Application {
struct SInpxSourceInfo;
}

namespace InpxWebReader::ApplicationJobs {

enum class EInpxScanMode
{
    InitialScan,
    Rescan
};

using TInpxScanJobId = std::uint64_t;

enum class EInpxScanJobStatus
{
    Pending,
    Running,
    Completed,
    Failed,
    Cancelled,
    Cancelling
};

struct SInpxScanRequest
{
    EInpxScanMode Mode = EInpxScanMode::InitialScan;
    std::size_t WarningLimit = 50;
};

struct SInpxScanJobSnapshot
{
    TInpxScanJobId JobId = 0;
    EInpxScanJobStatus Status = EInpxScanJobStatus::Pending;
    int Percent = 0;
    std::string Message;
    std::vector<std::string> Warnings;
    std::size_t TotalRecords = 0;
    std::size_t ScannedRecords = 0;
    std::size_t ParsedFb2Records = 0;
    std::size_t AddedRecords = 0;
    std::size_t UpdatedRecords = 0;
    std::size_t MarkedUnavailableRecords = 0;
    std::size_t SkippedRecords = 0;
    std::size_t ReusedRecords = 0;
    std::size_t SegmentsTotal = 0;
    std::size_t SegmentsUnchanged = 0;
    std::size_t SegmentsAdded = 0;
    std::size_t SegmentsChanged = 0;
    std::size_t SegmentsRemoved = 0;
    std::size_t ArchivesSkipped = 0;
    std::size_t ArchivesOpened = 0;
    std::uint64_t ArchiveBytesRead = 0;
    std::string CurrentArchiveNameUtf8;
    std::string CurrentEntryNameUtf8;

    [[nodiscard]] bool IsTerminal() const noexcept
    {
        return Status == EInpxScanJobStatus::Completed
            || Status == EInpxScanJobStatus::Failed
            || Status == EInpxScanJobStatus::Cancelled;
    }
};

struct SInpxScanResult
{
    std::size_t WarningCount = 0;
    std::size_t ParserWarningCount = 0;
    std::size_t CoverWarningCount = 0;
    std::size_t MetadataFallbackCount = 0;
    std::size_t TitleFallbackCount = 0;
    std::size_t AuthorFallbackCount = 0;
    std::size_t LanguageFallbackCount = 0;
};

struct SInpxScanJobResult
{
    SInpxScanJobSnapshot Snapshot;
    std::optional<SInpxScanResult> ScanResult = std::nullopt;
    std::optional<InpxWebReader::Domain::SDomainError> Error = std::nullopt;
};

class CInpxScanJobService final
{
public:
    struct SHooks
    {
        std::function<void()> BeforeWorkerStart;
        std::function<void()> OnSourceFingerprintCheckpoint;
        std::function<void()> OnInpParserCheckpoint;
        std::function<void()> OnArchiveManifestCheckpoint;
        std::function<void()> OnArchiveFallbackSnapshotBuild;
        std::function<void()> OnArchiveRootEntryVisited;
        std::function<void()> AfterRecordProcessed;
        std::function<void(std::size_t)> AfterProgressLogViewCaptured;
        std::function<void()> BeforeCatalogCommit;
        std::function<void()> AfterExecutionArchiveOpened;
    };

    struct SJobRecord
    {
        mutable std::mutex Mutex;
        std::condition_variable Condition;
        SInpxScanJobSnapshot Snapshot;
        std::optional<SInpxScanJobResult> Result = std::nullopt;
        std::jthread Worker;
        std::filesystem::path WorkingDirectory;
    };

    explicit CInpxScanJobService(
        std::filesystem::path runtimeWorkspaceRoot,
        SHooks hooks = {},
        std::filesystem::path databasePath = {},
        std::filesystem::path cacheRoot = {},
        const InpxWebReader::Domain::ICoverImageProcessor* coverImageProcessor = nullptr,
        std::size_t maxWorkerCount = 0,
        std::uint64_t maxCoverCacheBytes = 128ull * 1024ull * 1024ull,
        std::uint64_t maxSteadyStateMemoryBytes = 1024ull * 1024ull * 1024ull);
    ~CInpxScanJobService();

    [[nodiscard]] TInpxScanJobId Start(
        const InpxWebReader::Application::SInpxSourceInfo& source,
        const SInpxScanRequest& request,
        std::optional<std::string> expectedSourceFingerprintUtf8 = std::nullopt);
    [[nodiscard]] std::optional<SInpxScanJobSnapshot> TryGetSnapshot(TInpxScanJobId jobId) const;
    [[nodiscard]] std::optional<SInpxScanJobResult> TryGetResult(TInpxScanJobId jobId) const;
    [[nodiscard]] bool Cancel(TInpxScanJobId jobId) const;
    [[nodiscard]] bool Wait(
        TInpxScanJobId jobId,
        std::chrono::milliseconds timeout) const;
    [[nodiscard]] bool Remove(TInpxScanJobId jobId);

private:
    [[nodiscard]] std::shared_ptr<SJobRecord> TryGetRecord(TInpxScanJobId jobId) const;

    std::filesystem::path m_runtimeWorkspaceRoot;
    std::filesystem::path m_databasePath;
    std::filesystem::path m_cacheRoot;
    const InpxWebReader::Domain::ICoverImageProcessor* m_coverImageProcessor = nullptr;
    std::size_t m_maxWorkerCount = 0;
    std::uint64_t m_maxCoverCacheBytes = 0;
    std::uint64_t m_maxSteadyStateMemoryBytes = 0;
    SHooks m_hooks;
    mutable std::mutex m_jobsMutex;
    std::unordered_map<TInpxScanJobId, std::shared_ptr<SJobRecord>> m_jobs;
    TInpxScanJobId m_nextJobId = 1;
};

} // namespace InpxWebReader::ApplicationJobs
