#include "Server/HttpServer.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "Domain/BookFormat.hpp"
#include "Domain/DomainError.hpp"
#include "Domain/SearchQuery.hpp"
#include "Foundation/Logging.hpp"
#include "Foundation/StringUtils.hpp"
#include "Foundation/UnicodeConversion.hpp"
#include "Server/AuthMiddleware.hpp"
#include "Server/BookListCursor.hpp"
#include "Server/HttpError.hpp"
#include "Server/JsonDtoMapper.hpp"
#include "Server/ServerScanCoordinator.hpp"
#include "Storage/InpxCacheLayout.hpp"
#include "Storage/PathSafety.hpp"
#include "Storage/RuntimeWorkspaceLayout.hpp"

namespace InpxWebReader::Server {
namespace {

constexpr std::string_view GJsonContentType = "application/json; charset=utf-8";
constexpr std::size_t GMaxScanWarningLimit = 1'000;
constexpr std::uintmax_t GMaxStaticAssetBytes = 8ULL * 1024ULL * 1024ULL;
constexpr std::size_t GMaxLoggedMethodBytes = 32;
constexpr std::size_t GMaxLoggedPathBytes = 512;
constexpr std::size_t GMaxLoggedErrorBytes = 1'024;
constexpr auto GStorageStatusRefreshInterval = std::chrono::seconds{30};
thread_local bool GRejectHttpRequestAsOverloaded = false;

[[nodiscard]] std::int64_t ElapsedMilliseconds(const std::chrono::steady_clock::time_point startedAt) noexcept
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startedAt).count();
}

void SetJsonResponse(
    httplib::Response& response,
    const int status,
    const std::string& bodyUtf8)
{
    response.status = status;
    response.set_content(bodyUtf8, std::string{GJsonContentType});
}

[[nodiscard]] std::optional<std::string> GetAuthorizationHeader(const httplib::Request& request)
{
    auto value = request.get_header_value("Authorization");
    if (value.empty())
    {
        return std::nullopt;
    }

    return value;
}

[[nodiscard]] std::string EscapeLogField(
    const std::string_view valueUtf8,
    const std::size_t maxSourceBytes)
{
    constexpr char hexDigits[] = "0123456789ABCDEF";
    const bool validUtf8 = Unicode::IsValidUtf8(valueUtf8);
    const auto sourceSize = std::min(valueUtf8.size(), maxSourceBytes);
    std::string result;
    result.reserve(sourceSize);

    for (std::size_t index = 0; index < sourceSize; ++index)
    {
        const auto ch = static_cast<unsigned char>(valueUtf8[index]);
        if (ch == '\n')
        {
            result += "\\n";
        }
        else if (ch == '\r')
        {
            result += "\\r";
        }
        else if (ch == '\t')
        {
            result += "\\t";
        }
        else if (ch < 0x20U || ch == 0x7FU || (!validUtf8 && ch >= 0x80U))
        {
            result += "\\x";
            result.push_back(hexDigits[(ch >> 4U) & 0x0FU]);
            result.push_back(hexDigits[ch & 0x0FU]);
        }
        else
        {
            result.push_back(static_cast<char>(ch));
        }
    }

    if (validUtf8)
    {
        if (const auto complete = Unicode::TryTrimTrailingIncompleteUtf8(result))
        {
            result = *complete;
        }
    }
    if (valueUtf8.size() > sourceSize)
    {
        result += "...";
    }
    return result;
}

[[nodiscard]] bool IsSafeRequestText(const std::string_view valueUtf8) noexcept
{
    if (!Unicode::IsValidUtf8(valueUtf8))
    {
        return false;
    }

    return std::none_of(valueUtf8.begin(), valueUtf8.end(), [](const char value) {
        const auto ch = static_cast<unsigned char>(value);
        return ch < 0x20U || ch == 0x7FU;
    });
}

[[nodiscard]] bool HasSafeRequestTarget(const httplib::Request& request) noexcept
{
    if (!IsSafeRequestText(request.method) || !IsSafeRequestText(request.path))
    {
        return false;
    }

    return std::all_of(request.params.begin(), request.params.end(), [](const auto& parameter) {
        return IsSafeRequestText(parameter.first) && IsSafeRequestText(parameter.second);
    });
}

[[nodiscard]] bool IsPublicApiRoute(const httplib::Request& request) noexcept
{
    const bool readMethod = request.method == "GET" || request.method == "HEAD";
    return readMethod && (request.path == "/api/health" || request.path == "/api/ready");
}

[[nodiscard]] bool RequiresApiAuthentication(const httplib::Request& request) noexcept
{
    const bool apiPath = request.path == "/api"
        || request.path.rfind("/api/", 0) == 0;
    return apiPath && !IsPublicApiRoute(request);
}

[[nodiscard]] bool HasRequestBody(const httplib::Request& request)
{
    if (request.has_header("Transfer-Encoding"))
    {
        return true;
    }
    if (!request.has_header("Content-Length"))
    {
        return false;
    }

    const auto value = request.get_header_value("Content-Length");
    std::uint64_t length = 0;
    const auto [pointer, error] = std::from_chars(
        value.data(),
        value.data() + value.size(),
        length);
    return error != std::errc{}
        || pointer != value.data() + value.size()
        || length > 0;
}

[[nodiscard]] std::optional<SHttpError> ValidateRequestBeforeBody(
    const httplib::Request& request,
    const SServerSecurityConfig& security)
{
    if (GRejectHttpRequestAsOverloaded)
    {
        return SHttpError{
            .Status = 503,
            .CodeUtf8 = "server_overloaded",
            .MessageUtf8 = "HTTP request capacity is exhausted; retry later."
        };
    }

    if (!HasSafeRequestTarget(request))
    {
        return CHttpErrorMapper::BadRequest(
            "Request target must be valid UTF-8 without control characters.");
    }

    if (RequiresApiAuthentication(request))
    {
        const auto authResult = CAuthMiddleware::Authorize(security, GetAuthorizationHeader(request));
        if (!authResult.Authorized)
        {
            return CHttpErrorMapper::Unauthorized(authResult.MessageUtf8);
        }
    }

    const bool acceptsBody = request.method == "POST" && request.path == "/api/scan/start";
    if (!acceptsBody && HasRequestBody(request))
    {
        return CHttpErrorMapper::BadRequest("This endpoint does not accept a request body.");
    }

    return std::nullopt;
}

[[nodiscard]] std::string BuildReadinessJson(const bool backendOpen)
{
    return nlohmann::json{
        {"ok", backendOpen},
        {"service", "InpxWebReader"},
        {"status", backendOpen ? "ready" : "starting"}
    }.dump();
}

struct SHttpTaskQueueState
{
    SHttpTaskQueueState(
        const std::size_t maxWorkers,
        const std::size_t maxQueuedRequests)
        : MaxWorkers(maxWorkers)
        , MaxQueuedRequests(maxQueuedRequests)
    {
    }

    const std::size_t MaxWorkers = 0;
    const std::size_t MaxQueuedRequests = 0;
    std::atomic_size_t ActiveWorkers = 0;
    std::atomic_size_t QueuedRequests = 0;
};

class CHttpTaskQueue final : public httplib::TaskQueue
{
public:
    explicit CHttpTaskQueue(std::shared_ptr<SHttpTaskQueueState> state)
        : m_state(std::move(state))
    {
        if (m_state == nullptr || m_state->MaxWorkers == 0)
        {
            throw std::invalid_argument("HTTP worker count must be at least 1.");
        }

        m_workers.reserve(m_state->MaxWorkers);
        for (std::size_t index = 0; index < m_state->MaxWorkers; ++index)
        {
            m_workers.emplace_back([this]() { RunWorker(); });
        }
        m_overloadWorker = std::thread([this]() { RunOverloadWorker(); });
    }

    ~CHttpTaskQueue() override
    {
        shutdown();
    }

    CHttpTaskQueue(const CHttpTaskQueue&) = delete;
    CHttpTaskQueue& operator=(const CHttpTaskQueue&) = delete;

    bool enqueue(std::function<void()> fn) override
    {
        {
            std::scoped_lock lock(m_mutex);
            if (m_stopping)
            {
                return false;
            }

            if (m_state->MaxQueuedRequests > 0 && m_jobs.size() >= m_state->MaxQueuedRequests)
            {
                if (m_overloadJob.has_value())
                {
                    return false;
                }

                m_overloadJob = std::move(fn);
                m_wakeOverloadWorker.notify_one();
                return true;
            }

            m_jobs.push(std::move(fn));
            m_state->QueuedRequests.store(m_jobs.size(), std::memory_order_relaxed);
        }

        m_wakeWorker.notify_one();
        return true;
    }

