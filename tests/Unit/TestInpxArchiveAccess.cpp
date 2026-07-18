#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <zip.h>

#include "App/InpxArchiveAccess.hpp"
#include "App/IInpxCatalogApplication.hpp"
#include "Foundation/BookPayloadLimits.hpp"
#include "Foundation/UnicodeConversion.hpp"
#include "Storage/InpxArchivePathResolver.hpp"
#include "TestFilesystemHelpers.hpp"
#include "TestWorkspace.hpp"

namespace {

constexpr std::size_t GLargeButAllowedEntryBytes = 32ull * 1024ull * 1024ull + 1ull;
constexpr std::uint64_t GManifestMemoryBudgetBytes = 1ull * 1024ull * 1024ull;

void WritePlaceholderFile(const std::filesystem::path& path)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << "placeholder";
}

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

void WriteZipArchive(const std::filesystem::path& archivePath)
{
    std::filesystem::create_directories(archivePath.parent_path());

    int errorCode = ZIP_ER_OK;
    const auto archivePathUtf8 = InpxWebReader::Unicode::PathToUtf8(archivePath);
    zip_t* archive = zip_open(archivePathUtf8.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &errorCode);
    REQUIRE(archive != nullptr);

    const std::string firstContent = "first";
    const std::string secondContent = "second";
    AddZipEntry(archive, "first.fb2", firstContent);
    AddZipEntry(archive, "second.fb2", secondContent);

    REQUIRE(zip_close(archive) == 0);
}

void WriteZipArchive(
    const std::filesystem::path& archivePath,
    const std::vector<std::pair<std::string, std::string>>& entries)
{
    std::filesystem::create_directories(archivePath.parent_path());

    int errorCode = ZIP_ER_OK;
    const auto archivePathUtf8 = InpxWebReader::Unicode::PathToUtf8(archivePath);
    zip_t* archive = zip_open(archivePathUtf8.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &errorCode);
    REQUIRE(archive != nullptr);
    for (const auto& [entryName, content] : entries)
    {
        AddZipEntry(archive, entryName, content);
    }
    REQUIRE(zip_close(archive) == 0);
}

void WriteStoredZipArchive(
    const std::filesystem::path& archivePath,
    const std::string& entryName,
    const std::string& content)
{
    std::filesystem::create_directories(archivePath.parent_path());

    int errorCode = ZIP_ER_OK;
    const auto archivePathUtf8 = InpxWebReader::Unicode::PathToUtf8(archivePath);
    zip_t* archive = zip_open(archivePathUtf8.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &errorCode);
    REQUIRE(archive != nullptr);
    AddZipEntry(archive, entryName, content);
    REQUIRE(zip_set_file_compression(archive, 0, ZIP_CM_STORE, 0) == 0);
    REQUIRE(zip_close(archive) == 0);
}

void CorruptStoredPayload(
    const std::filesystem::path& archivePath,
    const std::string_view originalPayload)
{
    std::ifstream input(archivePath, std::ios::binary);
    REQUIRE(input);
    std::string archiveBytes(
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{});
    const auto payloadOffset = archiveBytes.find(originalPayload);
    REQUIRE(payloadOffset != std::string::npos);
    REQUIRE_FALSE(originalPayload.empty());
    archiveBytes[payloadOffset] ^= 0x01;

    std::ofstream output(archivePath, std::ios::binary | std::ios::trunc);
    REQUIRE(output);
    output.write(archiveBytes.data(), static_cast<std::streamsize>(archiveBytes.size()));
    REQUIRE(output);
}

void WriteLargeZipArchive(const std::filesystem::path& archivePath)
{
    std::filesystem::create_directories(archivePath.parent_path());

    int errorCode = ZIP_ER_OK;
    const auto archivePathUtf8 = InpxWebReader::Unicode::PathToUtf8(archivePath);
    zip_t* archive = zip_open(archivePathUtf8.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &errorCode);
    REQUIRE(archive != nullptr);

    const std::string content(GLargeButAllowedEntryBytes, 'x');
    AddZipEntry(archive, "large.fb2", content);

    REQUIRE(zip_close(archive) == 0);
}

