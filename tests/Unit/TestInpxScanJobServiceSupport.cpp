#include "TestInpxScanJobServiceSupport.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include <zip.h>

#include "App/CInpxCatalogApplication.hpp"
#include "App/InpxScanJobService.hpp"
#include "Database/CatalogStatisticsMaintenance.hpp"
#include "Database/SqliteConnection.hpp"
#include "Database/SqliteStatement.hpp"
#include "Database/SqliteTransaction.hpp"
#include "Foundation/Logging.hpp"
#include "Foundation/UnicodeConversion.hpp"
#include "Foundation/Sha256Fingerprint.hpp"
#include "TestWorkspace.hpp"

namespace InpxWebReader::Tests::InpxScanJobServiceSupport {

namespace {

const std::string GFieldSeparator(1, '\x04');

} // namespace

std::chrono::seconds InpxScanWaitTimeout()
{
    return std::chrono::seconds{60};
}

[[nodiscard]] bool WaitForApplicationScan(
    InpxWebReader::Application::IInpxCatalogApplication& application,
    const InpxWebReader::ApplicationJobs::TInpxScanJobId jobId,
    const std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        const auto snapshot = application.GetInpxScanJobSnapshot(jobId);
        if (snapshot.has_value() && snapshot->IsTerminal())
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    return false;
}

namespace {

void AddZipEntry(zip_t* archive, const std::string& entryName, const std::string& content)
{
    zip_source_t* source = zip_source_buffer(archive, content.data(), content.size(), 0);
    if (source == nullptr)
    {
        throw std::runtime_error("Failed to allocate ZIP source.");
    }

    if (zip_file_add(archive, entryName.c_str(), source, ZIP_FL_OVERWRITE | ZIP_FL_ENC_UTF_8) < 0)
    {
        zip_source_free(source);
        throw std::runtime_error("Failed to add ZIP entry.");
    }
}

} // namespace

std::string MakeInpxRecord(
    const std::string& fileName,
    const std::string& libId,
    const std::string& language,
    const std::string& unusedField12,
    const bool deleted,
    const std::string& fileExtension,
    const std::size_t fileSizeBytes)
{
    return std::string("Author")
        + GFieldSeparator + "genre"
        + GFieldSeparator + "Title " + libId
        + GFieldSeparator
        + GFieldSeparator
        + GFieldSeparator + fileName
        + GFieldSeparator + std::to_string(fileSizeBytes)
        + GFieldSeparator + libId
        + GFieldSeparator + (deleted ? "1" : "0")
        + GFieldSeparator + fileExtension
        + GFieldSeparator
        + GFieldSeparator + language
        + GFieldSeparator + unusedField12
        + GFieldSeparator
        + GFieldSeparator + "\n";
}

std::filesystem::path WriteInpxArchiveEntries(
    const std::filesystem::path& archivePath,
    const std::vector<std::pair<std::string, std::string>>& entries)
{
    std::filesystem::create_directories(archivePath.parent_path());

    int errorCode = ZIP_ER_OK;
    const auto archivePathUtf8 = InpxWebReader::Unicode::PathToUtf8(archivePath);
    zip_t* archive = zip_open(archivePathUtf8.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &errorCode);
    if (archive == nullptr)
    {
        throw std::runtime_error("Failed to create INPX fixture archive.");
    }

    for (const auto& [entryName, content] : entries)
    {
        AddZipEntry(archive, entryName, content);
    }

    if (zip_close(archive) != 0)
    {
        zip_discard(archive);
        throw std::runtime_error("Failed to finalize INPX fixture archive.");
    }

    return archivePath;
}

std::filesystem::path WriteInpxArchive(
    const std::filesystem::path& archivePath,
    const std::string& content)
{
    return WriteInpxArchiveEntries(
        archivePath,
        {
            {"fb2-main.zip.inp", content}
        });
}

std::string MakeFb2Payload(
    const std::string& title,
    const bool includeCover,
    const std::string& language)
{
    std::string payload = std::string("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n")
        + "<FictionBook xmlns:l=\"http://www.w3.org/1999/xlink\">"
        + "<description><title-info><book-title>" + title + "</book-title>"
        + "<author><first-name>Ivan</first-name><last-name>Testov</last-name></author>"
        + "<lang>" + language + "</lang>";
    if (includeCover)
    {
        payload += "<coverpage><image l:href=\"#cover.png\"/></coverpage>";
    }
    payload += "</title-info></description>";
    payload += "<body><section><p>Payload</p></section></body>";
    if (includeCover)
    {
        payload += "<binary id=\"cover.png\" content-type=\"image/png\">"
            "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+/p9sAAAAASUVORK5CYII="
            "</binary>";
    }
    payload += "</FictionBook>";
    return payload;
}

std::filesystem::path WriteInpxArchive(
    const std::filesystem::path& archivePath,
    const std::size_t recordCount,
    const bool includeCover)
{
    std::vector<std::string> titles;
    titles.reserve(recordCount);
    for (std::size_t index = 0; index < recordCount; ++index)
    {
        titles.push_back("Payload Title " + std::to_string(index + 1));
    }

    std::filesystem::create_directories(archivePath.parent_path());

    int errorCode = ZIP_ER_OK;
    const auto archivePathUtf8 = InpxWebReader::Unicode::PathToUtf8(archivePath);
    zip_t* archive = zip_open(archivePathUtf8.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &errorCode);
    if (archive == nullptr)
    {
        throw std::runtime_error("Failed to create INPX payload archive.");
    }

    std::vector<std::string> payloads;
    payloads.reserve(recordCount);
    for (std::size_t index = 0; index < recordCount; ++index)
    {
        const auto entryName = "book-" + std::to_string(index + 1) + ".fb2";
        payloads.push_back(MakeFb2Payload(titles[index], includeCover));
        AddZipEntry(archive, entryName, payloads.back());
    }

    if (zip_close(archive) != 0)
    {
        zip_discard(archive);
        throw std::runtime_error("Failed to finalize INPX payload archive.");
    }

    return archivePath;
}

std::filesystem::path WriteInpxArchive(
    const std::filesystem::path& archivePath,
    const std::vector<std::string>& titles,
    const bool includeCover)
{
    std::filesystem::create_directories(archivePath.parent_path());

    int errorCode = ZIP_ER_OK;
    const auto archivePathUtf8 = InpxWebReader::Unicode::PathToUtf8(archivePath);
    zip_t* archive = zip_open(archivePathUtf8.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &errorCode);
    if (archive == nullptr)
    {
        throw std::runtime_error("Failed to create INPX payload archive.");
    }

    std::vector<std::string> payloads;
    payloads.reserve(titles.size());
    for (std::size_t index = 0; index < titles.size(); ++index)
    {
        const auto entryName = "book-" + std::to_string(index + 1) + ".fb2";
        payloads.push_back(MakeFb2Payload(titles[index], includeCover));
        AddZipEntry(archive, entryName, payloads.back());
    }

    if (zip_close(archive) != 0)
    {
        zip_discard(archive);
        throw std::runtime_error("Failed to finalize INPX payload archive.");
    }

    return archivePath;
}

std::filesystem::path WriteSinglePayloadInpxArchive(
    const std::filesystem::path& archivePath,
    const std::string& payload)
{
    std::filesystem::create_directories(archivePath.parent_path());

    int errorCode = ZIP_ER_OK;
    const auto archivePathUtf8 = InpxWebReader::Unicode::PathToUtf8(archivePath);
    zip_t* archive = zip_open(archivePathUtf8.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &errorCode);
    if (archive == nullptr)
    {
        throw std::runtime_error("Failed to create INPX payload archive.");
    }

    AddZipEntry(archive, "book-1.fb2", payload);

    if (zip_close(archive) != 0)
    {
        zip_discard(archive);
        throw std::runtime_error("Failed to finalize INPX payload archive.");
    }

    return archivePath;
}

std::string ReadInpxBookAvailability(
    const std::filesystem::path& databasePath,
    const std::string& libId)
{
    InpxWebReader::Sqlite::CSqliteConnection connection(databasePath);
    InpxWebReader::Sqlite::CSqliteStatement statement(
        connection.GetNativeHandle(),
        "SELECT availability FROM inpx_book_locations WHERE lib_id = ?;");
    statement.BindText(1, libId);
    if (!statement.Step())
    {
        throw std::runtime_error("INPX location row was not found.");
    }

    return statement.GetColumnText(0);
}

bool ReadInpxPresence(
    const std::filesystem::path& databasePath,
    const std::string& libId)
{
    InpxWebReader::Sqlite::CSqliteConnection connection(databasePath);
    InpxWebReader::Sqlite::CSqliteStatement statement(
        connection.GetNativeHandle(),
        "SELECT present_in_segment FROM inpx_book_locations WHERE lib_id = ?;");
    statement.BindText(1, libId);
    if (!statement.Step())
    {
        throw std::runtime_error("INPX location row was not found.");
    }
    return statement.GetColumnInt(0) != 0;
}

std::int64_t CountInpxDeletionMarkers(
    const std::filesystem::path& databasePath,
    const std::string& libId)
{
    InpxWebReader::Sqlite::CSqliteConnection connection(databasePath);
    InpxWebReader::Sqlite::CSqliteStatement statement(
        connection.GetNativeHandle(),
        "SELECT COUNT(*) FROM inpx_deletions WHERE lib_id = ?;");
    statement.BindText(1, libId);
    if (!statement.Step())
    {
        throw std::runtime_error("Failed to count INPX deletion markers.");
    }
    return statement.GetColumnInt64(0);
}

bool ReadSegmentRequiresArchive(
    const std::filesystem::path& databasePath,
    const std::string& inpEntryNameUtf8)
{
    InpxWebReader::Sqlite::CSqliteConnection connection(databasePath);
    InpxWebReader::Sqlite::CSqliteStatement statement(
        connection.GetNativeHandle(),
        "SELECT requires_archive FROM inpx_segments WHERE inp_entry_name = ?;");
    statement.BindText(1, inpEntryNameUtf8);
    if (!statement.Step())
    {
        throw std::runtime_error("INPX segment row was not found.");
    }
    return statement.GetColumnInt(0) != 0;
}

std::int64_t CountPersistedWarnings(const std::filesystem::path& databasePath)
{
    InpxWebReader::Sqlite::CSqliteConnection connection(databasePath);
    InpxWebReader::Sqlite::CSqliteStatement statement(
        connection.GetNativeHandle(),
        "SELECT COUNT(*) FROM inpx_scan_warnings;");
    if (!statement.Step())
    {
        throw std::runtime_error("Failed to count INPX scan warnings.");
    }

    return statement.GetColumnInt64(0);
}

std::int64_t CountPersistedWarningScans(const std::filesystem::path& databasePath)
{
    InpxWebReader::Sqlite::CSqliteConnection connection(databasePath);
    InpxWebReader::Sqlite::CSqliteStatement statement(
        connection.GetNativeHandle(),
        "SELECT COUNT(DISTINCT scan_id) FROM inpx_scan_warnings;");
    if (!statement.Step())
    {
        throw std::runtime_error("Failed to count INPX warning scans.");
    }
    return statement.GetColumnInt64(0);
}

std::string ReadSourceFingerprint(const std::filesystem::path& databasePath)
{
    InpxWebReader::Sqlite::CSqliteConnection connection(databasePath);
    InpxWebReader::Sqlite::CSqliteStatement statement(
        connection.GetNativeHandle(),
        "SELECT source_fingerprint FROM inpx_sources WHERE id = 1;");
    if (!statement.Step())
    {
        throw std::runtime_error("INPX source row was not found.");
    }
    return statement.GetColumnText(0);
}

std::string ReadSourceDisplayName(const std::filesystem::path& databasePath)
{
    InpxWebReader::Sqlite::CSqliteConnection connection(databasePath);
    InpxWebReader::Sqlite::CSqliteStatement statement(
        connection.GetNativeHandle(),
        "SELECT display_name FROM inpx_sources WHERE id = 1;");
    if (!statement.Step())
    {
        throw std::runtime_error("INPX source row was not found.");
    }
    return statement.GetColumnText(0);
}

std::int64_t CountBooksWithCoverPath(const std::filesystem::path& databasePath)
{
    InpxWebReader::Sqlite::CSqliteConnection connection(databasePath);
    InpxWebReader::Sqlite::CSqliteStatement statement(
        connection.GetNativeHandle(),
        "SELECT COUNT(*) FROM inpx_books WHERE cover_path IS NOT NULL AND cover_path <> '';");
    if (!statement.Step())
    {
        throw std::runtime_error("Failed to count book cover paths.");
    }

    return statement.GetColumnInt64(0);
}

std::int64_t CountBookRows(const std::filesystem::path& databasePath)
{
    InpxWebReader::Sqlite::CSqliteConnection connection(databasePath);
    InpxWebReader::Sqlite::CSqliteStatement statement(
        connection.GetNativeHandle(),
        "SELECT COUNT(*) FROM inpx_books;");
    if (!statement.Step())
    {
        throw std::runtime_error("Failed to count books.");
    }

    return statement.GetColumnInt64(0);
}

std::size_t CountCoverFiles(const std::filesystem::path& cacheRoot)
{
    const auto coverRoot = cacheRoot / "Covers";
    if (!std::filesystem::exists(coverRoot))
    {
        return 0;
    }

    std::size_t count = 0;
    std::error_code errorCode;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(coverRoot, errorCode))
    {
        if (entry.is_regular_file(errorCode))
        {
            ++count;
        }
        errorCode.clear();
    }