    void shutdown() override
    {
        {
            std::scoped_lock lock(m_mutex);
            if (m_stopping)
            {
                return;
            }
            m_stopping = true;
        }

        m_wakeWorker.notify_all();
        m_wakeOverloadWorker.notify_all();
        for (auto& worker : m_workers)
        {
            if (worker.joinable())
            {
                worker.join();
            }
        }
        if (m_overloadWorker.joinable())
        {
            m_overloadWorker.join();
        }
    }

private:
    void RunWorker()
    {
        for (;;)
        {
            std::function<void()> job;
            {
                std::unique_lock lock(m_mutex);
                m_wakeWorker.wait(lock, [this]() {
                    return m_stopping || !m_jobs.empty();
                });

                if (m_stopping && m_jobs.empty())
                {
                    return;
                }

                job = std::move(m_jobs.front());
                m_jobs.pop();
                m_state->QueuedRequests.store(m_jobs.size(), std::memory_order_relaxed);
                m_state->ActiveWorkers.fetch_add(1, std::memory_order_relaxed);
            }

            try
            {
                job();
            }
            catch (...)
            {
                m_state->ActiveWorkers.fetch_sub(1, std::memory_order_relaxed);
                throw;
            }
            m_state->ActiveWorkers.fetch_sub(1, std::memory_order_relaxed);
        }
    }

    void RunOverloadWorker()
    {
        for (;;)
        {
            std::function<void()> job;
            {
                std::unique_lock lock(m_mutex);
                m_wakeOverloadWorker.wait(lock, [this]() {
                    return m_stopping || m_overloadJob.has_value();
                });

                if (!m_overloadJob.has_value())
                {
                    return;
                }

                job = std::move(m_overloadJob.value());
                m_overloadJob.reset();
            }

            GRejectHttpRequestAsOverloaded = true;
            job();
            GRejectHttpRequestAsOverloaded = false;
        }
    }

    std::shared_ptr<SHttpTaskQueueState> m_state;
    std::mutex m_mutex;
    std::condition_variable m_wakeWorker;
    std::condition_variable m_wakeOverloadWorker;
    std::queue<std::function<void()>> m_jobs;
    std::optional<std::function<void()>> m_overloadJob;
    std::vector<std::thread> m_workers;
    std::thread m_overloadWorker;
    bool m_stopping = false;
};

[[nodiscard]] SServerHttpRuntimeStatus SnapshotHttpRuntimeStatus(const SHttpTaskQueueState& state) noexcept
{
    return {
        .ActiveWorkers = state.ActiveWorkers.load(std::memory_order_relaxed),
        .QueuedRequests = state.QueuedRequests.load(std::memory_order_relaxed),
        .MaxWorkers = state.MaxWorkers,
        .MaxQueuedRequests = state.MaxQueuedRequests
    };
}

[[nodiscard]] std::optional<std::uint64_t> TryParseProcStatusBytes(
    const std::string& line,
    const std::string_view key)
{
    if (line.rfind(key, 0) != 0)
    {
        return std::nullopt;
    }

    std::size_t offset = key.size();
    while (offset < line.size() && (line[offset] == ' ' || line[offset] == '\t'))
    {
        ++offset;
    }

    std::uint64_t kibibytes = 0;
    const auto [pointer, error] = std::from_chars(line.data() + offset, line.data() + line.size(), kibibytes);
    if (error != std::errc{} || pointer == line.data() + offset)
    {
        return std::nullopt;
    }

    return kibibytes * 1024ULL;
}

[[nodiscard]] SServerResourceRuntimeStatus ReadResourceRuntimeStatus(
    const std::uint64_t maxCoverCacheBytes,
    const std::uint64_t maxSteadyStateMemoryBytes)
{
    SServerResourceRuntimeStatus result{
        .MaxCoverCacheBytes = maxCoverCacheBytes,
        .MaxSteadyStateMemoryBytes = maxSteadyStateMemoryBytes
    };
    std::ifstream stream("/proc/self/status");
    std::string line;
    while (std::getline(stream, line))
    {
        if (auto resident = TryParseProcStatusBytes(line, "VmRSS:"))
        {
            result.ResidentMemoryBytes = resident;
        }
        else if (auto peak = TryParseProcStatusBytes(line, "VmHWM:"))
        {
            result.PeakResidentMemoryBytes = peak;
        }
    }
    return result;
}

[[nodiscard]] bool TryIsDirectory(const std::filesystem::path& path)
{
    if (path.empty())
    {
        return false;
    }

    std::error_code errorCode;
    return std::filesystem::is_directory(path, errorCode) && !errorCode;
}

[[nodiscard]] bool TryIsRegularFile(const std::filesystem::path& path)
{
    if (path.empty())
    {
        return false;
    }

    std::error_code errorCode;
    return std::filesystem::is_regular_file(path, errorCode) && !errorCode;
}

[[nodiscard]] std::optional<std::uint64_t> TryComputeDirectorySize(
    const std::filesystem::path& root,
    const std::stop_token stopToken = {})
{
    if (root.empty())
    {
        return std::nullopt;
    }

    std::error_code errorCode;
    if (!std::filesystem::exists(root, errorCode))
    {
        return errorCode ? std::nullopt : std::optional<std::uint64_t>{0};
    }
    if (!std::filesystem::is_directory(root, errorCode) || errorCode)
    {
        return std::nullopt;
    }

    std::uint64_t total = 0;
    std::filesystem::recursive_directory_iterator iterator(
        root,
        std::filesystem::directory_options::skip_permission_denied,
        errorCode);
    if (errorCode)
    {
        return std::nullopt;
    }

    const std::filesystem::recursive_directory_iterator end;
    for (; iterator != end; iterator.increment(errorCode))
    {
        if (stopToken.stop_requested())
        {
            return std::nullopt;
        }

        if (errorCode)
        {
            errorCode.clear();
            continue;
        }

        if (!iterator->is_regular_file(errorCode) || errorCode)
        {
            errorCode.clear();
            continue;
        }

        const auto fileSize = iterator->file_size(errorCode);
        if (errorCode)
        {
            errorCode.clear();
            continue;
        }

        constexpr auto maxValue = std::numeric_limits<std::uint64_t>::max();
        if (fileSize > maxValue - total)
        {
            return maxValue;
        }
        total += static_cast<std::uint64_t>(fileSize);
    }

    return total;
}

[[nodiscard]] SServerStorageRuntimeStatus BuildStorageRuntimeStatus(
    const std::filesystem::path& cacheRoot,
    const std::filesystem::path& runtimeWorkspaceRoot,
    const std::stop_token stopToken = {})
{
    SServerStorageRuntimeStatus result;
    if (!cacheRoot.empty())
    {
        const auto layout = StoragePlanning::CInpxCacheLayout::Build(cacheRoot);
        result.CacheRootPresent = TryIsDirectory(cacheRoot);
        result.CacheDatabasePresent = TryIsRegularFile(
            StoragePlanning::CInpxCacheLayout::GetDatabasePath(cacheRoot));
        result.CoverCacheBytes = TryComputeDirectorySize(layout.CoversDirectory, stopToken);
    }

    if (!runtimeWorkspaceRoot.empty())
    {
        const auto runtimeLayout = StoragePlanning::BuildRuntimeWorkspaceLayout(runtimeWorkspaceRoot);
        result.RuntimeWorkspacePresent = TryIsDirectory(runtimeWorkspaceRoot);
        result.InpxScanWorkspaceBytes = TryComputeDirectorySize(runtimeLayout.ScanDirectory, stopToken);
        const auto downloadSourceBytes = TryComputeDirectorySize(
            runtimeLayout.DownloadSourceDirectory,
            stopToken);
        const auto serverDownloadBytes = TryComputeDirectorySize(
            runtimeLayout.ServerDownloadDirectory,
            stopToken);
        if (downloadSourceBytes.has_value() && serverDownloadBytes.has_value())
        {
            constexpr auto maxValue = std::numeric_limits<std::uint64_t>::max();
            result.DownloadWorkspaceBytes = *downloadSourceBytes > maxValue - *serverDownloadBytes
                ? maxValue
                : *downloadSourceBytes + *serverDownloadBytes;
        }
    }

    return result;
}

class CDownloadGate final
{
public:
    class CLease final
    {
    public:
        explicit CLease(CDownloadGate& gate)
            : m_gate(&gate)
        {
        }

        ~CLease()
        {
            Release();
        }

        CLease(const CLease&) = delete;
        CLease& operator=(const CLease&) = delete;

        CLease(CLease&& other) noexcept
            : m_gate(std::exchange(other.m_gate, nullptr))
        {
        }

        CLease& operator=(CLease&& other) noexcept
        {
            if (this != &other)
            {
                Release();
                m_gate = std::exchange(other.m_gate, nullptr);
            }
            return *this;
        }

        void Release() noexcept
        {
            if (m_gate != nullptr)
            {
                m_gate->Release();
                m_gate = nullptr;
            }
        }

    private:
        CDownloadGate* m_gate = nullptr;
    };

    explicit CDownloadGate(const std::size_t maxConcurrentDownloads)
        : m_maxConcurrentDownloads(maxConcurrentDownloads)
    {
    }

    [[nodiscard]] std::optional<CLease> TryAcquire()
    {
        std::scoped_lock lock(m_mutex);
        if (m_activeDownloads >= m_maxConcurrentDownloads)
        {
            return std::nullopt;
        }

        ++m_activeDownloads;
        return CLease{*this};
    }

