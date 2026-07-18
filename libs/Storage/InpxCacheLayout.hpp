#pragma once

#include <filesystem>
#include <string>

namespace InpxWebReader::StoragePlanning {

struct SInpxCacheLayoutPaths
{
    std::filesystem::path Root;
    std::filesystem::path DatabaseDirectory;
    std::filesystem::path CoversDirectory;
};

class CInpxCacheLayout
{
public:
    [[nodiscard]] static SInpxCacheLayoutPaths Build(const std::filesystem::path& cacheRoot);
    [[nodiscard]] static std::filesystem::path GetDatabasePath(const std::filesystem::path& cacheRoot);
    [[nodiscard]] static std::filesystem::path ResolveCacheRootFromDatabasePath(
        const std::filesystem::path& databasePath);
    [[nodiscard]] static std::filesystem::path GetContentAddressedCoverPath(
        const std::filesystem::path& cacheRoot,
        std::string_view fingerprintUtf8,
        std::string_view extension);
    [[nodiscard]] static bool IsContentAddressedCoverPath(
        const std::filesystem::path& cacheRoot,
        const std::filesystem::path& coverPath);
};

} // namespace InpxWebReader::StoragePlanning
