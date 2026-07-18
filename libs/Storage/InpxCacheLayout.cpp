#include "Storage/InpxCacheLayout.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <stdexcept>
#include <string>

#include "Foundation/Sha256Fingerprint.hpp"
#include "Foundation/StringUtils.hpp"

namespace {

[[nodiscard]] std::string NormalizeExtension(const std::string_view extension)
{
    const std::string_view value = extension.starts_with('.') ? extension.substr(1) : extension;
    if (value.empty() || value.size() > 10
        || !std::ranges::all_of(value, [](const unsigned char character) {
            return std::isalnum(character) != 0;
        }))
    {
        throw std::invalid_argument("Cover path requires a simple alphanumeric extension.");
    }

    return InpxWebReader::Foundation::ToLowerAscii(std::string{value});
}

} // namespace

namespace InpxWebReader::StoragePlanning {

SInpxCacheLayoutPaths CInpxCacheLayout::Build(const std::filesystem::path& cacheRoot)
{
    return {
        .Root = cacheRoot,
        .DatabaseDirectory = cacheRoot / "Database",
        .CoversDirectory = cacheRoot / "Covers"
    };
}

std::filesystem::path CInpxCacheLayout::GetDatabasePath(const std::filesystem::path& cacheRoot)
{
    return Build(cacheRoot).DatabaseDirectory / "inpx-web-reader.db";
}

std::filesystem::path CInpxCacheLayout::ResolveCacheRootFromDatabasePath(
    const std::filesystem::path& databasePath)
{
    const auto databaseDirectory = databasePath.parent_path();
    const auto candidateCacheRoot = databaseDirectory.parent_path();
    return Build(candidateCacheRoot).DatabaseDirectory == databaseDirectory
        ? candidateCacheRoot
        : databaseDirectory;
}

std::filesystem::path CInpxCacheLayout::GetContentAddressedCoverPath(
    const std::filesystem::path& cacheRoot,
    const std::string_view fingerprintUtf8,
    const std::string_view extension)
{
    const std::string normalizedTaggedFingerprint = Foundation::CSha256Fingerprint::Normalize(fingerprintUtf8);
    const std::string_view normalizedFingerprint = std::string_view{normalizedTaggedFingerprint}.substr(7);
    const std::string normalizedExtension = NormalizeExtension(extension);
    return Build(cacheRoot).CoversDirectory
        / normalizedFingerprint.substr(0, 2)
        / std::format("{}.{}", normalizedFingerprint, normalizedExtension);
}

bool CInpxCacheLayout::IsContentAddressedCoverPath(
    const std::filesystem::path& cacheRoot,
    const std::filesystem::path& coverPath)
{
    const auto coversDirectory = Build(cacheRoot).CoversDirectory.lexically_normal();
    const auto normalizedPath = coverPath.lexically_normal();
    const auto relativePath = normalizedPath.lexically_relative(coversDirectory);
    if (relativePath.empty() || relativePath.is_absolute())
    {
        return false;
    }

    auto part = relativePath.begin();
    if (part == relativePath.end())
    {
        return false;
    }
    const std::string shard = part->string();
    ++part;
    if (part == relativePath.end())
    {
        return false;
    }
    const std::filesystem::path fileName = *part;
    ++part;
    if (part != relativePath.end())
    {
        return false;
    }

    const std::string fingerprint = fileName.stem().string();
    const std::string extension = fileName.extension().string();
    if (shard.size() != 2 || fingerprint.size() != 64 || extension.size() < 2)
    {
        return false;
    }
    const auto isLowerHex = [](const unsigned char character) {
        return std::isdigit(character) != 0 || (character >= 'a' && character <= 'f');
    };
    if (!std::ranges::all_of(shard, isLowerHex)
        || !std::ranges::all_of(fingerprint, isLowerHex)
        || !fingerprint.starts_with(shard))
    {
        return false;
    }

    try
    {
        return GetContentAddressedCoverPath(cacheRoot, fingerprint, extension) == normalizedPath;
    }
    catch (const std::invalid_argument&)
    {
        return false;
    }
}

} // namespace InpxWebReader::StoragePlanning