std::filesystem::path Canonical(const std::filesystem::path& path)
{
    std::error_code errorCode;
    const auto canonicalPath = std::filesystem::weakly_canonical(path, errorCode);
    REQUIRE_FALSE(errorCode);
    return canonicalPath.lexically_normal();
}

} // namespace

TEST_CASE("INPX archive path resolver finds matching child archive directory", "[application][inpx]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-archive-access-child");
    const auto sourceRoot = sandbox.GetPath() / "source";
    const auto archivePath = sourceRoot / "payloads" / "fb2-000024-030559.zip";
    WritePlaceholderFile(archivePath);

    const InpxWebReader::Application::SInpxSourceInfo source{.InpxPath = sourceRoot / "catalog.inpx",
                                                              .ArchiveRoot = sourceRoot};

    REQUIRE(InpxWebReader::Application::ResolveInpxArchivePath(source, "fb2-000024-030559") == Canonical(archivePath));
}

TEST_CASE("INPX archive path resolver keeps direct archive root precedence", "[application][inpx]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-archive-access-direct");
    const auto sourceRoot = sandbox.GetPath() / "source";
    const auto directArchivePath = sourceRoot / "fb2-main.zip";
    WritePlaceholderFile(directArchivePath);
    WritePlaceholderFile(sourceRoot / "payloads" / "fb2-main.zip");

    const InpxWebReader::Application::SInpxSourceInfo source{.InpxPath = sourceRoot / "catalog.inpx",
                                                              .ArchiveRoot = sourceRoot};

    REQUIRE(InpxWebReader::Application::ResolveInpxArchivePath(source, "fb2-main.zip")
            == Canonical(directArchivePath));
}

TEST_CASE("INPX archive root index builds one fallback snapshot for repeated lookups", "[application][inpx]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-archive-access-index");
    const auto sourceRoot = sandbox.GetPath() / "source";
    const auto directArchivePath = sourceRoot / "direct.zip";
    WritePlaceholderFile(directArchivePath);

    constexpr std::size_t archiveCount = 24;
    std::vector<std::filesystem::path> archivePaths;
    archivePaths.reserve(archiveCount);
    for (std::size_t index = 0; index < archiveCount; ++index)
    {
        const auto archivePath = sourceRoot / ("payload-" + std::to_string(index)) / "nested"
                                 / ("archive-" + std::to_string(index) + ".zip");
        WritePlaceholderFile(archivePath);
        archivePaths.push_back(archivePath);
    }
    WritePlaceholderFile(sourceRoot / "z-selected" / "shared.zip");
    WritePlaceholderFile(sourceRoot / "a-earlier" / "shared.zip");

    std::size_t fallbackBuildCount = 0;
    std::size_t filesystemEntryVisitCount = 0;
    {
        InpxWebReader::StoragePlanning::CInpxArchiveRootIndex archiveRootIndex(
            sourceRoot,
            "INPX archive root could not be canonicalized.",
            {.OnFallbackSnapshotBuild = [&]() { ++fallbackBuildCount; },
             .OnFilesystemEntryVisited = [&]() { ++filesystemEntryVisitCount; }});

        REQUIRE(archiveRootIndex.TryResolveExistingZipArchivePath("direct", "INPX archive path is unsafe.")
                == Canonical(directArchivePath));
        REQUIRE(fallbackBuildCount == 0);
        REQUIRE(filesystemEntryVisitCount == 0);

        for (std::size_t index = 0; index < archiveCount; ++index)
        {
            REQUIRE(archiveRootIndex.TryResolveExistingZipArchivePath("nested/archive-" + std::to_string(index),
                                                                      "INPX archive path is unsafe.")
                    == Canonical(archivePaths[index]));
        }
        REQUIRE(archiveRootIndex.TryResolveExistingZipArchivePath("shared", "INPX archive path is unsafe.")
                == Canonical(sourceRoot / "a-earlier" / "shared.zip"));
        REQUIRE(fallbackBuildCount == 1);
        const std::size_t visitsAfterBuild = filesystemEntryVisitCount;
        REQUIRE(visitsAfterBuild > archiveCount);

        for (std::size_t index = 0; index < archiveCount; ++index)
        {
            REQUIRE(archiveRootIndex.TryResolveExistingZipArchivePath("nested/archive-" + std::to_string(index),
                                                                      "INPX archive path is unsafe.")
                    == Canonical(archivePaths[index]));
        }
        REQUIRE(fallbackBuildCount == 1);
        REQUIRE(filesystemEntryVisitCount == visitsAfterBuild);
    }

    InpxWebReader::StoragePlanning::CInpxArchiveRootIndex limitedIndex(
        sourceRoot,
        "INPX archive root could not be canonicalized.",
        {.MaxFilesystemEntries = 0});
    REQUIRE(limitedIndex.TryResolveExistingZipArchivePath("direct", "INPX archive path is unsafe.")
        == Canonical(directArchivePath));
    REQUIRE_THROWS_WITH(
        limitedIndex.TryResolveExistingZipArchivePath(
            "nested/archive-0",
            "INPX archive path is unsafe."),
        "INPX archive fallback lookup exceeds the configured filesystem entry limit of 0.");
}

