#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "Domain/SearchQuery.hpp"
#include "TestServerCatalogApiSupport.hpp"
#include "TestWorkspace.hpp"

using namespace InpxWebReader::Tests::ServerCatalogApiSupport;

namespace Domain = InpxWebReader::Domain;
namespace Server = InpxWebReader::Server;

TEST_CASE("HTTP catalog list maps query parameters and returns path-free DTOs", "[server][http][catalog]")
{
    CFakeCatalogServerHost host;
    auto server = StartTestServer(host, {.MaxPageSize = 25});
    httplib::Client client("127.0.0.1", server->GetBoundPort());

    const auto response = GetWithRetry(
        client,
        "/api/books?text=%D0%A2%D0%B5%D1%81%D1%82&fields=title,authors"
        "&languages=ru,en&genres=Science%20Fiction"
        "&sort=added&direction=desc&offset=3&limit=999&includeFacets=false");

    REQUIRE(response);
    REQUIRE(response->status == 200);
    const auto body = nlohmann::json::parse(response->body);
    REQUIRE(body.at("totalCount").get<std::uint64_t>() == 1);
    REQUIRE(body.at("catalogSnapshotId").get<std::string>() == "scan-fixture-17");
    REQUIRE(body.at("offset").get<std::size_t>() == 3);
    REQUIRE(body.at("limit").get<std::size_t>() == 25);
    REQUIRE(body.at("facets").at("languages").empty());
    REQUIRE(body.at("facets").at("genres").empty());
    REQUIRE_FALSE(body.contains("statistics"));
    const auto& item = body.at("items").at(0);
    REQUIRE(item.at("title").get<std::string>() == "Тестовая книга");
    REQUIRE(item.at("authors").at(0).get<std::string>() == "Иван Тестов");
    REQUIRE(item.at("coverUrl").get<std::string>() == "/api/covers/7");
    REQUIRE(item.at("downloadUrl").get<std::string>() == "/api/books/7/download?format=original");
    REQUIRE(item.at("epubDownloadUrl").get<std::string>() == "/api/books/7/download?format=epub");
    REQUIRE_FALSE(item.contains("coverPath"));

    std::scoped_lock lock(host.Mutex);
    REQUIRE(host.ListCalls == 1);
    REQUIRE(host.LastListRequest.has_value());
    REQUIRE(host.LastListRequest->TextUtf8 == "Тест");
    REQUIRE(host.LastListRequest->SearchFields.Title);
    REQUIRE(host.LastListRequest->SearchFields.Authors);
    REQUIRE_FALSE(host.LastListRequest->SearchFields.Description);
    REQUIRE(host.LastListRequest->Languages == std::vector<std::string>{"ru", "en"});
    REQUIRE(host.LastListRequest->GenresUtf8 == std::vector<std::string>{"Science Fiction"});
    REQUIRE(host.LastListRequest->SortBy == Domain::EBookSort::DateAdded);
    REQUIRE(host.LastListRequest->SortDirection == Domain::ESortDirection::Descending);
    REQUIRE(host.LastListRequest->Offset == 3);
    REQUIRE(host.LastListRequest->Limit == 25);
    REQUIRE_FALSE(host.LastListRequest->IncludeFacets);
}