    [[nodiscard]] SServerDownloadRuntimeStatus GetStatus() const
    {
        std::scoped_lock lock(m_mutex);
        return {
            .Active = m_activeDownloads,
            .MaxConcurrent = m_maxConcurrentDownloads
        };
    }

private:
    void Release() noexcept
    {
        std::scoped_lock lock(m_mutex);
        if (m_activeDownloads > 0)
        {
            --m_activeDownloads;
        }
    }

    std::size_t m_maxConcurrentDownloads = 1;
    std::size_t m_activeDownloads = 0;
    mutable std::mutex m_mutex;
};

[[nodiscard]] std::vector<std::string> SplitCsvValues(const std::string& valueUtf8)
{
    std::vector<std::string> result;
    std::string current;
    for (const char ch : valueUtf8)
    {
        if (ch == ',')
        {
            if (!current.empty())
            {
                result.push_back(std::move(current));
                current.clear();
            }
            continue;
        }

        current.push_back(ch);
    }

    if (!current.empty())
    {
        result.push_back(std::move(current));
    }

    return result;
}

[[nodiscard]] std::vector<std::string> ReadCsvQueryValues(
    const httplib::Request& request,
    const std::string& name)
{
    std::vector<std::string> result;
    const auto count = request.get_param_value_count(name);
    for (std::size_t index = 0; index < count; ++index)
    {
        auto values = SplitCsvValues(request.get_param_value(name, index));
        result.insert(
            result.end(),
            std::make_move_iterator(values.begin()),
            std::make_move_iterator(values.end()));
    }
    return result;
}

[[nodiscard]] std::optional<std::string> ReadSingleQueryValue(
    const httplib::Request& request,
    const std::string& name)
{
    if (!request.has_param(name))
    {
        return std::nullopt;
    }

    if (request.get_param_value_count(name) != 1)
    {
        throw CHttpErrorMapper::BadRequest("Query parameter '" + name + "' must appear at most once.");
    }

    return request.get_param_value(name);
}

[[nodiscard]] std::size_t ParseSizeQueryValue(
    const std::string& valueUtf8,
    const std::string_view fieldName)
{
    if (valueUtf8.empty() || valueUtf8.front() == '-')
    {
        throw CHttpErrorMapper::BadRequest("Query parameter '" + std::string{fieldName} + "' must be a non-negative integer.");
    }

    std::size_t value = 0;
    const auto [pointer, error] = std::from_chars(valueUtf8.data(), valueUtf8.data() + valueUtf8.size(), value);
    if (error != std::errc{} || pointer != valueUtf8.data() + valueUtf8.size())
    {
        throw CHttpErrorMapper::BadRequest("Query parameter '" + std::string{fieldName} + "' must be a non-negative integer.");
    }

    return value;
}

[[nodiscard]] bool ParseBoolQueryValue(
    std::string valueUtf8,
    const std::string_view fieldName)
{
    valueUtf8 = Foundation::ToLowerAscii(std::move(valueUtf8));
    if (valueUtf8 == "true" || valueUtf8 == "1" || valueUtf8 == "yes")
    {
        return true;
    }
    if (valueUtf8 == "false" || valueUtf8 == "0" || valueUtf8 == "no")
    {
        return false;
    }

    throw CHttpErrorMapper::BadRequest("Query parameter '" + std::string{fieldName} + "' must be true or false.");
}

[[nodiscard]] Domain::SSearchFieldScope ParseSearchFields(const httplib::Request& request)
{
    const auto fields = ReadSingleQueryValue(request, "fields");
    if (!fields.has_value())
    {
        return {};
    }

    Domain::SSearchFieldScope scope{
        .Title = false,
        .Authors = false,
        .Description = false
    };
    for (const auto& field : SplitCsvValues(*fields))
    {
        const auto normalized = Foundation::ToLowerAscii(field);
        if (normalized == "title")
        {
            scope.Title = true;
        }
        else if (normalized == "authors" || normalized == "author")
        {
            scope.Authors = true;
        }
        else if (normalized == "annotation" || normalized == "description")
        {
            scope.Description = true;
        }
        else
        {
            throw CHttpErrorMapper::BadRequest("Query parameter 'fields' contains an unsupported field.");
        }
    }

    if (!scope.HasAnyField())
    {
        throw CHttpErrorMapper::BadRequest("Query parameter 'fields' must select at least one field.");
    }

    return scope;
}

[[nodiscard]] std::optional<Domain::EBookSort> ParseBookSort(const httplib::Request& request)
{
    const auto value = ReadSingleQueryValue(request, "sort");
    if (!value.has_value() || value->empty())
    {
        return std::nullopt;
    }

    const auto normalized = Foundation::ToLowerAscii(*value);
    if (normalized == "title")
    {
        return Domain::EBookSort::Title;
    }
    if (normalized == "author" || normalized == "authors")
    {
        return Domain::EBookSort::Author;
    }
    if (normalized == "added" || normalized == "dateadded")
    {
        return Domain::EBookSort::DateAdded;
    }
    throw CHttpErrorMapper::BadRequest("Query parameter 'sort' is unsupported.");
}

[[nodiscard]] std::optional<Domain::ESortDirection> ParseSortDirection(const httplib::Request& request)
{
    const auto value = ReadSingleQueryValue(request, "direction");
    if (!value.has_value() || value->empty())
    {
        return std::nullopt;
    }

    const auto normalized = Foundation::ToLowerAscii(*value);
    if (normalized == "asc" || normalized == "ascending")
    {
        return Domain::ESortDirection::Ascending;
    }
    if (normalized == "desc" || normalized == "descending")
    {
        return Domain::ESortDirection::Descending;
    }

    throw CHttpErrorMapper::BadRequest("Query parameter 'direction' must be 'asc' or 'desc'.");
}

[[nodiscard]] std::optional<std::int64_t> ParseOptionalInt64Param(
    const httplib::Request& request,
    const std::string& name)
{
    const auto value = ReadSingleQueryValue(request, name);
    if (!value.has_value() || value->empty())
    {
        return std::nullopt;
    }

    std::int64_t parsed = 0;
    const auto [pointer, error] = std::from_chars(value->data(), value->data() + value->size(), parsed);
    if (error != std::errc{} || pointer != value->data() + value->size())
    {
        throw CHttpErrorMapper::BadRequest("Query parameter '" + name + "' must be an integer.");
    }

    return parsed;
}

[[nodiscard]] Application::SBookListRequest ParseBookListRequest(
    const httplib::Request& request,
    const std::size_t maxPageSize)
{
    Application::SBookListRequest result;
    result.TextUtf8 = ReadSingleQueryValue(request, "text").value_or("");
    result.SearchFields = ParseSearchFields(request);
    result.Languages = ReadCsvQueryValues(request, "languages");
    result.GenresUtf8 = ReadCsvQueryValues(request, "genres");
    result.SortBy = ParseBookSort(request);
    result.SortDirection = ParseSortDirection(request);

    const auto offset = ReadSingleQueryValue(request, "offset");
    if (offset.has_value())
    {
        result.Offset = ParseSizeQueryValue(*offset, "offset");
        if (static_cast<std::uintmax_t>(result.Offset)
            > static_cast<std::uintmax_t>((std::numeric_limits<std::int64_t>::max)()))
        {
            throw CHttpErrorMapper::BadRequest(
                "Query parameter 'offset' exceeds the supported SQLite range.");
        }
    }
    if (const auto limit = ReadSingleQueryValue(request, "limit"))
    {
        result.Limit = std::clamp<std::size_t>(
            ParseSizeQueryValue(*limit, "limit"),
            1,
            maxPageSize);
    }
    else
    {
        result.Limit = std::min(result.Limit, maxPageSize);
    }

    if (const auto includeFacets = ReadSingleQueryValue(request, "includeFacets"))
    {
        result.IncludeFacets = ParseBoolQueryValue(*includeFacets, "includeFacets");
        result.IncludeLanguageFacets = result.IncludeFacets;
        result.IncludeGenreFacets = result.IncludeFacets;
    }

    if (const auto cursor = ReadSingleQueryValue(request, "cursor"))
    {
        if (offset.has_value())
        {
            throw CHttpErrorMapper::BadRequest(
                "Query parameters 'cursor' and 'offset' cannot be combined.");
        }
        try
        {
            result.Cursor = CBookListCursorCodec::Decode(*cursor, result);
        }
        catch (const std::invalid_argument& error)
        {
            throw SHttpError{
                .Status = 400,
                .CodeUtf8 = "invalid_cursor",
                .MessageUtf8 = error.what()
            };
        }
    }

    return result;
}

[[nodiscard]] Domain::SBookId ParseBookId(std::string_view value)
{
    if (value.empty() || value.front() == '-')
    {
        throw CHttpErrorMapper::BadRequest("Book id must be a positive integer.");
    }

    std::int64_t parsed = 0;
    const auto [pointer, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || pointer != value.data() + value.size() || parsed <= 0)
    {
        throw CHttpErrorMapper::BadRequest("Book id must be a positive integer.");
    }

    return Domain::SBookId{parsed};
}

