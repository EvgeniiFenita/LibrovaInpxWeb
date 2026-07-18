#include "Server/ServerApplicationHost.hpp"

#include <chrono>
#include <format>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "App/CInpxCatalogApplication.hpp"
#include "Converter/BuiltInConverterCliValidation.hpp"
#include "Domain/BookFormat.hpp"
#include "Foundation/FilenamePolicy.hpp"
#include "Foundation/Logging.hpp"
#include "Foundation/StringUtils.hpp"
#include "Foundation/UnicodeConversion.hpp"
#include "Foundation/Version.hpp"
#include "Storage/InpxCacheLayout.hpp"
#include "Storage/PathSafety.hpp"
#include "Storage/RuntimeWorkspaceLayout.hpp"

namespace InpxWebReader::Server {

std::optional<ApplicationJobs::EInpxScanMode> ResolveConfiguredStartupScanMode(
    const SServerStartupConfig& startup,
    const SServerStatus& status) noexcept
{
    if (!startup.AutoScan || !status.InpxSource.has_value())
    {
        return std::nullopt;
    }

    if (status.CreatedCacheOnOpen)
    {
        return startup.AutoScanOnEmptyCache && status.InpxSource->TotalBookCount == 0
            ? std::make_optional(ApplicationJobs::EInpxScanMode::InitialScan)
            : std::nullopt;
    }

    return startup.AutoRescanOnSourceChange && status.InpxSource->RequiresRescan
        ? std::make_optional(ApplicationJobs::EInpxScanMode::Rescan)
        : std::nullopt;
}

namespace {

[[nodiscard]] std::int64_t ElapsedMilliseconds(const std::chrono::steady_clock::time_point startedAt) noexcept
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startedAt).count();
}

[[nodiscard]] std::unique_ptr<Application::IInpxCatalogApplication> CreateDefaultApplication(
    const Application::SInpxCatalogApplicationConfig& config)
{
    return std::make_unique<Application::CInpxCatalogApplication>(config);
}

[[nodiscard]] std::filesystem::path ComputeDatabasePath(const std::filesystem::path& cacheRoot)
{
    return StoragePlanning::CInpxCacheLayout::GetDatabasePath(cacheRoot);
}

[[nodiscard]] std::string_view FileExtensionForFormat(const Domain::EBookFormat format) noexcept
{
    switch (format)
    {
    case Domain::EBookFormat::Epub:
        return ".epub";
    case Domain::EBookFormat::Fb2:
        return ".fb2";
    }

    return ".book";
}

[[nodiscard]] std::string ContentTypeForBookFormat(const Domain::EBookFormat format)
{
    switch (format)
    {
    case Domain::EBookFormat::Epub:
        return "application/epub+zip";
    case Domain::EBookFormat::Fb2:
        return "application/x-fictionbook+xml";
    }

    return "application/octet-stream";
}

[[nodiscard]] std::string ContentTypeForPath(const std::filesystem::path& path)
{
    const std::string extensionUtf8 = Foundation::ToLowerAscii(Unicode::PathToUtf8(path.extension()));

    if (extensionUtf8 == ".jpg" || extensionUtf8 == ".jpeg")
    {
        return "image/jpeg";
    }
    if (extensionUtf8 == ".png")
    {
        return "image/png";
    }
    if (extensionUtf8 == ".webp")
    {
        return "image/webp";
    }

    return "application/octet-stream";
}

[[nodiscard]] std::string SanitizeDownloadBaseName(std::string valueUtf8)
{
    constexpr std::size_t maxBaseNameBytes = 160;
    return Foundation::SanitizeDownloadFilenameBaseUtf8(std::move(valueUtf8), maxBaseNameBytes);
}

[[nodiscard]] std::string BuildDownloadBaseName(const Application::SPreparedBookDownload& download)
{
    std::string baseName = download.TitleUtf8.empty() ? "book" : download.TitleUtf8;
    if (!download.AuthorsUtf8.empty())
    {
        std::string authors;
        for (const std::string& author : download.AuthorsUtf8)
        {
            if (!authors.empty())
            {
                authors += ", ";
            }
            authors += author;
        }

        if (!authors.empty())
        {
            baseName += " - " + authors;
        }
    }

    return SanitizeDownloadBaseName(std::move(baseName));
}

