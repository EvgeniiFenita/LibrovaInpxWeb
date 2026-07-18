#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "Foundation/UnicodeConversion.hpp"
#include "ScanSupport/ScanConcurrency.hpp"
#include "Server/HttpServer.hpp"
#include "Server/ServerConfig.hpp"
#include "TestServerFixtures.hpp"
#include "TestWorkspace.hpp"

namespace {

struct SServerConfigFixture
{
    explicit SServerConfigFixture(std::string_view prefix)
        : Workspace(prefix)
        , InpxPath(InpxWebReader::Tests::ServerFixtures::WriteInpxArchive(
              Workspace.GetPath() / "source" / "catalog.inpx"))
        , ArchiveRoot(Workspace.GetPath() / "archives")
        , CacheRoot(Workspace.GetPath() / "cache")
        , RuntimeRoot(Workspace.GetPath() / "runtime")
    {
        std::filesystem::create_directories(ArchiveRoot);
    }

    CTestWorkspace Workspace;
    std::filesystem::path InpxPath;
    std::filesystem::path ArchiveRoot;
    std::filesystem::path CacheRoot;
    std::filesystem::path RuntimeRoot;
};

[[nodiscard]] std::string MakeConfigJson(const SServerConfigFixture& fixture)
{
    return InpxWebReader::Tests::ServerFixtures::MakeServerConfigJson(
        fixture.CacheRoot,
        fixture.RuntimeRoot,
        fixture.InpxPath,
        fixture.ArchiveRoot);
}

} // namespace

TEST_CASE("Server config parser applies defaults and validates INPX source", "[server][config]")
{
    SServerConfigFixture fixture("inpx-web-reader-config-defaults");

    const auto config = InpxWebReader::Server::CServerConfigLoader::LoadFromJsonText(MakeConfigJson(fixture));

    REQUIRE(config.CacheRoot == fixture.CacheRoot);
    REQUIRE(config.RuntimeWorkspaceRoot == fixture.RuntimeRoot);
    REQUIRE(config.InpxSource.InpxPath == fixture.InpxPath);
    REQUIRE(config.InpxSource.ArchiveRoot == fixture.ArchiveRoot);
    REQUIRE(config.Server.HostUtf8 == "127.0.0.1");
    REQUIRE(config.Server.Port == 8080);
    REQUIRE(config.Security.TokenUtf8.empty());
    REQUIRE(config.Startup.AutoScan);
    REQUIRE(config.Startup.AutoScanOnEmptyCache);
    REQUIRE_FALSE(config.Startup.AutoRescanOnSourceChange);
    REQUIRE(config.Logging.Level == InpxWebReader::Server::EServerLogLevel::Info);
    REQUIRE(config.Logging.MaxFileSizeMiB == 20);
    REQUIRE(config.Logging.MaxRotatedFiles == 4);
    REQUIRE(config.Limits.MaxPageSize == 200);
    REQUIRE(config.Limits.MaxHttpThreads == 4);
    REQUIRE(config.Limits.MaxHttpQueuedRequests == 32);
    REQUIRE(config.Limits.MaxBackendQueueDepth == 64);
    REQUIRE(config.Limits.MaxScanWorkers == 4);
    REQUIRE(config.Limits.MaxRequestBodyBytes == 64 * 1024);
    REQUIRE(config.Limits.HttpReadTimeoutMs == 15 * 1000);
    REQUIRE(config.Limits.HttpWriteTimeoutMs == 30 * 1000);
}