[[nodiscard]] EServerDownloadFormat ParseDownloadFormat(const httplib::Request& request)
{
    const auto value = Foundation::ToLowerAscii(ReadSingleQueryValue(request, "format").value_or("original"));
    if (value == "original")
    {
        return EServerDownloadFormat::Original;
    }
    if (value == "epub")
    {
        return EServerDownloadFormat::Epub;
    }

    throw CHttpErrorMapper::BadRequest("Query parameter 'format' must be 'original' or 'epub'.");
}

[[nodiscard]] bool IsHeaderAttrChar(const unsigned char ch) noexcept
{
    return (ch >= '0' && ch <= '9')
        || (ch >= 'A' && ch <= 'Z')
        || (ch >= 'a' && ch <= 'z')
        || ch == '!'
        || ch == '#'
        || ch == '$'
        || ch == '&'
        || ch == '+'
        || ch == '-'
        || ch == '.'
        || ch == '^'
        || ch == '_'
        || ch == '`'
        || ch == '|'
        || ch == '~';
}

[[nodiscard]] std::string PercentEncodeHeaderValue(const std::string& valueUtf8)
{
    constexpr char digits[] = "0123456789ABCDEF";
    std::string result;
    for (const unsigned char ch : valueUtf8)
    {
        if (IsHeaderAttrChar(ch))
        {
            result.push_back(static_cast<char>(ch));
            continue;
        }

        result.push_back('%');
        result.push_back(digits[(ch >> 4) & 0x0F]);
        result.push_back(digits[ch & 0x0F]);
    }
    return result;
}

[[nodiscard]] std::string BuildAsciiFilenameFallback(const std::string& valueUtf8)
{
    std::string result;
    for (const unsigned char ch : valueUtf8)
    {
        if ((ch >= '0' && ch <= '9')
            || (ch >= 'A' && ch <= 'Z')
            || (ch >= 'a' && ch <= 'z')
            || ch == '.'
            || ch == '-'
            || ch == '_')
        {
            result.push_back(static_cast<char>(ch));
        }
        else
        {
            result.push_back('_');
        }
    }

    return result.empty() ? "book" : result;
}

[[nodiscard]] std::string BuildContentDisposition(const std::string& fileNameUtf8)
{
    return "attachment; filename=\"" + BuildAsciiFilenameFallback(fileNameUtf8)
        + "\"; filename*=UTF-8''" + PercentEncodeHeaderValue(fileNameUtf8);
}

struct SStaticAssetResponse
{
    std::filesystem::path Path;
    std::string ContentTypeUtf8;
};

[[nodiscard]] int HexValue(const char ch) noexcept
{
    if (ch >= '0' && ch <= '9')
    {
        return ch - '0';
    }
    if (ch >= 'A' && ch <= 'F')
    {
        return ch - 'A' + 10;
    }
    if (ch >= 'a' && ch <= 'f')
    {
        return ch - 'a' + 10;
    }
    return -1;
}

[[nodiscard]] std::string DecodeStaticPathSegment(const std::string_view value)
{
    std::string result;
    result.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index)
    {
        const char ch = value[index];
        if (ch != '%')
        {
            result.push_back(ch);
            continue;
        }

        if (index + 2 >= value.size())
        {
            throw CHttpErrorMapper::BadRequest("Static asset path is invalid.");
        }

        const int high = HexValue(value[index + 1]);
        const int low = HexValue(value[index + 2]);
        if (high < 0 || low < 0)
        {
            throw CHttpErrorMapper::BadRequest("Static asset path is invalid.");
        }

        result.push_back(static_cast<char>((high << 4) | low));
        index += 2;
    }
    return result;
}

[[nodiscard]] bool IsUnsafeStaticSegment(const std::string_view segment) noexcept
{
    return segment.empty()
        || segment == "."
        || segment == ".."
        || segment.front() == '.'
        || segment.find('/') != std::string_view::npos
        || segment.find('\\') != std::string_view::npos
        || segment.find(':') != std::string_view::npos;
}

[[nodiscard]] bool HasRequestedFileExtension(const std::string_view requestPath) noexcept
{
    const auto slash = requestPath.find_last_of('/');
    const auto segmentStart = slash == std::string_view::npos ? 0 : slash + 1;
    const auto dot = requestPath.find_last_of('.');
    return dot != std::string_view::npos && dot >= segmentStart + 1;
}

[[nodiscard]] std::optional<std::filesystem::path> TryResolveStaticFileWithinRoot(
    const std::filesystem::path& root,
    const std::filesystem::path& candidate)
{
    try
    {
        const auto resolved = SafePaths::TryResolvePathWithinRoot(
            root,
            candidate,
            "Static asset path escaped the configured root.",
            "Static asset path could not be canonicalized.");
        if (!resolved.has_value())
        {
            return std::nullopt;
        }

        std::error_code errorCode;
        return std::filesystem::is_regular_file(*resolved, errorCode) && !errorCode
            ? resolved
            : std::nullopt;
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
}

[[nodiscard]] std::string GuessStaticContentType(const std::filesystem::path& path)
{
    auto extension = Foundation::ToLowerAscii(Unicode::PathToUtf8(path.extension()));
    if (extension == ".html" || extension == ".htm")
    {
        return "text/html; charset=utf-8";
    }
    if (extension == ".js" || extension == ".mjs")
    {
        return "text/javascript; charset=utf-8";
    }
    if (extension == ".css")
    {
        return "text/css; charset=utf-8";
    }
    if (extension == ".json" || extension == ".map")
    {
        return "application/json; charset=utf-8";
    }
    if (extension == ".svg")
    {
        return "image/svg+xml";
    }
    if (extension == ".png")
    {
        return "image/png";
    }
    if (extension == ".jpg" || extension == ".jpeg")
    {
        return "image/jpeg";
    }
    if (extension == ".webp")
    {
        return "image/webp";
    }
    if (extension == ".ico")
    {
        return "image/x-icon";
    }
    if (extension == ".woff2")
    {
        return "font/woff2";
    }
    if (extension == ".txt")
    {
        return "text/plain; charset=utf-8";
    }
    return "application/octet-stream";
}

[[nodiscard]] SStaticAssetResponse ResolveStaticAsset(
    const std::filesystem::path& root,
    const std::string_view requestPath)
{
    std::filesystem::path candidate = root;
    bool hasSegments = false;
    std::size_t segmentStart = 1;

    while (segmentStart <= requestPath.size())
    {
        const auto slash = requestPath.find('/', segmentStart);
        const auto segmentEnd = slash == std::string_view::npos ? requestPath.size() : slash;
        const auto rawSegment = requestPath.substr(segmentStart, segmentEnd - segmentStart);
        if (!rawSegment.empty())
        {
            const auto decodedSegment = DecodeStaticPathSegment(rawSegment);
            if (IsUnsafeStaticSegment(decodedSegment))
            {
                throw CHttpErrorMapper::BadRequest("Static asset path is invalid.");
            }

            candidate /= Unicode::PathFromUtf8(decodedSegment);
            hasSegments = true;
        }

        if (slash == std::string_view::npos)
        {
            break;
        }
        segmentStart = slash + 1;
    }

    if (!hasSegments)
    {
        candidate = root / "index.html";
    }

    if (const auto resolvedCandidate = TryResolveStaticFileWithinRoot(root, candidate))
    {
        return {
            .Path = *resolvedCandidate,
            .ContentTypeUtf8 = GuessStaticContentType(*resolvedCandidate)
        };
    }

    if (HasRequestedFileExtension(requestPath))
    {
        throw CHttpErrorMapper::NotFound("Static asset was not found.");
    }

    const auto fallback = root / "index.html";
    const auto resolvedFallback = TryResolveStaticFileWithinRoot(root, fallback);
    if (!resolvedFallback.has_value())
    {
        throw CHttpErrorMapper::NotFound("Static asset was not found.");
    }

    return {
        .Path = *resolvedFallback,
        .ContentTypeUtf8 = GuessStaticContentType(*resolvedFallback)
    };
}

[[nodiscard]] std::chrono::milliseconds ToMilliseconds(const std::size_t value)
{
    return std::chrono::milliseconds{
        static_cast<std::chrono::milliseconds::rep>(value)
    };
}

[[nodiscard]] std::uint64_t MebibytesToBytes(const std::size_t value) noexcept
{
    constexpr std::uint64_t bytesPerMebibyte = 1024ULL * 1024ULL;
    constexpr auto maxValue = std::numeric_limits<std::uint64_t>::max();
    return value > maxValue / bytesPerMebibyte
        ? maxValue
        : static_cast<std::uint64_t>(value) * bytesPerMebibyte;
}

[[nodiscard]] SHttpError MapDomainError(const Domain::CDomainException& error)
{
    switch (error.Code())
    {
    case Domain::EDomainErrorCode::Validation:
        return CHttpErrorMapper::BadRequest(error.what());
    case Domain::EDomainErrorCode::NotFound:
        return CHttpErrorMapper::NotFound(error.what());
    case Domain::EDomainErrorCode::ConverterUnavailable:
    case Domain::EDomainErrorCode::Cancellation:
        return CHttpErrorMapper::Conflict(error.what());
    case Domain::EDomainErrorCode::ConverterTimeout:
        return {
            .Status = 504,
            .CodeUtf8 = "converter_timeout",
            .MessageUtf8 = error.what()
        };
    case Domain::EDomainErrorCode::CatalogSnapshotChanged:
        return {
            .Status = 409,
            .CodeUtf8 = "catalog_snapshot_changed",
            .MessageUtf8 = error.what()
        };
    case Domain::EDomainErrorCode::CatalogCursorExpired:
        return {
            .Status = 409,
            .CodeUtf8 = "catalog_cursor_expired",
            .MessageUtf8 = error.what()
        };
    case Domain::EDomainErrorCode::CatalogCursorCapacityExceeded:
        return {
            .Status = 409,
            .CodeUtf8 = "catalog_cursor_capacity_exceeded",
            .MessageUtf8 = error.what()
        };
    case Domain::EDomainErrorCode::CatalogCursorInvalid:
        return {
            .Status = 400,
            .CodeUtf8 = "invalid_cursor",
            .MessageUtf8 = error.what()
        };
    case Domain::EDomainErrorCode::ParserFailure:
    case Domain::EDomainErrorCode::ConverterFailed:
    case Domain::EDomainErrorCode::IntegrityIssue:
        return CHttpErrorMapper::InternalServerError();
    }

    return CHttpErrorMapper::InternalServerError();
}

[[nodiscard]] ApplicationJobs::EInpxScanMode ParseScanMode(const std::string& modeUtf8)
{
    if (modeUtf8 == "initial" || modeUtf8 == "initial_scan")
    {
        return ApplicationJobs::EInpxScanMode::InitialScan;
    }

    if (modeUtf8 == "rescan")
    {
        return ApplicationJobs::EInpxScanMode::Rescan;
    }

    throw CHttpErrorMapper::BadRequest("Field 'mode' must be 'initial', 'initial_scan', or 'rescan'.");
}

[[nodiscard]] ApplicationJobs::SInpxScanRequest ParseScanStartRequest(const httplib::Request& request)
{
    ApplicationJobs::SInpxScanRequest result;
    if (request.body.empty())
    {
        return result;
    }

    nlohmann::json root;
    try
    {
        root = nlohmann::json::parse(request.body);
    }
    catch (const nlohmann::json::exception& ex)
    {
        throw CHttpErrorMapper::BadRequest(std::string{"Request JSON is invalid: "} + ex.what());
    }

    if (!root.is_object())
    {
        throw CHttpErrorMapper::BadRequest("Request body must be a JSON object.");
    }

    if (const auto iterator = root.find("mode"); iterator != root.end() && !iterator->is_null())
    {
        if (!iterator->is_string())
        {
            throw CHttpErrorMapper::BadRequest("Field 'mode' must be a string.");
        }
        result.Mode = ParseScanMode(iterator->get<std::string>());
    }

    if (const auto iterator = root.find("warningLimit"); iterator != root.end() && !iterator->is_null())
    {
        if (iterator->is_number_unsigned())
        {
            result.WarningLimit = iterator->get<std::size_t>();
        }
        else if (iterator->is_number_integer())
        {
            const auto value = iterator->get<long long>();
            if (value < 0)
            {
                throw CHttpErrorMapper::BadRequest("Field 'warningLimit' must be an unsigned integer.");
            }
            result.WarningLimit = static_cast<std::size_t>(value);
        }
        else
        {
            throw CHttpErrorMapper::BadRequest("Field 'warningLimit' must be an unsigned integer.");
        }

        if (result.WarningLimit > GMaxScanWarningLimit)
        {
            throw CHttpErrorMapper::BadRequest("Field 'warningLimit' exceeds the server maximum.");
        }
    }

    return result;
}

} // namespace