class CDownloadRootCleanupGuard final
{
public:
    explicit CDownloadRootCleanupGuard(std::filesystem::path root)
        : m_root(std::move(root))
    {
    }

    ~CDownloadRootCleanupGuard()
    {
        if (!m_active || m_root.empty())
        {
            return;
        }

        std::error_code cleanupError;
        std::filesystem::remove_all(m_root, cleanupError);
        if (cleanupError)
        {
            Logging::WarnIfInitialized(
                "Server download staging cleanup failed after download error: path='{}' error='{}'",
                Unicode::PathToUtf8(m_root),
                cleanupError.message());
        }
    }

    CDownloadRootCleanupGuard(const CDownloadRootCleanupGuard&) = delete;
    CDownloadRootCleanupGuard& operator=(const CDownloadRootCleanupGuard&) = delete;

    void Release() noexcept
    {
        m_active = false;
    }

private:
    std::filesystem::path m_root;
    bool m_active = true;
};

[[nodiscard]] std::filesystem::path ResolveCoverPath(
    const std::filesystem::path& cacheRoot,
    const std::optional<std::filesystem::path>& coverPath)
{
    if (!coverPath.has_value() || coverPath->empty())
    {
        return {};
    }

    const auto resolved = SafePaths::TryResolvePathWithinRoot(
        cacheRoot,
        *coverPath,
        "Book cover path is unsafe.",
        "Book cover path could not be canonicalized.");
    if (!resolved.has_value() || !std::filesystem::is_regular_file(*resolved))
    {
        return {};
    }

    return *resolved;
}

[[nodiscard]] Application::ECacheOpenMode ResolveOpenMode(const std::filesystem::path& cacheRoot)
{
    std::error_code errorCode;
    return std::filesystem::is_regular_file(ComputeDatabasePath(cacheRoot), errorCode) && !errorCode
        ? Application::ECacheOpenMode::Open
        : Application::ECacheOpenMode::CreateNew;
}

[[nodiscard]] Application::SInpxCatalogApplicationConfig BuildInpxCatalogApplicationConfig(const SServerConfig& config)
{
    const auto openMode = ResolveOpenMode(config.CacheRoot);

    Application::SInpxCatalogApplicationConfig applicationConfig;
    applicationConfig.CacheRoot = config.CacheRoot;
    applicationConfig.RuntimeWorkspaceRoot = config.RuntimeWorkspaceRoot;
    applicationConfig.ConverterPath = config.Converter.Path;
    applicationConfig.MaxInpxScanWorkers = config.Limits.MaxScanWorkers;
    applicationConfig.MaxConcurrentDownloads = config.Limits.MaxConcurrentDownloads;
    applicationConfig.MaxCoverCacheBytes =
        static_cast<std::uint64_t>(config.Limits.MaxCoverCacheMiB) * 1024ull * 1024ull;
    applicationConfig.MaxSteadyStateMemoryBytes =
        static_cast<std::uint64_t>(config.Limits.MaxSteadyStateMemoryMiB) * 1024ull * 1024ull;
    applicationConfig.CacheOpenMode = openMode;
    if (!config.InpxSource.InpxPath.empty()
        && !config.InpxSource.ArchiveRoot.empty())
    {
        applicationConfig.InpxSource = Application::SInpxSourceInfo{
            .InpxPath = config.InpxSource.InpxPath,
            .ArchiveRoot = config.InpxSource.ArchiveRoot
        };
    }
    return applicationConfig;
}

void ValidateConfiguredConverter(const SServerConfig& config)
{
    if (!config.Converter.Path.has_value())
    {
        return;
    }

    const auto validation = ConverterValidation::ValidateBuiltInConverterExecutable(*config.Converter.Path);
    if (!validation.IsValid)
    {
        throw std::runtime_error("Configured converter.path must point to a valid built-in fbc executable.");
    }

    Logging::InfoIfInitialized(
        "Server application host: validated configured converter '{}'.",
        Unicode::PathToUtf8(*config.Converter.Path));
}