TEST_CASE("INPX archive root index contains fallback symlinks and stops cycles", "[application][inpx][files]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-archive-access-index-symlinks");
    const auto sourceRoot = sandbox.GetPath() / "source";
    const auto payloadRoot = sourceRoot / "payloads";
    const auto internalTarget = sourceRoot / "internal-target";
    const auto outsideRoot = sandbox.GetPath() / "outside";
    WritePlaceholderFile(payloadRoot / "nested" / "inside.zip");
    WritePlaceholderFile(internalTarget / "linked.zip");
    WritePlaceholderFile(outsideRoot / "outside.zip");
    if (!TryCreateDirectorySymlink(payloadRoot, payloadRoot / "nested" / "cycle"))
    {
        SKIP("Directory symlinks are unavailable.");
    }
    REQUIRE(TryCreateDirectorySymlink(internalTarget, payloadRoot / "alias"));
    REQUIRE(TryCreateDirectorySymlink(outsideRoot, payloadRoot / "external"));

    std::size_t filesystemEntryVisitCount = 0;
    InpxWebReader::StoragePlanning::CInpxArchiveRootIndex archiveRootIndex(
        sourceRoot, "INPX archive root could not be canonicalized.", {.OnFilesystemEntryVisited = [&]() {
            ++filesystemEntryVisitCount;
        }});

    REQUIRE(archiveRootIndex.TryResolveExistingZipArchivePath("nested/inside", "INPX archive path is unsafe.")
            == Canonical(payloadRoot / "nested" / "inside.zip"));
    REQUIRE(archiveRootIndex.TryResolveExistingZipArchivePath("alias/linked", "INPX archive path is unsafe.")
            == Canonical(internalTarget / "linked.zip"));
    REQUIRE_FALSE(archiveRootIndex.TryResolveExistingZipArchivePath("external/outside", "INPX archive path is unsafe.")
                      .has_value());
    REQUIRE(filesystemEntryVisitCount < 32);
}

TEST_CASE("Persisted INPX archive locator remains bound after an earlier "
          "candidate appears",
          "[application][inpx]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-archive-access-persisted");
    const auto sourceRoot = sandbox.GetPath() / "source";
    const auto selectedArchivePath = sourceRoot / "z-selected" / "fb2-main.zip";
    WritePlaceholderFile(selectedArchivePath);
    const InpxWebReader::Application::SInpxSourceInfo source{.InpxPath = sourceRoot / "catalog.inpx",
                                                              .ArchiveRoot = sourceRoot};

    const auto selected = InpxWebReader::Application::ResolvePortableInpxArchivePath(source, "fb2-main");
    REQUIRE(selected.RelativePathUtf8 == "z-selected/fb2-main.zip");
    REQUIRE(selected.AbsolutePath == Canonical(selectedArchivePath));

    WritePlaceholderFile(sourceRoot / "a-earlier" / "fb2-main.zip");
    REQUIRE(InpxWebReader::Application::ResolveInpxArchivePath(source, "fb2-main")
            == Canonical(sourceRoot / "a-earlier" / "fb2-main.zip"));
    REQUIRE(InpxWebReader::Application::ResolvePersistedInpxArchivePath(source, selected.RelativePathUtf8)
            == Canonical(selectedArchivePath));
}