SHttpServerOptions BuildHttpServerOptions(const SServerConfig& config)
{
    return {
        .HostUtf8 = config.Server.HostUtf8,
        .Port = config.Server.Port,
        .MaxThreads = config.Limits.MaxHttpThreads,
        .MaxQueuedRequests = config.Limits.MaxHttpQueuedRequests,
        .MaxPageSize = config.Limits.MaxPageSize,
        .MaxConcurrentDownloads = config.Limits.MaxConcurrentDownloads,
        .MaxRequestBodyBytes = config.Limits.MaxRequestBodyBytes,
        .ReadTimeoutMs = config.Limits.HttpReadTimeoutMs,
        .WriteTimeoutMs = config.Limits.HttpWriteTimeoutMs,
        .StaticAssetsRoot = config.Server.StaticAssetsRoot,
        .Security = config.Security,
        .CacheRoot = config.CacheRoot,
        .RuntimeWorkspaceRoot = config.RuntimeWorkspaceRoot,
        .MaxScanWorkers = config.Limits.MaxScanWorkers,
        .MaxCoverCacheBytes = MebibytesToBytes(config.Limits.MaxCoverCacheMiB),
        .MaxSteadyStateMemoryBytes = MebibytesToBytes(config.Limits.MaxSteadyStateMemoryMiB)
    };
}

struct CHttpServer::SImpl
{
    struct SFileResponseWithLease
    {
        SServerFileResponse File;
        std::optional<CDownloadGate::CLease> DownloadLease = std::nullopt;
    };

    class CFileResponseLifetime final
    {
    public:
        explicit CFileResponseLifetime(SFileResponseWithLease& file)
            : m_cleanupRoot(std::move(file.File.CleanupRoot))
            , m_downloadLease(std::move(file.DownloadLease))
        {
        }

        ~CFileResponseLifetime()
        {
            if (!m_cleanupRoot.has_value())
            {
                return;
            }

            std::error_code cleanupError;
            std::filesystem::remove_all(*m_cleanupRoot, cleanupError);
            if (cleanupError)
            {
                Logging::WarnIfInitialized(
                    "Server download cleanup failed: path='{}' error='{}'",
                    EscapeLogField(Unicode::PathToUtf8(*m_cleanupRoot), GMaxLoggedErrorBytes),
                    EscapeLogField(cleanupError.message(), GMaxLoggedErrorBytes));
            }
        }

        CFileResponseLifetime(const CFileResponseLifetime&) = delete;
        CFileResponseLifetime& operator=(const CFileResponseLifetime&) = delete;

    private:
        std::optional<std::filesystem::path> m_cleanupRoot;
        std::optional<CDownloadGate::CLease> m_downloadLease;
    };

    using FJsonHandler = std::function<std::string(const httplib::Request&)>;
    using FFileHandler = std::function<SFileResponseWithLease(const httplib::Request&)>;

    SImpl(IServerApplicationHost& applicationHost, SHttpServerOptions options)
        : ApplicationHost(applicationHost)
        , ScanCoordinator(applicationHost)
        , DownloadGate(options.MaxConcurrentDownloads)
        , Options(std::move(options))
        , HttpQueueState(std::make_shared<SHttpTaskQueueState>(
              Options.MaxThreads,
              Options.MaxQueuedRequests))
        , CachedStorageStatus(BuildStorageRuntimeStatus(
              Options.CacheRoot,
              Options.RuntimeWorkspaceRoot))
    {
        Server.new_task_queue = [state = HttpQueueState]() {
            return new CHttpTaskQueue(state);
        };
        Server.set_payload_max_length(Options.MaxRequestBodyBytes);
        Server.set_read_timeout(ToMilliseconds(Options.ReadTimeoutMs));
        Server.set_write_timeout(ToMilliseconds(Options.WriteTimeoutMs));
        if (!Options.Security.TokenUtf8.empty())
        {
            // Authenticated LAN deployments close each request so a rejected body cannot
            // retain its worker in the library's keep-alive loop without being consumed.
            Server.set_keep_alive_max_count(1);
        }
        Server.set_pre_routing_handler(
            [this](const httplib::Request& request, httplib::Response& response) {
                return HandleRequestBeforeBody(request, response);
            });
        Server.set_expect_100_continue_handler(
            [this](const httplib::Request& request, httplib::Response& response) {
                return HandleExpectContinue(request, response);
            });
        RegisterRoutes();
    }

    void SetEarlyErrorResponse(
        const httplib::Request& request,
        httplib::Response& response,
        const SHttpError& error)
    {
        const auto requestId = NextRequestId();
        response.set_header("X-Request-Id", requestId);
        response.set_header("Connection", "close");
        if (error.CodeUtf8 == "server_overloaded")
        {
            response.set_header("Retry-After", "1");
        }
        SetJsonResponse(response, error.Status, CJsonDtoMapper::BuildErrorJson(error, requestId));
        LogRequest(request, response.status, requestId, 0);
    }

    [[nodiscard]] httplib::Server::HandlerResponse HandleRequestBeforeBody(
        const httplib::Request& request,
        httplib::Response& response)
    {
        const auto error = ValidateRequestBeforeBody(request, Options.Security);
        if (!error.has_value())
        {
            return httplib::Server::HandlerResponse::Unhandled;
        }

        SetEarlyErrorResponse(request, response, *error);
        return httplib::Server::HandlerResponse::Handled;
    }

    [[nodiscard]] int HandleExpectContinue(
        const httplib::Request& request,
        httplib::Response& response)
    {
        const auto error = ValidateRequestBeforeBody(request, Options.Security);
        if (!error.has_value())
        {
            return 100;
        }

        SetEarlyErrorResponse(request, response, *error);
        return error->Status;
    }