    return count;
}

std::uint64_t SumCoverFileBytes(const std::filesystem::path& cacheRoot)
{
    const auto coverRoot = cacheRoot / "Covers";
    if (!std::filesystem::exists(coverRoot))
    {
        return 0;
    }

    std::uint64_t totalBytes = 0;
    std::error_code errorCode;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(coverRoot, errorCode))
    {
        if (entry.is_regular_file(errorCode))
        {
            std::error_code fileSizeError;
            const auto fileSize = entry.file_size(fileSizeError);
            if (!fileSizeError)
            {
                totalBytes += static_cast<std::uint64_t>(fileSize);
            }
        }
        errorCode.clear();
    }

    return totalBytes;
}

std::optional<std::string> ReadCoverPath(const std::filesystem::path& databasePath, const std::string& libId)
{
    InpxWebReader::Sqlite::CSqliteConnection connection(databasePath);
    InpxWebReader::Sqlite::CSqliteStatement statement(
        connection.GetNativeHandle(),
        "SELECT b.cover_path "
        "FROM inpx_books b "
        "INNER JOIN inpx_book_locations l ON l.book_id = b.id "
        "WHERE l.lib_id = ?;");
    statement.BindText(1, libId);
    if (!statement.Step())
    {
        throw std::runtime_error("INPX book row was not found.");
    }

    if (statement.IsColumnNull(0))
    {
        return std::nullopt;
    }

    return statement.GetColumnText(0);
}

