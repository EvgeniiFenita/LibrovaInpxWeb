#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <future>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "Server/ServerApplicationHost.hpp"
#include "Server/ServerConfig.hpp"
#include "TestServerCatalogApiSupport.hpp"
#include "TestServerFixtures.hpp"
#include "TestWorkspace.hpp"

using namespace std::chrono_literals;
using namespace InpxWebReader::Tests::ServerCatalogApiSupport;

namespace Server = InpxWebReader::Server;
namespace ServerFixtures = InpxWebReader::Tests::ServerFixtures;

TEST_CASE("HTTP cover endpoint serves a file by book id", "[server][http][catalog][files]")
{
    CTestWorkspace workspace("inpx-web-reader-server-catalog-cover");
    const auto coverPath = workspace.GetPath() / "cover.jpg";
    WriteBinaryFile(coverPath, "cover-bytes");

    CFakeCatalogServerHost host;
    host.CoverResponse = Server::SServerFileResponse{
        .Path = coverPath,
        .FileNameUtf8 = "cover.jpg",
        .ContentTypeUtf8 = "image/jpeg"
    };
    auto server = StartTestServer(host);
    httplib::Client client("127.0.0.1", server->GetBoundPort());

    const auto response = GetWithRetry(client, "/api/covers/7");

    REQUIRE(response);
    REQUIRE(response->status == 200);
    REQUIRE(response->body == "cover-bytes");
    REQUIRE(response->get_header_value("Content-Type").find("image/jpeg") != std::string::npos);
    REQUIRE(response->get_header_value("Content-Disposition").find("filename*=UTF-8''cover.jpg") != std::string::npos);
}

TEST_CASE("HTTP file responses implement byte ranges and HEAD consistently", "[server][http][catalog][files][range]")
{
    CTestWorkspace workspace("inpx-web-reader-server-file-ranges");
    const auto coverPath = workspace.GetPath() / "cover.jpg";
    WriteBinaryFile(coverPath, "0123456789");

    CFakeCatalogServerHost host;
    host.CoverResponse = Server::SServerFileResponse{
        .Path = coverPath,
        .FileNameUtf8 = "cover.jpg",
        .ContentTypeUtf8 = "image/jpeg"
    };
    auto server = StartTestServer(host);
    httplib::Client client("127.0.0.1", server->GetBoundPort());

    const auto single = client.Get("/api/covers/7", {{"Range", "bytes=0-3"}});
    REQUIRE(single);
    REQUIRE(single->status == 206);
    REQUIRE(single->body == "0123");
    REQUIRE(single->get_header_value("Content-Length") == "4");
    REQUIRE(single->get_header_value("Content-Range") == "bytes 0-3/10");
    REQUIRE(single->get_header_value("Accept-Ranges") == "bytes");

    const auto suffix = client.Get("/api/covers/7", {{"Range", "bytes=-3"}});
    REQUIRE(suffix);
    REQUIRE(suffix->status == 206);
    REQUIRE(suffix->body == "789");
    REQUIRE(suffix->get_header_value("Content-Range") == "bytes 7-9/10");

    const auto oversizedSuffix = client.Get("/api/covers/7", {{"Range", "bytes=-30"}});
    REQUIRE(oversizedSuffix);
    REQUIRE(oversizedSuffix->status == 206);
    REQUIRE(oversizedSuffix->body == "0123456789");
    REQUIRE(oversizedSuffix->get_header_value("Content-Range") == "bytes 0-9/10");

    const auto open = client.Get("/api/covers/7", {{"Range", "bytes=4-"}});
    REQUIRE(open);
    REQUIRE(open->status == 206);
    REQUIRE(open->body == "456789");
    REQUIRE(open->get_header_value("Content-Range") == "bytes 4-9/10");

    const auto multiple = client.Get("/api/covers/7", {{"Range", "bytes=0-1,8-9"}});
    REQUIRE(multiple);
    REQUIRE(multiple->status == 206);
    REQUIRE(multiple->get_header_value("Content-Type").find("multipart/byteranges") != std::string::npos);
    REQUIRE(multiple->body.find("Content-Range: bytes 0-1/10") != std::string::npos);
    REQUIRE(multiple->body.find("Content-Range: bytes 8-9/10") != std::string::npos);
    REQUIRE(multiple->body.find("01") != std::string::npos);
    REQUIRE(multiple->body.find("89") != std::string::npos);

    const auto mixed = client.Get("/api/covers/7", {{"Range", "bytes=20-30,0-1"}});
    REQUIRE(mixed);
    REQUIRE(mixed->status == 206);
    REQUIRE(mixed->body == "01");
    REQUIRE(mixed->get_header_value("Content-Range") == "bytes 0-1/10");

    const auto unsatisfiable = client.Get("/api/covers/7", {{"Range", "bytes=20-30"}});
    REQUIRE(unsatisfiable);
    REQUIRE(unsatisfiable->status == 416);
    REQUIRE(unsatisfiable->body.empty());
    REQUIRE(unsatisfiable->get_header_value("Content-Range") == "bytes */10");

    const auto head = client.Head("/api/covers/7", {{"Range", "bytes=2-5"}});
    REQUIRE(head);
    REQUIRE(head->status == 206);
    REQUIRE(head->body.empty());
    REQUIRE(head->get_header_value("Content-Length") == "4");
    REQUIRE(head->get_header_value("Content-Range") == "bytes 2-5/10");
}