    void RegisterRoutes()
    {
        Server.Get("/api/health", [this](const httplib::Request& request, httplib::Response& response) {
            HandleJsonRequest(
                request,
                response,
                false,
                false,
                200,
                [this](const httplib::Request&) {
                    return CJsonDtoMapper::BuildHealthJson(ApplicationHost.IsOpen());
                });
        });

        Server.Get("/api/ready", [this](const httplib::Request& request, httplib::Response& response) {
            HandleDynamicRequest(
                request,
                response,
                false,
                false,
                [this](const httplib::Request&) {
                    return ApplicationHost.IsOpen();
                },
                [](httplib::Response& target, const bool ready) {
                    SetJsonResponse(target, ready ? 200 : 503, BuildReadinessJson(ready));
                });
        });

        Server.Get("/api/version", [this](const httplib::Request& request, httplib::Response& response) {
            HandleJsonRequest(
                request,
                response,
                true,
                false,
                200,
                [](const httplib::Request&) {
                    return CJsonDtoMapper::BuildVersionJson();
                });
        });

        Server.Get("/api/status", [this](const httplib::Request& request, httplib::Response& response) {
            HandleJsonRequest(
                request,
                response,
                true,
                true,
                200,
                [this](const httplib::Request&) {
                    auto status = ApplicationHost.GetStatus();
                    status.ActiveScan = ScanCoordinator.PollProgress().Snapshot;
                    auto runtime = BuildRuntimeStatus();
                    runtime.Backend = status.Runtime.Backend;
                    status.Runtime = runtime;
                    return CJsonDtoMapper::BuildStatusJson(status);
                });
        });

        Server.Get("/api/books", [this](const httplib::Request& request, httplib::Response& response) {
            HandleJsonRequest(
                request,
                response,
                true,
                true,
                200,
                [this](const httplib::Request& listRequest) {
                    const auto bookListRequest = ParseBookListRequest(listRequest, Options.MaxPageSize);
                    return CJsonDtoMapper::BuildBooksJson(
                        ApplicationHost.ListBooks(bookListRequest),
                        bookListRequest);
                });
        });

        Server.Get(R"(/api/books/([^/]+)/download)", [this](const httplib::Request& request, httplib::Response& response) {
            HandleFileRequest(
                request,
                response,
                true,
                true,
                [this](const httplib::Request& downloadRequest) {
                    auto lease = DownloadGate.TryAcquire();
                    if (!lease.has_value())
                    {
                        throw CHttpErrorMapper::TooManyRequests("Too many downloads are already active.");
                    }

                    const auto bookId = ParseBookId(downloadRequest.matches[1].str());
                    auto file = ApplicationHost.PrepareBookDownload(
                        bookId,
                        ParseDownloadFormat(downloadRequest));
                    if (!file.has_value())
                    {
                        throw CHttpErrorMapper::NotFound("Book was not found.");
                    }

                    return SFileResponseWithLease{
                        .File = std::move(*file),
                        .DownloadLease = std::move(lease)
                    };
                });
        });

        Server.Get(R"(/api/books/([^/]+))", [this](const httplib::Request& request, httplib::Response& response) {
            HandleJsonRequest(
                request,
                response,
                true,
                true,
                200,
                [this](const httplib::Request& detailsRequest) {
                    const auto details = ApplicationHost.GetBookDetails(
                        ParseBookId(detailsRequest.matches[1].str()));
                    if (!details.has_value())
                    {
                        throw CHttpErrorMapper::NotFound("Book was not found.");
                    }

                    return CJsonDtoMapper::BuildBookDetailsJson(*details);
                });
        });

        const auto rejectMalformedBookIdPath = [this](
            const httplib::Request& request,
            httplib::Response& response) {
            HandleJsonRequest(
                request,
                response,
                true,
                false,
                200,
                [](const httplib::Request&) -> std::string {
                    throw CHttpErrorMapper::BadRequest("Book id must be a positive integer.");
                });
        };

        Server.Get(R"(/api/books/.+/download)", rejectMalformedBookIdPath);
        Server.Get(R"(/api/books/.+)", rejectMalformedBookIdPath);

        Server.Get("/api/stats", [this](const httplib::Request& request, httplib::Response& response) {
            HandleJsonRequest(
                request,
                response,
                true,
                true,
                200,
                [this](const httplib::Request&) {
                    return CJsonDtoMapper::BuildStatisticsJson(ApplicationHost.GetCatalogStatistics());
                });
        });

        Server.Get(R"(/api/covers/([^/]+))", [this](const httplib::Request& request, httplib::Response& response) {
            HandleFileRequest(
                request,
                response,
                true,
                true,
                [this](const httplib::Request& coverRequest) {
                    auto file = ApplicationHost.ResolveBookCover(ParseBookId(coverRequest.matches[1].str()));
                    if (!file.has_value())
                    {
                        throw CHttpErrorMapper::NotFound("Book cover was not found.");
                    }

                    return SFileResponseWithLease{
                        .File = std::move(*file),
                        .DownloadLease = std::nullopt
                    };
                });
        });

        Server.Get(R"(/api/covers/.+)", rejectMalformedBookIdPath);

        Server.Get("/api/source", [this](const httplib::Request& request, httplib::Response& response) {
            HandleJsonRequest(
                request,
                response,
                true,
                true,
                200,
                [this](const httplib::Request&) {
                    return CJsonDtoMapper::BuildSourceJson(ApplicationHost.GetInpxSourceOverview());
                });
        });

        Server.Post("/api/scan/start", [this](const httplib::Request& request, httplib::Response& response) {
            HandleJsonRequest(
                request,
                response,
                true,
                true,
                202,
                [this](const httplib::Request& jsonRequest) {
                    return CJsonDtoMapper::BuildScanStartJson(
                        ScanCoordinator.StartScan(ParseScanStartRequest(jsonRequest)));
                });
        });

        Server.Get("/api/scan/progress", [this](const httplib::Request& request, httplib::Response& response) {
            HandleJsonRequest(
                request,
                response,
                true,
                true,
                200,
                [this](const httplib::Request&) {
                    return CJsonDtoMapper::BuildScanProgressJson(ScanCoordinator.PollProgress());
                });
        });

        Server.Post("/api/scan/cancel", [this](const httplib::Request& request, httplib::Response& response) {
            HandleJsonRequest(
                request,
                response,
                true,
                true,
                202,
                [this](const httplib::Request&) {
                    return CJsonDtoMapper::BuildScanCancelJson(ScanCoordinator.CancelActiveScan());
                });
        });

        if (Options.StaticAssetsRoot.has_value())
        {
            Server.Get(R"(/(?!api(/|$)).*)", [this](const httplib::Request& request, httplib::Response& response) {
                HandleStaticRequest(request, response);
            });
        }

        Server.set_error_handler([this](const httplib::Request& request, httplib::Response& response) {
            if (response.status == 413)
            {
                const auto requestId = NextRequestId();
                response.set_header("X-Request-Id", requestId);
                const auto error = CHttpErrorMapper::PayloadTooLarge("Request body exceeds the configured limit.");
                SetJsonResponse(response, error.Status, CJsonDtoMapper::BuildErrorJson(error, requestId));
                LogRequest(request, response.status, requestId, 0);
                return httplib::Server::HandlerResponse::Handled;
            }

            if (!request.matched_route.empty() || response.status != 404)
            {
                return httplib::Server::HandlerResponse::Unhandled;
            }

            const auto requestId = NextRequestId();
            response.set_header("X-Request-Id", requestId);
            const auto error = CHttpErrorMapper::NotFound("Endpoint not found.");
            SetJsonResponse(response, error.Status, CJsonDtoMapper::BuildErrorJson(error, requestId));
            LogRequest(request, response.status, requestId, 0);
            return httplib::Server::HandlerResponse::Handled;
        });
    }