TEST_CASE("HTTP catalog cursor binds query context and omits repeated summary", "[server][http][catalog][pagination]")
{
    CFakeCatalogServerHost host;
    host.ListResult.NextCursor = Domain::SSearchCursor{
        .SessionIdUtf8 = "0123456789abcdef0123456789abcdef",
        .SourceFingerprintUtf8 = "sha256:fixture",
        .CatalogSnapshotIdUtf8 = "scan-fixture-17",
        .Position = 1
    };
    auto server = StartTestServer(host, {.MaxPageSize = 25});
    httplib::Client client("127.0.0.1", server->GetBoundPort());

    const auto first = GetWithRetry(
        client,
        "/api/books?sort=title&direction=asc&limit=1&includeFacets=true");
    REQUIRE(first);
    REQUIRE(first->status == 200);
    const auto firstBody = nlohmann::json::parse(first->body);
    REQUIRE(firstBody.at("totalCount").get<std::uint64_t>() == 1);
    REQUIRE(firstBody.at("facets").is_object());
    REQUIRE(firstBody.at("offset").get<std::size_t>() == 0);
    REQUIRE(firstBody.at("nextCursor").is_string());
    const auto encodedCursor = firstBody.at("nextCursor").get<std::string>();

    const auto continuation = GetWithRetry(
        client,
        ("/api/books?sort=title&direction=asc&limit=25&includeFacets=true&cursor=" + encodedCursor).c_str());
    REQUIRE(continuation);
    REQUIRE(continuation->status == 200);
    const auto continuationBody = nlohmann::json::parse(continuation->body);
    REQUIRE(continuationBody.at("totalCount").is_null());
    REQUIRE(continuationBody.at("facets").is_null());
    REQUIRE(continuationBody.at("nextCursor").is_null());
    REQUIRE(continuationBody.at("offset").get<std::size_t>() == 1);
    REQUIRE(continuationBody.at("limit").get<std::size_t>() == 25);

    {
        std::scoped_lock lock(host.Mutex);
        REQUIRE(host.LastListRequest.has_value());
        REQUIRE(host.LastListRequest->Cursor.has_value());
        REQUIRE(host.LastListRequest->Cursor->Position == 1);
    }

    const auto wrongQuery = GetWithRetry(
        client,
        ("/api/books?sort=author&direction=asc&limit=25&cursor=" + encodedCursor).c_str());
    REQUIRE(wrongQuery);
    REQUIRE(wrongQuery->status == 400);
    REQUIRE(nlohmann::json::parse(wrongQuery->body).at("error").at("code").get<std::string>() == "invalid_cursor");

    const auto combinedOffset = GetWithRetry(
        client,
        ("/api/books?sort=title&direction=asc&offset=0&cursor=" + encodedCursor).c_str());
    REQUIRE(combinedOffset);
    REQUIRE(combinedOffset->status == 400);

    const auto malformed = GetWithRetry(client, "/api/books?cursor=not-base64url!");
    REQUIRE(malformed);
    REQUIRE(malformed->status == 400);
    REQUIRE(nlohmann::json::parse(malformed->body).at("error").at("code").get<std::string>() == "invalid_cursor");

    const std::vector<std::pair<Domain::EDomainErrorCode, std::string>> cursorConflicts{
        {Domain::EDomainErrorCode::CatalogCursorExpired, "catalog_cursor_expired"},
        {Domain::EDomainErrorCode::CatalogSnapshotChanged, "catalog_snapshot_changed"},
        {Domain::EDomainErrorCode::CatalogCursorCapacityExceeded, "catalog_cursor_capacity_exceeded"}
    };
    for (const auto& [errorCode, responseCode] : cursorConflicts)
    {
        host.ListErrorCode = errorCode;
        const auto conflict = GetWithRetry(
            client,
            ("/api/books?sort=title&direction=asc&cursor=" + encodedCursor).c_str());
        REQUIRE(conflict);
        REQUIRE(conflict->status == 409);
        REQUIRE(nlohmann::json::parse(conflict->body).at("error").at("code").get<std::string>() == responseCode);
    }
}

TEST_CASE("HTTP catalog endpoints require bearer token before invoking host", "[server][http][catalog][auth]")
{
    CTestWorkspace workspace("inpx-web-reader-server-catalog-auth-matrix");
    const auto coverPath = workspace.GetPath() / "cover.png";
    const auto downloadPath = workspace.GetPath() / "book.fb2";
    WriteBinaryFile(coverPath, "cover");
    WriteBinaryFile(downloadPath, "payload");

    CFakeCatalogServerHost host;
    host.CoverResponse = Server::SServerFileResponse{
        .Path = coverPath,
        .FileNameUtf8 = "cover.png",
        .ContentTypeUtf8 = "image/png"
    };
    host.DownloadResponse = Server::SServerFileResponse{
        .Path = downloadPath,
        .FileNameUtf8 = "book.fb2",
        .ContentTypeUtf8 = "application/fb2+xml"
    };
    auto server = StartTestServer(host, {.Security = {.TokenUtf8 = "secret"}});
    httplib::Client client("127.0.0.1", server->GetBoundPort());

    const std::vector<std::string> endpoints{
        "/api/books?limit=1",
        "/api/books/7",
        "/api/stats",
        "/api/covers/7",
        "/api/books/7/download?format=original"
    };

    for (const auto& endpoint : endpoints)
    {
        const auto unauthenticated = GetWithRetry(client, endpoint);
        REQUIRE(unauthenticated);
        REQUIRE(unauthenticated->status == 401);

        const auto wrongToken = GetWithRetry(client, endpoint, {{"Authorization", "Bearer wrong"}});
        REQUIRE(wrongToken);
        REQUIRE(wrongToken->status == 401);
    }

    REQUIRE(host.ListCalls == 0);
    REQUIRE(host.DetailsCalls == 0);
    REQUIRE(host.StatisticsCalls == 0);
    REQUIRE(host.CoverCalls == 0);
    REQUIRE(host.DownloadCalls == 0);

    for (const auto& endpoint : endpoints)
    {
        const auto authorized = GetWithRetry(client, endpoint, {{"Authorization", "Bearer secret"}});
        REQUIRE(authorized);
        REQUIRE(authorized->status == 200);
    }

    REQUIRE(host.ListCalls == 1);
    REQUIRE(host.DetailsCalls == 1);
    REQUIRE(host.StatisticsCalls == 1);
    REQUIRE(host.CoverCalls == 1);
    REQUIRE(host.DownloadCalls == 1);
}