TEST_CASE("HTTP download endpoint streams original and EPUB files then cleans staging", "[server][http][catalog][files]")
{
    CTestWorkspace workspace("inpx-web-reader-server-catalog-download");
    const auto originalRoot = workspace.GetPath() / "staging-original";
    const auto originalPath = originalRoot / "Тестовая книга.fb2";
    WriteBinaryFile(originalPath, "original-download");

    CFakeCatalogServerHost host;
    host.DownloadResponse = Server::SServerFileResponse{
        .Path = originalPath,
        .CleanupRoot = originalRoot,
        .FileNameUtf8 = "Тестовая книга.fb2",
        .ContentTypeUtf8 = "application/x-fictionbook+xml; charset=utf-8"
    };
    auto server = StartTestServer(host);
    {
        httplib::Client client("127.0.0.1", server->GetBoundPort());
        const auto original = GetWithRetry(client, "/api/books/7/download?format=original");

        REQUIRE(original);
        REQUIRE(original->status == 200);
        REQUIRE(original->body == "original-download");
        REQUIRE(original->get_header_value("Content-Disposition").find("filename*=UTF-8''") != std::string::npos);
    }
    REQUIRE(WaitUntilRemoved(originalRoot));

    {
        std::scoped_lock lock(host.Mutex);
        REQUIRE(host.LastDownloadFormat == Server::EServerDownloadFormat::Original);
    }

    const auto epubRoot = workspace.GetPath() / "staging-epub";
    const auto epubPath = epubRoot / "Тестовая книга.epub";
    WriteBinaryFile(epubPath, "epub-download");
    {
        std::scoped_lock lock(host.Mutex);
        host.DownloadResponse = Server::SServerFileResponse{
            .Path = epubPath,
            .CleanupRoot = epubRoot,
            .FileNameUtf8 = "Тестовая книга.epub",
            .ContentTypeUtf8 = "application/epub+zip"
        };
    }

    httplib::Client epubClient("127.0.0.1", server->GetBoundPort());
    const auto epub = GetWithRetry(epubClient, "/api/books/7/download?format=epub");

    REQUIRE(epub);
    REQUIRE(epub->status == 200);
    REQUIRE(epub->body == "epub-download");
    REQUIRE(WaitUntilRemoved(epubRoot));

    std::scoped_lock lock(host.Mutex);
    REQUIRE(host.LastDownloadFormat == Server::EServerDownloadFormat::Epub);
    REQUIRE(host.DownloadCalls == 2);
}

