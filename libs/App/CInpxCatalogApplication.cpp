#include "App/CInpxCatalogApplication.hpp"

#include <chrono>
#include <filesystem>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include "App/InpxCacheBootstrap.hpp"
#include "Converter/ConverterConfiguration.hpp"
#include "Converter/ExternalBookConverter.hpp"
#include "Database/CatalogStatisticsMaintenance.hpp"
#include "Database/SchemaInitializer.hpp"
#include "Database/SqliteBookQueryRepository.hpp"
#include "Database/SqliteConnection.hpp"
#include "Database/SqliteStatement.hpp"
#include "Database/SqliteTimePoint.hpp"
#include "Domain/DomainError.hpp"
#include "Domain/InpxBookAvailability.hpp"
#include "Foundation/UnicodeConversion.hpp"
#include "Inpx/InpxSourceConfiguration.hpp"
#include "Foundation/Sha256Fingerprint.hpp"
#include "Storage/InpxCacheLayout.hpp"
#include "Storage/RuntimeWorkspaceLayout.hpp"
#include "Storage/StbCoverImageProcessor.hpp"

namespace InpxWebReader::Application {
namespace {

struct SInpxSourceRow
{
    std::int64_t SourceId = 0;
    std::string DisplayNameUtf8;
    std::string SourceFingerprintUtf8;
    std::string LastScanStartedAtUtc;
    std::string LastScanCompletedAtUtc;
    std::string LastSeenSnapshotId;
};

struct SSourceFingerprintCache
{
    std::filesystem::path InpxPath;
    std::uintmax_t SizeBytes = 0;
    std::filesystem::file_time_type LastWriteTime;
    std::string FingerprintUtf8;
    bool IsInitialized = false;
};

struct SInpxSourceRuntimeState
{
    bool CanRescan = false;
    bool CanDownload = false;
    std::string SourceWarningUtf8;
    std::optional<std::string> CurrentFingerprintUtf8 = std::nullopt;
};

[[nodiscard]] std::filesystem::path ComputeDatabasePath(const std::filesystem::path& cacheRoot)
{
    return StoragePlanning::CInpxCacheLayout::GetDatabasePath(cacheRoot);
}

[[nodiscard]] std::optional<ConverterRuntime::CExternalBookConverter> BuildConverter(
    const SInpxCatalogApplicationConfig& config)
{
    if (!config.ConverterPath.has_value())
    {
        return std::nullopt;
    }

    const auto profile = ConverterConfiguration::TryBuildCommandProfile({
        .Mode = ConverterConfiguration::EConverterConfigurationMode::BuiltInFb2Cng,
        .Fb2Cng = {
            .ExecutablePath = *config.ConverterPath
        }
    });
    if (!profile.has_value())
    {
        return std::nullopt;
    }

    const auto runtimeLayout = StoragePlanning::BuildRuntimeWorkspaceLayout(config.RuntimeWorkspaceRoot);
    return ConverterRuntime::CExternalBookConverter({
        .CommandProfile = *profile,
        .WorkingDirectory = runtimeLayout.ConverterDirectory,
        .RevalidateBuiltInFbcBeforeRun = true
    });
}

[[nodiscard]] SInpxSourceInfo ValidateNewSource(const SInpxCatalogApplicationConfig& config)
{
    if (!config.InpxSource.has_value())
    {
        throw std::runtime_error("A new INPX cache requires an INPX file and archive root.");
    }

    const auto validation = Inpx::CInpxSourceConfiguration::Validate(
        config.InpxSource->InpxPath,
        config.InpxSource->ArchiveRoot);
    if (!validation.IsValid)
    {
        throw std::runtime_error(validation.ErrorUtf8);
    }

    return {
        .InpxPath = validation.InpxPath,
        .ArchiveRoot = validation.ArchiveRoot
    };
}

[[nodiscard]] SInpxSourceInfo NormalizeConfiguredSource(const SInpxSourceInfo& source)
{
    if (source.InpxPath.empty() || source.ArchiveRoot.empty())
    {
        throw std::invalid_argument("Configured INPX source requires both the INPX file and archive root.");
    }
    if (!source.InpxPath.is_absolute() || !source.ArchiveRoot.is_absolute())
    {
        throw std::invalid_argument("Configured INPX source paths must be absolute.");
    }

    return {
        .InpxPath = source.InpxPath.lexically_normal(),
        .ArchiveRoot = source.ArchiveRoot.lexically_normal()
    };
}

[[nodiscard]] std::optional<SInpxSourceInfo> NormalizeConfiguredSource(
    const std::optional<SInpxSourceInfo>& source)
{
    return source.has_value()
        ? std::make_optional(NormalizeConfiguredSource(*source))
        : std::nullopt;
}

void PersistNewSource(
    const Sqlite::CSqliteConnection& connection,
    const SInpxSourceInfo& source,
    const std::string& sourceFingerprintUtf8)
{
    Sqlite::CSqliteStatement statement(
        connection.GetNativeHandle(),
        "INSERT INTO inpx_sources("
        "id, display_name, source_fingerprint) "
        "VALUES(1, ?, ?);");
    statement.BindText(1, Unicode::PathToUtf8(source.InpxPath.filename()));
    statement.BindText(2, sourceFingerprintUtf8);
    static_cast<void>(statement.Step());
    BookDatabase::CCatalogStatisticsMaintenance::RecomputeInpxSourceSize(
        connection,
        source.InpxPath,
        source.ArchiveRoot);
}

[[nodiscard]] SInpxSourceRow LoadSourceRow(const std::filesystem::path& databasePath)
{
    Sqlite::CSqliteConnection connection(databasePath);
    Sqlite::CSqliteStatement statement(
        connection.GetNativeHandle(),
        "SELECT id, display_name, source_fingerprint, last_scan_started_at_utc, "
        "last_scan_completed_at_utc, last_seen_snapshot_id "
        "FROM inpx_sources WHERE id = 1;");
    if (!statement.Step())
    {
        throw std::runtime_error("INPX cache source metadata is missing.");
    }

    return {
        .SourceId = statement.GetColumnInt64(0),
        .DisplayNameUtf8 = statement.GetColumnText(1),
        .SourceFingerprintUtf8 = statement.GetColumnText(2),
        .LastScanStartedAtUtc = statement.IsColumnNull(3) ? std::string{} : statement.GetColumnText(3),
        .LastScanCompletedAtUtc = statement.IsColumnNull(4) ? std::string{} : statement.GetColumnText(4),
        .LastSeenSnapshotId = statement.IsColumnNull(5) ? std::string{} : statement.GetColumnText(5)
    };
}

[[nodiscard]] std::string RefreshSourceFingerprint(
    const SInpxSourceInfo& source,
    SSourceFingerprintCache& cache,
    const bool forceRefresh)
{
    std::error_code errorCode;
    const auto sizeBytes = std::filesystem::file_size(source.InpxPath, errorCode);
    if (errorCode)
    {
        throw std::runtime_error("The configured INPX file size could not be read.");
    }
    const auto lastWriteTime = std::filesystem::last_write_time(source.InpxPath, errorCode);
    if (errorCode)
    {
        throw std::runtime_error("The configured INPX file timestamp could not be read.");
    }

    if (!forceRefresh
        && cache.IsInitialized
        && cache.InpxPath == source.InpxPath
        && cache.SizeBytes == sizeBytes
        && cache.LastWriteTime == lastWriteTime)
    {
        return cache.FingerprintUtf8;
    }

    const std::string fingerprintUtf8 = Foundation::CSha256Fingerprint::ComputeFile(source.InpxPath);
    const auto completedSizeBytes = std::filesystem::file_size(source.InpxPath, errorCode);
    if (errorCode)
    {
        throw std::runtime_error("The configured INPX file size could not be verified.");
    }
    const auto completedLastWriteTime = std::filesystem::last_write_time(source.InpxPath, errorCode);
    if (errorCode)
    {
        throw std::runtime_error("The configured INPX file timestamp could not be verified.");
    }
    if (completedSizeBytes != sizeBytes || completedLastWriteTime != lastWriteTime)
    {
        throw std::runtime_error("The configured INPX file changed while its fingerprint was calculated.");
    }

    cache = {
        .InpxPath = source.InpxPath,
        .SizeBytes = sizeBytes,
        .LastWriteTime = lastWriteTime,
        .FingerprintUtf8 = fingerprintUtf8,
        .IsInitialized = true
    };
    return fingerprintUtf8;
}

[[nodiscard]] SInpxSourceRuntimeState ResolveInpxSourceState(
    const std::optional<SInpxSourceInfo>& source,
    const std::string& storedFingerprintUtf8,
    SSourceFingerprintCache& fingerprintCache,
    const bool forceFingerprintRefresh)
{
    if (!source.has_value())
    {
        return {
            .SourceWarningUtf8 = "The INPX source is not configured."
        };
    }

    std::error_code errorCode;
    const bool inpxAvailable = std::filesystem::is_regular_file(source->InpxPath, errorCode) && !errorCode;
    errorCode.clear();
    const bool archivesAvailable = std::filesystem::is_directory(source->ArchiveRoot, errorCode) && !errorCode;
    if (!inpxAvailable || !archivesAvailable)
    {
        std::string warning;
        if (!inpxAvailable && !archivesAvailable)
        {
            warning = "The INPX file and archive root are unavailable.";
        }
        else
        {
            warning = inpxAvailable ? "The archive root is unavailable." : "The INPX file is unavailable.";
        }
        return {
            .SourceWarningUtf8 = std::move(warning)
        };
    }

    try
    {
        const std::string fingerprintUtf8 = RefreshSourceFingerprint(
            *source,
            fingerprintCache,
            forceFingerprintRefresh);
        const bool matchesStoredSource = fingerprintUtf8 == storedFingerprintUtf8;
        return {
            .CanRescan = true,
            .CanDownload = matchesStoredSource,
            .SourceWarningUtf8 = matchesStoredSource
                ? std::string{}
                : std::string{"The configured INPX file differs from the last successful scan; rescan is required."},
            .CurrentFingerprintUtf8 = fingerprintUtf8
        };
    }
    catch (const std::exception& ex)
    {
        return {
            .SourceWarningUtf8 = ex.what()
        };
    }
}

[[nodiscard]] std::string BuildWarningText(
    const std::string& archiveName,
    const std::string& entryName,
    const std::string& message)
{
    std::string result;
    if (!archiveName.empty())
    {
        result = archiveName;
    }
    if (!entryName.empty())
    {
        result += result.empty() ? entryName : " :: " + entryName;
    }
    return result.empty() ? message : result + " — " + message;
}

[[nodiscard]] SInpxSourceOverview LoadOverview(
    const std::filesystem::path& databasePath,
    const std::optional<SInpxSourceInfo>& source,
    const SInpxSourceRuntimeState& sourceState)
{
    Sqlite::CSqliteConnection connection(databasePath);
    const auto row = LoadSourceRow(databasePath);
    SInpxSourceOverview result{
        .Source = source.value_or(SInpxSourceInfo{}),
        .DisplayNameUtf8 = source.has_value()
            ? Unicode::PathToUtf8(source->InpxPath.filename())
            : row.DisplayNameUtf8,
        .SourceWarningUtf8 = sourceState.SourceWarningUtf8,
        .LastScanStartedAtUtc = row.LastScanStartedAtUtc,
        .LastScanCompletedAtUtc = row.LastScanCompletedAtUtc,
        .LastSeenSnapshotId = row.LastSeenSnapshotId
    };
    result.IsSourceAvailable = sourceState.CanDownload;
    result.RequiresRescan = sourceState.CanRescan && !sourceState.CanDownload;

    Sqlite::CSqliteStatement counts(
        connection.GetNativeHandle(),
        "SELECT COUNT(*), "
        "COALESCE(SUM(CASE WHEN availability = ? THEN 1 ELSE 0 END), 0), "
        "COALESCE(SUM(CASE WHEN availability <> ? THEN 1 ELSE 0 END), 0) "
        "FROM inpx_book_locations WHERE source_id = ?;");
    const auto available = Domain::ToString(Domain::EInpxBookAvailability::Available);
    counts.BindText(1, available);
    counts.BindText(2, available);
    counts.BindInt64(3, row.SourceId);
    if (counts.Step())
    {
        result.TotalBookCount = static_cast<std::size_t>(counts.GetColumnInt64(0));
        result.AvailableBookCount = static_cast<std::size_t>(counts.GetColumnInt64(1));
        result.UnavailableBookCount = static_cast<std::size_t>(counts.GetColumnInt64(2));
    }

    if (!row.LastSeenSnapshotId.empty())
    {
        Sqlite::CSqliteStatement warningCount(
            connection.GetNativeHandle(),
            "SELECT COUNT(*) FROM inpx_scan_warnings WHERE source_id = ? AND scan_id = ?;");
        warningCount.BindInt64(1, row.SourceId);
        warningCount.BindText(2, row.LastSeenSnapshotId);
        if (warningCount.Step())
        {
            result.WarningCount = static_cast<std::size_t>(warningCount.GetColumnInt64(0));
        }

        Sqlite::CSqliteStatement warnings(
            connection.GetNativeHandle(),
            "SELECT archive_name, entry_name, message "
            "FROM inpx_scan_warnings WHERE source_id = ? AND scan_id = ? "
            "ORDER BY id DESC LIMIT 5;");
        warnings.BindInt64(1, row.SourceId);
        warnings.BindText(2, row.LastSeenSnapshotId);
        while (warnings.Step())
        {
            result.RecentWarningsUtf8.push_back(BuildWarningText(
                warnings.IsColumnNull(0) ? std::string{} : warnings.GetColumnText(0),
                warnings.IsColumnNull(1) ? std::string{} : warnings.GetColumnText(1),
                warnings.GetColumnText(2)));
        }
    }
    return result;
}

[[nodiscard]] bool CanDownloadAsEpub(const Domain::IBookConverter* converter)
{
    return converter != nullptr
        && converter->CanConvert(Domain::EBookFormat::Fb2, Domain::EBookFormat::Epub);
}

[[nodiscard]] std::uint64_t ResolveCatalogCursorMemoryBudget(
    const std::uint64_t maxSteadyStateMemoryBytes) noexcept
{
    return (std::max<std::uint64_t>)(1, maxSteadyStateMemoryBytes / 16);
}

[[nodiscard]] std::uint64_t ResolveDownloadManifestAggregateMemoryBudget(
    const std::uint64_t maxSteadyStateMemoryBytes) noexcept
{
    return (std::max<std::uint64_t>)(1, maxSteadyStateMemoryBytes / 16);
}

[[nodiscard]] std::uint64_t ResolveDownloadManifestMemoryBudget(
    const std::uint64_t maxSteadyStateMemoryBytes,
    const std::size_t maxConcurrentDownloads) noexcept
{
    return ResolveDownloadManifestAggregateMemoryBudget(maxSteadyStateMemoryBytes)
        / static_cast<std::uint64_t>(maxConcurrentDownloads);
}

[[nodiscard]] std::uint64_t ResolveScanMemoryBudget(
    const std::uint64_t maxSteadyStateMemoryBytes) noexcept
{
    const std::uint64_t cursorBudget = ResolveCatalogCursorMemoryBudget(maxSteadyStateMemoryBytes);
    const std::uint64_t downloadManifestBudget = ResolveDownloadManifestAggregateMemoryBudget(
        maxSteadyStateMemoryBytes);
    return maxSteadyStateMemoryBytes - cursorBudget - downloadManifestBudget;
}

} // namespace

struct CInpxCatalogApplication::SImpl
{
    explicit SImpl(const SInpxCatalogApplicationConfig& config)
        : databasePath(ComputeDatabasePath(config.CacheRoot))
        , queryRepository(
              databasePath,
              ResolveCatalogCursorMemoryBudget(config.MaxSteadyStateMemoryBytes))
        , converter(BuildConverter(config))
        , converterPtr(converter.has_value() ? static_cast<const Domain::IBookConverter*>(&*converter) : nullptr)
        , source(config.InpxSource)
        , catalogFacade(queryRepository)
        , downloadFacade(
              config.CacheRoot,
              source.has_value()
                  ? std::make_optional(SBookDownloadSource{
                        .InpxPath = source->InpxPath,
                        .ArchiveRoot = source->ArchiveRoot
                    })
                  : std::nullopt,
              converterPtr,
              config.RuntimeWorkspaceRoot,
              databasePath,
              ResolveDownloadManifestMemoryBudget(
                  config.MaxSteadyStateMemoryBytes,
                  config.MaxConcurrentDownloads))
        , scanService(
              StoragePlanning::BuildRuntimeWorkspaceLayout(config.RuntimeWorkspaceRoot).ScanDirectory,
              {},
              databasePath,
              config.CacheRoot,
              &coverImageProcessor,
              config.MaxInpxScanWorkers,
              config.MaxCoverCacheBytes,
              ResolveScanMemoryBudget(config.MaxSteadyStateMemoryBytes))
    {
    }