TEST_CASE("HTTP catalog rejects invalid pagination and path-like ids", "[server][http][catalog]")
{
    CFakeCatalogServerHost host;
    auto server = StartTestServer(host);
    httplib::Client client("127.0.0.1", server->GetBoundPort());

    const auto invalidLimit = GetWithRetry(client, "/api/books?limit=abc");
    REQUIRE(invalidLimit);
    REQUIRE(invalidLimit->status == 400);
    REQUIRE(nlohmann::json::parse(invalidLimit->body).at("error").at("code").get<std::string>() == "bad_request");

    const std::vector<std::string> invalidQueries{
        "/api/books?fields=",
        "/api/books?fields=publisher",
        "/api/books?sort=year",
        "/api/books?direction=sideways",
        "/api/books?includeFacets=maybe",
        "/api/books?offset=-1",
        "/api/books?offset=9223372036854775808",
        "/api/books?offset=18446744073709551615",
        "/api/books?offset=184467440737095516160",
        "/api/books?limit=-1"
    };
    for (const auto& path : invalidQueries)
    {
        const auto response = GetWithRetry(client, path.c_str());
        REQUIRE(response);
        REQUIRE(response->status == 400);
        REQUIRE(nlohmann::json::parse(response->body).at("error").at("code").get<std::string>() == "bad_request");
    }

    const auto invalidDetailsId = GetWithRetry(client, "/api/books/not-a-number");
    REQUIRE(invalidDetailsId);
    REQUIRE(invalidDetailsId->status == 400);
    REQUIRE(nlohmann::json::parse(invalidDetailsId->body).at("error").at("code").get<std::string>() == "bad_request");

    const auto coverTraversal = GetWithRetry(client, "/api/covers/..%2Fsecret");
    REQUIRE(coverTraversal);
    REQUIRE(coverTraversal->status == 400);
    REQUIRE(nlohmann::json::parse(coverTraversal->body).at("error").at("code").get<std::string>() == "bad_request");

    const auto downloadTraversal = GetWithRetry(client, "/api/books/..%2Fsecret/download?format=original");
    REQUIRE(downloadTraversal);
    REQUIRE(downloadTraversal->status == 400);
    REQUIRE(nlohmann::json::parse(downloadTraversal->body).at("error").at("code").get<std::string>() == "bad_request");

    const auto missingDetails = GetWithRetry(client, "/api/books/999");
    REQUIRE(missingDetails);
    REQUIRE(missingDetails->status == 404);
    REQUIRE(nlohmann::json::parse(missingDetails->body).at("error").at("code").get<std::string>() == "not_found");

    std::scoped_lock lock(host.Mutex);
    REQUIRE(host.ListCalls == 0);
    REQUIRE(host.DetailsCalls == 1);
    REQUIRE(host.CoverCalls == 0);
    REQUIRE(host.DownloadCalls == 0);
}

TEST_CASE("HTTP catalog details and stats expose browser DTOs without filesystem paths", "[server][http][catalog]")
{
    CFakeCatalogServerHost host;
    auto server = StartTestServer(host);
    httplib::Client client("127.0.0.1", server->GetBoundPort());

    const auto details = GetWithRetry(client, "/api/books/7");
    REQUIRE(details);
    REQUIRE(details->status == 200);
    const auto detailsBody = nlohmann::json::parse(details->body);
    const auto& book = detailsBody.at("book");
    REQUIRE(book.at("title").get<std::string>() == "Тестовая книга");
    REQUIRE(book.at("description").get<std::string>() == "Описание для браузерного API.");
    REQUIRE_FALSE(book.contains("coverPath"));

    const auto stats = GetWithRetry(client, "/api/stats");
    REQUIRE(stats);
    REQUIRE(stats->status == 200);
    const auto statsBody = nlohmann::json::parse(stats->body);
    REQUIRE(statsBody.at("bookCount").get<std::uint64_t>() == 1);
    REQUIRE(statsBody.at("totalCatalogSizeBytes").get<std::uint64_t>() == 1024);
}
