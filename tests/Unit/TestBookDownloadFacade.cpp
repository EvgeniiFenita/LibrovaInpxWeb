#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iterator>
#include <mutex>
#include <optional>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <zip.h>

#include "App/BookDownloadFacade.hpp"
#include "App/InpxArchiveAccess.hpp"
#include "Database/SchemaInitializer.hpp"
#include "Database/SqliteConnection.hpp"
#include "Database/SqliteStatement.hpp"
#include "Database/SqliteTransaction.hpp"
#include "Domain/DomainError.hpp"
#include "Foundation/Sha256Fingerprint.hpp"
#include "Foundation/UnicodeConversion.hpp"
#include "TestWorkspace.hpp"

namespace {

constexpr std::uint64_t GManifestMemoryBudgetBytes = 1ull * 1024ull * 1024ull;

class CTestConverter final : public InpxWebReader::Domain::IBookConverter
{
public:
    [[nodiscard]] bool CanConvert(
        const InpxWebReader::Domain::EBookFormat sourceFormat,
        const InpxWebReader::Domain::EBookFormat destinationFormat) const override
    {
        return Available
            && sourceFormat == InpxWebReader::Domain::EBookFormat::Fb2
            && destinationFormat == InpxWebReader::Domain::EBookFormat::Epub;
    }

    [[nodiscard]] InpxWebReader::Domain::SConversionResult Convert(
        const InpxWebReader::Domain::SConversionRequest& request,
        InpxWebReader::Domain::IProgressSink&,
        std::stop_token) const override
    {
        ++Calls;
        LastRequest = request;
        if (OnConvert)
        {
            OnConvert();
        }
        if (Status == InpxWebReader::Domain::EConversionStatus::Succeeded)
        {
            std::filesystem::create_directories(request.DestinationPath.parent_path());
            std::ifstream input(request.SourcePath, std::ios::binary);
            std::ofstream output(request.DestinationPath, std::ios::binary);
            output << "epub:" << input.rdbuf();
            return {
                .Status = Status,
                .OutputPath = request.DestinationPath
            };
        }

        return {
            .Status = Status,
            .Warnings = {Status == InpxWebReader::Domain::EConversionStatus::Cancelled
                ? "Conversion cancelled."
                : "Injected converter failure."}
        };
    }

    bool Available = true;
    InpxWebReader::Domain::EConversionStatus Status = InpxWebReader::Domain::EConversionStatus::Succeeded;
    mutable int Calls = 0;
    mutable std::optional<InpxWebReader::Domain::SConversionRequest> LastRequest;
    std::function<void()> OnConvert;
};

class CConcurrentTestConverter final : public InpxWebReader::Domain::IBookConverter
{
public:
    [[nodiscard]] bool CanConvert(
        const InpxWebReader::Domain::EBookFormat sourceFormat,
        const InpxWebReader::Domain::EBookFormat destinationFormat) const override
    {
        return sourceFormat == InpxWebReader::Domain::EBookFormat::Fb2
            && destinationFormat == InpxWebReader::Domain::EBookFormat::Epub;
    }

    [[nodiscard]] InpxWebReader::Domain::SConversionResult Convert(
        const InpxWebReader::Domain::SConversionRequest& request,
        InpxWebReader::Domain::IProgressSink&,
        std::stop_token) const override
    {
        {
            const std::scoped_lock lock(m_mutex);
            m_requests.push_back(request);
        }

        std::filesystem::create_directories(request.DestinationPath.parent_path());
        std::ifstream input(request.SourcePath, std::ios::binary);
        std::ofstream output(request.DestinationPath, std::ios::binary);
        output << "epub:" << input.rdbuf();
        return {
            .Status = InpxWebReader::Domain::EConversionStatus::Succeeded,
            .OutputPath = request.DestinationPath
        };
    }