TEST_CASE("Server HTTP options preserve every runtime config limit", "[server][config][http]")
{
    InpxWebReader::Server::SServerConfig config;
    config.CacheRoot = "/srv/cache";
    config.RuntimeWorkspaceRoot = "/srv/runtime";
    config.Server = {
        .HostUtf8 = "0.0.0.0",
        .Port = 9123,
        .StaticAssetsRoot = "/srv/web"
    };
    config.Security.TokenUtf8 = "token";
    config.Limits = {
        .MaxPageSize = 11,
        .MaxHttpThreads = 12,
        .MaxHttpQueuedRequests = 13,
        .MaxBackendQueueDepth = 14,
        .MaxScanWorkers = 15,
        .MaxConcurrentDownloads = 16,
        .MaxRequestBodyBytes = 17,
        .HttpReadTimeoutMs = 18,
        .HttpWriteTimeoutMs = 19,
        .MaxCoverCacheMiB = 20,
        .MaxSteadyStateMemoryMiB = 21
    };

    const auto options = InpxWebReader::Server::BuildHttpServerOptions(config);

    REQUIRE(options.HostUtf8 == config.Server.HostUtf8);
    REQUIRE(options.Port == config.Server.Port);
    REQUIRE(options.MaxThreads == config.Limits.MaxHttpThreads);
    REQUIRE(options.MaxQueuedRequests == config.Limits.MaxHttpQueuedRequests);
    REQUIRE(options.MaxPageSize == config.Limits.MaxPageSize);
    REQUIRE(options.MaxConcurrentDownloads == config.Limits.MaxConcurrentDownloads);
    REQUIRE(options.MaxRequestBodyBytes == config.Limits.MaxRequestBodyBytes);
    REQUIRE(options.ReadTimeoutMs == config.Limits.HttpReadTimeoutMs);
    REQUIRE(options.WriteTimeoutMs == config.Limits.HttpWriteTimeoutMs);
    REQUIRE(options.StaticAssetsRoot == config.Server.StaticAssetsRoot);
    REQUIRE(options.Security.TokenUtf8 == config.Security.TokenUtf8);
    REQUIRE(options.CacheRoot == config.CacheRoot);
    REQUIRE(options.RuntimeWorkspaceRoot == config.RuntimeWorkspaceRoot);
    REQUIRE(options.MaxScanWorkers == config.Limits.MaxScanWorkers);
    REQUIRE(options.MaxCoverCacheBytes == 20ULL * 1024ULL * 1024ULL);
    REQUIRE(options.MaxSteadyStateMemoryBytes == 21ULL * 1024ULL * 1024ULL);
}

TEST_CASE("Server config parser applies explicit startup scan policy", "[server][config]")
{
    SServerConfigFixture fixture("inpx-web-reader-config-startup");
    const std::string json = InpxWebReader::Tests::ServerFixtures::MakeServerConfigJson(
        fixture.CacheRoot,
        fixture.RuntimeRoot,
        fixture.InpxPath,
        fixture.ArchiveRoot,
        {},
        {},
        {},
        R"({"autoScan":false,"autoScanOnEmptyCache":false,"autoRescanOnSourceChange":true})");

    const auto config = InpxWebReader::Server::CServerConfigLoader::LoadFromJsonText(json);

    REQUIRE_FALSE(config.Startup.AutoScan);
    REQUIRE_FALSE(config.Startup.AutoScanOnEmptyCache);
    REQUIRE(config.Startup.AutoRescanOnSourceChange);
}

TEST_CASE("Server config parser applies explicit logging policy", "[server][config][logging]")
{
    SServerConfigFixture fixture("inpx-web-reader-config-logging");
    const std::string json = InpxWebReader::Tests::ServerFixtures::MakeServerConfigJson(
        fixture.CacheRoot,
        fixture.RuntimeRoot,
        fixture.InpxPath,
        fixture.ArchiveRoot,
        {},
        {},
        {},
        {},
        R"({"level":"debug","maxFileSizeMiB":32,"maxRotatedFiles":6})");

    const auto config = InpxWebReader::Server::CServerConfigLoader::LoadFromJsonText(json);

    REQUIRE(config.Logging.Level == InpxWebReader::Server::EServerLogLevel::Debug);
    REQUIRE(config.Logging.MaxFileSizeMiB == 32);
    REQUIRE(config.Logging.MaxRotatedFiles == 6);
}

