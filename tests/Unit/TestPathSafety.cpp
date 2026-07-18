#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>
#include <fstream>

#include "Storage/PathSafety.hpp"
#include "TestFilesystemHelpers.hpp"
#include "TestWorkspace.hpp"

TEST_CASE("Path safety resolves a cached cover", "[path-safety]")
{
    CTestWorkspace workspace("inpx-path-safety-resolve");
    const auto cacheRoot = workspace.GetPath() / "Cache";
    const auto coverPath = cacheRoot / "Covers/07/7.jpg";
    std::filesystem::create_directories(coverPath.parent_path());
    std::ofstream(coverPath, std::ios::binary) << "jpeg";

    const auto resolved = InpxWebReader::SafePaths::TryResolvePathWithinRoot(
        cacheRoot,
        "Covers/07/7.jpg",
        "unsafe",
        "canonicalize");

    REQUIRE(resolved.has_value());
    REQUIRE(*resolved == std::filesystem::weakly_canonical(coverPath));
}

TEST_CASE("Path safety rejects a symlink escaping the cache", "[path-safety]")
{
    CTestWorkspace workspace("inpx-path-safety-symlink");
    const auto cacheRoot = workspace.GetPath() / "Cache";
    const auto outsideRoot = workspace.GetPath() / "Outside";
    const auto linkPath = cacheRoot / "Covers/07";
    std::filesystem::create_directories(outsideRoot);
    std::filesystem::create_directories(linkPath.parent_path());
    std::ofstream(outsideRoot / "7.jpg", std::ios::binary) << "jpeg";
    if (!TryCreateDirectorySymlink(outsideRoot, linkPath))
    {
        SKIP("Directory symlinks are unavailable.");
    }

    REQUIRE_THROWS_WITH(
        InpxWebReader::SafePaths::TryResolvePathWithinRoot(
            cacheRoot,
            "Covers/07/7.jpg",
            "unsafe",
            "canonicalize"),
        Catch::Matchers::ContainsSubstring("unsafe"));
}