TEST_CASE("HTTP ranged original and EPUB downloads retain cleanup ownership", "[server][http][catalog][files][range]")
{
    CTestWorkspace workspace("inpx-web-reader-server-download-ranges");
    CFakeCatalogServerHost host;
    auto server = StartTestServer(host);

    for (const auto& [format, extension, contentType] : std::vector<std::tuple<std::string, std::string, std::string>>{
             {"original", "fb2", "application/x-fictionbook+xml; charset=utf-8"},
             {"epub", "epub", "application/epub+zip"}})
    {
        const auto stagingRoot = workspace.GetPath() / ("staging-" + format);
        const auto downloadPath = stagingRoot / ("book." + extension);
        WriteBinaryFile(downloadPath, "0123456789");
        {
            std::scoped_lock lock(host.Mutex);
            host.DownloadResponse = Server::SServerFileResponse{
                .Path = downloadPath,
                .CleanupRoot = stagingRoot,
                .FileNameUtf8 = "book." + extension,
                .ContentTypeUtf8 = contentType
            };
        }

        httplib::Client client("127.0.0.1", server->GetBoundPort());
        const auto response = client.Get(
            "/api/books/7/download?format=" + format,
            {{"Range", "bytes=2-5"}});
        REQUIRE(response);
        REQUIRE(response->status == 206);
        REQUIRE(response->body == "2345");
        REQUIRE(response->get_header_value("Content-Range") == "bytes 2-5/10");
        REQUIRE(WaitUntilRemoved(stagingRoot));
    }

    const auto headRoot = workspace.GetPath() / "staging-head";
    const auto headPath = headRoot / "book.fb2";
    WriteBinaryFile(headPath, "0123456789");
    {
        std::scoped_lock lock(host.Mutex);
        host.DownloadResponse = Server::SServerFileResponse{
            .Path = headPath,
            .CleanupRoot = headRoot,
            .FileNameUtf8 = "book.fb2",
            .ContentTypeUtf8 = "application/x-fictionbook+xml; charset=utf-8"
        };
    }
    httplib::Client headClient("127.0.0.1", server->GetBoundPort());
    const auto head = headClient.Head(
        "/api/books/7/download?format=original",
        {{"Range", "bytes=0-1"}});
    REQUIRE(head);
    REQUIRE(head->status == 206);
    REQUIRE(head->body.empty());
    REQUIRE(head->get_header_value("Content-Length") == "2");
    REQUIRE(WaitUntilRemoved(headRoot));
}

TEST_CASE("HTTP download cleanup covers failures before provider registration", "[server][http][catalog][files]")
{
    CTestWorkspace workspace("inpx-web-reader-server-download-pre-provider-cleanup");
    const auto failedRoot = workspace.GetPath() / "failed-staging";
    WriteBinaryFile(failedRoot / "marker", "staged");

    CFakeCatalogServerHost host;
    host.DownloadResponse = Server::SServerFileResponse{
        .Path = failedRoot / "missing.fb2",
        .CleanupRoot = failedRoot,
        .FileNameUtf8 = "missing.fb2",
        .ContentTypeUtf8 = "application/x-fictionbook+xml; charset=utf-8"
    };
    auto server = StartTestServer(host, {.MaxConcurrentDownloads = 1});
    httplib::Client client("127.0.0.1", server->GetBoundPort());

    const auto failed = client.Get("/api/books/7/download?format=original");
    REQUIRE(failed);
    REQUIRE(failed->status == 404);
    REQUIRE(WaitUntilRemoved(failedRoot));

    const auto validRoot = workspace.GetPath() / "valid-staging";
    const auto validPath = validRoot / "book.fb2";
    WriteBinaryFile(validPath, "download");
    {
        std::scoped_lock lock(host.Mutex);
        host.DownloadResponse = Server::SServerFileResponse{
            .Path = validPath,
            .CleanupRoot = validRoot,
            .FileNameUtf8 = "book.fb2",
            .ContentTypeUtf8 = "application/x-fictionbook+xml; charset=utf-8"
        };
    }

    const auto accepted = client.Get("/api/books/7/download?format=original");
    REQUIRE(accepted);
    REQUIRE(accepted->status == 200);
    REQUIRE(accepted->body == "download");
    REQUIRE(WaitUntilRemoved(validRoot));
}

TEST_CASE("HTTP download endpoint enforces concurrent download limit", "[server][http][catalog][files]")
{
    CTestWorkspace workspace("inpx-web-reader-server-catalog-download-limit");
    const auto stagingRoot = workspace.GetPath() / "staging";
    const auto downloadPath = stagingRoot / "book.fb2";
    WriteBinaryFile(downloadPath, "download");

    CFakeCatalogServerHost host;
    host.BlockDownloads = true;
    host.DownloadResponse = Server::SServerFileResponse{
        .Path = downloadPath,
        .CleanupRoot = stagingRoot,
        .FileNameUtf8 = "book.fb2",
        .ContentTypeUtf8 = "application/x-fictionbook+xml; charset=utf-8"
    };
    auto server = StartTestServer(host, {.MaxThreads = 2, .MaxConcurrentDownloads = 1});

    auto first = std::async(std::launch::async, [port = server->GetBoundPort()]() {
        httplib::Client client("127.0.0.1", port);
        return client.Get("/api/books/7/download?format=original");
    });
    CBlockedDownloadReleaseGuard releaseGuard(host);
    REQUIRE(host.WaitForBlockedDownload(5s));

    httplib::Client secondClient("127.0.0.1", server->GetBoundPort());
    const auto rejected = GetWithRetry(secondClient, "/api/books/7/download?format=original");
    REQUIRE(rejected);
    REQUIRE(rejected->status == 429);
    REQUIRE(nlohmann::json::parse(rejected->body).at("error").at("code").get<std::string>() == "too_many_requests");

    releaseGuard.ReleaseNow();
    const auto accepted = first.get();
    REQUIRE(accepted);
    REQUIRE(accepted->status == 200);

    std::scoped_lock lock(host.Mutex);
    REQUIRE(host.DownloadCalls == 1);
}

