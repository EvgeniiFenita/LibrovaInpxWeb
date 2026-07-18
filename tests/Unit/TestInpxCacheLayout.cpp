#include <catch2/catch_test_macros.hpp>

#include <string>

#include "Storage/InpxCacheLayout.hpp"

TEST_CASE("INPX cache layout contains database and covers only", "[storage-planning][inpx]")
{
    const auto layout = InpxWebReader::StoragePlanning::CInpxCacheLayout::Build("/srv/cache");
    REQUIRE(layout.DatabaseDirectory == std::filesystem::path{"/srv/cache/Database"});
    REQUIRE(layout.CoversDirectory == std::filesystem::path{"/srv/cache/Covers"});
    REQUIRE(
        InpxWebReader::StoragePlanning::CInpxCacheLayout::GetDatabasePath("/srv/cache") ==
        std::filesystem::path{"/srv/cache/Database/inpx-web-reader.db"});
}

TEST_CASE("INPX content-addressed cover cache paths are stable", "[storage-planning][inpx]")
{
    const std::string fingerprint(64, 'a');
    REQUIRE(
        InpxWebReader::StoragePlanning::CInpxCacheLayout::GetContentAddressedCoverPath(
            "/srv/cache",
            fingerprint,
            ".png")
        == std::filesystem::path{"/srv/cache/Covers/aa"} / (fingerprint + ".png"));
    REQUIRE(
        InpxWebReader::StoragePlanning::CInpxCacheLayout::GetContentAddressedCoverPath(
            "/srv/cache",
            "sha256:" + fingerprint,
            "png")
        == std::filesystem::path{"/srv/cache/Covers/aa"} / (fingerprint + ".png"));
    REQUIRE(
        InpxWebReader::StoragePlanning::CInpxCacheLayout::GetContentAddressedCoverPath(
            "/srv/cache",
            std::string(64, 'A'),
            "PNG")
        == std::filesystem::path{"/srv/cache/Covers/aa"} / (fingerprint + ".png"));
    REQUIRE_THROWS_AS(
        InpxWebReader::StoragePlanning::CInpxCacheLayout::GetContentAddressedCoverPath(
            "/srv/cache",
            "short",
            "png"),
        std::invalid_argument);
    REQUIRE_THROWS_AS(
        InpxWebReader::StoragePlanning::CInpxCacheLayout::GetContentAddressedCoverPath(
            "/srv/cache",
            std::string(63, 'a') + "/",
            "png"),
        std::invalid_argument);
    REQUIRE_THROWS_AS(
        InpxWebReader::StoragePlanning::CInpxCacheLayout::GetContentAddressedCoverPath(
            "/srv/cache",
            fingerprint,
            "../png"),
        std::invalid_argument);
}

TEST_CASE("INPX content-addressed cover cache paths reject non-canonical shapes", "[storage-planning][inpx]")
{
    const std::filesystem::path cacheRoot = "/srv/cache";
    const std::string fingerprint(64, 'a');
    const auto validPath = cacheRoot / "Covers" / "aa" / (fingerprint + ".png");

    REQUIRE(InpxWebReader::StoragePlanning::CInpxCacheLayout::IsContentAddressedCoverPath(
        cacheRoot,
        validPath));
    REQUIRE_FALSE(InpxWebReader::StoragePlanning::CInpxCacheLayout::IsContentAddressedCoverPath(
        cacheRoot,
        cacheRoot / "Covers" / "ab" / (fingerprint + ".png")));
    REQUIRE_FALSE(InpxWebReader::StoragePlanning::CInpxCacheLayout::IsContentAddressedCoverPath(
        cacheRoot,
        cacheRoot / "Covers" / "aa" / "unrelated.txt"));
    REQUIRE_FALSE(InpxWebReader::StoragePlanning::CInpxCacheLayout::IsContentAddressedCoverPath(
        cacheRoot,
        cacheRoot / "outside" / "aa" / (fingerprint + ".png")));
}