    [[nodiscard]] std::vector<InpxWebReader::Domain::SConversionRequest> GetRequests() const
    {
        const std::scoped_lock lock(m_mutex);
        return m_requests;
    }

private:
    mutable std::mutex m_mutex;
    mutable std::vector<InpxWebReader::Domain::SConversionRequest> m_requests;
};

struct SDownloadFixture
{
    std::filesystem::path CacheRoot;
    std::filesystem::path RuntimeRoot;
    std::filesystem::path SourceRoot;
    std::filesystem::path DatabasePath;
};

void WriteArchive(
    const std::filesystem::path& archivePath,
    const std::string_view content = "original-fb2-payload")
{
    std::filesystem::create_directories(archivePath.parent_path());
    int errorCode = ZIP_ER_OK;
    const auto archiveUtf8 = InpxWebReader::Unicode::PathToUtf8(archivePath);
    zip_t* archive = zip_open(archiveUtf8.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &errorCode);
    REQUIRE(archive != nullptr);

    zip_source_t* source = zip_source_buffer(archive, content.data(), content.size(), 0);
    REQUIRE(source != nullptr);
    REQUIRE(zip_file_add(archive, "book.fb2", source, ZIP_FL_OVERWRITE | ZIP_FL_ENC_UTF_8) >= 0);
    REQUIRE(zip_close(archive) == 0);
}

SDownloadFixture CreateFixture(CTestWorkspace& workspace, const std::string_view availability = "available")
{
    SDownloadFixture fixture{
        .CacheRoot = workspace.GetPath() / "cache",
        .RuntimeRoot = workspace.GetPath() / "runtime",
        .SourceRoot = workspace.GetPath() / "source"
    };
    fixture.DatabasePath = fixture.CacheRoot / "Database" / "inpx-web-reader.db";
    std::filesystem::create_directories(fixture.DatabasePath.parent_path());
    InpxWebReader::DatabaseRuntime::CSchemaInitializer::Initialize(fixture.DatabasePath);
    std::filesystem::create_directories(fixture.SourceRoot);
    {
        std::ofstream inpxFile(fixture.SourceRoot / "catalog.inpx", std::ios::binary);
        inpxFile << "stable-inpx-fixture";
    }
    WriteArchive(fixture.SourceRoot / "books.zip");

    const InpxWebReader::Application::SInpxSourceInfo sourceInfo{
        .InpxPath = fixture.SourceRoot / "catalog.inpx",
        .ArchiveRoot = fixture.SourceRoot
    };
    const auto archive = InpxWebReader::Application::ResolvePortableInpxArchivePath(
        sourceInfo,
        "books.zip");
    const auto archiveState = InpxWebReader::Application::ReadInpxArchiveFileState(archive.AbsolutePath);
    const auto archiveManifest = InpxWebReader::Application::CInpxArchiveReader(
        archive.AbsolutePath).ComputeManifestFingerprint(GManifestMemoryBudgetBytes);
    const auto sourceFingerprint = InpxWebReader::Foundation::CSha256Fingerprint::ComputeFile(
        sourceInfo.InpxPath);

    InpxWebReader::Sqlite::CSqliteConnection connection(fixture.DatabasePath);
    InpxWebReader::Sqlite::CSqliteStatement sourceRow(
        connection.GetNativeHandle(),
        "INSERT INTO inpx_sources(id, display_name, source_fingerprint) "
        "VALUES(1, 'catalog.inpx', ?);");
    sourceRow.BindText(1, sourceFingerprint);
    static_cast<void>(sourceRow.Step());
    InpxWebReader::Sqlite::CSqliteStatement segment(
        connection.GetNativeHandle(),
        "INSERT INTO inpx_segments(id, source_id, inp_entry_name, archive_name, inp_fingerprint, "
        "record_count, active_record_count, deleted_record_count, availability, requires_archive, "
        "resolved_archive_path, archive_file_size_bytes, archive_mtime_ticks, archive_manifest_fingerprint) "
        "VALUES(1, 1, 'books.zip.inp', 'books.zip', 'sha256:fixture', 1, 1, 0, 'available', 1, ?, ?, ?, ?);");
    segment.BindText(1, archive.RelativePathUtf8);
    segment.BindInt64(2, static_cast<std::int64_t>(archiveState.FileSizeBytes));
    segment.BindInt64(3, archiveState.MtimeTicks);
    segment.BindText(4, archiveManifest);
    static_cast<void>(segment.Step());
    connection.Execute(
        "INSERT INTO inpx_books(id, title, normalized_title, language, added_at_utc) "
        "VALUES(7, 'Test Book', 'test book', 'en', '2026-01-01T00:00:00Z');");
    connection.Execute(
        "INSERT INTO authors(id, normalized_name, display_name) "
        "VALUES(1, 'test author', 'Test Author');"
        "INSERT INTO book_authors(book_id, author_id, author_order) VALUES(7, 1, 0);");
    InpxWebReader::Sqlite::CSqliteStatement location(
        connection.GetNativeHandle(),
        "INSERT INTO inpx_book_locations(book_id, source_id, segment_id, lib_id, archive_name, entry_name, availability, file_size_bytes) "
        "VALUES(7, 1, 1, 'book-7', 'books.zip', 'book.fb2', ?, 20);");
    location.BindText(1, availability);
    static_cast<void>(location.Step());

    return fixture;
}

InpxWebReader::Application::SBookDownloadSource BuildDownloadSource(
    const SDownloadFixture& fixture,
    const std::optional<std::filesystem::path>& relocatedSourceRoot = std::nullopt)
{
    const auto& sourceRoot = relocatedSourceRoot.value_or(fixture.SourceRoot);
    return {
        .InpxPath = sourceRoot / "catalog.inpx",
        .ArchiveRoot = sourceRoot
    };
}

void PersistResolvedArchiveGuard(
    const SDownloadFixture& fixture,
    const std::string_view archivePathUtf8)
{
    const InpxWebReader::Application::SInpxSourceInfo sourceInfo{
        .InpxPath = fixture.SourceRoot / "catalog.inpx",
        .ArchiveRoot = fixture.SourceRoot
    };
    const auto archive = InpxWebReader::Application::ResolvePortableInpxArchivePath(
        sourceInfo,
        archivePathUtf8);
    const auto state = InpxWebReader::Application::ReadInpxArchiveFileState(archive.AbsolutePath);
    const auto manifest = InpxWebReader::Application::CInpxArchiveReader(
        archive.AbsolutePath).ComputeManifestFingerprint(GManifestMemoryBudgetBytes);
    InpxWebReader::Sqlite::CSqliteConnection connection(fixture.DatabasePath);
    InpxWebReader::Sqlite::CSqliteStatement update(
        connection.GetNativeHandle(),
        "UPDATE inpx_segments SET resolved_archive_path = ?, archive_file_size_bytes = ?, "
        "archive_mtime_ticks = ?, archive_manifest_fingerprint = ? WHERE id = 1;");
    update.BindText(1, archive.RelativePathUtf8);
    update.BindInt64(2, static_cast<std::int64_t>(state.FileSizeBytes));
    update.BindInt64(3, state.MtimeTicks);
    update.BindText(4, manifest);
    static_cast<void>(update.Step());
}

std::string ReadFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

bool ContainsRegularFile(const std::filesystem::path& root)
{
    if (!std::filesystem::exists(root))
    {
        return false;
    }
    return std::ranges::any_of(
        std::filesystem::recursive_directory_iterator(root),
        [](const auto& entry) { return entry.is_regular_file(); });
}

InpxWebReader::Application::SFileReplacementHooks BuildInpxMutationOnPublicationHooks(
    const SDownloadFixture& fixture,
    const std::filesystem::path& destinationPath,
    bool& mutationObserved)
{
    InpxWebReader::Application::SFileReplacementHooks hooks;
    hooks.Rename = [inpxPath = fixture.SourceRoot / "catalog.inpx", destinationPath, &mutationObserved](
                       const std::filesystem::path& source,
                       const std::filesystem::path& destination) {
        if (destination == destinationPath)
        {
            if (mutationObserved)
            {
                throw std::logic_error("The publication mutation hook ran more than once.");
            }
            std::ofstream output(inpxPath, std::ios::binary | std::ios::app);
            output << "changed-during-publication";
            output.close();
            if (!output)
            {
                throw std::runtime_error("Failed to mutate the INPX publication fixture.");
            }
            mutationObserved = true;
        }
        std::filesystem::rename(source, destination);
    };
    return hooks;
}

template <typename F>
void RequireDomainError(F&& action, const InpxWebReader::Domain::EDomainErrorCode expectedCode)
{
    try
    {
        std::invoke(std::forward<F>(action));
        FAIL("Expected a domain exception.");
    }
    catch (const InpxWebReader::Domain::CDomainException& ex)
    {
        REQUIRE(ex.Code() == expectedCode);
    }
}

} // namespace

