#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include "Foundation/FileSystemUtils.hpp"
#include "TestWorkspace.hpp"

namespace {

void WriteTextFile(const std::filesystem::path& path, const std::string& text)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output << text;
}

std::string ReadTextFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

} // namespace

TEST_CASE("MoveFileWithCopyFallback creates the destination directory when rename falls back to copy", "[foundation][filesystem]")
{
    CTestWorkspace sandbox("inpx-web-reader-filesystem-utils-move-fallback");
    const std::filesystem::path sourcePath = sandbox.GetPath() / "source" / "book.epub";
    const std::filesystem::path destinationPath = sandbox.GetPath() / "missing" / "nested" / "book.epub";

    WriteTextFile(sourcePath, "moved-content");
    REQUIRE_FALSE(std::filesystem::exists(destinationPath.parent_path()));

    InpxWebReader::Foundation::MoveFileWithCopyFallback(sourcePath, destinationPath, "test");

    REQUIRE_FALSE(std::filesystem::exists(sourcePath));
    REQUIRE(std::filesystem::exists(destinationPath));
    REQUIRE(ReadTextFile(destinationPath) == "moved-content");
}
