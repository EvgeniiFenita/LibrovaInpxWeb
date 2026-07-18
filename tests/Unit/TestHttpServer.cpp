#include <catch2/catch_test_macros.hpp>

#include <arpa/inet.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "Domain/DomainError.hpp"
#include "Foundation/Logging.hpp"
#include "Server/HttpServer.hpp"
#include "TestFilesystemHelpers.hpp"
#include "TestWorkspace.hpp"

namespace {

class CFakeServerHost final : public InpxWebReader::Server::IServerApplicationHost
{
public:
    [[nodiscard]] bool IsOpen() const noexcept override
    {
        return Open;
    }

    [[nodiscard]] InpxWebReader::Server::SServerStatus GetStatus() override
    {
        if (ThrowConverterTimeoutOnStatus)
        {
            throw InpxWebReader::Domain::CDomainException(
                InpxWebReader::Domain::EDomainErrorCode::ConverterTimeout,
                "Book conversion timed out.");
        }
        if (ThrowBackendOverloadOnStatus)
        {
            throw InpxWebReader::Server::CBackendOverloadError();
        }
        if (ThrowOnStatus)
        {
            throw std::runtime_error("status failed");
        }

        return Status;
    }

    [[nodiscard]] InpxWebReader::Application::SBookListResult ListBooks(
        const InpxWebReader::Application::SBookListRequest&) override
    {
        return {};
    }

    [[nodiscard]] std::optional<InpxWebReader::Application::SBookDetails> GetBookDetails(
        InpxWebReader::Domain::SBookId) override
    {
        return std::nullopt;
    }

    [[nodiscard]] InpxWebReader::Application::SCatalogStatistics GetCatalogStatistics() override
    {
        return {};
    }

    [[nodiscard]] std::optional<InpxWebReader::Server::SServerFileResponse> ResolveBookCover(
        InpxWebReader::Domain::SBookId) override
    {
        return CoverResponse;
    }

    [[nodiscard]] std::optional<InpxWebReader::Server::SServerFileResponse> PrepareBookDownload(
        InpxWebReader::Domain::SBookId,
        InpxWebReader::Server::EServerDownloadFormat) override
    {
        return std::nullopt;
    }

    [[nodiscard]] std::optional<InpxWebReader::Application::SInpxSourceOverview> GetInpxSourceOverview() override
    {
        return std::nullopt;
    }

    [[nodiscard]] InpxWebReader::ApplicationJobs::TInpxScanJobId StartInpxScan(
        const InpxWebReader::ApplicationJobs::SInpxScanRequest&) override
    {
        return 0;
    }

    [[nodiscard]] std::optional<InpxWebReader::ApplicationJobs::SInpxScanJobSnapshot> GetInpxScanJobSnapshot(
        InpxWebReader::ApplicationJobs::TInpxScanJobId) override
    {
        return std::nullopt;
    }

    [[nodiscard]] std::optional<InpxWebReader::ApplicationJobs::SInpxScanJobResult> GetInpxScanJobResult(
        InpxWebReader::ApplicationJobs::TInpxScanJobId) override
    {
        return std::nullopt;
    }

    [[nodiscard]] bool CancelInpxScanJob(InpxWebReader::ApplicationJobs::TInpxScanJobId) override
    {
        return false;
    }

    [[nodiscard]] bool RemoveInpxScanJob(InpxWebReader::ApplicationJobs::TInpxScanJobId) override
    {
        return false;
    }

