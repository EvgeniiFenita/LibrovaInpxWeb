#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>

#include "Database/SqliteConnection.hpp"
#include "Domain/DomainError.hpp"
#include "Foundation/UnicodeConversion.hpp"
#include "Server/ServerApplicationHost.hpp"
#include "Server/ServerConfig.hpp"
#include "TestServerFixtures.hpp"
#include "TestWorkspace.hpp"

namespace {

namespace Application = InpxWebReader::Application;
namespace ApplicationJobs = InpxWebReader::ApplicationJobs;
namespace Domain = InpxWebReader::Domain;
namespace Server = InpxWebReader::Server;

[[nodiscard]] std::chrono::system_clock::time_point FixedTime()
{
    return std::chrono::system_clock::time_point{std::chrono::seconds{1'779'276'600}};
}

class CFakeDownloadApplication final : public Application::IInpxCatalogApplication
{
public:
    explicit CFakeDownloadApplication(Application::SInpxSourceInfo source)
        : Source(std::move(source))
    {
        Details = Application::SBookDetails{
            .Id = Domain::SBookId{7},
            .TitleUtf8 = "Очень длинное название книги с кириллицей Очень длинное название книги с кириллицей "
                         "Очень длинное название книги с кириллицей Очень длинное название книги с кириллицей",
            .AuthorsUtf8 = {"Автор с очень длинным именем Автор с очень длинным именем"},
            .Language = "ru",
            .Format = Domain::EBookFormat::Fb2,
            .SizeBytes = 7,
            .AddedAtUtc = FixedTime(),
            .CanDownloadOriginal = true,
            .CanDownloadAsEpub = true,
            .IsAvailable = true
        };
    }

    [[nodiscard]] Application::SCatalogSessionInfo GetCatalogSessionInfo() override
    {
        return {
            .Capabilities = {
                .CanRescanInpxSource = true,
                .CanDownloadOriginal = true,
                .CanDownloadAsEpub = true
            },
            .InpxSource = Source
        };
    }

    Application::SBookListResult ListBooks(const Application::SBookListRequest&) override
    {
        return {};
    }

    std::optional<Application::SBookDetails> GetBookDetails(const Domain::SBookId id) override
    {
        ++GetBookDetailsCalls;
        return id.Value == Details.Id.Value ? std::optional{Details} : std::nullopt;
    }

    Application::SCatalogStatistics GetCatalogStatistics() override { return {}; }
    ApplicationJobs::TInpxScanJobId StartInpxScan(const ApplicationJobs::SInpxScanRequest&) override { return 0; }
    std::optional<ApplicationJobs::SInpxScanJobSnapshot> GetInpxScanJobSnapshot(ApplicationJobs::TInpxScanJobId) override { return std::nullopt; }
    std::optional<ApplicationJobs::SInpxScanJobResult> GetInpxScanJobResult(ApplicationJobs::TInpxScanJobId) override { return std::nullopt; }
    std::optional<Application::SInpxSourceOverview> GetInpxSourceOverview() override { return std::nullopt; }
    bool CancelInpxScanJob(ApplicationJobs::TInpxScanJobId) override { return false; }
    bool RemoveInpxScanJob(ApplicationJobs::TInpxScanJobId) override { return false; }

    std::optional<Application::SPreparedBookDownload> PrepareDownload(
        const Application::SBookDownloadRequest& request) override
    {
        LastDestinationPath = request.DestinationPath;
        std::filesystem::create_directories(request.DestinationPath.parent_path());
        if (ThrowOnDownload)
        {
            throw std::runtime_error("download preparation failed");
        }
        if (BlockDownload)
        {
            std::unique_lock lock(DownloadMutex);
            DownloadEntered = true;
            DownloadWake.notify_all();
            while (!ReleaseDownload && !request.StopToken.stop_requested())
            {
                DownloadWake.wait_for(lock, std::chrono::milliseconds{10});
            }
            if (request.StopToken.stop_requested())
            {
                throw Domain::CDomainException(
                    Domain::EDomainErrorCode::Cancellation,
                    "download preparation cancelled");
            }
        }

        std::ofstream output(request.DestinationPath, std::ios::binary);
        output << "payload";
        return Application::SPreparedBookDownload{
            .Path = request.DestinationPath,
            .TitleUtf8 = Details.TitleUtf8,
            .AuthorsUtf8 = Details.AuthorsUtf8,
            .Format = request.RequestedFormat.value_or(Details.Format)
        };
    }

    bool WaitForDownloadEntry(const std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(DownloadMutex);
        return DownloadWake.wait_for(lock, timeout, [&]() { return DownloadEntered; });
    }

    Application::SInpxSourceInfo Source;
    Application::SBookDetails Details;
    std::filesystem::path LastDestinationPath;
    int GetBookDetailsCalls = 0;
    bool ThrowOnDownload = false;
    bool BlockDownload = false;
    bool DownloadEntered = false;
    bool ReleaseDownload = false;
    std::mutex DownloadMutex;
    std::condition_variable DownloadWake;
};

[[nodiscard]] Server::SServerConfig MakeHostConfig(CTestWorkspace& workspace)
{
    const auto inpxPath = InpxWebReader::Tests::ServerFixtures::WriteInpxArchive(
        workspace.GetPath() / "source" / "catalog.inpx");
    const auto archiveRoot = workspace.GetPath() / "archives";
    std::filesystem::create_directories(archiveRoot);

    return Server::CServerConfigLoader::LoadFromJsonText(
        InpxWebReader::Tests::ServerFixtures::MakeServerConfigJson(
            workspace.GetPath() / "cache",
            workspace.GetPath() / "runtime",
            inpxPath,
            archiveRoot));
}

} // namespace

TEST_CASE("Configured startup scan mode covers initial and rescan policies", "[server][host][scan]")
{
    Server::SServerStartupConfig startup;
    Server::SServerStatus status;

    SECTION("source is not configured")
    {
        REQUIRE_FALSE(Server::ResolveConfiguredStartupScanMode(startup, status).has_value());
    }

    status.InpxSource = Server::SServerInpxSourceStatus{};

    SECTION("new empty cache starts an initial scan")
    {
        status.CreatedCacheOnOpen = true;
        REQUIRE(Server::ResolveConfiguredStartupScanMode(startup, status)
            == ApplicationJobs::EInpxScanMode::InitialScan);
    }

    SECTION("master auto-scan switch disables initial scan")
    {
        startup.AutoScan = false;
        status.CreatedCacheOnOpen = true;
        REQUIRE_FALSE(Server::ResolveConfiguredStartupScanMode(startup, status).has_value());
    }

    SECTION("empty-cache policy disables initial scan")
    {
        startup.AutoScanOnEmptyCache = false;
        status.CreatedCacheOnOpen = true;
        REQUIRE_FALSE(Server::ResolveConfiguredStartupScanMode(startup, status).has_value());
    }

    SECTION("non-empty new cache does not start an initial scan")
    {
        status.CreatedCacheOnOpen = true;
        status.InpxSource->TotalBookCount = 1;
        REQUIRE_FALSE(Server::ResolveConfiguredStartupScanMode(startup, status).has_value());
    }

    SECTION("changed source starts configured automatic rescan")
    {
        startup.AutoRescanOnSourceChange = true;
        status.InpxSource->RequiresRescan = true;
        REQUIRE(Server::ResolveConfiguredStartupScanMode(startup, status)
            == ApplicationJobs::EInpxScanMode::Rescan);
    }

    SECTION("automatic rescan is opt-in")
    {
        status.InpxSource->RequiresRescan = true;
        REQUIRE_FALSE(Server::ResolveConfiguredStartupScanMode(startup, status).has_value());
    }

    SECTION("unchanged source does not start a rescan")
    {
        startup.AutoRescanOnSourceChange = true;
        REQUIRE_FALSE(Server::ResolveConfiguredStartupScanMode(startup, status).has_value());
    }

    SECTION("master auto-scan switch disables rescan")
    {
        startup.AutoScan = false;
        startup.AutoRescanOnSourceChange = true;
        status.InpxSource->RequiresRescan = true;
        REQUIRE_FALSE(Server::ResolveConfiguredStartupScanMode(startup, status).has_value());
    }
}

TEST_CASE("ServerApplicationHost creates INPX catalog and reports status", "[server][host]")
{
    CTestWorkspace workspace("inpx-web-reader-server-host-create");
    const auto inpxPath = InpxWebReader::Tests::ServerFixtures::WriteInpxArchive(
        workspace.GetPath() / "source" / "catalog.inpx");
    const auto archiveRoot = workspace.GetPath() / "archives";
    const auto cacheRoot = workspace.GetPath() / "cache";
    const auto runtimeRoot = workspace.GetPath() / "runtime";
    std::filesystem::create_directories(archiveRoot);

    const auto config = InpxWebReader::Server::CServerConfigLoader::LoadFromJsonText(
        InpxWebReader::Tests::ServerFixtures::MakeServerConfigJson(
            cacheRoot,
            runtimeRoot,
            inpxPath,
            archiveRoot));

    InpxWebReader::Server::CServerApplicationHost host(config);
    host.Open();

    const auto status = host.GetStatus();
    REQUIRE(host.IsOpen());
    REQUIRE(status.IsOpen);
    REQUIRE(status.CreatedCacheOnOpen);
    REQUIRE(status.Capabilities.CanRescanInpxSource);
    REQUIRE(status.InpxSource.has_value());
    REQUIRE(status.InpxSource->TotalBookCount == 0);
    REQUIRE(std::filesystem::is_regular_file(cacheRoot / "Database" / "inpx-web-reader.db"));

    host.Close();
    REQUIRE_FALSE(host.IsOpen());
}

TEST_CASE("ServerApplicationHost recovers an empty interrupted cache database", "[server][host][sqlite]")
{
    CTestWorkspace workspace("inpx-web-reader-server-host-recover-empty-cache");
    const auto config = MakeHostConfig(workspace);
    const auto databaseDirectory = config.CacheRoot / "Database";
    std::filesystem::create_directories(databaseDirectory);
    std::filesystem::create_directories(config.CacheRoot / "Covers");
    {
        InpxWebReader::Sqlite::CSqliteConnection interruptedDatabase(
            databaseDirectory / "inpx-web-reader.db");
    }

    Server::CServerApplicationHost host(config);
    REQUIRE_NOTHROW(host.Open());
    REQUIRE(host.IsOpen());
    REQUIRE(host.GetStatus().InpxSource.has_value());
    host.Close();
}

TEST_CASE("ServerApplicationHost rejects a current cache without source metadata", "[server][host][sqlite]")
{
    CTestWorkspace workspace("inpx-web-reader-server-host-missing-source");
    const auto config = MakeHostConfig(workspace);
    {
        Server::CServerApplicationHost createHost(config);
        createHost.Open();
        createHost.Close();
    }
    {
        InpxWebReader::Sqlite::CSqliteConnection connection(
            config.CacheRoot / "Database" / "inpx-web-reader.db");
        connection.Execute("DELETE FROM inpx_sources WHERE id = 1;");
    }

    Server::CServerApplicationHost openHost(config);
    REQUIRE_THROWS_WITH(
        openHost.Open(),
        Catch::Matchers::ContainsSubstring("INPX source singleton is missing or invalid"));
    REQUIRE_FALSE(openHost.IsOpen());
}

TEST_CASE("ServerApplicationHost rejects an invalid configured converter path", "[server][host][converter]")
{
    CTestWorkspace workspace("inpx-web-reader-server-host-invalid-converter");
    auto config = MakeHostConfig(workspace);
    config.Converter.Path = workspace.GetPath() / "tools" / "fbc";

    bool applicationFactoryCalled = false;
    Server::CServerApplicationHost host(
        config,
        [&](const Application::SInpxCatalogApplicationConfig& appConfig) {
            applicationFactoryCalled = true;
            return std::make_unique<CFakeDownloadApplication>(*appConfig.InpxSource);
        });

    REQUIRE_THROWS_WITH(
        host.Open(),
        Catch::Matchers::ContainsSubstring("converter.path")
            && Catch::Matchers::ContainsSubstring("valid built-in fbc"));
    REQUIRE_FALSE(applicationFactoryCalled);
    REQUIRE_FALSE(host.IsOpen());
}

TEST_CASE("ServerApplicationHost forwards configured scan worker limit", "[server][host]")
{
    CTestWorkspace workspace("inpx-web-reader-server-host-scan-worker-limit");
    auto config = MakeHostConfig(workspace);
    config.Limits.MaxScanWorkers = 2;
    config.Limits.MaxConcurrentDownloads = 3;

    std::size_t observedMaxScanWorkers = 0;
    std::size_t observedMaxConcurrentDownloads = 0;
    Server::CServerApplicationHost host(
        config,
        [&](const Application::SInpxCatalogApplicationConfig& appConfig) {
            observedMaxScanWorkers = appConfig.MaxInpxScanWorkers;
            observedMaxConcurrentDownloads = appConfig.MaxConcurrentDownloads;
            return std::make_unique<CFakeDownloadApplication>(*appConfig.InpxSource);
        });

    host.Open();

    REQUIRE(observedMaxScanWorkers == 2);
    REQUIRE(observedMaxConcurrentDownloads == 3);

    host.Close();
}

TEST_CASE("ServerApplicationHost rebinds an existing cache to configured source paths", "[server][host]")
{
    CTestWorkspace workspace("inpx-web-reader-server-host-open-existing-source");
    const auto originalInpxPath = InpxWebReader::Tests::ServerFixtures::WriteInpxArchive(
        workspace.GetPath() / "source" / "catalog.inpx");
    const auto originalArchiveRoot = workspace.GetPath() / "archives";
    const auto cacheRoot = workspace.GetPath() / "cache";
    const auto runtimeRoot = workspace.GetPath() / "runtime";
    std::filesystem::create_directories(originalArchiveRoot);

    {
        const auto createConfig = InpxWebReader::Server::CServerConfigLoader::LoadFromJsonText(
            InpxWebReader::Tests::ServerFixtures::MakeServerConfigJson(
                cacheRoot,
                runtimeRoot,
                originalInpxPath,
                originalArchiveRoot));
        InpxWebReader::Server::CServerApplicationHost host(createConfig);
        host.Open();
        host.Close();
    }

    const auto overrideInpxPath = workspace.GetPath() / "override" / "renamed.inpx";
    std::filesystem::create_directories(overrideInpxPath.parent_path());
    std::filesystem::copy_file(originalInpxPath, overrideInpxPath);
    const auto overrideArchiveRoot = workspace.GetPath() / "override-archives";
    std::filesystem::create_directories(overrideArchiveRoot);

    const auto openConfig = InpxWebReader::Server::CServerConfigLoader::LoadFromJsonText(
        InpxWebReader::Tests::ServerFixtures::MakeServerConfigJson(
            cacheRoot,
            workspace.GetPath() / "runtime-open",
            overrideInpxPath,
            overrideArchiveRoot));

    InpxWebReader::Server::CServerApplicationHost host(openConfig);
    host.Open();

    const auto status = host.GetStatus();
    REQUIRE_FALSE(status.CreatedCacheOnOpen);
    REQUIRE(status.InpxSource.has_value());
    REQUIRE(status.Capabilities.CanRescanInpxSource);
    REQUIRE(status.Capabilities.CanDownloadOriginal);
    REQUIRE(status.InpxSource->InpxPathUtf8 == InpxWebReader::Unicode::PathToUtf8(overrideInpxPath));
    REQUIRE(status.InpxSource->ArchiveRootUtf8 == InpxWebReader::Unicode::PathToUtf8(overrideArchiveRoot));

    host.Close();
}

TEST_CASE("ServerApplicationHost opens existing INPX catalog without source config", "[server][host]")
{
    CTestWorkspace workspace("inpx-web-reader-server-host-open-existing-no-source-config");
    const auto inpxPath = InpxWebReader::Tests::ServerFixtures::WriteInpxArchive(
        workspace.GetPath() / "source" / "catalog.inpx");
    const auto archiveRoot = workspace.GetPath() / "archives";
    const auto cacheRoot = workspace.GetPath() / "cache";
    const auto runtimeRoot = workspace.GetPath() / "runtime";
    std::filesystem::create_directories(archiveRoot);

    {
        const auto createConfig = InpxWebReader::Server::CServerConfigLoader::LoadFromJsonText(
            InpxWebReader::Tests::ServerFixtures::MakeServerConfigJson(
                cacheRoot,
                runtimeRoot,
                inpxPath,
                archiveRoot));
        InpxWebReader::Server::CServerApplicationHost host(createConfig);
        host.Open();
        host.Close();
    }

    const std::string openJson = "{"
        "\"cacheRoot\":\"" + InpxWebReader::Unicode::PathToUtf8(cacheRoot) + "\","
        "\"runtimeWorkspaceRoot\":\"" + InpxWebReader::Unicode::PathToUtf8(workspace.GetPath() / "runtime-open") + "\""
        "}";
    const auto openConfig = InpxWebReader::Server::CServerConfigLoader::LoadFromJsonText(openJson);

    InpxWebReader::Server::CServerApplicationHost host(openConfig);
    host.Open();

    const auto status = host.GetStatus();
    REQUIRE_FALSE(status.CreatedCacheOnOpen);
    REQUIRE(status.InpxSource.has_value());
    REQUIRE_FALSE(status.Capabilities.CanRescanInpxSource);
    REQUIRE_FALSE(status.Capabilities.CanDownloadOriginal);
    REQUIRE_FALSE(status.InpxSource->IsSourceAvailable);
    REQUIRE(status.InpxSource->SourceWarningUtf8.find("not configured") != std::string::npos);
    REQUIRE(status.InpxSource->InpxPathUtf8.empty());
    REQUIRE(status.InpxSource->ArchiveRootUtf8.empty());

    host.Close();
}

TEST_CASE("ServerApplicationHost requires rescan when configured INPX bytes change", "[server][host][inpx]")
{
    CTestWorkspace workspace("inpx-web-reader-server-host-source-fingerprint-change");
    const auto inpxPath = InpxWebReader::Tests::ServerFixtures::WriteInpxArchive(
        workspace.GetPath() / "source" / "catalog.inpx");
    const auto archiveRoot = workspace.GetPath() / "archives";
    const auto cacheRoot = workspace.GetPath() / "cache";
    std::filesystem::create_directories(archiveRoot);

    {
        const auto createConfig = InpxWebReader::Server::CServerConfigLoader::LoadFromJsonText(
            InpxWebReader::Tests::ServerFixtures::MakeServerConfigJson(
                cacheRoot,
                workspace.GetPath() / "runtime-create",
                inpxPath,
                archiveRoot));
        InpxWebReader::Server::CServerApplicationHost host(createConfig);
        host.Open();
        host.Close();
    }

    {
        std::ofstream changedInpx(inpxPath, std::ios::binary | std::ios::app);
        changedInpx.put('\0');
    }

    const auto openConfig = InpxWebReader::Server::CServerConfigLoader::LoadFromJsonText(
        InpxWebReader::Tests::ServerFixtures::MakeServerConfigJson(
            cacheRoot,
            workspace.GetPath() / "runtime-open",
            inpxPath,
            archiveRoot));
    InpxWebReader::Server::CServerApplicationHost host(openConfig);
    host.Open();

    const auto status = host.GetStatus();
    REQUIRE(status.Capabilities.CanRescanInpxSource);
    REQUIRE_FALSE(status.Capabilities.CanDownloadOriginal);
    REQUIRE(status.InpxSource.has_value());
    REQUIRE_FALSE(status.InpxSource->IsSourceAvailable);
    REQUIRE(status.InpxSource->RequiresRescan);
    REQUIRE(status.InpxSource->SourceWarningUtf8.find("rescan is required") != std::string::npos);

    host.Close();
}

TEST_CASE("ServerApplicationHost opens existing INPX cache when the source is unavailable", "[server][host]")
{
    CTestWorkspace workspace("inpx-web-reader-server-host-open-existing-outage");
    const auto inpxPath = InpxWebReader::Tests::ServerFixtures::WriteInpxArchive(
        workspace.GetPath() / "source" / "catalog.inpx");
    const auto archiveRoot = workspace.GetPath() / "archives";
    const auto cacheRoot = workspace.GetPath() / "cache";
    const auto runtimeRoot = workspace.GetPath() / "runtime";
    std::filesystem::create_directories(archiveRoot);

    {
        const auto createConfig = InpxWebReader::Server::CServerConfigLoader::LoadFromJsonText(
            InpxWebReader::Tests::ServerFixtures::MakeServerConfigJson(
                cacheRoot,
                runtimeRoot,
                inpxPath,
                archiveRoot));
        InpxWebReader::Server::CServerApplicationHost host(createConfig);
        host.Open();
        host.Close();
    }

    std::filesystem::remove_all(workspace.GetPath() / "source");
    std::filesystem::remove_all(archiveRoot);

    const auto openConfig = InpxWebReader::Server::CServerConfigLoader::LoadFromJsonText(
        InpxWebReader::Tests::ServerFixtures::MakeServerConfigJson(
            cacheRoot,
            workspace.GetPath() / "runtime-open",
            inpxPath,
            archiveRoot));

    InpxWebReader::Server::CServerApplicationHost host(openConfig);
    host.Open();

    const auto status = host.GetStatus();
    REQUIRE_FALSE(status.CreatedCacheOnOpen);
    REQUIRE(status.InpxSource.has_value());
    REQUIRE_FALSE(status.Capabilities.CanRescanInpxSource);
    REQUIRE_FALSE(status.Capabilities.CanDownloadOriginal);
    REQUIRE_FALSE(status.InpxSource->IsSourceAvailable);
    REQUIRE_FALSE(status.InpxSource->SourceWarningUtf8.empty());
    REQUIRE(status.InpxSource->InpxPathUtf8 == InpxWebReader::Unicode::PathToUtf8(inpxPath));

    host.Close();
}

TEST_CASE("ServerApplicationHost builds UTF-8 safe bounded download names", "[server][host][download]")
{
    CTestWorkspace workspace("inpx-web-reader-server-host-download-name");
    const auto config = MakeHostConfig(workspace);
    CFakeDownloadApplication* application = nullptr;
    Server::CServerApplicationHost host(
        config,
        [&](const Application::SInpxCatalogApplicationConfig& appConfig) {
            auto fake = std::make_unique<CFakeDownloadApplication>(*appConfig.InpxSource);
            application = fake.get();
            return fake;
        });

    host.Open();
    const auto response = host.PrepareBookDownload(Domain::SBookId{7}, Server::EServerDownloadFormat::Original);

    REQUIRE(response.has_value());
    REQUIRE(application != nullptr);
    REQUIRE(application->GetBookDetailsCalls == 0);
    REQUIRE(InpxWebReader::Unicode::IsValidUtf8(response->FileNameUtf8));
    REQUIRE(response->FileNameUtf8.size() <= 165);
    REQUIRE(application->LastDestinationPath.filename() == "prepared.fb2");
    REQUIRE(response->FileNameUtf8 != InpxWebReader::Unicode::PathToUtf8(
        application->LastDestinationPath.filename()));
    REQUIRE(std::filesystem::is_regular_file(response->Path));

    host.Close();
}

TEST_CASE("ServerApplicationHost cleans download staging when preparation throws", "[server][host][download]")
{
    CTestWorkspace workspace("inpx-web-reader-server-host-download-cleanup");
    const auto config = MakeHostConfig(workspace);
    CFakeDownloadApplication* application = nullptr;
    Server::CServerApplicationHost host(
        config,
        [&](const Application::SInpxCatalogApplicationConfig& appConfig) {
            auto fake = std::make_unique<CFakeDownloadApplication>(*appConfig.InpxSource);
            fake->ThrowOnDownload = true;
            application = fake.get();
            return fake;
        });

    host.Open();
    REQUIRE_THROWS_AS(
        host.PrepareBookDownload(Domain::SBookId{7}, Server::EServerDownloadFormat::Original),
        std::runtime_error);
    REQUIRE(application != nullptr);
    REQUIRE_FALSE(application->LastDestinationPath.empty());

    const auto downloadRoot = config.RuntimeWorkspaceRoot / "ServerDownloads";
    const bool hasResidue = std::filesystem::exists(downloadRoot)
        && std::filesystem::directory_iterator(downloadRoot) != std::filesystem::directory_iterator{};
    REQUIRE_FALSE(hasResidue);

    host.Close();
}

TEST_CASE("ServerApplicationHost keeps control responsive and cancels downloads during shutdown", "[server][host][download]")
{
    CTestWorkspace workspace("inpx-web-reader-server-host-download-shutdown");
    const auto config = MakeHostConfig(workspace);
    CFakeDownloadApplication* application = nullptr;
    Server::CServerApplicationHost host(
        config,
        [&](const Application::SInpxCatalogApplicationConfig& appConfig) {
            auto fake = std::make_unique<CFakeDownloadApplication>(*appConfig.InpxSource);
            fake->BlockDownload = true;
            application = fake.get();
            return fake;
        });

    host.Open();
    auto download = std::async(std::launch::async, [&host]() {
        return host.PrepareBookDownload(Domain::SBookId{7}, Server::EServerDownloadFormat::Epub);
    });
    REQUIRE(application != nullptr);
    REQUIRE(application->WaitForDownloadEntry(std::chrono::seconds{5}));

    auto status = std::async(std::launch::async, [&host]() { return host.GetStatus(); });
    REQUIRE(status.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
    REQUIRE(status.get().IsOpen);

    auto close = std::async(std::launch::async, [&host]() { host.Close(); });
    REQUIRE(close.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
    close.get();
    try
    {
        static_cast<void>(download.get());
        FAIL("Expected shutdown to cancel the active download.");
    }
    catch (const Domain::CDomainException& exception)
    {
        REQUIRE(exception.Code() == Domain::EDomainErrorCode::Cancellation);
    }

    const auto downloadRoot = config.RuntimeWorkspaceRoot / "ServerDownloads";
    const bool hasResidue = std::filesystem::exists(downloadRoot)
        && std::filesystem::directory_iterator(downloadRoot) != std::filesystem::directory_iterator{};
    REQUIRE_FALSE(hasResidue);
}