    template <typename THandler, typename TSuccessWriter>
    void HandleDynamicRequest(
        const httplib::Request& request,
        httplib::Response& response,
        const bool requiresAuth,
        const bool requiresBackend,
        THandler&& handler,
        TSuccessWriter&& writeSuccess)
    {
        const auto startedAt = std::chrono::steady_clock::now();
        const auto requestId = NextRequestId();
        response.set_header("X-Request-Id", requestId);

        try
        {
            if (requiresAuth)
            {
                const auto authResult = CAuthMiddleware::Authorize(Options.Security, GetAuthorizationHeader(request));
                if (!authResult.Authorized)
                {
                    throw CHttpErrorMapper::Unauthorized(authResult.MessageUtf8);
                }
            }

            if (requiresBackend && !ApplicationHost.IsOpen())
            {
                throw CHttpErrorMapper::BackendNotReady();
            }

            auto result = std::invoke(std::forward<THandler>(handler), request);
            std::invoke(std::forward<TSuccessWriter>(writeSuccess), response, std::move(result));
        }
        catch (const SHttpError& error)
        {
            SetJsonResponse(response, error.Status, CJsonDtoMapper::BuildErrorJson(error, requestId));
        }
        catch (const Domain::CDomainException& error)
        {
            const auto httpError = MapDomainError(error);
            if (httpError.Status >= 500)
            {
                Logging::ErrorIfInitialized(
                    "HTTP dynamic request failed: requestId={} method={} path={} domainError={} message={}",
                    requestId,
                    EscapeLogField(request.method, GMaxLoggedMethodBytes),
                    EscapeLogField(request.path, GMaxLoggedPathBytes),
                    Domain::ToString(error.Code()),
                    EscapeLogField(error.what(), GMaxLoggedErrorBytes));
            }
            SetJsonResponse(response, httpError.Status, CJsonDtoMapper::BuildErrorJson(httpError, requestId));
        }
        catch (const CBackendOverloadError& ex)
        {
            const auto error = CHttpErrorMapper::TooManyRequests(ex.what());
            SetJsonResponse(response, error.Status, CJsonDtoMapper::BuildErrorJson(error, requestId));
        }
        catch (const std::exception& ex)
        {
            Logging::ErrorIfInitialized(
                "HTTP dynamic request failed: requestId={} method={} path={} error={}",
                requestId,
                EscapeLogField(request.method, GMaxLoggedMethodBytes),
                EscapeLogField(request.path, GMaxLoggedPathBytes),
                EscapeLogField(ex.what(), GMaxLoggedErrorBytes));
            const auto error = CHttpErrorMapper::InternalServerError();
            SetJsonResponse(response, error.Status, CJsonDtoMapper::BuildErrorJson(error, requestId));
        }

        LogRequest(request, response.status, requestId, ElapsedMilliseconds(startedAt));
    }

    void HandleJsonRequest(
        const httplib::Request& request,
        httplib::Response& response,
        const bool requiresAuth,
        const bool requiresBackend,
        const int successStatus,
        const FJsonHandler& handler)
    {
        HandleDynamicRequest(
            request,
            response,
            requiresAuth,
            requiresBackend,
            handler,
            [successStatus](httplib::Response& target, const std::string& body) {
                SetJsonResponse(target, successStatus, body);
            });
    }

    void HandleFileRequest(
        const httplib::Request& request,
        httplib::Response& response,
        const bool requiresAuth,
        const bool requiresBackend,
        const FFileHandler& handler)
    {
        HandleDynamicRequest(
            request,
            response,
            requiresAuth,
            requiresBackend,
            handler,
            [&request](httplib::Response& target, SFileResponseWithLease file) {
                SetFileResponse(request, target, std::move(file));
            });
    }

    void HandleStaticRequest(
        const httplib::Request& request,
        httplib::Response& response)
    {
        const auto startedAt = std::chrono::steady_clock::now();
        const auto requestId = NextRequestId();
        response.set_header("X-Request-Id", requestId);

        try
        {
            if (!Options.StaticAssetsRoot.has_value())
            {
                throw CHttpErrorMapper::NotFound("Static assets are not configured.");
            }

            SetStaticAssetResponse(
                response,
                ResolveStaticAsset(*Options.StaticAssetsRoot, request.path));
        }
        catch (const SHttpError& error)
        {
            SetJsonResponse(response, error.Status, CJsonDtoMapper::BuildErrorJson(error, requestId));
        }
        catch (const std::exception& ex)
        {
            Logging::ErrorIfInitialized(
                "HTTP static asset request failed: requestId={} method={} path={} error={}",
                requestId,
                EscapeLogField(request.method, GMaxLoggedMethodBytes),
                EscapeLogField(request.path, GMaxLoggedPathBytes),
                EscapeLogField(ex.what(), GMaxLoggedErrorBytes));
            const auto error = CHttpErrorMapper::InternalServerError();
            SetJsonResponse(response, error.Status, CJsonDtoMapper::BuildErrorJson(error, requestId));
        }

        LogRequest(request, response.status, requestId, ElapsedMilliseconds(startedAt));
    }

    static void SetStaticAssetResponse(
        httplib::Response& response,
        const SStaticAssetResponse& file)
    {
        std::error_code errorCode;
        const auto fileSize = std::filesystem::file_size(file.Path, errorCode);
        if (errorCode)
        {
            throw CHttpErrorMapper::NotFound("Static asset was not found.");
        }
        if (fileSize > GMaxStaticAssetBytes)
        {
            throw CHttpErrorMapper::PayloadTooLarge("Static asset exceeds the configured safety limit.");
        }

        std::ifstream stream(file.Path, std::ios::binary);
        if (!stream)
        {
            throw CHttpErrorMapper::NotFound("Static asset was not found.");
        }

        const std::string content(
            (std::istreambuf_iterator<char>(stream)),
            std::istreambuf_iterator<char>());
        response.set_header(
            "Cache-Control",
            file.ContentTypeUtf8.rfind("text/html", 0) == 0
                ? "no-cache"
                : "public, max-age=31536000, immutable");
        response.status = 200;
        response.set_content(content, file.ContentTypeUtf8);
    }

    [[nodiscard]] static bool NormalizeRequestedRanges(
        const httplib::Request& request,
        const std::uintmax_t fileSize)
    {
        if (request.ranges.empty())
        {
            return true;
        }
        if (fileSize == 0)
        {
            const_cast<httplib::Request&>(request).ranges.clear();
            return false;
        }

        // cpp-httplib normalizes this mutable request state after handlers return;
        // normalize first so oversized suffixes and mixed range sets follow RFC 9110.
        auto& ranges = const_cast<httplib::Request&>(request).ranges;
        if (ranges.size() > CPPHTTPLIB_RANGE_MAX_COUNT)
        {
            ranges.clear();
            return false;
        }
        using TRangeOffset = std::decay_t<decltype(ranges.front().first)>;
        if (fileSize > static_cast<std::uintmax_t>((std::numeric_limits<TRangeOffset>::max)()))
        {
            ranges.clear();
            return false;
        }

        const auto lastByte = static_cast<TRangeOffset>(fileSize - 1);
        bool hasReversedRange = false;
        ranges.erase(
            std::remove_if(ranges.begin(), ranges.end(), [fileSize, lastByte, &hasReversedRange](auto& range) {
                auto& [first, last] = range;
                if (first < 0)
                {
                    if (last <= 0)
                    {
                        return true;
                    }

                    first = static_cast<std::uintmax_t>(last) >= fileSize
                        ? 0
                        : lastByte - last + 1;
                    last = lastByte;
                    return false;
                }

                if (last >= 0 && first > last)
                {
                    hasReversedRange = true;
                    return true;
                }
                if (static_cast<std::uintmax_t>(first) >= fileSize)
                {
                    return true;
                }
                if (last < 0 || static_cast<std::uintmax_t>(last) >= fileSize)
                {
                    last = lastByte;
                }
                return false;
            }),
            ranges.end());
        if (hasReversedRange || ranges.empty())
        {
            ranges.clear();
            return false;
        }

        // Match cpp-httplib's input-order overlap count in O(R log R). R is
        // independently bounded above by CPPHTTPLIB_RANGE_MAX_COUNT.
        std::vector<TRangeOffset> sortedStarts;
        sortedStarts.reserve(ranges.size());
        for (const auto& range : ranges)
        {
            sortedStarts.push_back(range.first);
        }
        std::ranges::sort(sortedStarts);
        sortedStarts.erase(
            std::ranges::unique(sortedStarts).begin(),
            sortedStarts.end());

        std::vector<TRangeOffset> maximumEndTree(sortedStarts.size() + 1, -1);
        const auto readMaximumEnd = [&](std::size_t index) {
            TRangeOffset maximumEnd = -1;
            while (index > 0)
            {
                maximumEnd = (std::max)(maximumEnd, maximumEndTree[index]);
                index -= index & (~index + 1);
            }
            return maximumEnd;
        };
        const auto recordEnd = [&](std::size_t index, const TRangeOffset end) {
            while (index < maximumEndTree.size())
            {
                maximumEndTree[index] = (std::max)(maximumEndTree[index], end);
                index += index & (~index + 1);
            }
        };

        std::size_t overlappingRangeCount = 0;
        for (const auto& range : ranges)
        {
            const auto startsThroughEnd = static_cast<std::size_t>(
                std::ranges::upper_bound(sortedStarts, range.second) - sortedStarts.begin());
            if (readMaximumEnd(startsThroughEnd) >= range.first
                && ++overlappingRangeCount > 2)
            {
                ranges.clear();
                return false;
            }
            const auto startIndex = static_cast<std::size_t>(
                std::ranges::lower_bound(sortedStarts, range.first) - sortedStarts.begin()) + 1;
            recordEnd(startIndex, range.second);
        }
        return true;
    }

