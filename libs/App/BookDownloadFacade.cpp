#include "App/BookDownloadFacade.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "App/InpxArchiveAccess.hpp"
#include "App/IInpxCatalogApplication.hpp"
#include "Database/SqliteConnection.hpp"
#include "Database/SqliteStatement.hpp"
#include "Database/SqliteTransaction.hpp"
#include "Domain/DomainError.hpp"
#include "Domain/InpxBookAvailability.hpp"
#include "Foundation/FileSystemUtils.hpp"
#include "Foundation/Logging.hpp"
#include "Foundation/Sha256Fingerprint.hpp"
#include "Foundation/UnicodeConversion.hpp"
#include "Storage/PathSafety.hpp"
#include "Storage/RuntimeWorkspaceLayout.hpp"

namespace InpxWebReader::Application {
namespace {

class CScopedDirectoryCleanup final
{
public:
    explicit CScopedDirectoryCleanup(std::filesystem::path path)
        : m_path(std::move(path))
    {
    }

    ~CScopedDirectoryCleanup() noexcept
    {
        InpxWebReader::Foundation::RemovePathBestEffortNoThrow(m_path);
    }

    CScopedDirectoryCleanup(const CScopedDirectoryCleanup&) = delete;
    CScopedDirectoryCleanup& operator=(const CScopedDirectoryCleanup&) = delete;

private:
    std::filesystem::path m_path;
};

class CNoOpProgressSink final : public InpxWebReader::Domain::IProgressSink
{
public:
    void ReportValue(int, std::string_view) override
    {
    }