    bool Open = true;
    bool ThrowOnStatus = false;
    bool ThrowBackendOverloadOnStatus = false;
    bool ThrowConverterTimeoutOnStatus = false;
    std::optional<InpxWebReader::Server::SServerFileResponse> CoverResponse = std::nullopt;
    InpxWebReader::Server::SServerStatus Status{
        .VersionUtf8 = "0.1.0",
        .IsOpen = true,
        .Capabilities = {
            .CanRescanInpxSource = true,
            .CanDownloadOriginal = true,
            .CanDownloadAsEpub = true
        },
        .InpxSource = InpxWebReader::Server::SServerInpxSourceStatus{
            .InpxPathUtf8 = "/source/catalog.inpx",
            .ArchiveRootUtf8 = "/source",
            .IsSourceAvailable = true,
            .SourceWarningUtf8 = {},
            .TotalBookCount = 42,
            .AvailableBookCount = 40,
            .UnavailableBookCount = 2,
            .WarningCount = 3
        }
    };
};

class CRawHttpSocket final
{
public:
    CRawHttpSocket(const int port, const std::chrono::milliseconds timeout)
    {
        m_socket = ::socket(AF_INET, SOCK_STREAM, 0);
        if (m_socket < 0)
        {
            throw std::runtime_error("Could not create raw HTTP test socket.");
        }

        timeval socketTimeout{};
        socketTimeout.tv_sec = timeout.count() / 1'000;
        socketTimeout.tv_usec = (timeout.count() % 1'000) * 1'000;
        if (::setsockopt(m_socket, SOL_SOCKET, SO_RCVTIMEO, &socketTimeout, sizeof(socketTimeout)) != 0)
        {
            throw std::runtime_error("Could not configure raw HTTP test socket timeout.");
        }

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<std::uint16_t>(port));
        if (::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1
            || ::connect(m_socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0)
        {
            throw std::runtime_error("Could not connect raw HTTP test socket.");
        }
    }

    ~CRawHttpSocket()
    {
        if (m_socket >= 0)
        {
            ::close(m_socket);
        }
    }

    CRawHttpSocket(const CRawHttpSocket&) = delete;
    CRawHttpSocket& operator=(const CRawHttpSocket&) = delete;

    void Send(const std::string_view request)
    {
        std::size_t offset = 0;
        while (offset < request.size())
        {
            const auto sent = ::send(m_socket, request.data() + offset, request.size() - offset, 0);
            if (sent <= 0)
            {
                throw std::runtime_error("Could not send raw HTTP test request.");
            }
            offset += static_cast<std::size_t>(sent);
        }
    }

    [[nodiscard]] std::string ReadAll()
    {
        std::string response;
        char buffer[4'096];
        for (;;)
        {
            const auto received = ::recv(m_socket, buffer, sizeof(buffer), 0);
            if (received == 0)
            {
                break;
            }
            if (received < 0)
            {
                if (!response.empty())
                {
                    break;
                }
                throw std::runtime_error("Timed out waiting for raw HTTP test response.");
            }
            response.append(buffer, static_cast<std::size_t>(received));
        }
        return response;
    }

private:
    int m_socket = -1;
};

[[nodiscard]] httplib::Result GetWithRetry(
    httplib::Client& client,
    const char* path,
    const httplib::Headers& headers = {})
{
    for (int attempt = 0; attempt < 50; ++attempt)
    {
        if (auto response = client.Get(path, headers))
        {
            return response;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    return {};
}

[[nodiscard]] std::unique_ptr<InpxWebReader::Server::CHttpServer> StartTestServer(
    CFakeServerHost& host,
    InpxWebReader::Server::SHttpServerOptions options)
{
    options.HostUtf8 = "127.0.0.1";
    options.Port = 0;
    auto server = std::make_unique<InpxWebReader::Server::CHttpServer>(host, std::move(options));
    server->Start();
    return server;
}

[[nodiscard]] std::unique_ptr<InpxWebReader::Server::CHttpServer> StartTestServer(
    CFakeServerHost& host,
    InpxWebReader::Server::SServerSecurityConfig security = {})
{
    return StartTestServer(
        host,
        InpxWebReader::Server::SHttpServerOptions{
            .Security = std::move(security)
        });
}

void WriteTextFile(
    const std::filesystem::path& path,
    const std::string& content)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output << content;
}

[[nodiscard]] std::string ReadTextFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

void WriteSparseFile(
    const std::filesystem::path& path,
    const std::uintmax_t size)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output.seekp(static_cast<std::streamoff>(size - 1));
    output.put('\0');
}

} // namespace

TEST_CASE("HTTP health is available before backend is open and without auth", "[server][http]")
{
    CFakeServerHost host;
    host.Open = false;
    auto server = StartTestServer(host, {.TokenUtf8 = "secret"});
    httplib::Client client("127.0.0.1", server->GetBoundPort());

    const auto response = GetWithRetry(client, "/api/health");

    REQUIRE(response);
    REQUIRE(response->status == 200);
    REQUIRE_FALSE(response->get_header_value("X-Request-Id").empty());
    const auto body = nlohmann::json::parse(response->body);
    REQUIRE(body.at("ok").get<bool>());
    REQUIRE(body.at("status").get<std::string>() == "starting");

    const auto notReady = GetWithRetry(client, "/api/ready");
    REQUIRE(notReady);
    REQUIRE(notReady->status == 503);
    REQUIRE_FALSE(nlohmann::json::parse(notReady->body).at("ok").get<bool>());

    host.Open = true;
    const auto ready = GetWithRetry(client, "/api/ready");
    REQUIRE(ready);
    REQUIRE(ready->status == 200);
    REQUIRE(nlohmann::json::parse(ready->body).at("ok").get<bool>());
}

TEST_CASE("HTTP successful health probes stay out of the info access log", "[server][http][logging]")
{
    CTestWorkspace workspace("inpx-web-reader-http-health-logging");
    const auto logPath = workspace.GetPath() / "server.log";
    CFakeServerHost host;

    InpxWebReader::Logging::CLogging::InitializeHostLogger(logPath);
    auto server = StartTestServer(host);
    httplib::Client client("127.0.0.1", server->GetBoundPort());
    const auto health = GetWithRetry(client, "/api/health");
    const auto status = GetWithRetry(client, "/api/status");
    server->Stop();
    server.reset();
    InpxWebReader::Logging::CLogging::Shutdown();

    REQUIRE(health);
    REQUIRE(health->status == 200);
    REQUIRE(status);
    REQUIRE(status->status == 200);
    const auto logText = ReadTextFile(logPath);
    REQUIRE(logText.find("path=/api/health") == std::string::npos);
    REQUIRE(logText.find("path=/api/status") != std::string::npos);
}

TEST_CASE("HTTP byte range safety limits log the final 416 status", "[server][http][range][logging][limits]")
{
    CTestWorkspace workspace("inpx-web-reader-http-range-limit-logging");
    const auto coverPath = workspace.GetPath() / "cover.jpg";
    const auto logPath = workspace.GetPath() / "server.log";
    WriteTextFile(coverPath, std::string(2'048, 'x'));
    CFakeServerHost host;
    host.CoverResponse = InpxWebReader::Server::SServerFileResponse{
        .Path = coverPath,
        .FileNameUtf8 = "cover.jpg",
        .ContentTypeUtf8 = "image/jpeg"
    };

    InpxWebReader::Logging::CLogging::InitializeHostLogger(logPath);
    auto server = StartTestServer(host);
    const auto sendRange = [&](const std::string& range) {
        CRawHttpSocket socket(server->GetBoundPort(), std::chrono::seconds{2});
        socket.Send(
            "GET /api/covers/7 HTTP/1.1\r\n"
            "Host: 127.0.0.1\r\n"
            "Range: bytes=" + range + "\r\n"
            "Connection: close\r\n\r\n");
        return socket.ReadAll();
    };
    const auto buildDistinctRanges = [](const std::size_t count) {
        std::string value;
        for (std::size_t index = 0; index < count; ++index)
        {
            if (!value.empty())
            {
                value.push_back(',');
            }
            value += std::to_string(index) + '-' + std::to_string(index);
        }
        return value;
    };

    const auto maximumAccepted = sendRange(buildDistinctRanges(CPPHTTPLIB_RANGE_MAX_COUNT));
    const auto tooMany = sendRange(buildDistinctRanges(CPPHTTPLIB_RANGE_MAX_COUNT + 1));
    const auto maximumOverlapAccepted = sendRange("0-3,1-4,2-5");
    const auto chainOverlap = sendRange("0-1,1-2,2-3,3-4");
    const auto nestedOverlap = sendRange("0-9,1-8,2-7,3-6");
    server->Stop();
    server.reset();
    InpxWebReader::Logging::CLogging::Shutdown();

    REQUIRE(maximumAccepted.find("HTTP/1.1 206") == 0);
    REQUIRE(tooMany.find("HTTP/1.1 416") == 0);
    REQUIRE(tooMany.find("Content-Range: bytes */2048") != std::string::npos);
    REQUIRE(maximumOverlapAccepted.find("HTTP/1.1 206") == 0);
    REQUIRE(chainOverlap.find("HTTP/1.1 416") == 0);
    REQUIRE(nestedOverlap.find("HTTP/1.1 416") == 0);
    REQUIRE(ReadTextFile(logPath).find(" path=/api/covers/7 status=416 ") != std::string::npos);
}

TEST_CASE("HTTP rejects and escapes decoded control bytes as one physical log record", "[server][http][logging]")
{
    CTestWorkspace workspace("inpx-web-reader-http-request-log-controls");
    const auto logPath = workspace.GetPath() / "server.log";
    CFakeServerHost host;

    InpxWebReader::Logging::CLogging::InitializeHostLogger(logPath);
    auto server = StartTestServer(host);
    for (const auto* encodedControl : {"%0A", "%0D", "%1B", "%00", "%FF"})
    {
        CRawHttpSocket socket(server->GetBoundPort(), std::chrono::seconds{2});
        socket.Send(
            std::string{"GET /api/not"} + encodedControl
            + "forged-record HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n");
        const auto response = socket.ReadAll();
        REQUIRE(response.find("HTTP/1.1 400") == 0);
    }
    server->Stop();
    server.reset();
    InpxWebReader::Logging::CLogging::Shutdown();

    const auto logText = ReadTextFile(logPath);
    std::size_t accessRecordCount = 0;
    std::size_t offset = 0;
    while ((offset = logText.find("HTTP request:", offset)) != std::string::npos)
    {
        ++accessRecordCount;
        offset += 1;
    }
    REQUIRE(accessRecordCount == 5);
    REQUIRE(logText.find("path=/api/not\\nforged-record") != std::string::npos);
    REQUIRE(logText.find("path=/api/not\\rforged-record") != std::string::npos);
    REQUIRE(logText.find("path=/api/not\\x1Bforged-record") != std::string::npos);
    REQUIRE(logText.find("path=/api/not\\x00forged-record") != std::string::npos);
    REQUIRE(logText.find("path=/api/not\\xFFforged-record") != std::string::npos);
}

TEST_CASE("HTTP non-health endpoints enforce bearer token when configured", "[server][http][auth]")
{
    CFakeServerHost host;
    auto server = StartTestServer(host, {.TokenUtf8 = "secret"});
    httplib::Client client("127.0.0.1", server->GetBoundPort());

    const auto unauthenticated = GetWithRetry(client, "/api/status");
    REQUIRE(unauthenticated);
    REQUIRE(unauthenticated->status == 401);

    const auto wrongToken = GetWithRetry(
        client,
        "/api/status",
        {{"Authorization", "Bearer wrong"}});
    REQUIRE(wrongToken);
    REQUIRE(wrongToken->status == 401);

    const auto version = GetWithRetry(
        client,
        "/api/version",
        {{"Authorization", "Bearer secret"}});
    REQUIRE(version);
    REQUIRE(version->status == 200);
    REQUIRE(nlohmann::json::parse(version->body).at("version").get<std::string>() == "0.1.0");
}

TEST_CASE("HTTP authenticates protected requests before reading slow bodies", "[server][http][auth][limits]")
{
    CFakeServerHost host;
    auto server = StartTestServer(
        host,
        InpxWebReader::Server::SHttpServerOptions{
            .MaxThreads = 1,
            .MaxQueuedRequests = 1,
            .ReadTimeoutMs = 2'000,
            .Security = {.TokenUtf8 = "secret"}
        });

    for (const bool expectContinue : {false, true})
    {
        CRawHttpSocket slowRequest(server->GetBoundPort(), std::chrono::seconds{2});
        slowRequest.Send(
            std::string{"POST /api/scan/start HTTP/1.1\r\n"}
            + "Host: 127.0.0.1\r\n"
            + "Content-Type: application/json\r\n"
            + "Content-Length: 1048576\r\n"
            + (expectContinue ? "Expect: 100-continue\r\n" : "")
            + "\r\n");

        const auto rejected = slowRequest.ReadAll();
        REQUIRE(rejected.find("HTTP/1.1 401") == 0);
        REQUIRE(rejected.find("\"code\":\"unauthorized\"") != std::string::npos);
        REQUIRE(rejected.find("100 Continue") == std::string::npos);

        httplib::Client healthClient("127.0.0.1", server->GetBoundPort());
        healthClient.set_read_timeout(std::chrono::seconds{1});
        const auto health = healthClient.Get("/api/health");
        REQUIRE(health);
        REQUIRE(health->status == 200);
    }
}

TEST_CASE("HTTP queue saturation has a bounded explicit overload response", "[server][http][limits]")
{
    CFakeServerHost host;
    auto server = StartTestServer(
        host,
        InpxWebReader::Server::SHttpServerOptions{
            .MaxThreads = 1,
            .MaxQueuedRequests = 1,
            .ReadTimeoutMs = 2'000,
            .Security = {.TokenUtf8 = "secret"}
        });

    const std::string slowHeaders =
        "POST /api/scan/start HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Authorization: Bearer secret\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 1048576\r\n\r\n";
    CRawHttpSocket activeRequest(server->GetBoundPort(), std::chrono::seconds{2});
    activeRequest.Send(slowHeaders);
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    CRawHttpSocket queuedRequest(server->GetBoundPort(), std::chrono::seconds{2});
    queuedRequest.Send(slowHeaders);
    std::this_thread::sleep_for(std::chrono::milliseconds{100});

    httplib::Client client("127.0.0.1", server->GetBoundPort());
    client.set_read_timeout(std::chrono::seconds{1});
    const auto overloaded = client.Get("/api/health");

    REQUIRE(overloaded);
    REQUIRE(overloaded->status == 503);
    REQUIRE(overloaded->get_header_value("Retry-After") == "1");
    REQUIRE(nlohmann::json::parse(overloaded->body).at("error").at("code").get<std::string>() == "server_overloaded");
}

TEST_CASE("HTTP status reports capabilities source overview converter and scan state", "[server][http]")
{
    CTestWorkspace workspace("inpx-web-reader-http-runtime-status");
    const auto cacheRoot = workspace.GetPath() / "cache";
    const auto runtimeRoot = workspace.GetPath() / "runtime";
    WriteTextFile(cacheRoot / "Database" / "inpx-web-reader.db", "database marker");
    WriteTextFile(cacheRoot / "Covers" / "00" / "cover.bin", "cover");
    WriteTextFile(runtimeRoot / "Scans" / "scan.tmp", "scan");
    WriteTextFile(runtimeRoot / "Downloads" / "source.tmp", "source");
    WriteTextFile(runtimeRoot / "ServerDownloads" / "download.tmp", "download");

    CFakeServerHost host;
    auto server = StartTestServer(
        host,
        InpxWebReader::Server::SHttpServerOptions{
            .CacheRoot = cacheRoot,
            .RuntimeWorkspaceRoot = runtimeRoot,
            .MaxScanWorkers = 3,
            .MaxCoverCacheBytes = 8ULL * 1024ULL * 1024ULL,
            .MaxSteadyStateMemoryBytes = 256ULL * 1024ULL * 1024ULL
        });
    httplib::Client client("127.0.0.1", server->GetBoundPort());

    const auto response = GetWithRetry(client, "/api/status");

    REQUIRE(response);
    REQUIRE(response->status == 200);
    const auto body = nlohmann::json::parse(response->body);
    REQUIRE(body.at("capabilities").at("canRescanInpxSource").get<bool>());
    REQUIRE(body.at("capabilities").at("canDownloadAsEpub").get<bool>());
    REQUIRE(body.at("converter").at("available").get<bool>());
    REQUIRE_FALSE(body.at("scan").at("active").get<bool>());
    REQUIRE(body.at("inpxSource").at("totalBookCount").get<int>() == 42);
    REQUIRE(body.at("inpxSource").at("warningCount").get<int>() == 3);
    REQUIRE_FALSE(body.at("inpxSource").contains("inpxPath"));
    REQUIRE_FALSE(body.at("inpxSource").contains("archiveRoot"));
    REQUIRE(body.at("runtime").at("uptimeSeconds").is_number_unsigned());
    REQUIRE(body.at("runtime").at("http").at("activeWorkers").get<int>() >= 1);
    REQUIRE(body.at("runtime").at("http").at("maxWorkers").get<int>() == 4);
    REQUIRE(body.at("runtime").at("http").at("maxQueuedRequests").get<int>() == 32);
    REQUIRE_FALSE(body.at("runtime").at("scan").at("active").get<bool>());
    REQUIRE(body.at("runtime").at("scan").at("activeJobs").get<int>() == 0);
    REQUIRE(body.at("runtime").at("scan").at("maxWorkers").get<int>() == 3);
    REQUIRE(body.at("runtime").at("downloads").at("active").get<int>() == 0);
    REQUIRE(body.at("runtime").at("downloads").at("maxConcurrent").get<int>() == 2);
    REQUIRE(body.at("runtime").at("storage").at("cacheRootPresent").get<bool>());
    REQUIRE(body.at("runtime").at("storage").at("cacheDatabasePresent").get<bool>());
    REQUIRE(body.at("runtime").at("storage").at("runtimeWorkspacePresent").get<bool>());
    REQUIRE(body.at("runtime").at("storage").at("coverCacheBytes").get<int>() == 5);
    REQUIRE(body.at("runtime").at("storage").at("inpxScanWorkspaceBytes").get<int>() == 4);
    REQUIRE(body.at("runtime").at("storage").at("downloadWorkspaceBytes").get<int>() == 14);
    REQUIRE(body.at("runtime").at("resources").contains("residentMemoryBytes"));
    REQUIRE(body.at("runtime").at("resources").at("maxCoverCacheBytes").get<int>() == 8 * 1024 * 1024);
    REQUIRE(body.at("runtime").at("resources").at("maxSteadyStateMemoryBytes").get<int>() == 256 * 1024 * 1024);
}

TEST_CASE("HTTP status refreshes cheap storage presence without waiting for telemetry", "[server][http]")
{
    CTestWorkspace workspace("inpx-web-reader-http-runtime-presence-refresh");
    const auto cacheRoot = workspace.GetPath() / "late-cache";
    const auto runtimeRoot = workspace.GetPath() / "late-runtime";

    CFakeServerHost host;
    auto server = StartTestServer(
        host,
        InpxWebReader::Server::SHttpServerOptions{
            .CacheRoot = cacheRoot,
            .RuntimeWorkspaceRoot = runtimeRoot
        });

    WriteTextFile(cacheRoot / "Database" / "inpx-web-reader.db", "database marker");
    std::filesystem::create_directories(runtimeRoot);

    httplib::Client client("127.0.0.1", server->GetBoundPort());
    const auto response = GetWithRetry(client, "/api/status");

    REQUIRE(response);
    REQUIRE(response->status == 200);
    const auto storage = nlohmann::json::parse(response->body).at("runtime").at("storage");
    REQUIRE(storage.at("cacheRootPresent").get<bool>());
    REQUIRE(storage.at("cacheDatabasePresent").get<bool>());
    REQUIRE(storage.at("runtimeWorkspacePresent").get<bool>());
}

TEST_CASE("HTTP server returns structured payload-too-large errors", "[server][http][limits]")
{
    CFakeServerHost host;
    auto server = StartTestServer(
        host,
        InpxWebReader::Server::SHttpServerOptions{
            .MaxRequestBodyBytes = 16
        });
    httplib::Client client("127.0.0.1", server->GetBoundPort());

    const auto response = client.Post(
        "/api/scan/start",
        R"({"mode":"initial","warningLimit":25})",
        "application/json");

    REQUIRE(response);
    REQUIRE(response->status == 413);
    REQUIRE_FALSE(response->get_header_value("X-Request-Id").empty());
    const auto body = nlohmann::json::parse(response->body);
    REQUIRE(body.at("error").at("code").get<std::string>() == "payload_too_large");
    REQUIRE(body.at("error").at("requestId").get<std::string>() == response->get_header_value("X-Request-Id"));
}

TEST_CASE("HTTP scan start returns structured bad request for malformed JSON", "[server][http][scan]")
{
    CFakeServerHost host;
    auto server = StartTestServer(host);
    httplib::Client client("127.0.0.1", server->GetBoundPort());

    const auto response = client.Post("/api/scan/start", R"({"mode":)", "application/json");

    REQUIRE(response);
    REQUIRE(response->status == 400);
    REQUIRE_FALSE(response->get_header_value("X-Request-Id").empty());
    const auto body = nlohmann::json::parse(response->body);
    REQUIRE(body.at("error").at("code").get<std::string>() == "bad_request");
    REQUIRE(body.at("error").at("message").get<std::string>().find("Request JSON is invalid") != std::string::npos);
    REQUIRE(body.at("error").at("requestId").get<std::string>() == response->get_header_value("X-Request-Id"));
}

TEST_CASE("HTTP scan start rejects oversized warning buffers", "[server][http][scan][limits]")
{
    CFakeServerHost host;
    auto server = StartTestServer(host);
    httplib::Client client("127.0.0.1", server->GetBoundPort());

    const auto response = client.Post(
        "/api/scan/start",
        R"({"mode":"initial","warningLimit":1001})",
        "application/json");

    REQUIRE(response);
    REQUIRE(response->status == 400);
    const auto body = nlohmann::json::parse(response->body);
    REQUIRE(body.at("error").at("code").get<std::string>() == "bad_request");
    REQUIRE(body.at("error").at("message").get<std::string>().find("warningLimit") != std::string::npos);
}

TEST_CASE("HTTP scan start rejects invalid modes and warning limits", "[server][http][scan]")
{
    CFakeServerHost host;
    auto server = StartTestServer(host);
    httplib::Client client("127.0.0.1", server->GetBoundPort());

    const std::vector<std::string> invalidBodies{
        R"({"mode":"incremental"})",
        R"({"warningLimit":-1})",
        R"({"warningLimit":"many"})"
    };
    for (const auto& requestBody : invalidBodies)
    {
        const auto response = client.Post("/api/scan/start", requestBody, "application/json");
        REQUIRE(response);
        REQUIRE(response->status == 400);
        REQUIRE(nlohmann::json::parse(response->body).at("error").at("code").get<std::string>() == "bad_request");
    }
}

TEST_CASE("HTTP status reports backend-not-ready with structured error", "[server][http]")
{
    CFakeServerHost host;
    host.Open = false;
    auto server = StartTestServer(host);
    httplib::Client client("127.0.0.1", server->GetBoundPort());

    const auto response = GetWithRetry(client, "/api/status");

    REQUIRE(response);
    REQUIRE(response->status == 503);
    REQUIRE_FALSE(response->get_header_value("X-Request-Id").empty());
    const auto body = nlohmann::json::parse(response->body);
    REQUIRE(body.at("error").at("code").get<std::string>() == "backend_not_ready");
    REQUIRE(body.at("error").at("requestId").get<std::string>() == response->get_header_value("X-Request-Id"));
}

TEST_CASE("HTTP status maps backend exceptions to structured internal errors", "[server][http]")
{
    CFakeServerHost host;
    host.ThrowOnStatus = true;
    auto server = StartTestServer(host);
    httplib::Client client("127.0.0.1", server->GetBoundPort());

    const auto response = GetWithRetry(client, "/api/status");

    REQUIRE(response);
    REQUIRE(response->status == 500);
    REQUIRE_FALSE(response->get_header_value("X-Request-Id").empty());
    const auto body = nlohmann::json::parse(response->body);
    REQUIRE(body.at("error").at("code").get<std::string>() == "internal_error");
    REQUIRE(body.at("error").at("requestId").get<std::string>() == response->get_header_value("X-Request-Id"));
}

TEST_CASE("HTTP maps converter timeout to a distinct gateway timeout", "[server][http][converter]")
{
    CFakeServerHost host;
    host.ThrowConverterTimeoutOnStatus = true;
    auto server = StartTestServer(host);
    httplib::Client client("127.0.0.1", server->GetBoundPort());

    const auto response = GetWithRetry(client, "/api/status");

    REQUIRE(response);
    REQUIRE(response->status == 504);
    const auto body = nlohmann::json::parse(response->body);
    REQUIRE(body.at("error").at("code").get<std::string>() == "converter_timeout");
    REQUIRE(body.at("error").at("message").get<std::string>() == "Book conversion timed out.");
}

TEST_CASE("HTTP status maps saturated backend queue to non-500 overload response", "[server][http][limits]")
{
    CFakeServerHost host;
    host.ThrowBackendOverloadOnStatus = true;
    auto server = StartTestServer(host);
    httplib::Client client("127.0.0.1", server->GetBoundPort());

    const auto response = GetWithRetry(client, "/api/status");

    REQUIRE(response);
    REQUIRE(response->status == 429);
    REQUIRE_FALSE(response->get_header_value("X-Request-Id").empty());
    const auto body = nlohmann::json::parse(response->body);
    REQUIRE(body.at("error").at("code").get<std::string>() == "too_many_requests");
    REQUIRE(body.at("error").at("message").get<std::string>().find("queue is full") != std::string::npos);
}

TEST_CASE("HTTP server serves static SPA assets without taking over API errors", "[server][http][static]")
{
    CTestWorkspace workspace("inpx-web-reader-http-static-assets");
    const auto assetRoot = workspace.GetPath() / "web";
    WriteTextFile(assetRoot / "index.html", "<!doctype html><div id=\"root\">InpxWebReader web</div>");
    WriteTextFile(assetRoot / "assets" / "app.js", "console.log('inpx-web-reader');");
    WriteTextFile(assetRoot / "assets" / "app.css", ".app{color:#f4c76b;}");

    CFakeServerHost host;
    auto server = StartTestServer(
        host,
        InpxWebReader::Server::SHttpServerOptions{
            .StaticAssetsRoot = assetRoot,
            .Security = {.TokenUtf8 = "secret"}
        });
    httplib::Client client("127.0.0.1", server->GetBoundPort());

    const auto root = GetWithRetry(client, "/");
    REQUIRE(root);
    REQUIRE(root->status == 200);
    REQUIRE(root->body.find("InpxWebReader web") != std::string::npos);
    REQUIRE(root->get_header_value("Content-Type").find("text/html") != std::string::npos);
    REQUIRE(root->get_header_value("Content-Disposition").empty());

    const auto script = GetWithRetry(client, "/assets/app.js");
    REQUIRE(script);
    REQUIRE(script->status == 200);
    REQUIRE(script->body == "console.log('inpx-web-reader');");
    REQUIRE(script->get_header_value("Content-Type").find("text/javascript") != std::string::npos);

    const auto spaFallback = GetWithRetry(client, "/books/42");
    REQUIRE(spaFallback);
    REQUIRE(spaFallback->status == 200);
    REQUIRE(spaFallback->body == root->body);

    const auto missingAsset = GetWithRetry(client, "/assets/missing.js");
    REQUIRE(missingAsset);
    REQUIRE(missingAsset->status == 404);
    REQUIRE(nlohmann::json::parse(missingAsset->body).at("error").at("code").get<std::string>() == "not_found");

    const auto traversal = GetWithRetry(client, "/..%2Fsecret.txt");
    REQUIRE(traversal);
    REQUIRE(traversal->status == 400);

    const auto unauthenticatedApiError = GetWithRetry(client, "/api/not-found");
    REQUIRE(unauthenticatedApiError);
    REQUIRE(unauthenticatedApiError->status == 401);

    const auto apiError = GetWithRetry(
        client,
        "/api/not-found",
        {{"Authorization", "Bearer secret"}});
    REQUIRE(apiError);
    REQUIRE(apiError->status == 404);
    REQUIRE(nlohmann::json::parse(apiError->body).at("error").at("code").get<std::string>() == "not_found");
}

TEST_CASE("HTTP access log records the resolved static success status", "[server][http][static][logging]")
{
    CTestWorkspace workspace("inpx-web-reader-http-static-access-log");
    const auto assetRoot = workspace.GetPath() / "web";
    const auto logPath = workspace.GetPath() / "server.log";
    WriteTextFile(assetRoot / "index.html", "<!doctype html><div>InpxWebReader web</div>");

    CFakeServerHost host;
    InpxWebReader::Logging::CLogging::InitializeHostLogger(logPath);
    auto server = StartTestServer(
        host,
        InpxWebReader::Server::SHttpServerOptions{.StaticAssetsRoot = assetRoot});
    httplib::Client client("127.0.0.1", server->GetBoundPort());

    const auto response = GetWithRetry(client, "/");
    server->Stop();
    server.reset();
    InpxWebReader::Logging::CLogging::Shutdown();

    REQUIRE(response);
    REQUIRE(response->status == 200);
    REQUIRE(ReadTextFile(logPath).find(" path=/ status=200 ") != std::string::npos);
}

TEST_CASE("HTTP static asset serving rejects oversized pre-auth assets", "[server][http][static][limits]")
{
    CTestWorkspace workspace("inpx-web-reader-http-static-large-asset");
    const auto assetRoot = workspace.GetPath() / "web";
    WriteTextFile(assetRoot / "index.html", "<!doctype html>");
    WriteSparseFile(assetRoot / "assets" / "large.bin", 8ULL * 1024ULL * 1024ULL + 1ULL);

    CFakeServerHost host;
    auto server = StartTestServer(
        host,
        InpxWebReader::Server::SHttpServerOptions{
            .StaticAssetsRoot = assetRoot,
            .Security = {.TokenUtf8 = "secret"}
        });
    httplib::Client client("127.0.0.1", server->GetBoundPort());

    const auto response = GetWithRetry(client, "/assets/large.bin");

    REQUIRE(response);
    REQUIRE(response->status == 413);
    const auto body = nlohmann::json::parse(response->body);
    REQUIRE(body.at("error").at("code").get<std::string>() == "payload_too_large");
}

TEST_CASE("HTTP static asset serving rejects symlinks escaping the asset root", "[server][http][static]")
{
    CTestWorkspace workspace("inpx-web-reader-http-static-symlink");
    const auto assetRoot = workspace.GetPath() / "web";
    const auto outsideRoot = workspace.GetPath() / "outside";
    WriteTextFile(assetRoot / "index.html", "<!doctype html>");
    WriteTextFile(outsideRoot / "secret.js", "outside asset");
    if (!TryCreateDirectorySymlink(outsideRoot, assetRoot / "linked"))
    {
        SKIP("Directory symlinks are unavailable.");
    }

    CFakeServerHost host;
    auto server = StartTestServer(
        host,
        InpxWebReader::Server::SHttpServerOptions{.StaticAssetsRoot = assetRoot});
    httplib::Client client("127.0.0.1", server->GetBoundPort());

    const auto response = GetWithRetry(client, "/linked/secret.js");

    REQUIRE(response);
    REQUIRE(response->status == 404);
    REQUIRE(nlohmann::json::parse(response->body).at("error").at("code").get<std::string>() == "not_found");
}