void UpdateCoverPath(
    const std::filesystem::path& databasePath,
    const std::string& libId,
    const std::optional<std::string>& coverPathUtf8)
{
    InpxWebReader::Sqlite::CSqliteConnection connection(databasePath);
    InpxWebReader::Sqlite::CSqliteStatement statement(
        connection.GetNativeHandle(),
        "UPDATE inpx_books "
        "SET cover_path = ? "
        "WHERE id = (SELECT book_id FROM inpx_book_locations WHERE lib_id = ?);");
    coverPathUtf8.has_value() ? statement.BindText(1, *coverPathUtf8) : statement.BindNull(1);
    statement.BindText(2, libId);
    static_cast<void>(statement.Step());
}

std::string ReadAllText(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

InpxWebReader::Domain::SCoverProcessingResult CRecordingCoverImageProcessor::ProcessForCache(
    const InpxWebReader::Domain::SCoverProcessingRequest& request) const
{
    {
        std::lock_guard lock(m_mutex);
        m_threadIds.push_back(std::this_thread::get_id());
    }

    return {
        .Status = InpxWebReader::Domain::ECoverProcessingStatus::Unchanged,
        .Cover = request.Cover,
        .PixelWidth = 1,
        .PixelHeight = 1
    };
}

std::vector<std::thread::id> CRecordingCoverImageProcessor::GetThreadIds() const
{
    std::lock_guard lock(m_mutex);
    return m_threadIds;
}

InpxWebReader::Domain::SCoverProcessingResult CBlockingCoverImageProcessor::ProcessForCache(
    const InpxWebReader::Domain::SCoverProcessingRequest& request) const
{
    static_cast<void>(request);
    {
        std::unique_lock lock(m_mutex);
        ++m_startedCount;
        m_condition.notify_all();
        m_condition.wait(lock, [this] {
            return m_release;
        });
        ++m_finishedCount;
        m_condition.notify_all();
    }

    throw std::runtime_error("injected queued payload failure");
}

bool CBlockingCoverImageProcessor::WaitUntilStarted(
    const std::size_t expectedCount,
    const std::chrono::milliseconds timeout) const
{
    std::unique_lock lock(m_mutex);
    return m_condition.wait_for(lock, timeout, [this, expectedCount] {
        return m_startedCount >= expectedCount;
    });
}

bool CBlockingCoverImageProcessor::WaitUntilFinished(
    const std::size_t expectedCount,
    const std::chrono::milliseconds timeout) const
{
    std::unique_lock lock(m_mutex);
    return m_condition.wait_for(lock, timeout, [this, expectedCount] {
        return m_finishedCount >= expectedCount;
    });
}

void CBlockingCoverImageProcessor::Release() const
{
    {
        std::lock_guard lock(m_mutex);
        m_release = true;
    }
    m_condition.notify_all();
}

InpxWebReader::Application::SInpxCatalogApplicationConfig MakeBaseConfig(const CTestWorkspace& sandbox)
{
    return {
        .CacheRoot = sandbox.GetPath() / "Cache",
        .RuntimeWorkspaceRoot = sandbox.GetPath() / "Runtime",
        .CacheOpenMode = InpxWebReader::Application::ECacheOpenMode::CreateNew
    };
}

InpxWebReader::Application::SInpxCatalogApplicationConfig MakeInpxConfig(
    const CTestWorkspace& sandbox,
    const std::size_t recordCount)
{
    std::string records;
    for (std::size_t index = 0; index < recordCount; ++index)
    {
        records += MakeInpxRecord("book-" + std::to_string(index + 1), std::to_string(index + 1));
    }

    auto config = MakeBaseConfig(sandbox);
    config.InpxSource = InpxWebReader::Application::SInpxSourceInfo{
        .InpxPath = WriteInpxArchive(sandbox.GetPath() / "source" / "catalog.inpx", records),
        .ArchiveRoot = sandbox.GetPath() / "source" / "archives"
    };
    std::filesystem::create_directories(config.InpxSource->ArchiveRoot);
    WriteInpxArchive(config.InpxSource->ArchiveRoot / "fb2-main.zip", recordCount);
    return config;
}

} // namespace InpxWebReader::Tests::InpxScanJobServiceSupport