[[nodiscard]] SServerInpxSourceStatus BuildInpxSourceStatus(
    const Application::SInpxSourceOverview& overview)
{
    return {
        .InpxPathUtf8 = Unicode::PathToUtf8(overview.Source.InpxPath),
        .ArchiveRootUtf8 = Unicode::PathToUtf8(overview.Source.ArchiveRoot),
        .IsSourceAvailable = overview.IsSourceAvailable,
        .RequiresRescan = overview.RequiresRescan,
        .SourceWarningUtf8 = overview.SourceWarningUtf8,
        .TotalBookCount = overview.TotalBookCount,
        .AvailableBookCount = overview.AvailableBookCount,
        .UnavailableBookCount = overview.UnavailableBookCount,
        .WarningCount = overview.WarningCount
    };
}

[[nodiscard]] SServerStatus BuildStatus(
    Application::IInpxCatalogApplication& application,
    const bool createdCacheOnOpen)
{
    const auto sessionInfo = application.GetCatalogSessionInfo();
    SServerStatus status{
        .VersionUtf8 = std::string{Core::CVersion::GetValue()},
        .IsOpen = true,
        .CreatedCacheOnOpen = createdCacheOnOpen,
        .Capabilities = sessionInfo.Capabilities
    };

    const auto overview = application.GetInpxSourceOverview();
    if (overview.has_value())
    {
        status.InpxSource = BuildInpxSourceStatus(overview.value());
    }

    return status;
}

} // namespace

CServerApplicationHost::CServerApplicationHost(
    SServerConfig config,
    FApplicationFactory applicationFactory)
    : m_config(std::move(config))
    , m_applicationFactory(applicationFactory ? std::move(applicationFactory) : CreateDefaultApplication)
    , m_executor(m_config.Limits.MaxBackendQueueDepth)
{
}

CServerApplicationHost::~CServerApplicationHost()
{
    Close();
}

void CServerApplicationHost::Open()
{
    if (m_isOpen.load())
    {
        throw std::runtime_error("Server application host is already open.");
    }

    std::filesystem::create_directories(m_config.RuntimeWorkspaceRoot);

    const auto startedAt = std::chrono::steady_clock::now();
    Logging::InfoIfInitialized(
        "Server application host: opening INPX cache root '{}'.",
        Unicode::PathToUtf8(m_config.CacheRoot));

    Application::SInpxCatalogApplicationConfig applicationConfig;
    bool createdCacheOnOpen = false;
    try
    {
        ValidateConfiguredConverter(m_config);
        applicationConfig = BuildInpxCatalogApplicationConfig(m_config);
        createdCacheOnOpen = applicationConfig.CacheOpenMode == Application::ECacheOpenMode::CreateNew;

        auto openFuture = m_executor.Submit([this, applicationConfig = std::move(applicationConfig)]() {
            m_application.reset();
            m_application = m_applicationFactory(applicationConfig);
        });
        openFuture.get();
        {
            std::scoped_lock lock(m_downloadMutex);
            m_downloadStopSource = std::stop_source{};
        }
        m_createdCacheOnOpen.store(createdCacheOnOpen);
        m_isOpen.store(true);
    }
    catch (const std::exception& ex)
    {
        m_isOpen.store(false);
        m_createdCacheOnOpen.store(false);
        try
        {
            auto cleanupFuture = m_executor.Submit([this]() {
                m_application.reset();
            });
            cleanupFuture.get();
        }
        catch (const std::exception& cleanupEx)
        {
            Logging::ErrorIfInitialized("Server application host: cleanup after open failure failed: {}", cleanupEx.what());
        }
        Logging::ErrorIfInitialized("Server application host: open failed: {}", ex.what());
        throw;
    }

    Logging::InfoIfInitialized(
        "Server application host: opened in {} ms.",
        ElapsedMilliseconds(startedAt));
}

