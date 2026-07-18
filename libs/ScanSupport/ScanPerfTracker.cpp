#include "ScanSupport/ScanPerfTracker.hpp"

#include <algorithm>
#include <cstdint>
#include <string>

#include "Foundation/Logging.hpp"
#include "Foundation/UnicodeConversion.hpp"

namespace InpxWebReader::ScanSupport {

namespace {

[[nodiscard]] std::uint64_t NsToMs(const std::uint64_t ns) noexcept
{
    return ns / 1'000'000ULL;
}

[[nodiscard]] std::string BuildOutcomeLine(
    const CScanPerfTracker::SOutcomeLabels& labels,
    const std::uint64_t addedTotal,
    const std::uint64_t updatedTotal,
    const std::uint64_t failedTotal)
{
    std::string line;
    line.reserve(labels.Added.size() + labels.Updated.size() + labels.Failed.size() + 32);
    line += labels.Added;
    line += "=";
    line += std::to_string(addedTotal);
    line += " ";
    line += labels.Updated;
    line += "=";
    line += std::to_string(updatedTotal);
    line += " ";
    line += labels.Failed;
    line += "=";
    line += std::to_string(failedTotal);
    return line;
}

void LogPeriodic(
    const CScanPerfTracker::SStageStats* stages,
    const CScanPerfTracker::SOutcomeLabels& labels,
    const std::uint64_t jobId,
    const std::uint64_t bookCount,
    const std::uint64_t addedTotal,
    const std::uint64_t updatedTotal,
    const std::uint64_t failedTotal,
    const std::chrono::steady_clock::time_point startTime)
{
    const auto elapsed = std::chrono::steady_clock::now() - startTime;
    const double elapsedSec = std::chrono::duration<double>(elapsed).count();
    const double throughput = elapsedSec > 0.0 ? static_cast<double>(bookCount) / elapsedSec : 0.0;

    // Compute per-stage avg_ms and total worker time for bottleneck %
    std::uint64_t stageTotalMs[CScanPerfTracker::kStageCount]{};
    std::uint64_t workerTotalMs = 0;
    for (std::size_t i = 0; i < CScanPerfTracker::kStageCount; ++i)
    {
        const std::uint64_t totalNs = stages[i].TotalNs.load(std::memory_order_relaxed);
        stageTotalMs[i] = NsToMs(totalNs);
        workerTotalMs += stageTotalMs[i];
    }

    // Build stage avg string
    std::string stageLine;
    for (std::size_t i = 0; i < CScanPerfTracker::kStageCount; ++i)
    {
        const std::uint64_t count = stages[i].Count.load(std::memory_order_relaxed);
        const std::uint64_t avgMs = count > 0
            ? NsToMs(stages[i].TotalNs.load(std::memory_order_relaxed)) / count
            : 0;
        if (i > 0)
        {
            stageLine += " | ";
        }
        stageLine += CScanPerfTracker::kStageNames[i];
        stageLine += "=";
        stageLine += std::to_string(avgMs);
        stageLine += "ms";
    }

    // Build bottleneck percentage string.
    struct SEntry
    {
        std::size_t Idx = 0;
        std::uint64_t TotalMs = 0;
    };

    std::array<SEntry, CScanPerfTracker::kStageCount> entries{};
    for (std::size_t i = 0; i < CScanPerfTracker::kStageCount; ++i)
    {
        entries[i] = {
            .Idx = i,
            .TotalMs = stageTotalMs[i]
        };
    }
    std::sort(entries.begin(), entries.end(), [](const SEntry& left, const SEntry& right) {
        return left.TotalMs > right.TotalMs;
    });

    std::string bottleneckLine;
    for (const auto& entry : entries)
    {
        const uint32_t pct = workerTotalMs > 0
            ? static_cast<uint32_t>(entry.TotalMs * 100 / workerTotalMs)
            : 0;
        if (!bottleneckLine.empty())
        {
            bottleneckLine += " ";
        }

        bottleneckLine += CScanPerfTracker::kStageNames[entry.Idx];
        bottleneckLine += "=";
        bottleneckLine += std::to_string(pct);
        bottleneckLine += "%";
    }

    const std::string outcomeLine = BuildOutcomeLine(
        labels,
        addedTotal,
        updatedTotal,
        failedTotal);

    if (jobId != 0)
    {
        InpxWebReader::Logging::Info(
            "[scan-perf] job={} books={} throughput={:.1f} bk/s | {} | "
            "bottleneck: {} | {}",
            jobId,
            bookCount,
            throughput,
            stageLine,
            bottleneckLine,
            outcomeLine);
        return;
    }

    InpxWebReader::Logging::Info(
        "[scan-perf] books={} throughput={:.1f} bk/s | {} | "
        "bottleneck: {} | {}",
        bookCount,
        throughput,
        stageLine,
        bottleneckLine,
        outcomeLine);
}

} // namespace

CScanPerfTracker::CScanPerfTracker(const std::uint64_t jobId) noexcept
    : CScanPerfTracker(jobId, SOutcomeLabels{})
{
}

CScanPerfTracker::CScanPerfTracker(
    const std::uint64_t jobId,
    SOutcomeLabels outcomeLabels) noexcept
    : m_lastLogTimeNs(GetNowNs())
    , m_startTime(std::chrono::steady_clock::now())
    , m_jobId(jobId)
    , m_outcomeLabels(outcomeLabels)
{
}

std::int64_t CScanPerfTracker::GetNowNs() noexcept
{
    return static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

void CScanPerfTracker::OnBookProcessed(
    const std::uint64_t addedDelta,
    const std::uint64_t updatedDelta,
    const std::uint64_t failedDelta) noexcept
{
    m_addedCount.fetch_add(addedDelta, std::memory_order_relaxed);
    m_updatedCount.fetch_add(updatedDelta, std::memory_order_relaxed);
    m_failedCount.fetch_add(failedDelta, std::memory_order_relaxed);

    const std::uint64_t bookCount = m_bookCount.fetch_add(1, std::memory_order_relaxed) + 1;

    std::uint64_t lastLogCount = m_lastLogBookCount.load(std::memory_order_relaxed);
    const bool enoughBooks = (bookCount - lastLogCount) >= kLogEveryN;

    const auto nowNs = GetNowNs();
    const std::int64_t lastLogNs = m_lastLogTimeNs.load(std::memory_order_relaxed);
    const bool enoughTime = (nowNs - lastLogNs) >=
        std::chrono::duration_cast<std::chrono::nanoseconds>(kLogEveryT).count();

    if (!enoughBooks && !enoughTime)
    {
        return;
    }

    // Attempt to claim the log slot — only one thread logs per interval
    if (!m_lastLogBookCount.compare_exchange_strong(
            lastLogCount, bookCount,
            std::memory_order_relaxed))
    {
        return;
    }
    m_lastLogTimeNs.store(nowNs, std::memory_order_relaxed);

    try
    {
        LogPeriodic(
            m_stages.data(),
            m_outcomeLabels,
            m_jobId,
            bookCount,
            m_addedCount.load(std::memory_order_relaxed),
            m_updatedCount.load(std::memory_order_relaxed),
            m_failedCount.load(std::memory_order_relaxed),
            m_startTime);
    }
    catch (...)
    {
        // Logging must never crash the scan.
        (void)0;
    }
}

void CScanPerfTracker::NoteOutlierIfSlow(
    const std::string_view bookPath,
    const std::chrono::milliseconds elapsed) noexcept
{
    if (elapsed.count() < static_cast<std::int64_t>(kOutlierThresholdMs))
    {
        return;
    }

    try
    {
        if (m_jobId != 0)
        {
            InpxWebReader::Logging::Warn(
                "[scan-perf] job={} Slow book: {}ms for \"{}\"",
                m_jobId,
                elapsed.count(),
                bookPath);
        }
        else
        {
            InpxWebReader::Logging::Warn(
                "[scan-perf] Slow book: {}ms for \"{}\"",
                elapsed.count(),
                bookPath);
        }
    }
    catch (...)
    {
        // Intentionally ignored in best-effort cleanup/logging paths.
        (void)0;
    }
}

void CScanPerfTracker::LogSummary(
    const std::chrono::steady_clock::duration totalDuration) noexcept
{
    try
    {
        const std::uint64_t bookCount     = m_bookCount.load(std::memory_order_relaxed);
        const std::uint64_t added      = m_addedCount.load(std::memory_order_relaxed);
        const std::uint64_t updated     = m_updatedCount.load(std::memory_order_relaxed);
        const std::uint64_t failed        = m_failedCount.load(std::memory_order_relaxed);
        const double elapsedSec = std::chrono::duration<double>(totalDuration).count();
        const double throughput = elapsedSec > 0.0
            ? static_cast<double>(bookCount) / elapsedSec : 0.0;
        const std::uint64_t elapsedMin = static_cast<std::uint64_t>(elapsedSec) / 60;
        const std::uint64_t elapsedSecPart = static_cast<std::uint64_t>(elapsedSec) % 60;

        // Build bottleneck ranking (sorted by total time, descending)
        struct SEntry { std::size_t Idx; std::uint64_t TotalMs; };
        std::array<SEntry, kStageCount> entries{};
        for (std::size_t i = 0; i < kStageCount; ++i)
        {
            entries[i] = {i, NsToMs(m_stages[i].TotalNs.load(std::memory_order_relaxed))};
        }
        std::sort(entries.begin(), entries.end(),
            [](const SEntry& a, const SEntry& b) { return a.TotalMs > b.TotalMs; });

        std::uint64_t workerTotalMs = 0;
        for (std::size_t i = 0; i < kStageCount; ++i)
        {
            workerTotalMs += entries[i].TotalMs;
        }

        std::string bottleneckLine;
        for (const auto& entry : entries)
        {
            const uint32_t pct = workerTotalMs > 0
                ? static_cast<uint32_t>(entry.TotalMs * 100 / workerTotalMs) : 0;
            if (!bottleneckLine.empty())
            {
                bottleneckLine += " ";
            }

            bottleneckLine += kStageNames[entry.Idx];
            bottleneckLine += "=";
            bottleneckLine += std::to_string(pct);
            bottleneckLine += "%";
        }

        const std::string outcomeLine = BuildOutcomeLine(
            m_outcomeLabels,
            added,
            updated,
            failed);

        if (m_jobId != 0)
        {
            InpxWebReader::Logging::Info(
                "[scan-perf] SUMMARY job={} total={} {} | "
                "elapsed={}m{}s throughput={:.1f} bk/s | "
                "bottleneck: {}",
                m_jobId,
                bookCount,
                outcomeLine,
                elapsedMin,
                elapsedSecPart,
                throughput,
                bottleneckLine);
            return;
        }

        InpxWebReader::Logging::Info(
            "[scan-perf] SUMMARY total={} {} | "
            "elapsed={}m{}s throughput={:.1f} bk/s | "
            "bottleneck: {}",
            bookCount,
            outcomeLine,
            elapsedMin,
            elapsedSecPart,
            throughput,
            bottleneckLine);
    }
    catch (...)
    {
        // Intentionally ignored in best-effort cleanup/logging paths.
        (void)0;
    }
}

CScanPerfTracker::SStageSnapshot CScanPerfTracker::SnapshotStages() const noexcept
{
    SStageSnapshot snap;
    for (std::size_t i = 0; i < kStageCount; ++i)
    {
        snap.StageNs[i]     = m_stages[i].TotalNs.load(std::memory_order_relaxed);
        snap.StageCounts[i] = m_stages[i].Count.load(std::memory_order_relaxed);
    }
    snap.BookCount = m_bookCount.load(std::memory_order_relaxed);
    snap.Added  = m_addedCount.load(std::memory_order_relaxed);
    snap.Updated = m_updatedCount.load(std::memory_order_relaxed);
    snap.Failed    = m_failedCount.load(std::memory_order_relaxed);
    return snap;
}

void CScanPerfTracker::LogArchiveSummary(
    const std::filesystem::path& archivePath,
    const SStageSnapshot& before,
    const std::uint64_t jobId) noexcept
{
    try
    {
        // Compute per-archive deltas
        const std::uint64_t books    = m_bookCount.load(std::memory_order_relaxed) - before.BookCount;
        const std::uint64_t added = m_addedCount.load(std::memory_order_relaxed)  - before.Added;
        const std::uint64_t updated = m_updatedCount.load(std::memory_order_relaxed) - before.Updated;
        const std::uint64_t failed   = m_failedCount.load(std::memory_order_relaxed)    - before.Failed;

        if (books == 0)
        {
            return;
        }

        std::uint64_t deltaNs[kStageCount]{};
        std::uint64_t deltaCount[kStageCount]{};
        std::uint64_t workerTotalMs = 0;

        for (std::size_t i = 0; i < kStageCount; ++i)
        {
            deltaNs[i]    = m_stages[i].TotalNs.load(std::memory_order_relaxed) - before.StageNs[i];
            deltaCount[i] = m_stages[i].Count.load(std::memory_order_relaxed)   - before.StageCounts[i];
            workerTotalMs += NsToMs(deltaNs[i]);
        }

        // avg ms per stage for this archive
        std::string stageLine;
        for (std::size_t i = 0; i < kStageCount; ++i)
        {
            const std::uint64_t avgMs = deltaCount[i] > 0 ? NsToMs(deltaNs[i]) / deltaCount[i] : 0;
            if (i > 0)
            {
                stageLine += " | ";
            }

            stageLine += kStageNames[i];
            stageLine += "=";
            stageLine += std::to_string(avgMs);
            stageLine += "ms";
        }

        // bottleneck %
        struct SEntry { std::size_t Idx; std::uint64_t TotalMs; };
        std::array<SEntry, kStageCount> entries{};
        for (std::size_t i = 0; i < kStageCount; ++i)
        {
            entries[i] = {i, NsToMs(deltaNs[i])};
        }
        std::sort(entries.begin(), entries.end(),
            [](const SEntry& a, const SEntry& b) { return a.TotalMs > b.TotalMs; });

        std::string bottleneckLine;
        for (const auto& e : entries)
        {
            const std::uint32_t pct = workerTotalMs > 0
                ? static_cast<std::uint32_t>(e.TotalMs * 100 / workerTotalMs) : 0;
            if (!bottleneckLine.empty())
            {
                bottleneckLine += " ";
            }

            bottleneckLine += kStageNames[e.Idx];
            bottleneckLine += "=";
            bottleneckLine += std::to_string(pct);
            bottleneckLine += "%";
        }

        const std::string archiveUtf8 = InpxWebReader::Unicode::PathToUtf8(archivePath);
        const std::string outcomeLine = BuildOutcomeLine(
            m_outcomeLabels,
            added,
            updated,
            failed);

        if (jobId != 0)
        {
            InpxWebReader::Logging::Info(
                "[scan-perf] archive job={} archive='{}' books={} {} | {} | bottleneck: {}",
                jobId, archiveUtf8, books, outcomeLine, stageLine, bottleneckLine);
        }
        else
        {
            InpxWebReader::Logging::Info(
                "[scan-perf] archive archive='{}' books={} {} | {} | bottleneck: {}",
                archiveUtf8, books, outcomeLine, stageLine, bottleneckLine);
        }
    }
    catch (...)
    {
        // Intentionally ignored in best-effort cleanup/logging paths.
        (void)0;
    }
}

} // namespace InpxWebReader::ScanSupport