TEST_CASE("Server config parser applies environment overrides after JSON", "[server][config]")
{
    SServerConfigFixture fixture("inpx-web-reader-config-env");
    const auto overriddenCacheRoot =
        fixture.Workspace.GetPath() / InpxWebReader::Unicode::PathFromUtf8("библиотека");
    const auto overriddenRuntimeRoot = fixture.Workspace.GetPath() / "runtime-override";
    const auto overriddenConverterPath = fixture.Workspace.GetPath() / "tools" / "fbc";

    InpxWebReader::Server::TServerEnvironment environment{
        {"INPX_WEB_READER_CACHE_ROOT", InpxWebReader::Unicode::PathToUtf8(overriddenCacheRoot)},
        {"INPX_WEB_READER_RUNTIME_ROOT", InpxWebReader::Unicode::PathToUtf8(overriddenRuntimeRoot)},
        {"INPX_WEB_READER_CONVERTER_PATH", InpxWebReader::Unicode::PathToUtf8(overriddenConverterPath)},
        {"INPX_WEB_READER_SERVER_HOST", "0.0.0.0"},
        {"INPX_WEB_READER_SERVER_PORT", "9090"},
        {"INPX_WEB_READER_AUTH_TOKEN", "local-token"},
        {"INPX_WEB_READER_LOG_LEVEL", "warning"},
        {"INPX_WEB_READER_LOG_MAX_FILE_SIZE_MIB", "40"},
        {"INPX_WEB_READER_LOG_MAX_ROTATED_FILES", "7"},
        {"INPX_WEB_READER_MAX_HTTP_QUEUED_REQUESTS", "5"},
        {"INPX_WEB_READER_MAX_BACKEND_QUEUE_DEPTH", "7"},
        {"INPX_WEB_READER_MAX_SCAN_WORKERS", "3"},
        {"INPX_WEB_READER_MAX_REQUEST_BODY_BYTES", "1024"},
        {"INPX_WEB_READER_HTTP_READ_TIMEOUT_MS", "2000"},
        {"INPX_WEB_READER_HTTP_WRITE_TIMEOUT_MS", "3000"}
    };

    const auto config = InpxWebReader::Server::CServerConfigLoader::LoadFromJsonText(
        MakeConfigJson(fixture),
        environment);

    REQUIRE(config.CacheRoot == overriddenCacheRoot);
    REQUIRE(config.RuntimeWorkspaceRoot == overriddenRuntimeRoot);
    REQUIRE(config.Converter.Path == overriddenConverterPath);
    REQUIRE(config.Server.HostUtf8 == "0.0.0.0");
    REQUIRE(config.Server.Port == 9090);
    REQUIRE(config.Security.TokenUtf8 == "local-token");
    REQUIRE(config.Logging.Level == InpxWebReader::Server::EServerLogLevel::Warning);
    REQUIRE(config.Logging.MaxFileSizeMiB == 40);
    REQUIRE(config.Logging.MaxRotatedFiles == 7);
    REQUIRE(config.Limits.MaxHttpQueuedRequests == 5);
    REQUIRE(config.Limits.MaxBackendQueueDepth == 7);
    REQUIRE(config.Limits.MaxScanWorkers == 3);
    REQUIRE(config.Limits.MaxRequestBodyBytes == 1024);
    REQUIRE(config.Limits.HttpReadTimeoutMs == 2000);
    REQUIRE(config.Limits.HttpWriteTimeoutMs == 3000);
}

TEST_CASE("Server config rejects invalid logging policy", "[server][config][logging]")
{
    SServerConfigFixture fixture("inpx-web-reader-config-invalid-logging");

    const auto makeJson = [&](const std::string& loggingJson) {
        return InpxWebReader::Tests::ServerFixtures::MakeServerConfigJson(
            fixture.CacheRoot,
            fixture.RuntimeRoot,
            fixture.InpxPath,
            fixture.ArchiveRoot,
            {},
            {},
            {},
            {},
            loggingJson);
    };

    REQUIRE_THROWS_WITH(
        InpxWebReader::Server::CServerConfigLoader::LoadFromJsonText(
            makeJson(R"({"level":"verbose"})")),
        Catch::Matchers::ContainsSubstring("logging.level"));
    REQUIRE_THROWS_WITH(
        InpxWebReader::Server::CServerConfigLoader::LoadFromJsonText(
            makeJson(R"({"maxFileSizeMiB":0})")),
        Catch::Matchers::ContainsSubstring("logging.maxFileSizeMiB"));
    REQUIRE_THROWS_WITH(
        InpxWebReader::Server::CServerConfigLoader::LoadFromJsonText(
            makeJson(R"({"maxRotatedFiles":101})")),
        Catch::Matchers::ContainsSubstring("logging.maxRotatedFiles"));
}

TEST_CASE("Server config rejects LAN bind without token", "[server][config]")
{
    SServerConfigFixture fixture("inpx-web-reader-config-lan-auth");
    const std::string json = InpxWebReader::Tests::ServerFixtures::MakeServerConfigJson(
        fixture.CacheRoot,
        fixture.RuntimeRoot,
        fixture.InpxPath,
        fixture.ArchiveRoot,
        R"({"host":"0.0.0.0","port":8080})",
        R"({"token":""})");

    try
    {
        (void)InpxWebReader::Server::CServerConfigLoader::LoadFromJsonText(json);
        FAIL("Expected LAN bind without token to fail.");
    }
    catch (const std::runtime_error& ex)
    {
        REQUIRE(std::string{ex.what()}.find("security.token") != std::string::npos);
    }
}

