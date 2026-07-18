#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string_view>
#include <utility>

namespace InpxWebReader::ScanSupport {

// Thread-safe per-stage INPX scan performance tracker.
//
// Usage:
//   CScanPerfTracker perf;
//   {
//       auto _ = perf.MeasureStage(CScanPerfTracker::EStage::Parse);
//       // ... do parse work ...
//   }
//   perf.OnBookProcessed(logger);  // may emit periodic log
//   perf.LogSummary(logger, totalDuration);
class CScanPerfTracker
{
public:
    struct SOutcomeLabels
    {
        std::string_view Added = "added";
        std::string_view Updated = "updated";
        std::string_view Failed = "failed";
    };

    explicit CScanPerfTracker(std::uint64_t jobId = 0) noexcept;
    CScanPerfTracker(std::uint64_t jobId, SOutcomeLabels outcomeLabels) noexcept;

    enum class EStage : std::size_t
    {
        Parse = 0,
        Cover,
        DbWriteWait,
        ZipScan,
        ZipExtract,
        CommitStorage,
        kCount
    };

    static constexpr std::size_t kStageCount = static_cast<std::size_t>(EStage::kCount);

    static constexpr std::string_view kStageNames[kStageCount] = {
        "parse", "cover", "db_wait", "zip_scan", "zip_extract", "db_commit"
    };

    // RAII stage timer — adds elapsed time to the stage accumulator on destruction.
    class CScopedStageTimer
    {
    public:
        CScopedStageTimer(CScanPerfTracker& tracker, EStage stage) noexcept
            : m_tracker(tracker)
            , m_stage(stage)
            , m_start(std::chrono::steady_clock::now())
        {
        }

        CScopedStageTimer(CScopedStageTimer&& other) noexcept
            : m_tracker(other.m_tracker)
            , m_stage(other.m_stage)
            , m_start(other.m_start)
            , m_active(std::exchange(other.m_active, false))
        {
        }

        ~CScopedStageTimer() noexcept
        {
            if (!m_active)
            {
                return;
            }

            const auto elapsed = std::chrono::steady_clock::now() - m_start;
            const auto ns = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
            const auto idx = static_cast<std::size_t>(m_stage);
            m_tracker.m_stages[idx].TotalNs.fetch_add(ns, std::memory_order_relaxed);
            m_tracker.m_stages[idx].Count.fetch_add(1, std::memory_order_relaxed);
        }

        CScopedStageTimer(const CScopedStageTimer&) = delete;
        CScopedStageTimer& operator=(const CScopedStageTimer&) = delete;
        CScopedStageTimer& operator=(CScopedStageTimer&&) = delete;

    private:
        CScanPerfTracker& m_tracker;
        EStage              m_stage;
        std::chrono::steady_clock::time_point m_start;
        bool m_active = true;
    };

    [[nodiscard]] CScopedStageTimer MeasureStage(EStage stage) noexcept
    {
        return CScopedStageTimer(*this, stage);
    }

    // Called by a worker after each processed book/payload.
    // Emits an Info log every kLogEveryN books or kLogEveryT seconds —
    // whichever comes first. Thread-safe.
    void OnBookProcessed(std::uint64_t addedDelta = 0,
                         std::uint64_t updatedDelta = 0,
                         std::uint64_t failedDelta    = 0) noexcept;

    // Emits a Warn log when a single book takes longer than kOutlierThresholdMs.
    // Thread-safe. Call with the total per-book elapsed time.
    void NoteOutlierIfSlow(std::string_view bookPath,
                           std::chrono::milliseconds elapsed) noexcept;

    // Emits a final summary log. Call once after all workers are done.
    void LogSummary(std::chrono::steady_clock::duration totalDuration) noexcept;

    // Captures a snapshot of current cumulative stage totals and book counters.
    // Used with LogArchiveSummary to produce per-archive delta logs.
    struct SStageSnapshot
    {
        std::uint64_t StageNs[kStageCount]{};
        std::uint64_t StageCounts[kStageCount]{};
        std::uint64_t BookCount = 0;
        std::uint64_t Added  = 0;
        std::uint64_t Updated = 0;
        std::uint64_t Failed    = 0;
    };

    [[nodiscard]] SStageSnapshot SnapshotStages() const noexcept;

    // Logs a per-archive performance summary: stage avg_ms and bottleneck % computed
    // as deltas from the given snapshot. Call at the end of each archive's Run().
    void LogArchiveSummary(
        const std::filesystem::path& archivePath,
        const SStageSnapshot& before,
        std::uint64_t jobId) noexcept;

    static constexpr std::uint64_t kLogEveryN             = 500;
    static constexpr auto          kLogEveryT              = std::chrono::seconds(30);
    static constexpr std::uint64_t kOutlierThresholdMs     = 5'000;

    // Aligned to cache line to avoid false sharing between worker threads.
    struct alignas(64) SStageStats
    {
        std::atomic<std::uint64_t> TotalNs{0};
        std::atomic<std::uint64_t> Count{0};
    };

private:
    [[nodiscard]] static std::int64_t GetNowNs() noexcept;

    std::array<SStageStats, kStageCount> m_stages{};

    std::atomic<std::uint64_t> m_bookCount{0};
    std::atomic<std::uint64_t> m_addedCount{0};
    std::atomic<std::uint64_t> m_updatedCount{0};
    std::atomic<std::uint64_t> m_failedCount{0};

    std::atomic<std::uint64_t> m_lastLogBookCount{0};
    std::atomic<std::int64_t>  m_lastLogTimeNs;

    std::chrono::steady_clock::time_point m_startTime;
    std::uint64_t m_jobId = 0;
    SOutcomeLabels m_outcomeLabels;
};

} // namespace InpxWebReader::ScanSupport