TEST_CASE("INPX archive path resolver rejects escaped archive names", "[application][inpx]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-archive-access-unsafe");
    const auto sourceRoot = sandbox.GetPath() / "source";
    std::filesystem::create_directories(sourceRoot);

    const InpxWebReader::Application::SInpxSourceInfo source{.InpxPath = sourceRoot / "catalog.inpx",
                                                              .ArchiveRoot = sourceRoot};

    REQUIRE_THROWS_WITH(InpxWebReader::Application::ResolveInpxArchivePath(source, "../escaped"),
                        "INPX archive path is unsafe.");
}

TEST_CASE("INPX archive reader reuses one opened archive for multiple entries", "[application][inpx]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-archive-reader");
    const auto sourceRoot = sandbox.GetPath() / "source";
    WriteZipArchive(sourceRoot / "fb2-main.zip");

    const InpxWebReader::Application::SInpxSourceInfo source{.InpxPath = sourceRoot / "catalog.inpx",
                                                              .ArchiveRoot = sourceRoot};

    const InpxWebReader::Application::CInpxArchiveReader reader(source, "fb2-main");
    const auto outputRoot = sandbox.GetPath() / "out";
    reader.ExtractEntryToPath("first.fb2", outputRoot / "first.fb2");
    reader.ExtractEntryToPath("second.fb2", outputRoot / "second.fb2");

    REQUIRE(std::ifstream(outputRoot / "first.fb2", std::ios::binary).peek() == 'f');
    REQUIRE(std::ifstream(outputRoot / "second.fb2", std::ios::binary).peek() == 's');
}

TEST_CASE("INPX archive reader reads entry bytes without extracting to disk", "[application][inpx]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-archive-reader-bytes");
    const auto sourceRoot = sandbox.GetPath() / "source";
    WriteZipArchive(sourceRoot / "fb2-main.zip");

    const InpxWebReader::Application::SInpxSourceInfo source{
        .InpxPath = sourceRoot / "catalog.inpx",
        .ArchiveRoot = sourceRoot
    };

    const InpxWebReader::Application::CInpxArchiveReader reader(source, "fb2-main");

    REQUIRE(reader.ReadEntryBytes("first.fb2") == "first");
    REQUIRE(reader.ReadEntryBytes("second.fb2") == "second");
    REQUIRE(reader.ReadEntrySize("first.fb2") == 5);
}

TEST_CASE("INPX archive reader checkpoints during streamed entry inflation", "[application][inpx][cancellation]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-archive-reader-byte-cancellation");
    const auto sourceRoot = sandbox.GetPath() / "source";
    const std::string payload(512ull * 1024ull, 'x');
    WriteStoredZipArchive(sourceRoot / "fb2-main.zip", "book.fb2", payload);
    const InpxWebReader::Application::SInpxSourceInfo source{
        .InpxPath = sourceRoot / "catalog.inpx",
        .ArchiveRoot = sourceRoot
    };
    const InpxWebReader::Application::CInpxArchiveReader reader(source, "fb2-main");
    std::size_t checkpointCount = 0;

    REQUIRE_THROWS_WITH(
        reader.ReadEntryBytes(
            "book.fb2",
            [&]() {
                ++checkpointCount;
                if (checkpointCount == 4)
                {
                    throw std::runtime_error("injected entry-read cancellation");
                }
            }),
        "injected entry-read cancellation");
    REQUIRE(checkpointCount == 4);
    REQUIRE(reader.ReadEntryBytes("book.fb2") == payload);
}