TEST_CASE("Server config cannot bypass LAN authentication", "[server][config][auth]")
{
    SServerConfigFixture fixture("inpx-web-reader-config-lan-auth-bypass");
    const std::string json = InpxWebReader::Tests::ServerFixtures::MakeServerConfigJson(
        fixture.CacheRoot,
        fixture.RuntimeRoot,
        fixture.InpxPath,
        fixture.ArchiveRoot,
        R"({"host":"0.0.0.0","port":8080})",
        R"({"token":"","allowUnauthenticatedLan":true})");
    const InpxWebReader::Server::TServerEnvironment environment{
        {"INPX_WEB_READER_ALLOW_UNAUTHENTICATED_LAN", "true"}
    };

    try
    {
        (void)InpxWebReader::Server::CServerConfigLoader::LoadFromJsonText(json, environment);
        FAIL("Expected LAN authentication bypass settings to be rejected.");
    }
    catch (const std::runtime_error& ex)
    {
        REQUIRE(std::string{ex.what()}.find("security.token") != std::string::npos);
    }
}

TEST_CASE("Server config only treats numeric 127/8 hosts as loopback", "[server][config]")
{
    SServerConfigFixture fixture("inpx-web-reader-config-loopback-host");

    const std::string impostor = InpxWebReader::Tests::ServerFixtures::MakeServerConfigJson(
        fixture.CacheRoot,
        fixture.RuntimeRoot,
        fixture.InpxPath,
        fixture.ArchiveRoot,
        R"({"host":"127.example.com","port":8080})",
        R"({"token":""})");

    try
    {
        (void)InpxWebReader::Server::CServerConfigLoader::LoadFromJsonText(impostor);
        FAIL("Expected non-numeric 127-prefixed host to require a token.");
    }
    catch (const std::runtime_error& ex)
    {
        REQUIRE(std::string{ex.what()}.find("security.token") != std::string::npos);
    }

    const std::string numericLoopback = InpxWebReader::Tests::ServerFixtures::MakeServerConfigJson(
        fixture.CacheRoot,
        fixture.RuntimeRoot,
        fixture.InpxPath,
        fixture.ArchiveRoot,
        R"({"host":"127.0.0.2","port":8080})",
        R"({"token":""})");

    const auto config = InpxWebReader::Server::CServerConfigLoader::LoadFromJsonText(numericLoopback);
    REQUIRE(config.Server.HostUtf8 == "127.0.0.2");
}

TEST_CASE("Server config rejects runtime workspace inside cache root", "[server][config]")
{
    SServerConfigFixture fixture("inpx-web-reader-config-runtime-inside-cache");
    const auto nestedRuntime = fixture.CacheRoot / "runtime";

    const std::string json = InpxWebReader::Tests::ServerFixtures::MakeServerConfigJson(
        fixture.CacheRoot,
        nestedRuntime,
        fixture.InpxPath,
        fixture.ArchiveRoot);

    try
    {
        (void)InpxWebReader::Server::CServerConfigLoader::LoadFromJsonText(json);
        FAIL("Expected nested runtime workspace to fail.");
    }
    catch (const std::runtime_error& ex)
    {
        REQUIRE(std::string{ex.what()}.find("runtimeWorkspaceRoot") != std::string::npos);
    }
}

TEST_CASE("Server config rejects invalid INPX paths", "[server][config]")
{
    SServerConfigFixture fixture("inpx-web-reader-config-invalid-inpx");
    InpxWebReader::Server::TServerEnvironment environment{
        {"INPX_WEB_READER_INPX_PATH", InpxWebReader::Unicode::PathToUtf8(fixture.Workspace.GetPath() / "missing.inpx")}
    };

    try
    {
        (void)InpxWebReader::Server::CServerConfigLoader::LoadFromJsonText(
            MakeConfigJson(fixture),
            environment);
        FAIL("Expected missing INPX path to fail.");
    }
    catch (const std::runtime_error& ex)
    {
        REQUIRE(std::string{ex.what()}.find("INPX file does not exist") != std::string::npos);
    }
}

