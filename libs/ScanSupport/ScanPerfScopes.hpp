#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <utility>

#include "ScanSupport/ScanPerfTracker.hpp"

namespace InpxWebReader::ScanSupport {

class CScanPerfSummaryScope final
{
public:
    CScanPerfSummaryScope(
        CScanPerfTracker& perf,
        std::chrono::steady_clock::time_point startedAt) noexcept
        : m_perf(perf)
        , m_startedAt(startedAt)
    {
    }

    CScanPerfSummaryScope(const CScanPerfSummaryScope&) = delete;
    CScanPerfSummaryScope& operator=(const CScanPerfSummaryScope&) = delete;

    ~CScanPerfSummaryScope() noexcept
    {
        LogNow();
    }

    void LogNow() noexcept
    {
        if (!m_active)
        {
            return;
        }

        m_perf.LogSummary(std::chrono::steady_clock::now() - m_startedAt);
        m_active = false;
    }

private:
    CScanPerfTracker& m_perf;
    std::chrono::steady_clock::time_point m_startedAt;
    bool m_active = true;
};

class CScanArchivePerfScope final
{
public:
    CScanArchivePerfScope(
        CScanPerfTracker& perf,
        const std::uint64_t jobId) noexcept
        : m_perf(perf)
        , m_jobId(jobId)
    {
    }

    CScanArchivePerfScope(const CScanArchivePerfScope&) = delete;
    CScanArchivePerfScope& operator=(const CScanArchivePerfScope&) = delete;

    ~CScanArchivePerfScope() noexcept
    {
        FinishCurrentArchive();
    }

    void BeginArchive(std::filesystem::path archivePath) noexcept
    {
        FinishCurrentArchive();
        m_archivePath = std::move(archivePath);
        m_snapshot = m_perf.SnapshotStages();
    }

    void FinishCurrentArchive() noexcept
    {
        if (!m_snapshot.has_value())
        {
            return;
        }

        m_perf.LogArchiveSummary(m_archivePath, *m_snapshot, m_jobId);
        m_snapshot = std::nullopt;
        m_archivePath.clear();
    }

private:
    CScanPerfTracker& m_perf;
    std::uint64_t m_jobId = 0;
    std::optional<CScanPerfTracker::SStageSnapshot> m_snapshot = std::nullopt;
    std::filesystem::path m_archivePath;
};

} // namespace InpxWebReader::ScanSupport
