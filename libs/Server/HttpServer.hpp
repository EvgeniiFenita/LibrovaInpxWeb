#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include "Server/ServerApplicationHost.hpp"
#include "Server/ServerConfig.hpp"
#include "Server/ServerScanCoordinator.hpp"

namespace InpxWebReader::Server {

struct SHttpServerOptions
{
    std::string HostUtf8 = "127.0.0.1";
    int Port = 8080;
    std::size_t MaxThreads = 4;
    std::size_t MaxQueuedRequests = 32;
    std::size_t MaxPageSize = 200;
    std::size_t MaxConcurrentDownloads = 2;
    std::size_t MaxRequestBodyBytes = static_cast<std::size_t>(64) * 1024U;
    std::size_t ReadTimeoutMs = static_cast<std::size_t>(15) * 1000U;
    std::size_t WriteTimeoutMs = static_cast<std::size_t>(30) * 1000U;
    std::optional<std::filesystem::path> StaticAssetsRoot = std::nullopt;
    SServerSecurityConfig Security;
    std::filesystem::path CacheRoot;
    std::filesystem::path RuntimeWorkspaceRoot;
    std::size_t MaxScanWorkers = 4;
    std::uint64_t MaxCoverCacheBytes = 128ULL * 1024ULL * 1024ULL;
    std::uint64_t MaxSteadyStateMemoryBytes = 1024ULL * 1024ULL * 1024ULL;
};

[[nodiscard]] SHttpServerOptions BuildHttpServerOptions(const SServerConfig& config);

class CHttpServer final
{
public:
    CHttpServer(IServerApplicationHost& applicationHost, SHttpServerOptions options);
    ~CHttpServer();

    CHttpServer(const CHttpServer&) = delete;
    CHttpServer& operator=(const CHttpServer&) = delete;
    CHttpServer(CHttpServer&&) = delete;
    CHttpServer& operator=(CHttpServer&&) = delete;

    void Start();
    void Stop() noexcept;
    [[nodiscard]] int GetBoundPort() const noexcept;
    [[nodiscard]] SServerScanStartResult StartScan(
        const ApplicationJobs::SInpxScanRequest& request);

private:
    struct SImpl;

    std::unique_ptr<SImpl> m_impl;
};

} // namespace InpxWebReader::Server