TEST_CASE("Book download facade validates ids and destination boundaries", "[application][download][files]")
{
    CTestWorkspace workspace("inpx-web-reader-download-validation");
    auto fixture = CreateFixture(workspace);
    const InpxWebReader::Application::CBookDownloadFacade facade(
        fixture.CacheRoot,
        BuildDownloadSource(fixture),
        nullptr,
        fixture.RuntimeRoot,
        fixture.DatabasePath,
        GManifestMemoryBudgetBytes);

    REQUIRE_THROWS_AS(
        facade.PrepareDownload({.BookId = {0}, .DestinationPath = workspace.GetPath() / "invalid.fb2"}),
        std::invalid_argument);
    REQUIRE_THROWS_AS(
        facade.PrepareDownload({.BookId = {7}, .DestinationPath = "relative.fb2"}),
        std::invalid_argument);

    const auto directoryDestination = workspace.GetPath() / "directory";
    std::filesystem::create_directories(directoryDestination);
    REQUIRE_THROWS_AS(
        facade.PrepareDownload({.BookId = {7}, .DestinationPath = directoryDestination}),
        std::invalid_argument);
    REQUIRE_THROWS_AS(
        facade.PrepareDownload({.BookId = {7}, .DestinationPath = fixture.CacheRoot / "inside.fb2"}),
        std::invalid_argument);
}

TEST_CASE("Book download facade enforces its archive manifest memory budget", "[application][download][inpx][limits]")
{
    CTestWorkspace workspace("inpx-web-reader-download-manifest-budget");
    auto fixture = CreateFixture(workspace);
    const auto destination = workspace.GetPath() / "downloads" / "book.fb2";
    const InpxWebReader::Application::CBookDownloadFacade facade(
        fixture.CacheRoot,
        BuildDownloadSource(fixture),
        nullptr,
        fixture.RuntimeRoot,
        fixture.DatabasePath,
        1);

    RequireDomainError(
        [&] {
            static_cast<void>(facade.PrepareDownload({
                .BookId = {7},
                .DestinationPath = destination
            }));
        },
        InpxWebReader::Domain::EDomainErrorCode::IntegrityIssue);
    REQUIRE_FALSE(std::filesystem::exists(destination));
}

TEST_CASE("Book download facade cancels during archive manifest validation", "[application][download][inpx][cancellation]")
{
    CTestWorkspace workspace("inpx-web-reader-download-manifest-cancellation");
    auto fixture = CreateFixture(workspace);
    const auto destination = workspace.GetPath() / "downloads" / "book.fb2";
    std::stop_source stopSource;
    bool manifestCheckpointObserved = false;
    const InpxWebReader::Application::CBookDownloadFacade facade(
        fixture.CacheRoot,
        BuildDownloadSource(fixture),
        nullptr,
        fixture.RuntimeRoot,
        fixture.DatabasePath,
        GManifestMemoryBudgetBytes,
        {},
        {
            .BeforeArchiveManifestCheckpoint = [&]() {
                manifestCheckpointObserved = true;
                stopSource.request_stop();
            }
        });

    RequireDomainError(
        [&] {
            static_cast<void>(facade.PrepareDownload({
                .BookId = {7},
                .DestinationPath = destination,
                .StopToken = stopSource.get_token()
            }));
        },
        InpxWebReader::Domain::EDomainErrorCode::Cancellation);
    REQUIRE(manifestCheckpointObserved);
    REQUIRE_FALSE(std::filesystem::exists(destination));
}