    static void SetFileResponse(
        const httplib::Request& request,
        httplib::Response& response,
        SFileResponseWithLease file)
    {
        auto lifetime = std::make_shared<CFileResponseLifetime>(file);
        std::error_code errorCode;
        const auto fileSize = std::filesystem::file_size(file.File.Path, errorCode);
        if (errorCode)
        {
            throw CHttpErrorMapper::NotFound("File was not found.");
        }
        if (fileSize > static_cast<std::uintmax_t>((std::numeric_limits<std::size_t>::max)()))
        {
            throw CHttpErrorMapper::PayloadTooLarge("File exceeds the supported response size.");
        }

        auto stream = std::make_shared<std::ifstream>(file.File.Path, std::ios::binary);
        if (!*stream)
        {
            throw CHttpErrorMapper::NotFound("File was not found.");
        }

        response.set_header("Content-Disposition", BuildContentDisposition(file.File.FileNameUtf8));
        response.set_header("Accept-Ranges", "bytes");
        if (!NormalizeRequestedRanges(request, fileSize))
        {
            response.status = 416;
            response.set_header("Content-Range", "bytes */" + std::to_string(fileSize));
            return;
        }
        response.status = request.ranges.empty() ? 200 : 206;

        const auto contentType = file.File.ContentTypeUtf8.empty()
            ? std::string{"application/octet-stream"}
            : file.File.ContentTypeUtf8;
        response.set_content_provider(
            static_cast<std::size_t>(fileSize),
            contentType,
            [stream](const std::size_t offset, const std::size_t length, httplib::DataSink& sink) {
                if (sink.is_writable && !sink.is_writable())
                {
                    return false;
                }

                stream->clear();
                stream->seekg(static_cast<std::streamoff>(offset), std::ios::beg);
                if (!*stream)
                {
                    return false;
                }

                constexpr std::size_t BufferSize = static_cast<std::size_t>(64) * 1024U;
                std::vector<char> buffer(std::min<std::size_t>(length, BufferSize));
                std::size_t remaining = length;
                while (remaining > 0)
                {
                    const auto chunkSize = std::min<std::size_t>(remaining, buffer.size());
                    stream->read(buffer.data(), static_cast<std::streamsize>(chunkSize));
                    const auto readCount = static_cast<std::size_t>(stream->gcount());
                    if (readCount == 0)
                    {
                        return false;
                    }

                    if (!sink.write(buffer.data(), readCount))
                    {
                        return false;
                    }

                    remaining -= readCount;
                }

                return true;
            },
            [
                stream,
                lifetime = std::move(lifetime)
            ](bool) mutable {
                if (stream != nullptr && stream->is_open())
                {
                    stream->close();
                }
                lifetime.reset();
            });
    }

    [[nodiscard]] std::string NextRequestId()
    {
        return "req-" + std::to_string(NextRequestSequence.fetch_add(1, std::memory_order_relaxed) + 1);
    }

    void LogRequest(
        const httplib::Request& request,
        const int status,
        const std::string& requestId,
        const std::int64_t durationMs) const
    {
        const auto method = EscapeLogField(request.method, GMaxLoggedMethodBytes);
        const auto path = EscapeLogField(request.path, GMaxLoggedPathBytes);
        const bool quietProbe = request.method == "GET"
            && (request.path == "/api/health" || request.path == "/api/ready")
            && status < 400;
        if (quietProbe)
        {
            Logging::DebugIfInitialized(
                "HTTP request: requestId={} method={} path={} status={} durationMs={}",
                requestId,
                method,
                path,
                status,
                durationMs);
            return;
        }

        Logging::InfoIfInitialized(
            "HTTP request: requestId={} method={} path={} status={} durationMs={}",
            requestId,
            method,
            path,
            status,
            durationMs);
    }

    [[nodiscard]] SServerRuntimeStatus BuildRuntimeStatus() const
    {
        return {
            .UptimeSeconds = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - StartedAt).count()),
            .Http = SnapshotHttpRuntimeStatus(*HttpQueueState),
            .Backend = {},
            .Scan = ScanCoordinator.GetRuntimeStatus(Options.MaxScanWorkers),
            .Downloads = DownloadGate.GetStatus(),
            .Storage = BuildCachedStorageRuntimeStatus(),
            .Resources = ReadResourceRuntimeStatus(
                Options.MaxCoverCacheBytes,
                Options.MaxSteadyStateMemoryBytes)
        };
    }

    [[nodiscard]] SServerStorageRuntimeStatus BuildCachedStorageRuntimeStatus() const
    {
        SServerStorageRuntimeStatus result;
        {
            std::scoped_lock lock(StorageStatusCacheMutex);
            result = CachedStorageStatus;
        }
        result.CacheRootPresent = TryIsDirectory(Options.CacheRoot);
        result.CacheDatabasePresent = !Options.CacheRoot.empty()
            && TryIsRegularFile(StoragePlanning::CInpxCacheLayout::GetDatabasePath(Options.CacheRoot));
        result.RuntimeWorkspacePresent = TryIsDirectory(Options.RuntimeWorkspaceRoot);
        return result;
    }

    void StartStorageStatusMonitor()
    {
        if (StorageStatusWorker.joinable())
        {
            return;
        }

        StorageStatusWorker = std::jthread([this](const std::stop_token stopToken) {
            std::unique_lock wakeLock(StorageStatusWakeMutex);
            while (!stopToken.stop_requested())
            {
                StorageStatusWake.wait_for(
                    wakeLock,
                    GStorageStatusRefreshInterval,
                    [&stopToken]() { return stopToken.stop_requested(); });
                if (stopToken.stop_requested())
                {
                    return;
                }

                wakeLock.unlock();
                try
                {
                    auto snapshot = BuildStorageRuntimeStatus(
                        Options.CacheRoot,
                        Options.RuntimeWorkspaceRoot,
                        stopToken);
                    if (!stopToken.stop_requested())
                    {
                        std::scoped_lock cacheLock(StorageStatusCacheMutex);
                        CachedStorageStatus = snapshot;
                    }
                }
                catch (const std::exception& exception)
                {
                    if (!stopToken.stop_requested())
                    {
                        Logging::WarnIfInitialized(
                            "Storage runtime status refresh failed. error='{}'",
                            exception.what());
                    }
                }
                catch (...)
                {
                    if (!stopToken.stop_requested())
                    {
                        Logging::WarnIfInitialized(
                            "Storage runtime status refresh failed with a non-standard exception.");
                    }
                }
                wakeLock.lock();
            }
        });
    }

    void StopStorageStatusMonitor()
    {
        if (!StorageStatusWorker.joinable())
        {
            return;
        }

        StorageStatusWorker.request_stop();
        StorageStatusWake.notify_all();
        StorageStatusWorker.join();
    }

    void Start()
    {
        if (Running.load())
        {
            throw std::runtime_error("HTTP server is already running.");
        }

        if (Options.Port == 0)
        {
            BoundPort = Server.bind_to_any_port(Options.HostUtf8);
        }
        else if (Server.bind_to_port(Options.HostUtf8, Options.Port))
        {
            BoundPort = Options.Port;
        }
        else
        {
            BoundPort = -1;
        }

        if (BoundPort <= 0)
        {
            throw std::runtime_error("Failed to bind HTTP server.");
        }

        Worker = std::thread([this]() {
            const auto listened = Server.listen_after_bind();
            Running.store(false);

            if (!listened)
            {
                Logging::WarnIfInitialized("HTTP server listen loop finished unexpectedly.");
            }
        });

        Server.wait_until_ready();
        if (!Server.is_running())
        {
            if (Worker.joinable())
            {
                Worker.join();
            }

            BoundPort = -1;
            throw std::runtime_error("Failed to start HTTP server.");
        }

        Running.store(true);
        StartStorageStatusMonitor();

        Logging::InfoIfInitialized(
            "HTTP server started: host={} port={}",
            Options.HostUtf8,
            BoundPort);
    }

    void Stop() noexcept
    {
        StopStorageStatusMonitor();
        if (!Server.is_running() && !Worker.joinable())
        {
            return;
        }

        Server.stop();
        if (Worker.joinable())
        {
            Worker.join();
        }

        Logging::InfoIfInitialized("HTTP server stopped.");
    }

    IServerApplicationHost& ApplicationHost;
    CServerScanCoordinator ScanCoordinator;
    CDownloadGate DownloadGate;
    SHttpServerOptions Options;
    std::shared_ptr<SHttpTaskQueueState> HttpQueueState;
    mutable std::mutex StorageStatusCacheMutex;
    SServerStorageRuntimeStatus CachedStorageStatus;
    httplib::Server Server;
    std::thread Worker;
    std::jthread StorageStatusWorker;
    std::mutex StorageStatusWakeMutex;
    std::condition_variable StorageStatusWake;
    std::atomic_bool Running = false;
    std::atomic<std::uint64_t> NextRequestSequence = 0;
    std::chrono::steady_clock::time_point StartedAt = std::chrono::steady_clock::now();
    int BoundPort = -1;
};

CHttpServer::CHttpServer(IServerApplicationHost& applicationHost, SHttpServerOptions options)
    : m_impl(std::make_unique<SImpl>(applicationHost, std::move(options)))
{
}

CHttpServer::~CHttpServer()
{
    Stop();
}

void CHttpServer::Start()
{
    m_impl->Start();
}

SServerScanStartResult CHttpServer::StartScan(
    const ApplicationJobs::SInpxScanRequest& request)
{
    return m_impl->ScanCoordinator.StartScan(request);
}

void CHttpServer::Stop() noexcept
{
    m_impl->Stop();
}

int CHttpServer::GetBoundPort() const noexcept
{
    return m_impl->BoundPort;
}

} // namespace InpxWebReader::Server