TEST_CASE("HTTP catalog API runs a real INPX scan list details cover and download flow", "[server][http][catalog][integration]")
{
    CTestWorkspace workspace("inpx-web-reader-server-catalog-api-real");
    const auto inpxPath = WriteInpxArchive(workspace.GetPath() / "source" / "catalog.inpx");
    const auto archiveRoot = workspace.GetPath() / "archives";
    const auto cacheRoot = workspace.GetPath() / "cache";
    const auto runtimeRoot = workspace.GetPath() / "runtime";
    WriteInpxPayloadArchive(archiveRoot / "fb2-main.zip");

    const auto config = Server::CServerConfigLoader::LoadFromJsonText(
        ServerFixtures::MakeServerConfigJson(
            cacheRoot,
            runtimeRoot,
            inpxPath,
            archiveRoot));

    Server::CServerApplicationHost host(config);
    host.Open();
    auto server = StartTestServer(host);
    httplib::Client client("127.0.0.1", server->GetBoundPort());

    const auto started = PostWithRetry(client, "/api/scan/start", R"({"mode":"initial","warningLimit":5})");
    REQUIRE(started);
    REQUIRE(started->status == 202);

    const auto terminal = WaitForTerminalScan(client);
    REQUIRE(terminal.at("status").get<std::string>() == "completed");
    REQUIRE(terminal.at("result").at("totalRecords").get<std::size_t>() == 1);

    const auto list = GetWithRetry(
        client,
        "/api/books?text=%D0%A2%D0%B5%D1%81%D1%82%D0%BE%D0%B2%D0%B0%D1%8F"
        "&fields=title&sort=title&direction=asc&limit=10");
    REQUIRE(list);
    REQUIRE(list->status == 200);
    const auto listBody = nlohmann::json::parse(list->body);
    REQUIRE(listBody.at("totalCount").get<std::uint64_t>() == 1);
    REQUIRE_FALSE(listBody.contains("statistics"));
    const auto bookId = listBody.at("items").at(0).at("id").get<std::int64_t>();
    REQUIRE(listBody.at("items").at(0).at("title").get<std::string>() == "Тестовая книга");

    const auto details = GetWithRetry(client, "/api/books/" + std::to_string(bookId));
    REQUIRE(details);
    REQUIRE(details->status == 200);
    const auto detailsBody = nlohmann::json::parse(details->body);
    REQUIRE(detailsBody.at("book").at("title").get<std::string>() == "Тестовая книга");
    REQUIRE(detailsBody.at("book").at("coverUrl").get<std::string>() == "/api/covers/" + std::to_string(bookId));

    const auto cover = GetWithRetry(client, "/api/covers/" + std::to_string(bookId));
    REQUIRE(cover);
    REQUIRE(cover->status == 200);
    REQUIRE_FALSE(cover->body.empty());

    const auto download = GetWithRetry(client, "/api/books/" + std::to_string(bookId) + "/download?format=original");
    REQUIRE(download);
    REQUIRE(download->status == 200);
    REQUIRE(download->body.find("Тестовая книга") != std::string::npos);
    REQUIRE(download->get_header_value("Content-Disposition").find("filename*=UTF-8''") != std::string::npos);

    const auto stats = GetWithRetry(client, "/api/stats");
    REQUIRE(stats);
    REQUIRE(stats->status == 200);
    const auto statsBody = nlohmann::json::parse(stats->body);
    REQUIRE(statsBody.at("bookCount").get<std::uint64_t>() == listBody.at("totalCount").get<std::uint64_t>());

    server->Stop();
    host.Close();
}