TEST_CASE("Book download facade cancels during INPX source fingerprint validation", "[application][download][inpx][cancellation]")
{
    CTestWorkspace workspace("inpx-web-reader-download-source-fingerprint-cancellation");
    auto fixture = CreateFixture(workspace);
    const auto destination = workspace.GetPath() / "downloads" / "book.fb2";
    std::stop_source stopSource;
    bool sourceFingerprintCheckpointObserved = false;
    const InpxWebReader::Application::CBookDownloadFacade facade(
        fixture.CacheRoot,
        BuildDownloadSource(fixture),
        nullptr,
        fixture.RuntimeRoot,
        fixture.DatabasePath,
        GManifestMemoryBudgetBytes,
        {},
        {
            .BeforeSourceFingerprintCheckpoint = [&]() {
                sourceFingerprintCheckpointObserved = true;
                stopSource.request_stop();
            }
        });

    RequireDomainError(
        [&] {
            static_cast<void>(facade.PrepareDownload({
                .BookId = {7},
                .DestinationPath = destination,
                .StopToken = stopSource.get_token()
            }));
        },
        InpxWebReader::Domain::EDomainErrorCode::Cancellation);
    REQUIRE(sourceFingerprintCheckpointObserved);
    REQUIRE_FALSE(std::filesystem::exists(destination));
}

TEST_CASE("Book download keeps its validated archive handle across an ABA path replacement", "[application][download][inpx][files]")
{
    CTestWorkspace workspace("inpx-web-reader-download-archive-aba");
    auto fixture = CreateFixture(workspace);
    constexpr std::string_view replacementPayload = "replaced-fb2-payload";
    static_assert(replacementPayload.size() == std::string_view{"original-fb2-payload"}.size());
    const auto archivePath = fixture.SourceRoot / "books.zip";
    const auto originalArchiveBackupPath = fixture.SourceRoot / "books-original.zip";
    const auto replacementArchivePath = fixture.SourceRoot / "books-replacement.zip";
    WriteArchive(replacementArchivePath, replacementPayload);

    bool archiveReplaced = false;
    bool archiveRestored = false;
    const auto destination = workspace.GetPath() / "downloads" / "book.fb2";
    const InpxWebReader::Application::CBookDownloadFacade facade(
        fixture.CacheRoot,
        BuildDownloadSource(fixture),
        nullptr,
        fixture.RuntimeRoot,
        fixture.DatabasePath,
        GManifestMemoryBudgetBytes,
        {},
        {
            .AfterArchiveSnapshotValidated = [&]() {
                if (archiveReplaced)
                {
                    throw std::logic_error("The archive ABA replacement hook ran more than once.");
                }
                std::filesystem::rename(archivePath, originalArchiveBackupPath);
                std::filesystem::rename(replacementArchivePath, archivePath);
                archiveReplaced = true;
            },
            .BeforeArchiveExtractionCheckpoint = [&]() {
                if (!archiveRestored)
                {
                    if (!archiveReplaced)
                    {
                        throw std::logic_error("The archive ABA restoration ran before replacement.");
                    }
                    std::filesystem::rename(archivePath, replacementArchivePath);
                    std::filesystem::rename(originalArchiveBackupPath, archivePath);
                    archiveRestored = true;
                }
            }
        });

    const auto result = facade.PrepareDownload({.BookId = {7}, .DestinationPath = destination});

    REQUIRE(archiveReplaced);
    REQUIRE(archiveRestored);
    REQUIRE(result.has_value());
    REQUIRE(ReadFile(destination) == "original-fb2-payload");
    REQUIRE(std::filesystem::exists(replacementArchivePath));
    REQUIRE_FALSE(std::filesystem::exists(originalArchiveBackupPath));
}

TEST_CASE("Book download facade returns original bytes and replaces an existing destination", "[application][download][files][inpx]")
{
    CTestWorkspace workspace("inpx-web-reader-download-original");
    auto fixture = CreateFixture(workspace);
    const auto destination = workspace.GetPath() / "downloads" / "book.fb2";
    const auto relocatedSourceRoot = workspace.GetPath() / "relocated-source";
    std::filesystem::rename(fixture.SourceRoot, relocatedSourceRoot);
    std::filesystem::create_directories(destination.parent_path());
    {
        std::ofstream existing(destination, std::ios::binary);
        existing << "stale";
    }
    const InpxWebReader::Application::CBookDownloadFacade facade(
        fixture.CacheRoot,
        BuildDownloadSource(fixture, relocatedSourceRoot),
        nullptr,
        fixture.RuntimeRoot,
        fixture.DatabasePath,
        GManifestMemoryBudgetBytes);

    const auto result = facade.PrepareDownload({.BookId = {7}, .DestinationPath = destination});

    REQUIRE(result.has_value());
    REQUIRE(result->Path == destination);
    REQUIRE(result->TitleUtf8 == "Test Book");
    REQUIRE(result->AuthorsUtf8 == std::vector<std::string>{"Test Author"});
    REQUIRE(ReadFile(destination) == "original-fb2-payload");
    REQUIRE_FALSE(ContainsRegularFile(fixture.RuntimeRoot / "Downloads"));
}