TEST_CASE("INPX archive manifest ignores ZIP order and detects same-size payload changes", "[application][inpx]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-archive-manifest");
    const auto sourceRoot = sandbox.GetPath() / "source";
    const auto archivePath = sourceRoot / "fb2-main.zip";
    const InpxWebReader::Application::SInpxSourceInfo source{
        .InpxPath = sourceRoot / "catalog.inpx",
        .ArchiveRoot = sourceRoot
    };

    WriteZipArchive(archivePath, {{"first.fb2", "first"}, {"second.fb2", "second"}});
    const auto firstState = InpxWebReader::Application::ReadInpxArchiveFileState(source, "fb2-main");
    const std::string firstFingerprint =
        InpxWebReader::Application::CInpxArchiveReader(source, "fb2-main")
            .ComputeManifestFingerprint(GManifestMemoryBudgetBytes);
    REQUIRE(firstFingerprint == "sha256:83a21f473dc06f0bd2816f7117730a00"
                                "b57f59e3de7c32b14f29d49439c63009");

    WriteZipArchive(archivePath, {{"second.fb2", "second"}, {"first.fb2", "first"}});
    const std::string reorderedFingerprint =
        InpxWebReader::Application::CInpxArchiveReader(source, "fb2-main")
            .ComputeManifestFingerprint(GManifestMemoryBudgetBytes);
    REQUIRE(reorderedFingerprint == firstFingerprint);

    WriteZipArchive(archivePath, {{"first.fb2", "FIRST"}, {"second.fb2", "second"}});
    const std::string changedFingerprint =
        InpxWebReader::Application::CInpxArchiveReader(source, "fb2-main")
            .ComputeManifestFingerprint(GManifestMemoryBudgetBytes);
    REQUIRE(changedFingerprint != firstFingerprint);
    REQUIRE(firstState.FileSizeBytes > 0);
}

TEST_CASE("INPX archive validated manifest streams every payload", "[application][inpx]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-archive-validated-manifest");
    const auto sourceRoot = sandbox.GetPath() / "source";
    WriteZipArchive(sourceRoot / "fb2-main.zip", {{"first.fb2", "first"}, {"second.fb2", "second"}});
    const InpxWebReader::Application::SInpxSourceInfo source{
        .InpxPath = sourceRoot / "catalog.inpx",
        .ArchiveRoot = sourceRoot
    };

    std::size_t checkpointCount = 0;
    const InpxWebReader::Application::CInpxArchiveReader reader(source, "fb2-main");
    const auto manifest = reader.ReadValidatedManifest(
        GManifestMemoryBudgetBytes,
        [&checkpointCount]() { ++checkpointCount; });

    REQUIRE(manifest.FingerprintUtf8
        == reader.ComputeManifestFingerprint(GManifestMemoryBudgetBytes));
    REQUIRE(manifest.PayloadBytesValidated == 11);
    REQUIRE(checkpointCount >= 2);
}

TEST_CASE("INPX archive validated manifest rejects payload corruption hidden by central metadata", "[application][inpx]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-archive-corrupt-payload");
    const auto sourceRoot = sandbox.GetPath() / "source";
    const auto archivePath = sourceRoot / "fb2-main.zip";
    constexpr std::string_view payload = "PAYLOAD-CONTENT-123";
    WriteStoredZipArchive(archivePath, "book.fb2", std::string{payload});
    const InpxWebReader::Application::SInpxSourceInfo source{
        .InpxPath = sourceRoot / "catalog.inpx",
        .ArchiveRoot = sourceRoot
    };

    const std::string centralFingerprint =
        InpxWebReader::Application::CInpxArchiveReader(source, "fb2-main")
            .ComputeManifestFingerprint(GManifestMemoryBudgetBytes);
    CorruptStoredPayload(archivePath, payload);

    const InpxWebReader::Application::CInpxArchiveReader reader(source, "fb2-main");
    REQUIRE(reader.ComputeManifestFingerprint(GManifestMemoryBudgetBytes) == centralFingerprint);
    REQUIRE_THROWS_AS(
        reader.ReadValidatedManifest(GManifestMemoryBudgetBytes),
        std::runtime_error);
    REQUIRE_THROWS_AS(reader.ReadEntryBytes("book.fb2"), std::runtime_error);

    const auto destination = sandbox.GetPath() / "out" / "existing.fb2";
    WritePlaceholderFile(destination);
    REQUIRE_THROWS_AS(
        reader.ExtractEntryToPath("book.fb2", destination),
        std::runtime_error);
    REQUIRE(std::filesystem::file_size(destination) == std::string{"placeholder"}.size());
}

