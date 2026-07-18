#pragma once

#include <mutex>
#include <optional>

#include "App/InpxScanJobService.hpp"
#include "Server/ServerApplicationHost.hpp"

namespace InpxWebReader::Server {

struct SServerScanProgress
{
    std::optional<ApplicationJobs::SInpxScanJobSnapshot> Snapshot = std::nullopt;
    std::optional<ApplicationJobs::SInpxScanJobResult> Result = std::nullopt;
};

struct SServerScanStartResult
{
    ApplicationJobs::TInpxScanJobId JobId = 0;
    SServerScanProgress Progress;
};

struct SServerScanCancelResult
{
    bool Accepted = false;
    SServerScanProgress Progress;
};

class CServerScanCoordinator final
{
public:
    explicit CServerScanCoordinator(IServerApplicationHost& applicationHost);

    [[nodiscard]] SServerScanStartResult StartScan(
        const ApplicationJobs::SInpxScanRequest& request);
    [[nodiscard]] SServerScanProgress PollProgress();
    [[nodiscard]] SServerScanRuntimeStatus GetRuntimeStatus(std::size_t maxWorkers) const;
    [[nodiscard]] SServerScanCancelResult CancelActiveScan();

private:
    [[nodiscard]] bool HasActiveScanLocked() const;
    [[nodiscard]] SServerScanProgress PollProgressLocked();

    IServerApplicationHost& m_applicationHost;
    mutable std::mutex m_mutex;
    std::optional<ApplicationJobs::TInpxScanJobId> m_activeJobId = std::nullopt;
    SServerScanProgress m_lastProgress;
};

} // namespace InpxWebReader::Server