TEST_CASE("Book download facade isolates concurrent staging for the same book", "[application][download][converter][concurrency]")
{
    CTestWorkspace workspace("inpx-web-reader-download-concurrent-same-book");
    auto fixture = CreateFixture(workspace);
    InpxWebReader::Sqlite::CSqliteConnection warmConnection(fixture.DatabasePath);
    REQUIRE(warmConnection.GetNativeHandle() != nullptr);
    CConcurrentTestConverter converter;
    const auto destinationDirectory = workspace.GetPath() / "downloads";
    const auto firstDestination = destinationDirectory / "first.epub";
    const auto secondDestination = destinationDirectory / "second.epub";
    const InpxWebReader::Application::CBookDownloadFacade facade(
        fixture.CacheRoot,
        BuildDownloadSource(fixture),
        &converter,
        fixture.RuntimeRoot,
        fixture.DatabasePath,
        GManifestMemoryBudgetBytes);

    std::promise<void> startPromise;
    const auto start = startPromise.get_future().share();
    auto first = std::async(std::launch::async, [&facade, start, &firstDestination]() {
        start.wait();
        return facade.PrepareDownload({
            .BookId = {7},
            .DestinationPath = firstDestination,
            .RequestedFormat = InpxWebReader::Domain::EBookFormat::Epub
        });
    });
    auto second = std::async(std::launch::async, [&facade, start, &secondDestination]() {
        start.wait();
        return facade.PrepareDownload({
            .BookId = {7},
            .DestinationPath = secondDestination,
            .RequestedFormat = InpxWebReader::Domain::EBookFormat::Epub
        });
    });
    startPromise.set_value();

    const auto firstResult = first.get();
    const auto secondResult = second.get();
    const auto requests = converter.GetRequests();

    REQUIRE(firstResult.has_value());
    REQUIRE(secondResult.has_value());
    REQUIRE(firstResult->Path == firstDestination);
    REQUIRE(secondResult->Path == secondDestination);
    REQUIRE(requests.size() == 2);
    REQUIRE(requests[0].SourcePath != requests[1].SourcePath);
    REQUIRE(requests[0].SourcePath.parent_path() != requests[1].SourcePath.parent_path());
    REQUIRE(requests[0].DestinationPath.parent_path() != destinationDirectory);
    REQUIRE(requests[1].DestinationPath.parent_path() != destinationDirectory);
    REQUIRE(requests[0].DestinationPath.parent_path() != requests[1].DestinationPath.parent_path());
    REQUIRE(ReadFile(firstDestination) == "epub:original-fb2-payload");
    REQUIRE(ReadFile(secondDestination) == "epub:original-fb2-payload");
    REQUIRE_FALSE(ContainsRegularFile(fixture.RuntimeRoot / "Downloads"));
}

TEST_CASE("Book download facade uses the exact portable archive selected by the scan", "[application][download][inpx]")
{
    CTestWorkspace workspace("inpx-web-reader-download-selected-archive");
    auto fixture = CreateFixture(workspace);
    const auto selectedArchive = fixture.SourceRoot / "z-selected" / "books.zip";
    std::filesystem::create_directories(selectedArchive.parent_path());
    std::filesystem::rename(fixture.SourceRoot / "books.zip", selectedArchive);
    PersistResolvedArchiveGuard(fixture, "z-selected/books.zip");

    WriteArchive(fixture.SourceRoot / "a-earlier" / "books.zip", "malicious-fb2-bytes");
    const auto destination = workspace.GetPath() / "downloads" / "book.fb2";
    const InpxWebReader::Application::CBookDownloadFacade facade(
        fixture.CacheRoot,
        BuildDownloadSource(fixture),
        nullptr,
        fixture.RuntimeRoot,
        fixture.DatabasePath,
        GManifestMemoryBudgetBytes);

    const auto result = facade.PrepareDownload({.BookId = {7}, .DestinationPath = destination});

    REQUIRE(result.has_value());
    REQUIRE(result->Path == destination);
    REQUIRE(ReadFile(destination) == "original-fb2-payload");
}

