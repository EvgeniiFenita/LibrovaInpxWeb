#include "Server/ServerConfig.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

#include "Foundation/StringUtils.hpp"
#include "Foundation/UnicodeConversion.hpp"
#include "Inpx/InpxSourceConfiguration.hpp"
#include "ScanSupport/ScanConcurrency.hpp"
#include "Storage/InpxCacheLayout.hpp"
#include "Storage/PathSafety.hpp"

namespace InpxWebReader::Server {
namespace {

const std::string GEnvCacheRoot = "INPX_WEB_READER_CACHE_ROOT";
const std::string GEnvRuntimeRoot = "INPX_WEB_READER_RUNTIME_ROOT";
const std::string GEnvInpxPath = "INPX_WEB_READER_INPX_PATH";
const std::string GEnvArchiveRoot = "INPX_WEB_READER_ARCHIVE_ROOT";
const std::string GEnvConverterPath = "INPX_WEB_READER_CONVERTER_PATH";
const std::string GEnvServerHost = "INPX_WEB_READER_SERVER_HOST";
const std::string GEnvServerPort = "INPX_WEB_READER_SERVER_PORT";
const std::string GEnvStaticAssetsRoot = "INPX_WEB_READER_STATIC_ASSETS_ROOT";
const std::string GEnvAuthToken = "INPX_WEB_READER_AUTH_TOKEN";
const std::string GEnvLogLevel = "INPX_WEB_READER_LOG_LEVEL";
const std::string GEnvLogMaxFileSizeMiB = "INPX_WEB_READER_LOG_MAX_FILE_SIZE_MIB";
const std::string GEnvLogMaxRotatedFiles = "INPX_WEB_READER_LOG_MAX_ROTATED_FILES";
const std::string GFieldCacheRoot = "cacheRoot";
const std::string GFieldRuntimeWorkspaceRoot = "runtimeWorkspaceRoot";
const std::string GFieldInpxSource = "inpxSource";
const std::string GFieldInpxPath = "inpxSource.inpxPath";
const std::string GFieldArchiveRoot = "inpxSource.archiveRoot";

struct SLimitDescriptor
{
    std::string_view JsonField;
    std::string_view EnvironmentName;
    std::size_t SServerLimitsConfig::* Value;
    std::optional<std::size_t> Maximum = std::nullopt;
};

constexpr std::array GLimitDescriptors = {
    SLimitDescriptor{"maxPageSize", "INPX_WEB_READER_MAX_PAGE_SIZE", &SServerLimitsConfig::MaxPageSize},
    SLimitDescriptor{"maxHttpThreads", "INPX_WEB_READER_MAX_HTTP_THREADS", &SServerLimitsConfig::MaxHttpThreads},
    SLimitDescriptor{"maxHttpQueuedRequests", "INPX_WEB_READER_MAX_HTTP_QUEUED_REQUESTS", &SServerLimitsConfig::MaxHttpQueuedRequests},
    SLimitDescriptor{"maxBackendQueueDepth", "INPX_WEB_READER_MAX_BACKEND_QUEUE_DEPTH", &SServerLimitsConfig::MaxBackendQueueDepth},
    SLimitDescriptor{"maxScanWorkers", "INPX_WEB_READER_MAX_SCAN_WORKERS", &SServerLimitsConfig::MaxScanWorkers, ScanSupport::GMaxScanWorkerCount},
    SLimitDescriptor{"maxConcurrentDownloads", "INPX_WEB_READER_MAX_CONCURRENT_DOWNLOADS", &SServerLimitsConfig::MaxConcurrentDownloads},
    SLimitDescriptor{"maxRequestBodyBytes", "INPX_WEB_READER_MAX_REQUEST_BODY_BYTES", &SServerLimitsConfig::MaxRequestBodyBytes},
    SLimitDescriptor{"httpReadTimeoutMs", "INPX_WEB_READER_HTTP_READ_TIMEOUT_MS", &SServerLimitsConfig::HttpReadTimeoutMs},
    SLimitDescriptor{"httpWriteTimeoutMs", "INPX_WEB_READER_HTTP_WRITE_TIMEOUT_MS", &SServerLimitsConfig::HttpWriteTimeoutMs},
    SLimitDescriptor{"maxCoverCacheMiB", "INPX_WEB_READER_MAX_COVER_CACHE_MIB", &SServerLimitsConfig::MaxCoverCacheMiB},
    SLimitDescriptor{"maxSteadyStateMemoryMiB", "INPX_WEB_READER_MAX_STEADY_STATE_MEMORY_MIB", &SServerLimitsConfig::MaxSteadyStateMemoryMiB}
};

[[nodiscard]] std::string LimitDisplayName(const SLimitDescriptor& descriptor)
{
    return "limits." + std::string{descriptor.JsonField};
}

[[nodiscard]] std::runtime_error ConfigError(const std::string& message)
{
    return std::runtime_error("Server config error: " + message);
}

[[nodiscard]] EServerLogLevel ParseLogLevel(
    std::string valueUtf8,
    const std::string_view fieldName)
{
    valueUtf8 = Foundation::ToLowerAscii(std::move(valueUtf8));
    if (valueUtf8 == "debug")
    {
        return EServerLogLevel::Debug;
    }
    if (valueUtf8 == "info")
    {
        return EServerLogLevel::Info;
    }
    if (valueUtf8 == "warning")
    {
        return EServerLogLevel::Warning;
    }
    if (valueUtf8 == "error")
    {
        return EServerLogLevel::Error;
    }

    throw ConfigError(
        "Field '" + std::string{fieldName}
        + "' must be one of: debug, info, warning, error.");
}

[[nodiscard]] std::filesystem::path PathFromConfigUtf8(
    const std::string& valueUtf8,
    const std::string_view fieldName)
{
    if (valueUtf8.empty())
    {
        throw ConfigError("Field '" + std::string{fieldName} + "' must not be empty.");
    }

    return Unicode::PathFromUtf8(valueUtf8);
}

[[nodiscard]] const nlohmann::json* FindOptionalObject(
    const nlohmann::json& parent,
    const std::string_view fieldName)
{
    const auto iterator = parent.find(fieldName);
    if (iterator == parent.end() || iterator->is_null())
    {
        return nullptr;
    }

    if (!iterator->is_object())
    {
        throw ConfigError("Config field '" + std::string{fieldName} + "' must be an object.");
    }

    return &*iterator;
}

[[nodiscard]] std::string RequireString(
    const nlohmann::json& parent,
    const std::string_view fieldName,
    const std::string_view displayName)
{
    const auto iterator = parent.find(fieldName);
    if (iterator == parent.end())
    {
        throw ConfigError("Missing required config field '" + std::string{displayName} + "'.");
    }

    if (!iterator->is_string())
    {
        throw ConfigError("Config field '" + std::string{displayName} + "' must be a string.");
    }

    return iterator->get<std::string>();
}

[[nodiscard]] std::optional<std::string> ReadOptionalString(
    const nlohmann::json& parent,
    const std::string_view fieldName,
    const std::string_view displayName)
{
    const auto iterator = parent.find(fieldName);
    if (iterator == parent.end() || iterator->is_null())
    {
        return std::nullopt;
    }

    if (!iterator->is_string())
    {
        throw ConfigError("Config field '" + std::string{displayName} + "' must be a string.");
    }

    return iterator->get<std::string>();
}

[[nodiscard]] bool ReadOptionalBool(
    const nlohmann::json& parent,
    const std::string_view fieldName,
    const std::string_view displayName,
    const bool fallback)
{
    const auto iterator = parent.find(fieldName);
    if (iterator == parent.end() || iterator->is_null())
    {
        return fallback;
    }

    if (!iterator->is_boolean())
    {
        throw ConfigError("Config field '" + std::string{displayName} + "' must be a boolean.");
    }

    return iterator->get<bool>();
}

[[nodiscard]] std::size_t ReadOptionalSize(
    const nlohmann::json& parent,
    const std::string_view fieldName,
    const std::string_view displayName,
    const std::size_t fallback)
{
    const auto iterator = parent.find(fieldName);
    if (iterator == parent.end() || iterator->is_null())
    {
        return fallback;
    }

    if (iterator->is_number_unsigned())
    {
        return iterator->get<std::size_t>();
    }

    if (iterator->is_number_integer())
    {
        const auto value = iterator->get<long long>();
        if (value < 0)
        {
            throw ConfigError("Config field '" + std::string{displayName} + "' must be an unsigned integer.");
        }

        return static_cast<std::size_t>(value);
    }

    throw ConfigError("Config field '" + std::string{displayName} + "' must be an unsigned integer.");
}

[[nodiscard]] std::uint16_t ReadOptionalPort(
    const nlohmann::json& parent,
    const std::string_view fieldName,
    const std::string_view displayName,
    const std::uint16_t fallback)
{
    const auto value = ReadOptionalSize(parent, fieldName, displayName, fallback);
    if (value == 0 || value > std::numeric_limits<std::uint16_t>::max())
    {
        throw ConfigError("Config field '" + std::string{displayName} + "' must be in range 1..65535.");
    }

    return static_cast<std::uint16_t>(value);
}

[[nodiscard]] std::size_t ParseSizeOverride(
    const std::string& valueUtf8,
    const std::string_view environmentName)
{
    std::size_t parsedLength = 0;
    unsigned long long value = 0;
    try
    {
        value = std::stoull(valueUtf8, &parsedLength, 10);
    }
    catch (const std::exception&)
    {
        throw ConfigError("Environment variable '" + std::string{environmentName} + "' must be an unsigned integer.");
    }

    if (parsedLength != valueUtf8.size() || value > std::numeric_limits<std::size_t>::max())
    {
        throw ConfigError("Environment variable '" + std::string{environmentName} + "' must be an unsigned integer.");
    }

    return static_cast<std::size_t>(value);
}

[[nodiscard]] std::uint16_t ParsePortOverride(
    const std::string& valueUtf8,
    const std::string_view environmentName)
{
    const auto value = ParseSizeOverride(valueUtf8, environmentName);
    if (value == 0 || value > std::numeric_limits<std::uint16_t>::max())
    {
        throw ConfigError("Environment variable '" + std::string{environmentName} + "' must be in range 1..65535.");
    }

    return static_cast<std::uint16_t>(value);
}

[[nodiscard]] bool TryGetEnvironment(
    const TServerEnvironment& environmentOverrides,
    const std::string_view name,
    std::string& valueUtf8)
{
    const auto iterator = environmentOverrides.find(std::string{name});
    if (iterator == environmentOverrides.end())
    {
        return false;
    }

    valueUtf8 = iterator->second;
    return true;
}

void ApplyPathOverride(
    const TServerEnvironment& environmentOverrides,
    const std::string& environmentName,
    std::filesystem::path& target,
    const std::string_view fieldName)
{
    std::string valueUtf8;
    if (TryGetEnvironment(environmentOverrides, environmentName, valueUtf8))
    {
        target = PathFromConfigUtf8(valueUtf8, fieldName);
    }
}

void ApplyOptionalPathOverride(
    const TServerEnvironment& environmentOverrides,
    const std::string& environmentName,
    std::optional<std::filesystem::path>& target,
    const std::string_view fieldName)
{
    std::string valueUtf8;
    if (TryGetEnvironment(environmentOverrides, environmentName, valueUtf8))
    {
        target = valueUtf8.empty()
            ? std::nullopt
            : std::make_optional(PathFromConfigUtf8(valueUtf8, fieldName));
    }
}

void ApplyStringOverride(
    const TServerEnvironment& environmentOverrides,
    const std::string& environmentName,
    std::string& target)
{
    std::string valueUtf8;
    if (TryGetEnvironment(environmentOverrides, environmentName, valueUtf8))
    {
        target = valueUtf8;
    }
}

void ApplySizeOverride(
    const TServerEnvironment& environmentOverrides,
    const std::string_view environmentName,
    std::size_t& target)
{
    std::string valueUtf8;
    if (TryGetEnvironment(environmentOverrides, environmentName, valueUtf8))
    {
        target = ParseSizeOverride(valueUtf8, environmentName);
    }
}

void ApplyEnvironmentOverrides(SServerConfig& config, const TServerEnvironment& environmentOverrides)
{
    ApplyPathOverride(environmentOverrides, GEnvCacheRoot, config.CacheRoot, GFieldCacheRoot);
    ApplyPathOverride(environmentOverrides, GEnvRuntimeRoot, config.RuntimeWorkspaceRoot, GFieldRuntimeWorkspaceRoot);
    ApplyPathOverride(environmentOverrides, GEnvInpxPath, config.InpxSource.InpxPath, GFieldInpxPath);
    ApplyPathOverride(environmentOverrides, GEnvArchiveRoot, config.InpxSource.ArchiveRoot, GFieldArchiveRoot);
    ApplyOptionalPathOverride(environmentOverrides, GEnvConverterPath, config.Converter.Path, "converter.path");
    ApplyStringOverride(environmentOverrides, GEnvServerHost, config.Server.HostUtf8);

    std::string valueUtf8;
    if (TryGetEnvironment(environmentOverrides, GEnvServerPort, valueUtf8))
    {
        config.Server.Port = ParsePortOverride(valueUtf8, GEnvServerPort);
    }

    ApplyOptionalPathOverride(environmentOverrides, GEnvStaticAssetsRoot, config.Server.StaticAssetsRoot, "server.staticAssetsRoot");
    ApplyStringOverride(environmentOverrides, GEnvAuthToken, config.Security.TokenUtf8);

    if (TryGetEnvironment(environmentOverrides, GEnvLogLevel, valueUtf8))
    {
        config.Logging.Level = ParseLogLevel(valueUtf8, GEnvLogLevel);
    }
    ApplySizeOverride(environmentOverrides, GEnvLogMaxFileSizeMiB, config.Logging.MaxFileSizeMiB);
    ApplySizeOverride(environmentOverrides, GEnvLogMaxRotatedFiles, config.Logging.MaxRotatedFiles);

    for (const auto& descriptor : GLimitDescriptors)
    {
        ApplySizeOverride(
            environmentOverrides,
            descriptor.EnvironmentName,
            config.Limits.*(descriptor.Value));
    }
}

[[nodiscard]] std::optional<std::string> ReadEnvironmentValueUtf8(const std::string& name)
{
    const char* value = std::getenv(name.c_str());
    if (value == nullptr)
    {
        return std::nullopt;
    }

    return std::string{value};
}

[[nodiscard]] std::string NormalizeHost(std::string hostUtf8)
{
    return Foundation::ToLowerAscii(std::move(hostUtf8));
}

[[nodiscard]] bool IsIpv4LoopbackAddress(std::string_view value) noexcept;

[[nodiscard]] bool IsLoopbackHost(const std::string& hostUtf8)
{
    const auto normalized = NormalizeHost(hostUtf8);
    return normalized == "localhost"
        || normalized == "::1"
        || normalized == "[::1]"
        || IsIpv4LoopbackAddress(normalized);
}

[[nodiscard]] bool TryParseIpv4Octet(
    const std::string_view value,
    std::size_t& offset,
    unsigned int& octet) noexcept
{
    if (offset >= value.size() || !std::isdigit(static_cast<unsigned char>(value[offset])))
    {
        return false;
    }

    unsigned int parsed = 0;
    std::size_t digits = 0;
    while (offset < value.size() && std::isdigit(static_cast<unsigned char>(value[offset])))
    {
        parsed = (parsed * 10U) + static_cast<unsigned int>(value[offset] - '0');
        if (parsed > 255U)
        {
            return false;
        }
        ++offset;
        ++digits;
    }

    if (digits == 0)
    {
        return false;
    }

    octet = parsed;
    return true;
}

[[nodiscard]] bool IsIpv4LoopbackAddress(const std::string_view value) noexcept
{
    unsigned int octets[4] = {};
    std::size_t offset = 0;
    for (std::size_t index = 0; index < 4; ++index)
    {
        if (!TryParseIpv4Octet(value, offset, octets[index]))
        {
            return false;
        }

        if (index < 3)
        {
            if (offset >= value.size() || value[offset] != '.')
            {
                return false;
            }
            ++offset;
        }
    }

    return offset == value.size() && octets[0] == 127U;
}

void ValidateAbsolutePath(const std::filesystem::path& path, const std::string_view fieldName)
{
    if (path.empty())
    {
        throw ConfigError("Field '" + std::string{fieldName} + "' must not be empty.");
    }

    if (!path.is_absolute())
    {
        throw ConfigError("Field '" + std::string{fieldName} + "' must be an absolute path.");
    }
}

void ValidatePositiveLimit(const std::size_t value, const std::string_view fieldName)
{
    if (value == 0)
    {
        throw ConfigError("Field '" + std::string{fieldName} + "' must be at least 1.");
    }
}

void ValidateLimitAtMost(
    const std::size_t value,
    const std::string_view fieldName,
    const std::size_t maximum)
{
    if (value > maximum)
    {
        throw ConfigError(
            "Field '" + std::string{fieldName} + "' must be at most " + std::to_string(maximum) + ".");
    }
}

[[nodiscard]] bool HasExistingCacheDatabase(const std::filesystem::path& cacheRoot)
{
    std::error_code errorCode;
    const auto databasePath = StoragePlanning::CInpxCacheLayout::GetDatabasePath(cacheRoot);
    return std::filesystem::is_regular_file(databasePath, errorCode) && !errorCode;
}

void ValidateRequiredInpxPath(
    const std::filesystem::path& path,
    const std::string_view fieldName)
{
    if (path.empty())
    {
        throw ConfigError("Missing required config field '" + std::string{fieldName} + "'.");
    }

    ValidateAbsolutePath(path, fieldName);
}

} // namespace

SServerConfig CServerConfigLoader::LoadFromFile(
    const std::filesystem::path& configPath,
    const TServerEnvironment& environmentOverrides)
{
    std::ifstream stream(configPath, std::ios::binary);
    if (!stream)
    {
        throw ConfigError("Could not open config file '" + Unicode::PathToUtf8(configPath) + "'.");
    }

    const std::string jsonUtf8(
        (std::istreambuf_iterator<char>(stream)),
        std::istreambuf_iterator<char>());
    return LoadFromJsonText(jsonUtf8, environmentOverrides);
}

SServerConfig CServerConfigLoader::LoadFromJsonText(
    const std::string_view jsonUtf8,
    const TServerEnvironment& environmentOverrides)
{
    nlohmann::json root;
    try
    {
        root = nlohmann::json::parse(jsonUtf8.begin(), jsonUtf8.end());
    }
    catch (const nlohmann::json::exception& ex)
    {
        throw ConfigError(std::string{"JSON is invalid: "} + ex.what());
    }

    if (!root.is_object())
    {
        throw ConfigError("Top-level JSON value must be an object.");
    }

    SServerConfig config;
    config.CacheRoot = PathFromConfigUtf8(
        RequireString(root, GFieldCacheRoot, GFieldCacheRoot),
        GFieldCacheRoot);
    config.RuntimeWorkspaceRoot = PathFromConfigUtf8(
        RequireString(root, GFieldRuntimeWorkspaceRoot, GFieldRuntimeWorkspaceRoot),
        GFieldRuntimeWorkspaceRoot);

    if (const auto* inpxSource = FindOptionalObject(root, GFieldInpxSource))
    {
        if (const auto inpxPathUtf8 = ReadOptionalString(*inpxSource, "inpxPath", GFieldInpxPath);
            inpxPathUtf8.has_value() && !inpxPathUtf8->empty())
        {
            config.InpxSource.InpxPath = PathFromConfigUtf8(*inpxPathUtf8, GFieldInpxPath);
        }

        if (const auto archiveRootUtf8 = ReadOptionalString(*inpxSource, "archiveRoot", GFieldArchiveRoot);
            archiveRootUtf8.has_value() && !archiveRootUtf8->empty())
        {
            config.InpxSource.ArchiveRoot = PathFromConfigUtf8(*archiveRootUtf8, GFieldArchiveRoot);
        }
    }

    if (const auto iterator = root.find("converter"); iterator != root.end() && !iterator->is_null())
    {
        if (!iterator->is_object())
        {
            throw ConfigError("Config field 'converter' must be an object.");
        }

        if (const auto pathUtf8 = ReadOptionalString(*iterator, "path", "converter.path");
            pathUtf8.has_value() && !pathUtf8->empty())
        {
            config.Converter.Path = PathFromConfigUtf8(*pathUtf8, "converter.path");
        }
    }

    if (const auto iterator = root.find("server"); iterator != root.end() && !iterator->is_null())
    {
        if (!iterator->is_object())
        {
            throw ConfigError("Config field 'server' must be an object.");
        }

        config.Server.HostUtf8 = ReadOptionalString(*iterator, "host", "server.host")
            .value_or(config.Server.HostUtf8);
        config.Server.Port = ReadOptionalPort(*iterator, "port", "server.port", config.Server.Port);
        if (const auto staticAssetsRootUtf8 = ReadOptionalString(*iterator, "staticAssetsRoot", "server.staticAssetsRoot");
            staticAssetsRootUtf8.has_value() && !staticAssetsRootUtf8->empty())
        {
            config.Server.StaticAssetsRoot = PathFromConfigUtf8(*staticAssetsRootUtf8, "server.staticAssetsRoot");
        }
    }

    if (const auto iterator = root.find("security"); iterator != root.end() && !iterator->is_null())
    {
        if (!iterator->is_object())
        {
            throw ConfigError("Config field 'security' must be an object.");
        }

        config.Security.TokenUtf8 = ReadOptionalString(*iterator, "token", "security.token")
            .value_or(config.Security.TokenUtf8);
    }

    if (const auto iterator = root.find("startup"); iterator != root.end() && !iterator->is_null())
    {
        if (!iterator->is_object())
        {
            throw ConfigError("Config field 'startup' must be an object.");
        }

        config.Startup.AutoScan = ReadOptionalBool(*iterator, "autoScan", "startup.autoScan", config.Startup.AutoScan);
        config.Startup.AutoScanOnEmptyCache = ReadOptionalBool(
            *iterator,
            "autoScanOnEmptyCache",
            "startup.autoScanOnEmptyCache",
            config.Startup.AutoScanOnEmptyCache);
        config.Startup.AutoRescanOnSourceChange = ReadOptionalBool(
            *iterator,
            "autoRescanOnSourceChange",
            "startup.autoRescanOnSourceChange",
            config.Startup.AutoRescanOnSourceChange);
    }

    if (const auto* logging = FindOptionalObject(root, "logging"))
    {
        if (const auto levelUtf8 = ReadOptionalString(*logging, "level", "logging.level"))
        {
            config.Logging.Level = ParseLogLevel(*levelUtf8, "logging.level");
        }
        config.Logging.MaxFileSizeMiB = ReadOptionalSize(
            *logging,
            "maxFileSizeMiB",
            "logging.maxFileSizeMiB",
            config.Logging.MaxFileSizeMiB);
        config.Logging.MaxRotatedFiles = ReadOptionalSize(
            *logging,
            "maxRotatedFiles",
            "logging.maxRotatedFiles",
            config.Logging.MaxRotatedFiles);
    }

    if (const auto iterator = root.find("limits"); iterator != root.end() && !iterator->is_null())
    {
        if (!iterator->is_object())
        {
            throw ConfigError("Config field 'limits' must be an object.");
        }

        for (const auto& descriptor : GLimitDescriptors)
        {
            auto& value = config.Limits.*(descriptor.Value);
            value = ReadOptionalSize(
                *iterator,
                descriptor.JsonField,
                LimitDisplayName(descriptor),
                value);
        }
    }

    ApplyEnvironmentOverrides(config, environmentOverrides);
    Validate(config);
    return config;
}

TServerEnvironment CServerConfigLoader::ReadProcessEnvironment()
{
    TServerEnvironment result;
    const std::string* environmentNames[] = {
        &GEnvCacheRoot,
        &GEnvRuntimeRoot,
        &GEnvInpxPath,
        &GEnvArchiveRoot,
        &GEnvConverterPath,
        &GEnvServerHost,
        &GEnvServerPort,
        &GEnvStaticAssetsRoot,
        &GEnvAuthToken,
        &GEnvLogLevel,
        &GEnvLogMaxFileSizeMiB,
        &GEnvLogMaxRotatedFiles
    };

    for (const std::string* name : environmentNames)
    {
        if (auto value = ReadEnvironmentValueUtf8(*name))
        {
            result.emplace(*name, std::move(*value));
        }
    }

    for (const auto& descriptor : GLimitDescriptors)
    {
        if (auto value = ReadEnvironmentValueUtf8(std::string{descriptor.EnvironmentName}))
        {
            result.emplace(std::string{descriptor.EnvironmentName}, std::move(*value));
        }
    }

    return result;
}

void CServerConfigLoader::Validate(const SServerConfig& config)
{
    ValidateAbsolutePath(config.CacheRoot, GFieldCacheRoot);
    ValidateAbsolutePath(config.RuntimeWorkspaceRoot, GFieldRuntimeWorkspaceRoot);

    if (SafePaths::IsPathWithinRoot(config.CacheRoot, config.RuntimeWorkspaceRoot))
    {
        throw ConfigError("Field 'runtimeWorkspaceRoot' must not be inside 'cacheRoot'.");
    }

    const bool hasExistingCacheDatabase = HasExistingCacheDatabase(config.CacheRoot);
    const bool hasConfiguredInpxPath = !config.InpxSource.InpxPath.empty();
    const bool hasConfiguredArchiveRoot = !config.InpxSource.ArchiveRoot.empty();
    if (!hasExistingCacheDatabase || hasConfiguredInpxPath || hasConfiguredArchiveRoot)
    {
        ValidateRequiredInpxPath(config.InpxSource.InpxPath, GFieldInpxPath);
        ValidateRequiredInpxPath(config.InpxSource.ArchiveRoot, GFieldArchiveRoot);
    }

    if (config.Converter.Path.has_value())
    {
        ValidateAbsolutePath(*config.Converter.Path, "converter.path");
    }

    if (config.Server.StaticAssetsRoot.has_value())
    {
        ValidateAbsolutePath(*config.Server.StaticAssetsRoot, "server.staticAssetsRoot");
    }

    if (config.Server.HostUtf8.empty())
    {
        throw ConfigError("Field 'server.host' must not be empty.");
    }

    if (config.Server.Port == 0)
    {
        throw ConfigError("Field 'server.port' must be in range 1..65535.");
    }

    if (!IsLoopbackHost(config.Server.HostUtf8)
        && config.Security.TokenUtf8.empty())
    {
        throw ConfigError("Non-loopback server.host requires security.token.");
    }

    ValidatePositiveLimit(config.Logging.MaxFileSizeMiB, "logging.maxFileSizeMiB");
    ValidateLimitAtMost(config.Logging.MaxFileSizeMiB, "logging.maxFileSizeMiB", 1024);
    ValidatePositiveLimit(config.Logging.MaxRotatedFiles, "logging.maxRotatedFiles");
    ValidateLimitAtMost(config.Logging.MaxRotatedFiles, "logging.maxRotatedFiles", 100);

    for (const auto& descriptor : GLimitDescriptors)
    {
        const std::size_t value = config.Limits.*(descriptor.Value);
        const std::string displayName = LimitDisplayName(descriptor);
        ValidatePositiveLimit(value, displayName);
        if (descriptor.Maximum.has_value())
        {
            ValidateLimitAtMost(value, displayName, *descriptor.Maximum);
        }
    }

    if (!hasExistingCacheDatabase)
    {
        const auto sourceValidation = Inpx::CInpxSourceConfiguration::Validate(
            config.InpxSource.InpxPath,
            config.InpxSource.ArchiveRoot);
        if (!sourceValidation.IsValid)
        {
            throw ConfigError(sourceValidation.ErrorUtf8);
        }
    }
}

} // namespace InpxWebReader::Server