void CServerApplicationHost::Close() noexcept
{
    if (!m_isOpen.exchange(false))
    {
        return;
    }
    m_createdCacheOnOpen.store(false);

    {
        std::unique_lock lock(m_downloadMutex);
        m_downloadStopSource.request_stop();
        m_downloadsFinished.wait(lock, [this]() {
            return m_activeDownloadOperations == 0;
        });
    }

    const auto startedAt = std::chrono::steady_clock::now();
    Logging::InfoIfInitialized("Server application host: closing.");
    try
    {
        auto closeFuture = m_executor.Submit([this]() {
            m_application.reset();
        });
        closeFuture.get();
    }
    catch (const std::exception& ex)
    {
        Logging::ErrorIfInitialized("Server application host: close failed: {}", ex.what());
    }

    Logging::InfoIfInitialized(
        "Server application host: closed in {} ms.",
        ElapsedMilliseconds(startedAt));
}

bool CServerApplicationHost::IsOpen() const noexcept
{
    return m_isOpen.load();
}

void CServerApplicationHost::EnsureOpen() const
{
    if (!m_isOpen.load())
    {
        throw std::runtime_error("Server application host is not open.");
    }
}

CServerApplicationHost::SDownloadOperationContext CServerApplicationHost::BeginDownloadOperation()
{
    std::scoped_lock lock(m_downloadMutex);
    if (!m_isOpen.load() || m_application == nullptr)
    {
        throw std::runtime_error("Server application host is not open.");
    }
    ++m_activeDownloadOperations;
    return {
        .Application = m_application.get(),
        .ShutdownStopToken = m_downloadStopSource.get_token()
    };
}

void CServerApplicationHost::EndDownloadOperation() noexcept
{
    {
        std::scoped_lock lock(m_downloadMutex);
        if (m_activeDownloadOperations > 0)
        {
            --m_activeDownloadOperations;
        }
    }
    m_downloadsFinished.notify_all();
}

SServerStatus CServerApplicationHost::GetStatus()
{
    EnsureOpen();
    const auto backendMetrics = m_executor.GetMetrics();
    const bool createdCacheOnOpen = m_createdCacheOnOpen.load();
    auto status = SubmitWithOpenApplication([createdCacheOnOpen](Application::IInpxCatalogApplication& application) {
        return BuildStatus(application, createdCacheOnOpen);
    });
    status.Runtime.Backend = backendMetrics;
    return status;
}

Application::SBookListResult CServerApplicationHost::ListBooks(
    const Application::SBookListRequest& request)
{
    return SubmitWithOpenApplication([request](Application::IInpxCatalogApplication& application) {
        return application.ListBooks(request);
    });
}

std::optional<Application::SBookDetails> CServerApplicationHost::GetBookDetails(
    const Domain::SBookId bookId)
{
    return SubmitWithOpenApplication([bookId](Application::IInpxCatalogApplication& application) {
        return application.GetBookDetails(bookId);
    });
}

Application::SCatalogStatistics CServerApplicationHost::GetCatalogStatistics()
{
    return SubmitWithOpenApplication([](Application::IInpxCatalogApplication& application) {
        return application.GetCatalogStatistics();
    });
}

std::optional<SServerFileResponse> CServerApplicationHost::ResolveBookCover(
    const Domain::SBookId bookId)
{
    return SubmitWithOpenApplication([this, bookId](Application::IInpxCatalogApplication& application)
        -> std::optional<SServerFileResponse> {
        const auto details = application.GetBookDetails(bookId);
        if (!details.has_value())
        {
            return std::nullopt;
        }

        const auto coverPath = ResolveCoverPath(m_config.CacheRoot, details->CoverPath);
        if (coverPath.empty())
        {
            return std::nullopt;
        }

        return SServerFileResponse{
            .Path = coverPath,
            .CleanupRoot = std::nullopt,
            .FileNameUtf8 = "cover" + Unicode::PathToUtf8(coverPath.extension()),
            .ContentTypeUtf8 = ContentTypeForPath(coverPath)
        };
    });
}

