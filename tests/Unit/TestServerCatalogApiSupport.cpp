#include "TestServerCatalogApiSupport.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>
#include <zip.h>

#include "Domain/BookFormat.hpp"
#include "Domain/SearchQuery.hpp"
#include "Foundation/UnicodeConversion.hpp"
#include "Server/HttpServer.hpp"
#include "Server/ServerApplicationHost.hpp"
#include "Server/ServerConfig.hpp"
#include "TestServerFixtures.hpp"
#include "TestWorkspace.hpp"

namespace InpxWebReader::Tests::ServerCatalogApiSupport {

using namespace std::chrono_literals;

namespace ServerFixtures = InpxWebReader::Tests::ServerFixtures;

namespace {

[[nodiscard]] std::chrono::system_clock::time_point FixedTime()
{
    return std::chrono::system_clock::time_point{std::chrono::seconds{1'779'276'600}};
}

[[nodiscard]] Application::SBookListItem MakeCatalogItem()
{
    return {
        .Id = Domain::SBookId{7},
        .TitleUtf8 = "Тестовая книга",
        .AuthorsUtf8 = {"Иван Тестов"},
        .Language = "ru",
        .SeriesUtf8 = std::string{"Серия"},
        .SeriesIndex = 1.0,
        .Year = 2026,
        .TagsUtf8 = {"featured"},
        .GenresUtf8 = {"Science Fiction"},
        .Format = Domain::EBookFormat::Fb2,
        .CoverPath = std::filesystem::path{"Objects/cover.jpg"},
        .SizeBytes = 321,
        .AddedAtUtc = FixedTime(),
        .CanDownloadOriginal = true,
        .CanDownloadAsEpub = true,
        .IsAvailable = true,
        .AvailabilityLabelUtf8 = "Available"
    };
}

[[nodiscard]] Application::SBookDetails MakeCatalogDetails()
{
    return {
        .Id = Domain::SBookId{7},
        .TitleUtf8 = "Тестовая книга",
        .AuthorsUtf8 = {"Иван Тестов"},
        .Language = "ru",
        .SeriesUtf8 = std::string{"Серия"},
        .SeriesIndex = 1.0,
        .PublisherUtf8 = std::string{"InpxWebReader Test Press"},
        .Year = 2026,
        .Isbn = std::string{"9780000000002"},
        .TagsUtf8 = {"featured"},
        .GenresUtf8 = {"Science Fiction"},
        .DescriptionUtf8 = std::string{"Описание для браузерного API."},
        .Identifier = std::string{"fixture-7"},
        .Format = Domain::EBookFormat::Fb2,
        .CoverPath = std::filesystem::path{"Objects/cover.jpg"},
        .SizeBytes = 321,
        .AddedAtUtc = FixedTime(),
        .CanDownloadOriginal = true,
        .CanDownloadAsEpub = true,
        .IsAvailable = true,
        .AvailabilityLabelUtf8 = "Available"
    };
}

} // namespace

CFakeCatalogServerHost::CFakeCatalogServerHost()
{
    ListResult = Application::SBookListResult{
        .Items = {MakeCatalogItem()},
        .TotalCount = 1,
        .AvailableLanguages = {Domain::SFacetItem{.Value = "ru", .Count = 1}},
        .AvailableGenres = {Domain::SFacetItem{.Value = "Science Fiction", .Count = 1}},
        .CatalogSnapshotIdUtf8 = "scan-fixture-17"
    };
    Details = MakeCatalogDetails();
    Statistics = Application::SCatalogStatistics{
        .BookCount = 1,
        .UnavailableBookCount = 0,
        .InpxSourceSizeBytes = 321,
        .TotalCatalogSizeBytes = 1024
    };
}

bool CFakeCatalogServerHost::IsOpen() const noexcept
{
    return Open.load();
}

Server::SServerStatus CFakeCatalogServerHost::GetStatus()
{
    return Status;
}

Application::SBookListResult CFakeCatalogServerHost::ListBooks(const Application::SBookListRequest& request)
{
    std::scoped_lock lock(Mutex);
    ++ListCalls;
    LastListRequest = request;
    if (ListErrorCode.has_value())
    {
        throw Domain::CDomainException(*ListErrorCode, "Catalog cursor test error.");
    }
    auto result = ListResult;
    if (request.Cursor.has_value())
    {
        result.TotalCount = std::nullopt;
        result.AvailableLanguages.clear();
        result.AvailableGenres.clear();
        result.NextCursor = std::nullopt;
    }
    if (!request.IncludeFacets)
    {
        result.AvailableLanguages.clear();
        result.AvailableGenres.clear();
    }
    return result;
}

std::optional<Application::SBookDetails> CFakeCatalogServerHost::GetBookDetails(const Domain::SBookId bookId)
{
    std::scoped_lock lock(Mutex);
    ++DetailsCalls;
    LastDetailsBookId = bookId;
    if (!Details.has_value() || Details->Id.Value != bookId.Value)
    {
        return std::nullopt;
    }
    return Details;
}

Application::SCatalogStatistics CFakeCatalogServerHost::GetCatalogStatistics()
{
    std::scoped_lock lock(Mutex);
    ++StatisticsCalls;
    return Statistics;
}

std::optional<Server::SServerFileResponse> CFakeCatalogServerHost::ResolveBookCover(
    const Domain::SBookId bookId)
{
    std::scoped_lock lock(Mutex);
    ++CoverCalls;
    LastCoverBookId = bookId;
    return CoverResponse;
}

std::optional<Server::SServerFileResponse> CFakeCatalogServerHost::PrepareBookDownload(
    const Domain::SBookId bookId,
    const Server::EServerDownloadFormat format)
{
    std::unique_lock lock(Mutex);
    ++DownloadCalls;
    LastDownloadBookId = bookId;
    LastDownloadFormat = format;

    if (BlockDownloads)
    {
        ++BlockedDownloadEntries;
        DownloadEntered.notify_all();
        DownloadReleased.wait(lock, [this]() {
            return ReleaseDownloads;
        });
    }

    return DownloadResponse;
}

std::optional<Application::SInpxSourceOverview> CFakeCatalogServerHost::GetInpxSourceOverview()
{
    return std::nullopt;
}

ApplicationJobs::TInpxScanJobId CFakeCatalogServerHost::StartInpxScan(
    const ApplicationJobs::SInpxScanRequest&)
{
    return 0;
}

std::optional<ApplicationJobs::SInpxScanJobSnapshot> CFakeCatalogServerHost::GetInpxScanJobSnapshot(
    ApplicationJobs::TInpxScanJobId)
{
    return std::nullopt;
}

std::optional<ApplicationJobs::SInpxScanJobResult> CFakeCatalogServerHost::GetInpxScanJobResult(
    ApplicationJobs::TInpxScanJobId)
{
    return std::nullopt;
}

bool CFakeCatalogServerHost::CancelInpxScanJob(ApplicationJobs::TInpxScanJobId)
{
    return false;
}

bool CFakeCatalogServerHost::RemoveInpxScanJob(ApplicationJobs::TInpxScanJobId)
{
    return false;
}

bool CFakeCatalogServerHost::WaitForBlockedDownload(const std::chrono::milliseconds timeout)
{
    std::unique_lock lock(Mutex);
    return DownloadEntered.wait_for(lock, timeout, [this]() {
        return BlockedDownloadEntries > 0;
    });
}

void CFakeCatalogServerHost::ReleaseBlockedDownloads()
{
    {
        std::scoped_lock lock(Mutex);
        ReleaseDownloads = true;
    }
    DownloadReleased.notify_all();
}

CBlockedDownloadReleaseGuard::CBlockedDownloadReleaseGuard(CFakeCatalogServerHost& host) noexcept
    : m_host(host)
{
}

CBlockedDownloadReleaseGuard::~CBlockedDownloadReleaseGuard()
{
    ReleaseNow();
}

void CBlockedDownloadReleaseGuard::ReleaseNow() noexcept
{
    if (!m_active)
    {
        return;
    }

    m_host.ReleaseBlockedDownloads();
    m_active = false;
}

[[nodiscard]] httplib::Result GetWithRetry(
    httplib::Client& client,
    const std::string& path,
    const httplib::Headers& headers)
{
    for (int attempt = 0; attempt < 50; ++attempt)
    {
        if (auto response = client.Get(path, headers))
        {
            return response;
        }

        std::this_thread::sleep_for(10ms);
    }

    return {};
}

[[nodiscard]] httplib::Result PostWithRetry(
    httplib::Client& client,
    const std::string& path,
    const std::string& bodyUtf8,
    const httplib::Headers& headers)
{
    for (int attempt = 0; attempt < 50; ++attempt)
    {
        if (auto response = client.Post(path, headers, bodyUtf8, "application/json"))
        {
            return response;
        }

        std::this_thread::sleep_for(10ms);
    }

    return {};
}

[[nodiscard]] std::unique_ptr<Server::CHttpServer> StartTestServer(
    Server::IServerApplicationHost& host,
    Server::SHttpServerOptions options)
{
    options.HostUtf8 = "127.0.0.1";
    options.Port = 0;
    auto server = std::make_unique<Server::CHttpServer>(host, std::move(options));
    server->Start();
    return server;
}

void WriteBinaryFile(const std::filesystem::path& path, const std::string& content)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output << content;
}

[[nodiscard]] bool WaitUntilRemoved(const std::filesystem::path& path)
{
    for (int attempt = 0; attempt < 100; ++attempt)
    {
        if (!std::filesystem::exists(path))
        {
            return true;
        }
        std::this_thread::sleep_for(10ms);
    }
    return false;
}

namespace {

[[nodiscard]] std::string MakeInpxRecord()
{
    return std::string("\xC8\xE2\xE0\xED,\xD2\xE5\xF1\xF2\xEE\xE2") + ServerFixtures::GFieldSeparator
        + "genre" + ServerFixtures::GFieldSeparator
        + "\xD2\xE5\xF1\xF2\xEE\xE2\xE0\xFF \xEA\xED\xE8\xE3\xE0" + ServerFixtures::GFieldSeparator
        + ServerFixtures::GFieldSeparator
        + ServerFixtures::GFieldSeparator
        + "book" + ServerFixtures::GFieldSeparator
        + "0" + ServerFixtures::GFieldSeparator
        + "1" + ServerFixtures::GFieldSeparator
        + "0" + ServerFixtures::GFieldSeparator
        + "fb2" + ServerFixtures::GFieldSeparator
        + ServerFixtures::GFieldSeparator
        + "ru" + ServerFixtures::GFieldSeparator
        + "5" + ServerFixtures::GFieldSeparator
        + ServerFixtures::GFieldSeparator
        + "\n";
}

} // namespace

std::filesystem::path WriteInpxArchive(const std::filesystem::path& archivePath)
{
    std::filesystem::create_directories(archivePath.parent_path());

    int errorCode = ZIP_ER_OK;
    const auto archivePathUtf8 = InpxWebReader::Unicode::PathToUtf8(archivePath);
    zip_t* archive = zip_open(archivePathUtf8.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &errorCode);
    if (archive == nullptr)
    {
        throw std::runtime_error("Failed to create INPX fixture archive.");
    }

    ServerFixtures::AddZipEntry(archive, "fb2-main.zip.inp", MakeInpxRecord());

    if (zip_close(archive) != 0)
    {
        zip_discard(archive);
        throw std::runtime_error("Failed to finalize INPX fixture archive.");
    }

    return archivePath;
}

namespace {

[[nodiscard]] std::string MakeFb2Payload()
{
    return R"(<?xml version="1.0" encoding="UTF-8"?>
<FictionBook xmlns:l="http://www.w3.org/1999/xlink">
  <description>
    <title-info>
      <book-title>Тестовая книга</book-title>
      <author>
        <first-name>Иван</first-name>
        <last-name>Тестов</last-name>
      </author>
      <lang>ru</lang>
      <coverpage><image l:href="#cover.png"/></coverpage>
    </title-info>
  </description>
  <body><section><p>Payload</p></section></body>
  <binary id="cover.png" content-type="image/png">iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+/p9sAAAAASUVORK5CYII=</binary>
</FictionBook>)";
}

} // namespace

std::filesystem::path WriteInpxPayloadArchive(const std::filesystem::path& archivePath)
{
    std::filesystem::create_directories(archivePath.parent_path());

    int errorCode = ZIP_ER_OK;
    const auto archivePathUtf8 = InpxWebReader::Unicode::PathToUtf8(archivePath);
    zip_t* archive = zip_open(archivePathUtf8.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &errorCode);
    if (archive == nullptr)
    {
        throw std::runtime_error("Failed to create INPX payload archive.");
    }

    ServerFixtures::AddZipEntry(archive, "book.fb2", MakeFb2Payload());

    if (zip_close(archive) != 0)
    {
        zip_discard(archive);
        throw std::runtime_error("Failed to finalize INPX payload archive.");
    }

    return archivePath;
}

[[nodiscard]] nlohmann::json WaitForTerminalScan(httplib::Client& client)
{
    for (int attempt = 0; attempt < 100; ++attempt)
    {
        const auto response = GetWithRetry(client, "/api/scan/progress");
        REQUIRE(response);
        REQUIRE(response->status == 200);
        auto body = nlohmann::json::parse(response->body);
        if (!body.at("active").get<bool>())
        {
            return body;
        }

        std::this_thread::sleep_for(20ms);
    }

    FAIL("Timed out waiting for terminal INPX scan progress.");
    return {};
}

} // namespace InpxWebReader::Tests::ServerCatalogApiSupport