    [[nodiscard]] SInpxSourceRuntimeState ResolveSourceState(
        const bool forceFingerprintRefresh = false,
        const std::string* catalogFingerprintUtf8 = nullptr)
    {
        const std::scoped_lock lock(fingerprintCacheMutex);
        return ResolveInpxSourceState(
            source,
            catalogFingerprintUtf8 != nullptr
                ? *catalogFingerprintUtf8
                : LoadSourceRow(databasePath).SourceFingerprintUtf8,
            fingerprintCache,
            forceFingerprintRefresh);
    }

    std::filesystem::path databasePath;
    BookDatabase::CSqliteBookQueryRepository queryRepository;
    CoverProcessingStb::CStbCoverImageProcessor coverImageProcessor;
    std::optional<ConverterRuntime::CExternalBookConverter> converter;
    const Domain::IBookConverter* converterPtr = nullptr;
    std::optional<SInpxSourceInfo> source = std::nullopt;
    CInpxCatalogFacade catalogFacade;
    CBookDownloadFacade downloadFacade;
    ApplicationJobs::CInpxScanJobService scanService;
    SSourceFingerprintCache fingerprintCache;
    std::mutex fingerprintCacheMutex;
};

CInpxCatalogApplication::CInpxCatalogApplication(const SInpxCatalogApplicationConfig& config)
{
    if (config.CacheRoot.empty() || config.RuntimeWorkspaceRoot.empty())
    {
        throw std::invalid_argument("INPX cache and runtime roots are required.");
    }
    if (config.MaxCoverCacheBytes == 0 || config.MaxSteadyStateMemoryBytes < 3)
    {
        throw std::invalid_argument(
            "INPX resource budgets must reserve positive cursor, scan, and download memory.");
    }
    if (config.MaxConcurrentDownloads == 0)
    {
        throw std::invalid_argument("Maximum concurrent downloads must be positive.");
    }
    if (ResolveDownloadManifestMemoryBudget(
            config.MaxSteadyStateMemoryBytes,
            config.MaxConcurrentDownloads) == 0)
    {
        throw std::invalid_argument(
            "Maximum concurrent downloads exceeds the reserved download manifest memory budget.");
    }

    SInpxCatalogApplicationConfig resolvedConfig = config;
    std::optional<SInpxSourceInfo> newSource = std::nullopt;
    if (config.CacheOpenMode == ECacheOpenMode::CreateNew)
    {
        newSource = ValidateNewSource(config);
        resolvedConfig.InpxSource = newSource;
    }
    else
    {
        resolvedConfig.InpxSource = NormalizeConfiguredSource(config.InpxSource);
    }

    CInpxCacheBootstrap::PrepareCacheRoot(config.CacheRoot, config.CacheOpenMode);
    std::filesystem::create_directories(config.RuntimeWorkspaceRoot);
    const auto databasePath = ComputeDatabasePath(config.CacheRoot);
    DatabaseRuntime::CSchemaInitializer::InitializeCatalog(
        databasePath,
        [&](const Sqlite::CSqliteConnection& connection) {
            if (!newSource.has_value())
            {
                newSource = ValidateNewSource(resolvedConfig);
                resolvedConfig.InpxSource = newSource;
            }
            PersistNewSource(
                connection,
                *newSource,
                Foundation::CSha256Fingerprint::ComputeFile(newSource->InpxPath));
        });
    m_impl = std::make_unique<SImpl>(resolvedConfig);
}

CInpxCatalogApplication::~CInpxCatalogApplication() = default;

SCatalogSessionInfo CInpxCatalogApplication::GetCatalogSessionInfo()
{
    const auto sourceState = m_impl->ResolveSourceState();
    return {
        .Capabilities = {
            .CanRescanInpxSource = sourceState.CanRescan,
            .CanDownloadOriginal = sourceState.CanDownload,
            .CanDownloadAsEpub = sourceState.CanDownload && CanDownloadAsEpub(m_impl->converterPtr)
        },
        .InpxSource = m_impl->source
    };
}

SBookListResult CInpxCatalogApplication::ListBooks(const SBookListRequest& request)
{
    auto result = m_impl->catalogFacade.ListBooks(request);
    const auto sourceState = m_impl->ResolveSourceState(
        false,
        &result.CatalogSourceFingerprintUtf8);
    const bool canDownloadEpub = sourceState.CanDownload && CanDownloadAsEpub(m_impl->converterPtr);
    for (auto& item : result.Items)
    {
        item.CanDownloadOriginal = item.IsAvailable && sourceState.CanDownload;
        item.CanDownloadAsEpub = item.IsAvailable
            && item.Format == Domain::EBookFormat::Fb2
            && canDownloadEpub;
    }
    return result;
}

std::optional<SBookDetails> CInpxCatalogApplication::GetBookDetails(const Domain::SBookId id)
{
    auto result = m_impl->catalogFacade.GetBookDetails(id);
    if (result.has_value())
    {
        const auto sourceState = m_impl->ResolveSourceState();
        result->CanDownloadOriginal = result->IsAvailable && sourceState.CanDownload;
        result->CanDownloadAsEpub = result->IsAvailable
            && sourceState.CanDownload
            && result->Format == Domain::EBookFormat::Fb2
            && CanDownloadAsEpub(m_impl->converterPtr);
    }
    return result;
}

SCatalogStatistics CInpxCatalogApplication::GetCatalogStatistics()
{
    return m_impl->catalogFacade.GetCatalogStatistics();
}

std::optional<SInpxSourceOverview> CInpxCatalogApplication::GetInpxSourceOverview()
{
    const auto sourceState = m_impl->ResolveSourceState();
    return LoadOverview(m_impl->databasePath, m_impl->source, sourceState);
}

ApplicationJobs::TInpxScanJobId CInpxCatalogApplication::StartInpxScan(
    const ApplicationJobs::SInpxScanRequest& request)
{
    const auto sourceState = m_impl->ResolveSourceState(true);
    if (!sourceState.CanRescan
        || !sourceState.CurrentFingerprintUtf8.has_value()
        || !m_impl->source.has_value())
    {
        throw Domain::CDomainException(
            Domain::EDomainErrorCode::Validation,
            sourceState.SourceWarningUtf8.empty()
                ? "The INPX source is unavailable."
                : sourceState.SourceWarningUtf8);
    }
    return m_impl->scanService.Start(
        *m_impl->source,
        request,
        sourceState.CurrentFingerprintUtf8);
}

std::optional<ApplicationJobs::SInpxScanJobSnapshot> CInpxCatalogApplication::GetInpxScanJobSnapshot(
    const ApplicationJobs::TInpxScanJobId jobId)
{
    return m_impl->scanService.TryGetSnapshot(jobId);
}

std::optional<ApplicationJobs::SInpxScanJobResult> CInpxCatalogApplication::GetInpxScanJobResult(
    const ApplicationJobs::TInpxScanJobId jobId)
{
    return m_impl->scanService.TryGetResult(jobId);
}

bool CInpxCatalogApplication::CancelInpxScanJob(const ApplicationJobs::TInpxScanJobId jobId)
{
    return m_impl->scanService.Cancel(jobId);
}

bool CInpxCatalogApplication::RemoveInpxScanJob(const ApplicationJobs::TInpxScanJobId jobId)
{
    return m_impl->scanService.Remove(jobId);
}

std::optional<SPreparedBookDownload> CInpxCatalogApplication::PrepareDownload(const SBookDownloadRequest& request)
{
    const auto sourceState = m_impl->ResolveSourceState(true);
    if (!sourceState.CanDownload)
    {
        throw Domain::CDomainException(
            Domain::EDomainErrorCode::Validation,
            sourceState.SourceWarningUtf8.empty()
                ? "The INPX source is unavailable."
                : sourceState.SourceWarningUtf8);
    }
    return m_impl->downloadFacade.PrepareDownload(request);
}

} // namespace InpxWebReader::Application