TEST_CASE("Server config accepts unavailable source paths for an existing cache", "[server][config]")
{
    SServerConfigFixture fixture("inpx-web-reader-config-existing-source-outage");
    std::filesystem::create_directories(fixture.CacheRoot / "Database");
    std::ofstream(fixture.CacheRoot / "Database" / "inpx-web-reader.db").put('\0');

    const std::string json = InpxWebReader::Tests::ServerFixtures::MakeServerConfigJson(
        fixture.CacheRoot,
        fixture.RuntimeRoot,
        fixture.Workspace.GetPath() / "missing.inpx",
        fixture.Workspace.GetPath() / "missing-archives");

    const auto config = InpxWebReader::Server::CServerConfigLoader::LoadFromJsonText(json);

    REQUIRE(config.InpxSource.InpxPath == fixture.Workspace.GetPath() / "missing.inpx");
    REQUIRE(config.InpxSource.ArchiveRoot == fixture.Workspace.GetPath() / "missing-archives");
}

TEST_CASE("Server config accepts missing INPX config for an existing cache", "[server][config]")
{
    SServerConfigFixture fixture("inpx-web-reader-config-existing-without-source-config");
    std::filesystem::create_directories(fixture.CacheRoot / "Database");
    std::ofstream(fixture.CacheRoot / "Database" / "inpx-web-reader.db").put('\0');

    const std::string json = "{"
        "\"cacheRoot\":\"" + InpxWebReader::Unicode::PathToUtf8(fixture.CacheRoot) + "\","
        "\"runtimeWorkspaceRoot\":\"" + InpxWebReader::Unicode::PathToUtf8(fixture.RuntimeRoot) + "\""
        "}";

    const auto config = InpxWebReader::Server::CServerConfigLoader::LoadFromJsonText(json);

    REQUIRE(config.InpxSource.InpxPath.empty());
    REQUIRE(config.InpxSource.ArchiveRoot.empty());
}

TEST_CASE("Server config rejects partial INPX config for an existing cache", "[server][config]")
{
    SServerConfigFixture fixture("inpx-web-reader-config-existing-with-partial-source-config");
    std::filesystem::create_directories(fixture.CacheRoot / "Database");
    std::ofstream(fixture.CacheRoot / "Database" / "inpx-web-reader.db").put('\0');

    const std::string json = "{"
        "\"cacheRoot\":\"" + InpxWebReader::Unicode::PathToUtf8(fixture.CacheRoot) + "\","
        "\"runtimeWorkspaceRoot\":\"" + InpxWebReader::Unicode::PathToUtf8(fixture.RuntimeRoot) + "\","
        "\"inpxSource\":{\"inpxPath\":\"" + InpxWebReader::Unicode::PathToUtf8(fixture.InpxPath) + "\"}"
        "}";

    REQUIRE_THROWS_WITH(
        InpxWebReader::Server::CServerConfigLoader::LoadFromJsonText(json),
        Catch::Matchers::ContainsSubstring("inpxSource.archiveRoot"));
}

TEST_CASE("Server config rejects missing INPX config for a new cache", "[server][config]")
{
    SServerConfigFixture fixture("inpx-web-reader-config-new-without-source-config");
    const std::string json = "{"
        "\"cacheRoot\":\"" + InpxWebReader::Unicode::PathToUtf8(fixture.CacheRoot) + "\","
        "\"runtimeWorkspaceRoot\":\"" + InpxWebReader::Unicode::PathToUtf8(fixture.RuntimeRoot) + "\""
        "}";

    try
    {
        (void)InpxWebReader::Server::CServerConfigLoader::LoadFromJsonText(json);
        FAIL("Expected missing INPX source config to fail for a new cache.");
    }
    catch (const std::runtime_error& ex)
    {
        REQUIRE(std::string{ex.what()}.find("inpxSource.inpxPath") != std::string::npos);
    }
}

TEST_CASE("Server config rejects zero queue depth", "[server][config]")
{
    SServerConfigFixture fixture("inpx-web-reader-config-limits");
    const std::string json = InpxWebReader::Tests::ServerFixtures::MakeServerConfigJson(
        fixture.CacheRoot,
        fixture.RuntimeRoot,
        fixture.InpxPath,
        fixture.ArchiveRoot,
        {},
        {},
        R"({"maxBackendQueueDepth":0})");

    try
    {
        (void)InpxWebReader::Server::CServerConfigLoader::LoadFromJsonText(json);
        FAIL("Expected zero backend queue depth to fail.");
    }
    catch (const std::runtime_error& ex)
    {
        REQUIRE(std::string{ex.what()}.find("limits.maxBackendQueueDepth") != std::string::npos);
    }
}