    [[nodiscard]] bool IsCancellationRequested() const override
    {
        return false;
    }
};

struct SInpxDownloadSource
{
    std::string EntryNameUtf8;
    std::string Availability;
    InpxWebReader::Domain::EBookFormat Format = InpxWebReader::Domain::EBookFormat::Fb2;
    std::string ResolvedArchivePathUtf8;
    std::uint64_t EntrySizeBytes = 0;
    SInpxArchiveFileState ArchiveFileState;
    std::string ArchiveManifestFingerprintUtf8;
    std::string SourceFingerprintUtf8;
};

struct SBookDownloadSnapshot
{
    std::string TitleUtf8;
    std::vector<std::string> AuthorsUtf8;
    SInpxDownloadSource Source;
};

[[noreturn]] void ThrowSourceSnapshotMismatch(const std::string& detail)
{
    throw InpxWebReader::Domain::CDomainException(
        InpxWebReader::Domain::EDomainErrorCode::IntegrityIssue,
        "The INPX source no longer matches the scanned download snapshot: " + detail);
}

void ThrowIfCancelled(const std::stop_token stopToken)
{
    if (stopToken.stop_requested())
    {
        throw InpxWebReader::Domain::CDomainException(
            InpxWebReader::Domain::EDomainErrorCode::Cancellation,
            "Download preparation was cancelled.");
    }
}

[[nodiscard]] std::filesystem::path CanonicalizeForComparison(const std::filesystem::path& path)
{
    const auto base = path.has_filename() ? path.parent_path() : path;
    std::error_code errorCode;
    const auto canonicalBase = std::filesystem::weakly_canonical(base, errorCode);
    if (errorCode)
    {
        throw std::runtime_error("Download destination path could not be canonicalized.");
    }
    return path.has_filename()
        ? (canonicalBase / path.filename()).lexically_normal()
        : canonicalBase.lexically_normal();
}

[[nodiscard]] std::optional<SBookDownloadSnapshot> LoadDownloadSnapshot(
    const std::filesystem::path& databasePath,
    const InpxWebReader::Domain::SBookId bookId)
{
    InpxWebReader::Sqlite::CSqliteConnection connection(databasePath);
    InpxWebReader::Sqlite::CSqliteTransaction transaction(
        connection,
        InpxWebReader::Sqlite::ESqliteTransactionMode::Deferred);
    InpxWebReader::Sqlite::CSqliteStatement statement(
        connection.GetNativeHandle(),
        "SELECT b.title, a.display_name, l.entry_name, l.availability, l.format, "
        "       l.file_size_bytes, s.resolved_archive_path, s.archive_file_size_bytes, "
        "       s.archive_mtime_ticks, s.archive_manifest_fingerprint, src.source_fingerprint "
        "FROM inpx_books b "
        "LEFT JOIN inpx_book_locations l ON l.book_id = b.id "
        "LEFT JOIN inpx_segments s ON s.id = l.segment_id "
        "LEFT JOIN inpx_sources src ON src.id = l.source_id "
        "LEFT JOIN book_authors ba ON ba.book_id = b.id "
        "LEFT JOIN authors a ON a.id = ba.author_id "
        "WHERE b.id = ? "
        "ORDER BY ba.author_order, a.id;");
    statement.BindInt64(1, bookId.Value);
    if (!statement.Step())
    {
        transaction.Commit();
        return std::nullopt;
    }

    if (statement.IsColumnNull(2) || statement.IsColumnNull(3) || statement.IsColumnNull(4))
    {
        throw InpxWebReader::Domain::CDomainException(
            InpxWebReader::Domain::EDomainErrorCode::IntegrityIssue,
            "Book does not have an INPX source locator.");
    }

    const auto format = InpxWebReader::Domain::TryParseBookFormat(statement.GetColumnText(4));
    if (!format.has_value())
    {
        throw InpxWebReader::Domain::CDomainException(
            InpxWebReader::Domain::EDomainErrorCode::IntegrityIssue,
            "INPX book locator contains an unsupported format.");
    }
    if (statement.IsColumnNull(6)
        || statement.IsColumnNull(9)
        || statement.IsColumnNull(10)
        || statement.GetColumnInt64(5) < 0
        || statement.GetColumnInt64(7) < 0)
    {
        throw InpxWebReader::Domain::CDomainException(
            InpxWebReader::Domain::EDomainErrorCode::IntegrityIssue,
            "INPX book locator is missing its persisted source guards.");
    }

    SBookDownloadSnapshot snapshot{
        .TitleUtf8 = statement.GetColumnText(0),
        .Source = {
            .EntryNameUtf8 = statement.GetColumnText(2),
            .Availability = statement.GetColumnText(3),
            .Format = *format,
            .ResolvedArchivePathUtf8 = statement.GetColumnText(6),
            .EntrySizeBytes = static_cast<std::uint64_t>(statement.GetColumnInt64(5)),
            .ArchiveFileState = {
                .FileSizeBytes = static_cast<std::uint64_t>(statement.GetColumnInt64(7)),
                .MtimeTicks = statement.GetColumnInt64(8)
            },
            .ArchiveManifestFingerprintUtf8 = statement.GetColumnText(9),
            .SourceFingerprintUtf8 = statement.GetColumnText(10)
        }
    };
    for (;;)
    {
        if (!statement.IsColumnNull(1))
        {
            snapshot.AuthorsUtf8.push_back(statement.GetColumnText(1));
        }
        if (!statement.Step())
        {
            break;
        }
    }
    transaction.Commit();
    return snapshot;
}

void ValidateInpxSourceFingerprint(
    const SBookDownloadSource& source,
    const SInpxDownloadSource& locator,
    const std::function<void()>& sourceFingerprintCheckpoint)
{
    try
    {
        if (Foundation::CSha256Fingerprint::ComputeFile(
                source.InpxPath,
                sourceFingerprintCheckpoint)
            != locator.SourceFingerprintUtf8)
        {
            ThrowSourceSnapshotMismatch("the INPX file changed; rescan is required.");
        }
    }
    catch (const InpxWebReader::Domain::CDomainException&)
    {
        throw;
    }
    catch (const std::exception& exception)
    {
        ThrowSourceSnapshotMismatch(exception.what());
    }
}

[[nodiscard]] CInpxArchiveReader OpenValidatedArchiveSnapshot(
    const SBookDownloadSource& source,
    const SInpxDownloadSource& locator,
    const std::uint64_t maxArchiveManifestMemoryBytes,
    const std::function<void()>& manifestCheckpoint)
{
    try
    {
        const SInpxSourceInfo sourceInfo{
            .InpxPath = source.InpxPath,
            .ArchiveRoot = source.ArchiveRoot
        };
        const auto archivePath = ResolvePersistedInpxArchivePath(
            sourceInfo,
            locator.ResolvedArchivePathUtf8);
        CInpxArchiveReader reader(archivePath);
        const auto currentState = reader.GetFileState();
        if (currentState.FileSizeBytes != locator.ArchiveFileState.FileSizeBytes
            || currentState.MtimeTicks != locator.ArchiveFileState.MtimeTicks)
        {
            ThrowSourceSnapshotMismatch("the selected archive changed; rescan is required.");
        }

        if (reader.ComputeManifestFingerprint(
                maxArchiveManifestMemoryBytes,
                manifestCheckpoint)
            != locator.ArchiveManifestFingerprintUtf8)
        {
            ThrowSourceSnapshotMismatch("the selected archive manifest changed; rescan is required.");
        }
        if (reader.ReadEntrySize(locator.EntryNameUtf8) != locator.EntrySizeBytes)
        {
            ThrowSourceSnapshotMismatch("the selected archive entry size changed; rescan is required.");
        }
        return reader;
    }
    catch (const InpxWebReader::Domain::CDomainException&)
    {
        throw;
    }
    catch (const std::exception& exception)
    {
        ThrowSourceSnapshotMismatch(exception.what());
    }
}

void ValidateDownloadSnapshot(
    const SBookDownloadSource& source,
    const SInpxDownloadSource& locator,
    const std::uint64_t maxArchiveManifestMemoryBytes,
    const std::function<void()>& sourceFingerprintCheckpoint,
    const std::function<void()>& manifestCheckpoint)
{
    ValidateInpxSourceFingerprint(source, locator, sourceFingerprintCheckpoint);
    static_cast<void>(OpenValidatedArchiveSnapshot(
        source,
        locator,
        maxArchiveManifestMemoryBytes,
        manifestCheckpoint));
}

} // namespace

CBookDownloadFacade::CBookDownloadFacade(
    std::filesystem::path cacheRoot,
    std::optional<SBookDownloadSource> source,
    const InpxWebReader::Domain::IBookConverter* converter,
    std::filesystem::path runtimeWorkspaceRoot,
    std::filesystem::path databasePath,
    const std::uint64_t maxArchiveManifestMemoryBytes,
    SFileReplacementHooks replacementHooks,
    SBookDownloadHooks downloadHooks)
    : m_cacheRoot(std::move(cacheRoot))
    , m_source(std::move(source))
    , m_converter(converter)
    , m_runtimeWorkspaceRoot(std::move(runtimeWorkspaceRoot))
    , m_databasePath(std::move(databasePath))
    , m_replacementHooks(std::move(replacementHooks))
    , m_downloadHooks(std::move(downloadHooks))
    , m_maxArchiveManifestMemoryBytes(maxArchiveManifestMemoryBytes)
{
}

std::optional<SPreparedBookDownload> CBookDownloadFacade::PrepareDownload(
    const SBookDownloadRequest& request) const
{
    if (!request.BookId.IsValid())
    {
        throw std::invalid_argument("Download request must use a valid book id.");
    }
    if (request.DestinationPath.empty() || !request.DestinationPath.is_absolute())
    {
        throw std::invalid_argument("Download destination path must be absolute.");
    }
    if (std::filesystem::is_directory(request.DestinationPath))
    {
        throw std::invalid_argument("Download destination path must point to a file.");
    }
    if (InpxWebReader::SafePaths::IsPathWithinRoot(
            CanonicalizeForComparison(m_cacheRoot),
            CanonicalizeForComparison(request.DestinationPath)))
    {
        throw std::invalid_argument("Download destination path must be outside the INPX cache root.");
    }

    if (!m_source.has_value())
    {
        throw InpxWebReader::Domain::CDomainException(
            InpxWebReader::Domain::EDomainErrorCode::Validation,
            "INPX source is not configured.");
    }
    const auto snapshot = LoadDownloadSnapshot(m_databasePath, request.BookId);
    if (!snapshot.has_value())
    {
        return std::nullopt;
    }
    const auto& source = snapshot->Source;
    if (!InpxWebReader::Domain::IsInpxBookAvailable(source.Availability))
    {
        throw InpxWebReader::Domain::CDomainException(
            InpxWebReader::Domain::EDomainErrorCode::NotFound,
            "Book is unavailable in the current INPX source.");
    }
    if (m_downloadHooks.AfterSnapshotLoaded)
    {
        m_downloadHooks.AfterSnapshotLoaded();
    }

    ThrowIfCancelled(request.StopToken);
    const std::function<void()> sourceFingerprintCheckpoint = [this, stopToken = request.StopToken]() {
        if (m_downloadHooks.BeforeSourceFingerprintCheckpoint)
        {
            m_downloadHooks.BeforeSourceFingerprintCheckpoint();
        }
        ThrowIfCancelled(stopToken);
    };
    const std::function<void()> manifestCheckpoint = [this, stopToken = request.StopToken]() {
        if (m_downloadHooks.BeforeArchiveManifestCheckpoint)
        {
            m_downloadHooks.BeforeArchiveManifestCheckpoint();
        }
        ThrowIfCancelled(stopToken);
    };
    ValidateDownloadSnapshot(
        *m_source,
        source,
        m_maxArchiveManifestMemoryBytes,
        sourceFingerprintCheckpoint,
        manifestCheckpoint);

    std::filesystem::create_directories(request.DestinationPath.parent_path());
    const auto downloadSourceDirectory = StoragePlanning::BuildRuntimeWorkspaceLayout(
        m_runtimeWorkspaceRoot).DownloadSourceDirectory;
    const auto sourceWorkspace = InpxWebReader::Foundation::CreateUniqueDirectory(
        downloadSourceDirectory,
        "download-");
    CScopedDirectoryCleanup sourceWorkspaceCleanup(sourceWorkspace);
    const auto temporarySourcePath = sourceWorkspace
        / ("source." + std::string(InpxWebReader::Domain::ToString(source.Format)));
    auto archiveReader = OpenValidatedArchiveSnapshot(
        *m_source,
        source,
        m_maxArchiveManifestMemoryBytes,
        manifestCheckpoint);
    if (m_downloadHooks.AfterArchiveSnapshotValidated)
    {
        m_downloadHooks.AfterArchiveSnapshotValidated();
    }
    ThrowIfCancelled(request.StopToken);
    archiveReader.ExtractEntryToPath(
        source.EntryNameUtf8,
        temporarySourcePath,
        [this, stopToken = request.StopToken]() {
            if (m_downloadHooks.BeforeArchiveExtractionCheckpoint)
            {
                m_downloadHooks.BeforeArchiveExtractionCheckpoint();
            }
            ThrowIfCancelled(stopToken);
        });
    if (std::filesystem::file_size(temporarySourcePath) != source.EntrySizeBytes)
    {
        ThrowSourceSnapshotMismatch("the staged archive entry size is inconsistent.");
    }
    ThrowIfCancelled(request.StopToken);
    ValidateDownloadSnapshot(
        *m_source,
        source,
        m_maxArchiveManifestMemoryBytes,
        sourceFingerprintCheckpoint,
        manifestCheckpoint);

    const auto publicationWorkspace = InpxWebReader::Foundation::CreateUniqueDirectory(
        request.DestinationPath.parent_path(),
        ".inpx-web-reader-download-");
    CScopedDirectoryCleanup publicationWorkspaceCleanup(publicationWorkspace);
    const auto preparedDestinationPath = publicationWorkspace / request.DestinationPath.filename();

    const auto effectiveFormat = request.RequestedFormat.value_or(source.Format);
    if (request.RequestedFormat.has_value() && *request.RequestedFormat != source.Format)
    {
        const auto convertedPath = PrepareConvertedDownload(
            source.Format,
            temporarySourcePath,
            preparedDestinationPath,
            request.DestinationPath,
            request.StopToken,
            [this, source, sourceFingerprintCheckpoint, manifestCheckpoint]() {
                ValidateDownloadSnapshot(
                    *m_source,
                    source,
                    m_maxArchiveManifestMemoryBytes,
                    sourceFingerprintCheckpoint,
                    manifestCheckpoint);
            });
        return SPreparedBookDownload{
            .Path = convertedPath,
            .TitleUtf8 = snapshot->TitleUtf8,
            .AuthorsUtf8 = snapshot->AuthorsUtf8,
            .Format = effectiveFormat
        };
    }

    InpxWebReader::Foundation::MoveFileWithCopyFallback(
        temporarySourcePath,
        preparedDestinationPath,
        "PrepareDownload");
    ThrowIfCancelled(request.StopToken);
    ValidateDownloadSnapshot(
        *m_source,
        source,
        m_maxArchiveManifestMemoryBytes,
        sourceFingerprintCheckpoint,
        manifestCheckpoint);
    ReplaceDestinationWithPreparedFile(
        preparedDestinationPath,
        request.DestinationPath,
        "PrepareDownload",
        m_replacementHooks,
        [this,
            source,
            sourceFingerprintCheckpoint,
            manifestCheckpoint,
            stopToken = request.StopToken]() {
            ThrowIfCancelled(stopToken);
            ValidateDownloadSnapshot(
                *m_source,
                source,
                m_maxArchiveManifestMemoryBytes,
                sourceFingerprintCheckpoint,
                manifestCheckpoint);
        });
    return SPreparedBookDownload{
        .Path = request.DestinationPath,
        .TitleUtf8 = snapshot->TitleUtf8,
        .AuthorsUtf8 = snapshot->AuthorsUtf8,
        .Format = effectiveFormat
    };
}

std::filesystem::path CBookDownloadFacade::PrepareConvertedDownload(
    const InpxWebReader::Domain::EBookFormat sourceFormat,
    const std::filesystem::path& sourcePath,
    const std::filesystem::path& preparedDestinationPath,
    const std::filesystem::path& destinationPath,
    const std::stop_token stopToken,
    const std::function<void()>& validateSourceSnapshot) const
{
    if (sourceFormat != InpxWebReader::Domain::EBookFormat::Fb2
        || m_converter == nullptr
        || !m_converter->CanConvert(InpxWebReader::Domain::EBookFormat::Fb2, InpxWebReader::Domain::EBookFormat::Epub))
    {
        throw InpxWebReader::Domain::CDomainException(
            InpxWebReader::Domain::EDomainErrorCode::ConverterUnavailable,
            "Configured FB2 to EPUB converter is unavailable.");
    }

    CNoOpProgressSink progressSink;
    const auto result = m_converter->Convert(
        {
            .SourcePath = sourcePath,
            .DestinationPath = preparedDestinationPath,
            .SourceFormat = InpxWebReader::Domain::EBookFormat::Fb2,
            .DestinationFormat = InpxWebReader::Domain::EBookFormat::Epub
        },
        progressSink,
        stopToken);
    if (result.IsCancelled())
    {
        throw InpxWebReader::Domain::CDomainException(
            InpxWebReader::Domain::EDomainErrorCode::Cancellation,
            "Download conversion was cancelled.");
    }
    if (result.IsTimedOut())
    {
        throw InpxWebReader::Domain::CDomainException(
            InpxWebReader::Domain::EDomainErrorCode::ConverterTimeout,
            "Download conversion timed out.");
    }
    if (!result.IsSuccess())
    {
        throw InpxWebReader::Domain::CDomainException(
            InpxWebReader::Domain::EDomainErrorCode::ConverterFailed,
            result.Warnings.empty() ? "Download conversion failed." : result.Warnings.front());
    }

    const auto preparedPath = result.HasOutput() ? result.OutputPath : preparedDestinationPath;
    if (preparedPath.lexically_normal() != preparedDestinationPath.lexically_normal())
    {
        throw InpxWebReader::Domain::CDomainException(
            InpxWebReader::Domain::EDomainErrorCode::IntegrityIssue,
            "Converter returned output outside the prepared download path.");
    }
    ThrowIfCancelled(stopToken);
    validateSourceSnapshot();
    ReplaceDestinationWithPreparedFile(
        preparedPath,
        destinationPath,
        "PrepareDownload",
        m_replacementHooks,
        [&]() {
            ThrowIfCancelled(stopToken);
            validateSourceSnapshot();
        });
    InpxWebReader::Logging::InfoIfInitialized(
        "Converted INPX book prepared for download at '{}'.",
        InpxWebReader::Unicode::PathToUtf8(destinationPath));
    return destinationPath;
}

} // namespace InpxWebReader::Application