TEST_CASE("INPX archive manifest applies coarse entry and name-volume limits", "[application][inpx][limits]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-archive-manifest-budget");
    const auto sourceRoot = sandbox.GetPath() / "source";
    WriteZipArchive(sourceRoot / "fb2-main.zip");
    const InpxWebReader::Application::SInpxSourceInfo source{
        .InpxPath = sourceRoot / "catalog.inpx",
        .ArchiveRoot = sourceRoot
    };
    const InpxWebReader::Application::CInpxArchiveReader reader(source, "fb2-main");
    constexpr std::uint64_t twoEntryBudgetBytes = 2ull * 2ull * 256ull;

    REQUIRE_THROWS_WITH(
        reader.ComputeManifestFingerprint(twoEntryBudgetBytes - 1),
        Catch::Matchers::ContainsSubstring("manifest metadata exceeds the configured memory budget"));
    REQUIRE_THROWS_WITH(
        reader.ReadValidatedManifest(twoEntryBudgetBytes - 1),
        Catch::Matchers::ContainsSubstring("manifest metadata exceeds the configured memory budget"));
    REQUIRE(reader.ComputeManifestFingerprint(twoEntryBudgetBytes)
        == "sha256:83a21f473dc06f0bd2816f7117730a00"
           "b57f59e3de7c32b14f29d49439c63009");

    const std::string firstBoundaryName = std::string(252, 'a') + ".fb2";
    const std::string secondBoundaryName = std::string(252, 'b') + ".fb2";
    WriteZipArchive(
        sourceRoot / "names-at-limit.zip",
        {{firstBoundaryName, "first"}, {secondBoundaryName, "second"}});
    const InpxWebReader::Application::CInpxArchiveReader namesAtLimit(source, "names-at-limit");
    REQUIRE_FALSE(namesAtLimit.ComputeManifestFingerprint(twoEntryBudgetBytes).empty());

    const std::string aboveBoundaryName = std::string(253, 'b') + ".fb2";
    WriteZipArchive(
        sourceRoot / "names-above-limit.zip",
        {{firstBoundaryName, "first"}, {aboveBoundaryName, "second"}});
    const InpxWebReader::Application::CInpxArchiveReader namesAboveLimit(source, "names-above-limit");
    REQUIRE_THROWS_WITH(
        namesAboveLimit.ComputeManifestFingerprint(twoEntryBudgetBytes),
        Catch::Matchers::ContainsSubstring("manifest metadata exceeds the configured memory budget"));
}

TEST_CASE("INPX archive manifest checkpoints metadata and sorting for empty entries", "[application][inpx][cancellation]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-empty-manifest-cancellation");
    const auto sourceRoot = sandbox.GetPath() / "source";
    constexpr std::size_t entryCount = 64;
    std::vector<std::pair<std::string, std::string>> entries;
    entries.reserve(entryCount);
    for (std::size_t index = 0; index < entryCount; ++index)
    {
        entries.emplace_back(
            "empty-" + std::to_string(entryCount - index) + ".fb2",
            std::string{});
    }
    WriteZipArchive(sourceRoot / "fb2-main.zip", entries);
    const InpxWebReader::Application::SInpxSourceInfo source{
        .InpxPath = sourceRoot / "catalog.inpx",
        .ArchiveRoot = sourceRoot
    };
    const InpxWebReader::Application::CInpxArchiveReader reader(source, "fb2-main");
    constexpr std::size_t throwAtCheckpoint = (entryCount * 2) + 4;

    SECTION("fingerprint")
    {
        std::size_t checkpointCount = 0;
        REQUIRE_THROWS_WITH(
            reader.ComputeManifestFingerprint(
                GManifestMemoryBudgetBytes,
                [&]() {
                    if (++checkpointCount == throwAtCheckpoint)
                    {
                        throw std::runtime_error("cancelled during manifest sort");
                    }
                }),
            "cancelled during manifest sort");
        REQUIRE(checkpointCount == throwAtCheckpoint);
    }

    SECTION("validated manifest")
    {
        std::size_t checkpointCount = 0;
        REQUIRE_THROWS_WITH(
            reader.ReadValidatedManifest(
                GManifestMemoryBudgetBytes,
                [&]() {
                    if (++checkpointCount == throwAtCheckpoint)
                    {
                        throw std::runtime_error("cancelled during manifest sort");
                    }
                }),
            "cancelled during manifest sort");
        REQUIRE(checkpointCount == throwAtCheckpoint);
    }
}

