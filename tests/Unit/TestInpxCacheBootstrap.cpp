#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>
#include <fstream>

#include "App/InpxCacheBootstrap.hpp"
#include "TestWorkspace.hpp"

namespace {

void WriteFile(const std::filesystem::path& path)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream{path} << "test";
}

} // namespace

TEST_CASE("INPX cache bootstrap creates only database and cover directories", "[application][inpx]")
{
    CTestWorkspace workspace("inpx-bootstrap-create");
    const auto root = workspace.GetPath() / "Cache";

    InpxWebReader::Application::CInpxCacheBootstrap::PrepareCacheRoot(
        root,
        InpxWebReader::Application::ECacheOpenMode::CreateNew);

    REQUIRE(std::filesystem::is_directory(root / "Database"));
    REQUIRE(std::filesystem::is_directory(root / "Covers"));
    std::size_t entryCount = 0;
    for ([[maybe_unused]] const auto& entry : std::filesystem::directory_iterator(root))
    {
        ++entryCount;
    }
    REQUIRE(entryCount == 2);
}

TEST_CASE("INPX cache bootstrap opens only the new database layout", "[application][inpx]")
{
    CTestWorkspace workspace("inpx-bootstrap-open");
    const auto root = workspace.GetPath() / "Cache";
    std::filesystem::create_directories(root / "Covers");
    WriteFile(root / "Database" / "inpx-web-reader.db");

    REQUIRE_NOTHROW(InpxWebReader::Application::CInpxCacheBootstrap::PrepareCacheRoot(
        root,
        InpxWebReader::Application::ECacheOpenMode::Open));
}

TEST_CASE("INPX cache bootstrap rejects non-empty new roots", "[application][inpx]")
{
    CTestWorkspace workspace("inpx-bootstrap-nonempty");
    const auto root = workspace.GetPath() / "Cache";
    WriteFile(root / "unexpected-file");

    REQUIRE_THROWS_WITH(
        InpxWebReader::Application::CInpxCacheBootstrap::PrepareCacheRoot(
            root,
            InpxWebReader::Application::ECacheOpenMode::CreateNew),
        Catch::Matchers::ContainsSubstring("must be empty"));
}

TEST_CASE("INPX cache bootstrap rejects symbolic links in managed cache paths", "[application][inpx]")
{
    CTestWorkspace workspace("inpx-bootstrap-symlinks");
    const auto outside = workspace.GetPath() / "Outside";
    std::filesystem::create_directories(outside / "Database");
    std::filesystem::create_directories(outside / "Covers");
    WriteFile(outside / "Database" / "inpx-web-reader.db");

    SECTION("cache root")
    {
        const auto root = workspace.GetPath() / "Cache";
        std::filesystem::create_directory_symlink(outside, root);
        REQUIRE_THROWS_WITH(
            InpxWebReader::Application::CInpxCacheBootstrap::PrepareCacheRoot(
                root,
                InpxWebReader::Application::ECacheOpenMode::Open),
            Catch::Matchers::ContainsSubstring("incomplete or missing"));
    }

    SECTION("database directory")
    {
        const auto root = workspace.GetPath() / "Cache";
        std::filesystem::create_directories(root / "Covers");
        std::filesystem::create_directory_symlink(outside / "Database", root / "Database");
        REQUIRE_THROWS_WITH(
            InpxWebReader::Application::CInpxCacheBootstrap::PrepareCacheRoot(
                root,
                InpxWebReader::Application::ECacheOpenMode::Open),
            Catch::Matchers::ContainsSubstring("incomplete or missing"));
    }

    SECTION("covers directory")
    {
        const auto root = workspace.GetPath() / "Cache";
        std::filesystem::create_directories(root / "Database");
        WriteFile(root / "Database" / "inpx-web-reader.db");
        std::filesystem::create_directory_symlink(outside / "Covers", root / "Covers");
        REQUIRE_THROWS_WITH(
            InpxWebReader::Application::CInpxCacheBootstrap::PrepareCacheRoot(
                root,
                InpxWebReader::Application::ECacheOpenMode::Open),
            Catch::Matchers::ContainsSubstring("incomplete or missing"));
    }

    SECTION("database file")
    {
        const auto root = workspace.GetPath() / "Cache";
        std::filesystem::create_directories(root / "Database");
        std::filesystem::create_directories(root / "Covers");
        std::filesystem::create_symlink(
            outside / "Database" / "inpx-web-reader.db",
            root / "Database" / "inpx-web-reader.db");
        REQUIRE_THROWS_WITH(
            InpxWebReader::Application::CInpxCacheBootstrap::PrepareCacheRoot(
                root,
                InpxWebReader::Application::ECacheOpenMode::Open),
            Catch::Matchers::ContainsSubstring("incomplete or missing"));
    }
}

TEST_CASE("INPX cache bootstrap rejects a symbolic link for a new root", "[application][inpx]")
{
    CTestWorkspace workspace("inpx-bootstrap-new-symlink");
    const auto outside = workspace.GetPath() / "Outside";
    const auto root = workspace.GetPath() / "Cache";
    std::filesystem::create_directories(outside);
    std::filesystem::create_directory_symlink(outside, root);

    REQUIRE_THROWS_WITH(
        InpxWebReader::Application::CInpxCacheBootstrap::PrepareCacheRoot(
            root,
            InpxWebReader::Application::ECacheOpenMode::CreateNew),
        Catch::Matchers::ContainsSubstring("must not be a symbolic link"));
}