TEST_CASE("Book download snapshot keeps filename metadata and locator coherent across a catalog commit", "[application][download][database][concurrency]")
{
    CTestWorkspace workspace("inpx-web-reader-download-coherent-snapshot");
    auto fixture = CreateFixture(workspace);
    constexpr std::string_view replacementPayload = "replacement-fb2-data";
    WriteArchive(fixture.SourceRoot / "replacement.zip", replacementPayload);

    const InpxWebReader::Application::SInpxSourceInfo sourceInfo{
        .InpxPath = fixture.SourceRoot / "catalog.inpx",
        .ArchiveRoot = fixture.SourceRoot
    };
    const auto replacementArchive = InpxWebReader::Application::ResolvePortableInpxArchivePath(
        sourceInfo,
        "replacement.zip");
    const auto replacementState = InpxWebReader::Application::ReadInpxArchiveFileState(
        replacementArchive.AbsolutePath);
    const auto replacementManifest = InpxWebReader::Application::CInpxArchiveReader(
        replacementArchive.AbsolutePath).ComputeManifestFingerprint(GManifestMemoryBudgetBytes);
    bool catalogCommitCompleted = false;
    const auto destination = workspace.GetPath() / "downloads" / "book.fb2";
    const InpxWebReader::Application::CBookDownloadFacade facade(
        fixture.CacheRoot,
        BuildDownloadSource(fixture),
        nullptr,
        fixture.RuntimeRoot,
        fixture.DatabasePath,
        GManifestMemoryBudgetBytes,
        {},
        {
            .AfterSnapshotLoaded = [&]() {
                InpxWebReader::Sqlite::CSqliteConnection connection(fixture.DatabasePath);
                InpxWebReader::Sqlite::CSqliteTransaction transaction(connection);
                connection.Execute(
                    "UPDATE inpx_books SET title = 'Replacement Book', normalized_title = 'replacement book' "
                    "WHERE id = 7;"
                    "UPDATE authors SET display_name = 'Replacement Author', "
                    "normalized_name = 'replacement author' WHERE id = 1;");
                InpxWebReader::Sqlite::CSqliteStatement segment(
                    connection.GetNativeHandle(),
                    "UPDATE inpx_segments SET resolved_archive_path = ?, archive_file_size_bytes = ?, "
                    "archive_mtime_ticks = ?, archive_manifest_fingerprint = ? WHERE id = 1;");
                segment.BindText(1, replacementArchive.RelativePathUtf8);
                segment.BindInt64(2, static_cast<std::int64_t>(replacementState.FileSizeBytes));
                segment.BindInt64(3, replacementState.MtimeTicks);
                segment.BindText(4, replacementManifest);
                static_cast<void>(segment.Step());
                InpxWebReader::Sqlite::CSqliteStatement location(
                    connection.GetNativeHandle(),
                    "UPDATE inpx_book_locations SET archive_name = 'replacement.zip', "
                    "file_size_bytes = ? WHERE book_id = 7;");
                location.BindInt64(1, static_cast<std::int64_t>(replacementPayload.size()));
                static_cast<void>(location.Step());
                transaction.Commit();
                catalogCommitCompleted = true;
            }
        });

    const auto result = facade.PrepareDownload({.BookId = {7}, .DestinationPath = destination});

    REQUIRE(catalogCommitCompleted);
    REQUIRE(result.has_value());
    REQUIRE(result->Path == destination);
    REQUIRE(result->TitleUtf8 == "Test Book");
    REQUIRE(result->AuthorsUtf8 == std::vector<std::string>{"Test Author"});
    REQUIRE(ReadFile(destination) == "original-fb2-payload");
}

TEST_CASE("Book download facade rejects source mutation before publishing staged bytes", "[application][download][inpx]")
{
    CTestWorkspace workspace("inpx-web-reader-download-source-mutation");

    SECTION("archive replacement")
    {
        auto fixture = CreateFixture(workspace);
        WriteArchive(fixture.SourceRoot / "books.zip", "replacement-fb2-data");
        const auto destination = workspace.GetPath() / "downloads" / "replaced.fb2";
        const InpxWebReader::Application::CBookDownloadFacade facade(
            fixture.CacheRoot,
            BuildDownloadSource(fixture),
            nullptr,
            fixture.RuntimeRoot,
            fixture.DatabasePath,
            GManifestMemoryBudgetBytes);

        RequireDomainError(
            [&] { static_cast<void>(facade.PrepareDownload({.BookId = {7}, .DestinationPath = destination})); },
            InpxWebReader::Domain::EDomainErrorCode::IntegrityIssue);
        REQUIRE_FALSE(std::filesystem::exists(destination));
        REQUIRE_FALSE(ContainsRegularFile(fixture.RuntimeRoot / "Downloads"));
    }

    SECTION("concurrent INPX mutation during conversion")
    {
        auto fixture = CreateFixture(workspace);
        CTestConverter converter;
        converter.OnConvert = [&fixture]() {
            std::ofstream output(fixture.SourceRoot / "catalog.inpx", std::ios::binary | std::ios::app);
            output << "changed";
        };
        const auto destination = workspace.GetPath() / "downloads" / "changed.epub";
        const InpxWebReader::Application::CBookDownloadFacade facade(
            fixture.CacheRoot,
            BuildDownloadSource(fixture),
            &converter,
            fixture.RuntimeRoot,
            fixture.DatabasePath,
            GManifestMemoryBudgetBytes);

        RequireDomainError(
            [&] { static_cast<void>(facade.PrepareDownload({
                .BookId = {7},
                .DestinationPath = destination,
                .RequestedFormat = InpxWebReader::Domain::EBookFormat::Epub
            })); },
            InpxWebReader::Domain::EDomainErrorCode::IntegrityIssue);
        REQUIRE_FALSE(std::filesystem::exists(destination));
        REQUIRE_FALSE(ContainsRegularFile(fixture.RuntimeRoot / "Downloads"));
    }
}