std::optional<SServerFileResponse> CServerApplicationHost::PrepareBookDownload(
    const Domain::SBookId bookId,
    const EServerDownloadFormat format)
{
    const auto operation = BeginDownloadOperation();
    const std::unique_ptr<CServerApplicationHost, std::function<void(CServerApplicationHost*)>> operationGuard(
        this,
        [](CServerApplicationHost* host) { host->EndDownloadOperation(); });
    std::stop_source requestStopSource;
    std::stop_callback shutdownCallback(
        operation.ShutdownStopToken,
        [&requestStopSource]() { requestStopSource.request_stop(); });

    Application::IInpxCatalogApplication& application = *operation.Application;
    const auto sequence = m_downloadSequence.fetch_add(1, std::memory_order_relaxed) + 1;
    const std::optional<Domain::EBookFormat> requestedFormat =
        format == EServerDownloadFormat::Epub
            ? std::optional{Domain::EBookFormat::Epub}
            : std::nullopt;
    const Domain::EBookFormat stagingFormat = requestedFormat.value_or(Domain::EBookFormat::Fb2);
    const auto downloadRoot = StoragePlanning::BuildRuntimeWorkspaceLayout(
        m_config.RuntimeWorkspaceRoot).ServerDownloadDirectory
        / std::format("{}-{}", bookId.Value, sequence);
    std::filesystem::create_directories(downloadRoot);
    CDownloadRootCleanupGuard cleanupGuard(downloadRoot);

    const auto destinationPath = downloadRoot
        / Unicode::PathFromUtf8("prepared" + std::string{FileExtensionForFormat(stagingFormat)});

    const auto preparedDownload = application.PrepareDownload(Application::SBookDownloadRequest{
        .BookId = bookId,
        .DestinationPath = destinationPath,
        .RequestedFormat = requestedFormat,
        .StopToken = requestStopSource.get_token()
    });
    if (!preparedDownload.has_value())
    {
        return std::nullopt;
    }

    const auto fileName = BuildDownloadBaseName(*preparedDownload)
        + std::string{FileExtensionForFormat(preparedDownload->Format)};

    SServerFileResponse response{
        .Path = preparedDownload->Path,
        .CleanupRoot = downloadRoot,
        .FileNameUtf8 = fileName,
        .ContentTypeUtf8 = ContentTypeForBookFormat(preparedDownload->Format)
    };
    cleanupGuard.Release();
    return response;
}

std::optional<Application::SInpxSourceOverview> CServerApplicationHost::GetInpxSourceOverview()
{
    return SubmitWithOpenApplication([](Application::IInpxCatalogApplication& application) {
        return application.GetInpxSourceOverview();
    });
}

ApplicationJobs::TInpxScanJobId CServerApplicationHost::StartInpxScan(
    const ApplicationJobs::SInpxScanRequest& request)
{
    return SubmitWithOpenApplication([request](Application::IInpxCatalogApplication& application) {
        return application.StartInpxScan(request);
    });
}

std::optional<ApplicationJobs::SInpxScanJobSnapshot> CServerApplicationHost::GetInpxScanJobSnapshot(
    const ApplicationJobs::TInpxScanJobId jobId)
{
    return SubmitWithOpenApplication([jobId](Application::IInpxCatalogApplication& application) {
        return application.GetInpxScanJobSnapshot(jobId);
    });
}

std::optional<ApplicationJobs::SInpxScanJobResult> CServerApplicationHost::GetInpxScanJobResult(
    const ApplicationJobs::TInpxScanJobId jobId)
{
    return SubmitWithOpenApplication([jobId](Application::IInpxCatalogApplication& application) {
        return application.GetInpxScanJobResult(jobId);
    });
}

bool CServerApplicationHost::CancelInpxScanJob(const ApplicationJobs::TInpxScanJobId jobId)
{
    return SubmitWithOpenApplication([jobId](Application::IInpxCatalogApplication& application) {
        return application.CancelInpxScanJob(jobId);
    });
}

bool CServerApplicationHost::RemoveInpxScanJob(const ApplicationJobs::TInpxScanJobId jobId)
{
    return SubmitWithOpenApplication([jobId](Application::IInpxCatalogApplication& application) {
        return application.RemoveInpxScanJob(jobId);
    });
}

} // namespace InpxWebReader::Server
