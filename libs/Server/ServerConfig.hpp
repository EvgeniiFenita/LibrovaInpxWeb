#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace InpxWebReader::Server {

struct SServerInpxSourceConfig
{
    std::filesystem::path InpxPath;
    std::filesystem::path ArchiveRoot;
};

struct SServerConverterConfig
{
    std::optional<std::filesystem::path> Path = std::nullopt;
};

struct SServerEndpointConfig
{
    std::string HostUtf8 = "127.0.0.1";
    std::uint16_t Port = 8080;
    std::optional<std::filesystem::path> StaticAssetsRoot = std::nullopt;
};

struct SServerSecurityConfig
{
    std::string TokenUtf8;
};

struct SServerStartupConfig
{
    bool AutoScan = true;
    bool AutoScanOnEmptyCache = true;
    bool AutoRescanOnSourceChange = false;
};

enum class EServerLogLevel
{
    Debug,
    Info,
    Warning,
    Error
};

struct SServerLoggingConfig
{
    EServerLogLevel Level = EServerLogLevel::Info;
    std::size_t MaxFileSizeMiB = 20;
    std::size_t MaxRotatedFiles = 4;
};

struct SServerLimitsConfig
{
    std::size_t MaxPageSize = 200;
    std::size_t MaxHttpThreads = 4;
    std::size_t MaxHttpQueuedRequests = 32;
    std::size_t MaxBackendQueueDepth = 64;
    std::size_t MaxScanWorkers = 4;
    std::size_t MaxConcurrentDownloads = 2;
    std::size_t MaxRequestBodyBytes = static_cast<std::size_t>(64) * 1024U;
    std::size_t HttpReadTimeoutMs = static_cast<std::size_t>(15) * 1000U;
    std::size_t HttpWriteTimeoutMs = static_cast<std::size_t>(30) * 1000U;
    std::size_t MaxCoverCacheMiB = 128;
    std::size_t MaxSteadyStateMemoryMiB = 1024;
};

struct SServerConfig
{
    std::filesystem::path CacheRoot;
    std::filesystem::path RuntimeWorkspaceRoot;
    SServerInpxSourceConfig InpxSource;
    SServerConverterConfig Converter;
    SServerEndpointConfig Server;
    SServerSecurityConfig Security;
    SServerStartupConfig Startup;
    SServerLoggingConfig Logging;
    SServerLimitsConfig Limits;
};

using TServerEnvironment = std::unordered_map<std::string, std::string>;

class CServerConfigLoader final
{
public:
    [[nodiscard]] static SServerConfig LoadFromFile(
        const std::filesystem::path& configPath,
        const TServerEnvironment& environmentOverrides = ReadProcessEnvironment());

    [[nodiscard]] static SServerConfig LoadFromJsonText(
        std::string_view jsonUtf8,
        const TServerEnvironment& environmentOverrides = {});

    [[nodiscard]] static TServerEnvironment ReadProcessEnvironment();

    static void Validate(const SServerConfig& config);
};

} // namespace InpxWebReader::Server