TEST_CASE("Book download facade restores an existing destination when source changes during publication", "[application][download][inpx][files]")
{
    CTestWorkspace workspace("inpx-web-reader-download-publication-mutation");

    SECTION("original download")
    {
        auto fixture = CreateFixture(workspace);
        const auto destination = workspace.GetPath() / "downloads" / "existing.fb2";
        std::filesystem::create_directories(destination.parent_path());
        {
            std::ofstream existing(destination, std::ios::binary);
            existing << "existing-original";
        }
        bool mutationObserved = false;
        const InpxWebReader::Application::CBookDownloadFacade facade(
            fixture.CacheRoot,
            BuildDownloadSource(fixture),
            nullptr,
            fixture.RuntimeRoot,
            fixture.DatabasePath,
            GManifestMemoryBudgetBytes,
            BuildInpxMutationOnPublicationHooks(fixture, destination, mutationObserved));

        RequireDomainError(
            [&] {
                static_cast<void>(facade.PrepareDownload({
                    .BookId = {7},
                    .DestinationPath = destination
                }));
            },
            InpxWebReader::Domain::EDomainErrorCode::IntegrityIssue);

        REQUIRE(mutationObserved);
        REQUIRE(ReadFile(destination) == "existing-original");
        REQUIRE_FALSE(ContainsRegularFile(fixture.RuntimeRoot / "Downloads"));
        REQUIRE(std::ranges::all_of(
            std::filesystem::directory_iterator(destination.parent_path()),
            [&](const auto& entry) { return entry.path() == destination; }));
    }

    SECTION("converted download")
    {
        auto fixture = CreateFixture(workspace);
        CTestConverter converter;
        const auto destination = workspace.GetPath() / "downloads" / "existing.epub";
        std::filesystem::create_directories(destination.parent_path());
        {
            std::ofstream existing(destination, std::ios::binary);
            existing << "existing-converted";
        }
        bool mutationObserved = false;
        const InpxWebReader::Application::CBookDownloadFacade facade(
            fixture.CacheRoot,
            BuildDownloadSource(fixture),
            &converter,
            fixture.RuntimeRoot,
            fixture.DatabasePath,
            GManifestMemoryBudgetBytes,
            BuildInpxMutationOnPublicationHooks(fixture, destination, mutationObserved));

        RequireDomainError(
            [&] {
                static_cast<void>(facade.PrepareDownload({
                    .BookId = {7},
                    .DestinationPath = destination,
                    .RequestedFormat = InpxWebReader::Domain::EBookFormat::Epub
                }));
            },
            InpxWebReader::Domain::EDomainErrorCode::IntegrityIssue);

        REQUIRE(mutationObserved);
        REQUIRE(ReadFile(destination) == "existing-converted");
        REQUIRE_FALSE(ContainsRegularFile(fixture.RuntimeRoot / "Downloads"));
        REQUIRE(std::ranges::all_of(
            std::filesystem::directory_iterator(destination.parent_path()),
            [&](const auto& entry) { return entry.path() == destination; }));
    }
}

TEST_CASE("Book download facade reports missing unavailable and damaged locators", "[application][download][inpx]")
{
    CTestWorkspace workspace("inpx-web-reader-download-locators");

    SECTION("missing book")
    {
        auto fixture = CreateFixture(workspace);
        {
            InpxWebReader::Sqlite::CSqliteConnection connection(fixture.DatabasePath);
            connection.Execute("DELETE FROM inpx_books WHERE id = 7;");
        }
        const InpxWebReader::Application::CBookDownloadFacade facade(
            fixture.CacheRoot,
            BuildDownloadSource(fixture),
            nullptr,
            fixture.RuntimeRoot,
            fixture.DatabasePath,
            GManifestMemoryBudgetBytes);
        REQUIRE_FALSE(facade.PrepareDownload({
            .BookId = {7},
            .DestinationPath = workspace.GetPath() / "missing.fb2"
        }).has_value());
    }

    SECTION("unavailable source entry")
    {
        auto fixture = CreateFixture(workspace, "missing_from_index");
        const InpxWebReader::Application::CBookDownloadFacade facade(
            fixture.CacheRoot,
            BuildDownloadSource(fixture),
            nullptr,
            fixture.RuntimeRoot,
            fixture.DatabasePath,
            GManifestMemoryBudgetBytes);
        RequireDomainError(
            [&] { static_cast<void>(facade.PrepareDownload({
                .BookId = {7},
                .DestinationPath = workspace.GetPath() / "unavailable.fb2"
            })); },
            InpxWebReader::Domain::EDomainErrorCode::NotFound);
    }

    SECTION("missing source locator")
    {
        auto fixture = CreateFixture(workspace);
        {
            InpxWebReader::Sqlite::CSqliteConnection connection(fixture.DatabasePath);
            connection.Execute("DELETE FROM inpx_book_locations WHERE book_id = 7;");
        }
        const InpxWebReader::Application::CBookDownloadFacade facade(
            fixture.CacheRoot,
            BuildDownloadSource(fixture),
            nullptr,
            fixture.RuntimeRoot,
            fixture.DatabasePath,
            GManifestMemoryBudgetBytes);
        RequireDomainError(
            [&] { static_cast<void>(facade.PrepareDownload({
                .BookId = {7},
                .DestinationPath = workspace.GetPath() / "no-locator.fb2"
            })); },
            InpxWebReader::Domain::EDomainErrorCode::IntegrityIssue);
    }

    SECTION("invalid stored format")
    {
        auto fixture = CreateFixture(workspace);
        {
            InpxWebReader::Sqlite::CSqliteConnection connection(fixture.DatabasePath);
            connection.Execute("UPDATE inpx_book_locations SET format = 'mobi' WHERE book_id = 7;");
        }
        const InpxWebReader::Application::CBookDownloadFacade facade(
            fixture.CacheRoot,
            BuildDownloadSource(fixture),
            nullptr,
            fixture.RuntimeRoot,
            fixture.DatabasePath,
            GManifestMemoryBudgetBytes);
        RequireDomainError(
            [&] { static_cast<void>(facade.PrepareDownload({
                .BookId = {7},
                .DestinationPath = workspace.GetPath() / "invalid-format.fb2"
            })); },
            InpxWebReader::Domain::EDomainErrorCode::IntegrityIssue);
    }
}