TEST_CASE("INPX archive reader preserves destination when the entry is missing", "[application][inpx][files]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-archive-reader-missing-entry");
    const auto sourceRoot = sandbox.GetPath() / "source";
    WriteZipArchive(sourceRoot / "fb2-main.zip");
    const InpxWebReader::Application::SInpxSourceInfo source{
        .InpxPath = sourceRoot / "catalog.inpx",
        .ArchiveRoot = sourceRoot
    };
    const auto destination = sandbox.GetPath() / "out" / "existing.fb2";
    WritePlaceholderFile(destination);

    const InpxWebReader::Application::CInpxArchiveReader reader(source, "fb2-main");
    REQUIRE_THROWS_WITH(
        reader.ExtractEntryToPath("missing.fb2", destination),
        "INPX archive entry is missing: missing.fb2");
    REQUIRE(std::filesystem::file_size(destination) == std::string{"placeholder"}.size());
    REQUIRE(std::ranges::none_of(
        std::filesystem::directory_iterator(destination.parent_path()),
        [&destination](const auto& entry) {
            return entry.path() != destination;
        }));
}

TEST_CASE("INPX archive reader rejects a corrupt archive", "[application][inpx][files]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-archive-reader-corrupt");
    const auto sourceRoot = sandbox.GetPath() / "source";
    WritePlaceholderFile(sourceRoot / "fb2-main.zip");
    const InpxWebReader::Application::SInpxSourceInfo source{
        .InpxPath = sourceRoot / "catalog.inpx",
        .ArchiveRoot = sourceRoot
    };

    REQUIRE_THROWS_AS(
        InpxWebReader::Application::CInpxArchiveReader(source, "fb2-main"),
        std::runtime_error);
}

TEST_CASE("INPX archive reader extracts entries above the former 32 MiB limit", "[application][inpx]")
{
    static_assert(GLargeButAllowedEntryBytes < InpxWebReader::Foundation::GMaxBookPayloadBytes);

    CTestWorkspace sandbox("inpx-web-reader-inpx-archive-reader-large-entry");
    const auto sourceRoot = sandbox.GetPath() / "source";
    WriteLargeZipArchive(sourceRoot / "fb2-main.zip");

    const InpxWebReader::Application::SInpxSourceInfo source{
        .InpxPath = sourceRoot / "catalog.inpx",
        .ArchiveRoot = sourceRoot
    };

    const auto outputPath = sandbox.GetPath() / "out" / "large.fb2";
    const InpxWebReader::Application::CInpxArchiveReader reader(source, "fb2-main");
    reader.ExtractEntryToPath("large.fb2", outputPath);

    REQUIRE(std::filesystem::file_size(outputPath) == GLargeButAllowedEntryBytes);
}

TEST_CASE("INPX archive reader payload limit includes its exact boundary", "[application][inpx][limits]")
{
    REQUIRE(InpxWebReader::Foundation::IsBookPayloadSizeAllowed(
        InpxWebReader::Foundation::GMaxBookPayloadBytes - 1));
    REQUIRE(InpxWebReader::Foundation::IsBookPayloadSizeAllowed(
        InpxWebReader::Foundation::GMaxBookPayloadBytes));
    REQUIRE_FALSE(InpxWebReader::Foundation::IsBookPayloadSizeAllowed(
        InpxWebReader::Foundation::GMaxBookPayloadBytes + 1));
}
