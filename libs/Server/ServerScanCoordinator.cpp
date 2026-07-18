#include "Server/ServerScanCoordinator.hpp"

#include "Foundation/Logging.hpp"
#include "Server/HttpError.hpp"

namespace InpxWebReader::Server {

CServerScanCoordinator::CServerScanCoordinator(IServerApplicationHost& applicationHost)
    : m_applicationHost(applicationHost)
{
}

SServerScanStartResult CServerScanCoordinator::StartScan(
    const ApplicationJobs::SInpxScanRequest& request)
{
    std::scoped_lock lock(m_mutex);
    if (m_activeJobId.has_value())
    {
        const auto progress = PollProgressLocked();
        if (HasActiveScanLocked())
        {
            throw CHttpErrorMapper::Conflict("An INPX scan is already running.");
        }

        m_lastProgress = progress;
    }

    const auto jobId = m_applicationHost.StartInpxScan(request);
    m_activeJobId = jobId;
    m_lastProgress = PollProgressLocked();

    Logging::InfoIfInitialized("Server INPX scan started: jobId={}", jobId);
    return {
        .JobId = jobId,
        .Progress = m_lastProgress
    };
}

SServerScanProgress CServerScanCoordinator::PollProgress()
{
    std::scoped_lock lock(m_mutex);
    return PollProgressLocked();
}

SServerScanRuntimeStatus CServerScanCoordinator::GetRuntimeStatus(const std::size_t maxWorkers) const
{
    std::scoped_lock lock(m_mutex);
    const bool active = HasActiveScanLocked();
    return {
        .Active = active,
        .ActiveJobs = active ? 1U : 0U,
        .MaxConcurrentJobs = 1,
        .ActiveWorkers = active ? maxWorkers : 0U,
        .MaxWorkers = maxWorkers
    };
}

SServerScanCancelResult CServerScanCoordinator::CancelActiveScan()
{
    std::scoped_lock lock(m_mutex);
    if (!m_activeJobId.has_value())
    {
        throw CHttpErrorMapper::NotFound("No active INPX scan is running.");
    }

    const auto jobId = *m_activeJobId;
    const bool accepted = m_applicationHost.CancelInpxScanJob(jobId);
    m_lastProgress = PollProgressLocked();

    if (!accepted && !HasActiveScanLocked())
    {
        throw CHttpErrorMapper::NotFound("No active INPX scan is running.");
    }

    Logging::InfoIfInitialized("Server INPX scan cancel requested: jobId={} accepted={}", jobId, accepted);
    return {
        .Accepted = accepted,
        .Progress = m_lastProgress
    };
}

bool CServerScanCoordinator::HasActiveScanLocked() const
{
    return m_activeJobId.has_value()
        && (!m_lastProgress.Snapshot.has_value()
            || !m_lastProgress.Snapshot->IsTerminal());
}

SServerScanProgress CServerScanCoordinator::PollProgressLocked()
{
    if (!m_activeJobId.has_value())
    {
        return m_lastProgress;
    }

    const auto jobId = *m_activeJobId;
    auto snapshot = m_applicationHost.GetInpxScanJobSnapshot(jobId);
    if (!snapshot.has_value())
    {
        Logging::WarnIfInitialized("Server INPX scan snapshot disappeared: jobId={}", jobId);
        m_activeJobId.reset();
        m_lastProgress = {};
        return m_lastProgress;
    }

    m_lastProgress.Snapshot = snapshot;
    if (!snapshot->IsTerminal())
    {
        m_lastProgress.Result = std::nullopt;
        return m_lastProgress;
    }

    m_lastProgress.Result = m_applicationHost.GetInpxScanJobResult(jobId);
    if (!m_applicationHost.RemoveInpxScanJob(jobId))
    {
        Logging::WarnIfInitialized("Server INPX scan cleanup returned false: jobId={}", jobId);
    }
    m_activeJobId.reset();
    return m_lastProgress;
}

} // namespace InpxWebReader::Server