TEST_CASE("Server config rejects zero HTTP request limits", "[server][config]")
{
    SServerConfigFixture fixture("inpx-web-reader-config-http-limits");
    const std::string json = InpxWebReader::Tests::ServerFixtures::MakeServerConfigJson(
        fixture.CacheRoot,
        fixture.RuntimeRoot,
        fixture.InpxPath,
        fixture.ArchiveRoot,
        {},
        {},
        R"({"maxRequestBodyBytes":0})");

    try
    {
        (void)InpxWebReader::Server::CServerConfigLoader::LoadFromJsonText(json);
        FAIL("Expected zero request body limit to fail.");
    }
    catch (const std::runtime_error& ex)
    {
        REQUIRE(std::string{ex.what()}.find("limits.maxRequestBodyBytes") != std::string::npos);
    }
}

TEST_CASE("Server config rejects zero for every runtime limit field", "[server][config][limits]")
{
    SServerConfigFixture fixture("inpx-web-reader-config-zero-limits");
    const std::vector<std::string> fields{
        "maxPageSize",
        "maxHttpThreads",
        "maxHttpQueuedRequests",
        "maxBackendQueueDepth",
        "maxScanWorkers",
        "maxConcurrentDownloads",
        "maxRequestBodyBytes",
        "httpReadTimeoutMs",
        "httpWriteTimeoutMs",
        "maxCoverCacheMiB",
        "maxSteadyStateMemoryMiB"
    };

    for (const auto& field : fields)
    {
        const std::string json = InpxWebReader::Tests::ServerFixtures::MakeServerConfigJson(
            fixture.CacheRoot,
            fixture.RuntimeRoot,
            fixture.InpxPath,
            fixture.ArchiveRoot,
            {},
            {},
            "{\"" + field + "\":0}");

        try
        {
            (void)InpxWebReader::Server::CServerConfigLoader::LoadFromJsonText(json);
            FAIL("Expected zero server limit to fail.");
        }
        catch (const std::runtime_error& ex)
        {
            REQUIRE(std::string{ex.what()}.find("limits." + field) != std::string::npos);
        }
    }
}

TEST_CASE("Server config rejects scan worker counts above the bounded scan worker maximum", "[server][config][limits]")
{
    SServerConfigFixture fixture("inpx-web-reader-config-scan-worker-upper-bound");
    const std::string json = InpxWebReader::Tests::ServerFixtures::MakeServerConfigJson(
        fixture.CacheRoot,
        fixture.RuntimeRoot,
        fixture.InpxPath,
        fixture.ArchiveRoot,
        {},
        {},
        "{\"maxScanWorkers\":" + std::to_string(InpxWebReader::ScanSupport::GMaxScanWorkerCount + 1) + "}");

    try
    {
        (void)InpxWebReader::Server::CServerConfigLoader::LoadFromJsonText(json);
        FAIL("Expected an oversized scan worker limit to fail.");
    }
    catch (const std::runtime_error& ex)
    {
        const std::string message{ex.what()};
        REQUIRE(message.find("limits.maxScanWorkers") != std::string::npos);
        REQUIRE(message.find(std::to_string(InpxWebReader::ScanSupport::GMaxScanWorkerCount)) != std::string::npos);
    }
}

TEST_CASE("Server config rejects non-numeric runtime limit fields", "[server][config][limits]")
{
    SServerConfigFixture fixture("inpx-web-reader-config-nonnumeric-limits");
    const std::vector<std::string> fields{
        "maxPageSize",
        "maxHttpThreads",
        "maxHttpQueuedRequests",
        "maxBackendQueueDepth",
        "maxScanWorkers",
        "maxConcurrentDownloads",
        "maxRequestBodyBytes",
        "httpReadTimeoutMs",
        "httpWriteTimeoutMs",
        "maxCoverCacheMiB",
        "maxSteadyStateMemoryMiB"
    };

    for (const auto& field : fields)
    {
        const std::string json = InpxWebReader::Tests::ServerFixtures::MakeServerConfigJson(
            fixture.CacheRoot,
            fixture.RuntimeRoot,
            fixture.InpxPath,
            fixture.ArchiveRoot,
            {},
            {},
            "{\"" + field + "\":\"not-a-number\"}");

        try
        {
            (void)InpxWebReader::Server::CServerConfigLoader::LoadFromJsonText(json);
            FAIL("Expected non-numeric server limit to fail.");
        }
        catch (const std::runtime_error& ex)
        {
            REQUIRE(std::string{ex.what()}.find("limits." + field) != std::string::npos);
        }
    }
}
