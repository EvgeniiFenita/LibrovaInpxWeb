#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <zip.h>

#include "Inpx/InpxSourceConfiguration.hpp"
#include "Foundation/UnicodeConversion.hpp"
#include "TestWorkspace.hpp"

namespace {

const std::string GFieldSeparator(1, '\x04');

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

std::filesystem::path WriteInpxArchive(
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

std::string MakeInpxRecord()
{
    return std::string("A")
        + GFieldSeparator + "g"
        + GFieldSeparator + "Book"
        + GFieldSeparator
        + GFieldSeparator
        + GFieldSeparator + "book"
        + GFieldSeparator + "1"
        + GFieldSeparator + "123"
        + GFieldSeparator + "0"
        + GFieldSeparator + "fb2"
        + GFieldSeparator
        + GFieldSeparator + "ru"
        + GFieldSeparator
        + GFieldSeparator
        + GFieldSeparator + "\n";
}

} // namespace

TEST_CASE("INPX source validation accepts readable archives and infers parent archive root", "[inpx]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-source-valid");
    const auto inpxPath = WriteInpxArchive(
        sandbox.GetPath() / "source" / "catalog.inpx",
        {{"fb2-main.inp", MakeInpxRecord()}});

    const auto result = InpxWebReader::Inpx::CInpxSourceConfiguration::Validate(inpxPath);

    REQUIRE(result.IsValid);
    REQUIRE(result.InpxPath == inpxPath.lexically_normal());
    REQUIRE(result.ArchiveRoot == inpxPath.parent_path().lexically_normal());
    REQUIRE(result.ErrorUtf8.empty());
}

TEST_CASE("INPX source validation discovers matching sibling archive directory", "[inpx]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-source-archive-discovery");
    const auto sourceRoot = sandbox.GetPath() / "source";
    const auto inpxPath = WriteInpxArchive(
        sourceRoot / "catalog.inpx",
        {{"fb2-main.inp", MakeInpxRecord()}});
    const auto archiveRoot = sourceRoot / "payloads";
    std::filesystem::create_directories(archiveRoot);
    std::ofstream(archiveRoot / "fb2-main.zip", std::ios::binary) << "archive";

    const auto result = InpxWebReader::Inpx::CInpxSourceConfiguration::Validate(inpxPath);

    REQUIRE(result.IsValid);
    REQUIRE(result.ArchiveRoot == archiveRoot.lexically_normal());
}

TEST_CASE("INPX source validation uses entry names without decoding full inp payloads", "[inpx]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-source-validation-fast");
    const auto sourceRoot = sandbox.GetPath() / "source";
    std::string invalidUtf16Payload;
    invalidUtf16Payload.push_back(static_cast<char>(0xFF));
    invalidUtf16Payload.push_back(static_cast<char>(0xFE));
    invalidUtf16Payload.push_back('A');
    const auto inpxPath = WriteInpxArchive(
        sourceRoot / "catalog.inpx",
        {{"fb2-main.inp", invalidUtf16Payload}});
    const auto archiveRoot = sourceRoot / "payloads";
    std::filesystem::create_directories(archiveRoot);
    std::ofstream(archiveRoot / "fb2-main.zip", std::ios::binary) << "archive";

    const auto result = InpxWebReader::Inpx::CInpxSourceConfiguration::Validate(inpxPath);

    REQUIRE(result.IsValid);
    REQUIRE(result.ArchiveRoot == archiveRoot.lexically_normal());
}

TEST_CASE("INPX source validation rejects archives without inp entries", "[inpx]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-source-missing-inp");
    const auto inpxPath = WriteInpxArchive(
        sandbox.GetPath() / "source" / "catalog.inpx",
        {{"notes.txt", "not an inp"}});

    const auto result = InpxWebReader::Inpx::CInpxSourceConfiguration::Validate(inpxPath);

    REQUIRE_FALSE(result.IsValid);
    REQUIRE(result.ErrorUtf8.find(".inp entries") != std::string::npos);
}

TEST_CASE("INPX source validation preserves explicit archive root override", "[inpx]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-source-override");
    const auto inpxPath = WriteInpxArchive(
        sandbox.GetPath() / "source" / "Каталог.inpx",
        {{"fb2-main.zip.inp", MakeInpxRecord()}});
    const auto archiveRoot = sandbox.GetPath() / "source" / "Архивы";
    std::filesystem::create_directories(archiveRoot);

    const auto result = InpxWebReader::Inpx::CInpxSourceConfiguration::Validate(inpxPath, archiveRoot);

    REQUIRE(result.IsValid);
    REQUIRE(result.ArchiveRoot == archiveRoot.lexically_normal());
}

TEST_CASE("INPX source validation rejects missing explicit archive root", "[inpx]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-source-missing-archive-root");
    const auto inpxPath = WriteInpxArchive(
        sandbox.GetPath() / "source" / "catalog.inpx",
        {{"fb2-main.zip.inp", MakeInpxRecord()}});

    const auto result = InpxWebReader::Inpx::CInpxSourceConfiguration::Validate(
        inpxPath,
        sandbox.GetPath() / "source" / "missing-archives");

    REQUIRE_FALSE(result.IsValid);
    REQUIRE(result.ErrorUtf8.find("Archive root") != std::string::npos);
}

TEST_CASE("INPX source validation rejects explicit archive root pointing to a file", "[inpx]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-source-file-archive-root");
    const auto inpxPath = WriteInpxArchive(
        sandbox.GetPath() / "source" / "catalog.inpx",
        {{"fb2-main.zip.inp", MakeInpxRecord()}});
    const auto archiveRootFile = sandbox.GetPath() / "source" / "archives.txt";
    std::ofstream(archiveRootFile, std::ios::binary) << "not a directory";

    const auto result = InpxWebReader::Inpx::CInpxSourceConfiguration::Validate(inpxPath, archiveRootFile);

    REQUIRE_FALSE(result.IsValid);
    REQUIRE(result.ErrorUtf8.find("file") != std::string::npos);
}