TEST_CASE("Book download facade preserves converter success failure and cancellation semantics", "[application][download][converter]")
{
    CTestWorkspace workspace("inpx-web-reader-download-converter");

    SECTION("converter unavailable")
    {
        auto fixture = CreateFixture(workspace);
        const InpxWebReader::Application::CBookDownloadFacade facade(
            fixture.CacheRoot,
            BuildDownloadSource(fixture),
            nullptr,
            fixture.RuntimeRoot,
            fixture.DatabasePath,
            GManifestMemoryBudgetBytes);
        RequireDomainError(
            [&] { static_cast<void>(facade.PrepareDownload({
                .BookId = {7},
                .DestinationPath = workspace.GetPath() / "unavailable.epub",
                .RequestedFormat = InpxWebReader::Domain::EBookFormat::Epub
            })); },
            InpxWebReader::Domain::EDomainErrorCode::ConverterUnavailable);
        REQUIRE_FALSE(ContainsRegularFile(fixture.RuntimeRoot / "Downloads"));
    }

    SECTION("conversion failure")
    {
        auto fixture = CreateFixture(workspace);
        CTestConverter converter;
        converter.Status = InpxWebReader::Domain::EConversionStatus::Failed;
        const auto destination = workspace.GetPath() / "failed.epub";
        const InpxWebReader::Application::CBookDownloadFacade facade(
            fixture.CacheRoot,
            BuildDownloadSource(fixture),
            &converter,
            fixture.RuntimeRoot,
            fixture.DatabasePath,
            GManifestMemoryBudgetBytes);
        RequireDomainError(
            [&] { static_cast<void>(facade.PrepareDownload({
                .BookId = {7},
                .DestinationPath = destination,
                .RequestedFormat = InpxWebReader::Domain::EBookFormat::Epub
            })); },
            InpxWebReader::Domain::EDomainErrorCode::ConverterFailed);
        REQUIRE_FALSE(std::filesystem::exists(destination));
        REQUIRE_FALSE(ContainsRegularFile(fixture.RuntimeRoot / "Downloads"));
    }

    SECTION("conversion cancellation")
    {
        auto fixture = CreateFixture(workspace);
        CTestConverter converter;
        converter.Status = InpxWebReader::Domain::EConversionStatus::Cancelled;
        const InpxWebReader::Application::CBookDownloadFacade facade(
            fixture.CacheRoot,
            BuildDownloadSource(fixture),
            &converter,
            fixture.RuntimeRoot,
            fixture.DatabasePath,
            GManifestMemoryBudgetBytes);
        RequireDomainError(
            [&] { static_cast<void>(facade.PrepareDownload({
                .BookId = {7},
                .DestinationPath = workspace.GetPath() / "cancelled.epub",
                .RequestedFormat = InpxWebReader::Domain::EBookFormat::Epub
            })); },
            InpxWebReader::Domain::EDomainErrorCode::Cancellation);
        REQUIRE_FALSE(ContainsRegularFile(fixture.RuntimeRoot / "Downloads"));
    }

    SECTION("conversion timeout")
    {
        auto fixture = CreateFixture(workspace);
        CTestConverter converter;
        converter.Status = InpxWebReader::Domain::EConversionStatus::TimedOut;
        const InpxWebReader::Application::CBookDownloadFacade facade(
            fixture.CacheRoot,
            BuildDownloadSource(fixture),
            &converter,
            fixture.RuntimeRoot,
            fixture.DatabasePath,
            GManifestMemoryBudgetBytes);
        RequireDomainError(
            [&] { static_cast<void>(facade.PrepareDownload({
                .BookId = {7},
                .DestinationPath = workspace.GetPath() / "timed-out.epub",
                .RequestedFormat = InpxWebReader::Domain::EBookFormat::Epub
            })); },
            InpxWebReader::Domain::EDomainErrorCode::ConverterTimeout);
        REQUIRE_FALSE(ContainsRegularFile(fixture.RuntimeRoot / "Downloads"));
    }

    SECTION("conversion success")
    {
        auto fixture = CreateFixture(workspace);
        CTestConverter converter;
        const auto destination = workspace.GetPath() / "converted.epub";
        const InpxWebReader::Application::CBookDownloadFacade facade(
            fixture.CacheRoot,
            BuildDownloadSource(fixture),
            &converter,
            fixture.RuntimeRoot,
            fixture.DatabasePath,
            GManifestMemoryBudgetBytes);
        const auto result = facade.PrepareDownload({
            .BookId = {7},
            .DestinationPath = destination,
            .RequestedFormat = InpxWebReader::Domain::EBookFormat::Epub
        });

        REQUIRE(result.has_value());
        REQUIRE(result->Path == destination);
        REQUIRE(result->Format == InpxWebReader::Domain::EBookFormat::Epub);
        REQUIRE(ReadFile(destination) == "epub:original-fb2-payload");
        REQUIRE(converter.Calls == 1);
        REQUIRE(converter.LastRequest.has_value());
        REQUIRE_FALSE(ContainsRegularFile(fixture.RuntimeRoot / "Downloads"));
    }
}
