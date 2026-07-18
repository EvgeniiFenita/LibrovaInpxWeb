#include "Foundation/ZipArchivePaths.hpp"

#include <string>

#include "Foundation/StringUtils.hpp"
#include "Foundation/UnicodeConversion.hpp"

namespace InpxWebReader::Foundation {

bool HasZipArchiveExtension(const std::filesystem::path& path)
{
    const auto extensionUtf8 = Unicode::PathToUtf8(path.extension());
    return extensionUtf8.size() == std::string_view{".zip"}.size()
        && EndsWithAsciiInsensitive(extensionUtf8, ".zip");
}

std::filesystem::path BuildZipArchiveFileRelativePath(const std::string_view archiveNameUtf8)
{
    std::string archiveName = std::string{archiveNameUtf8};
    if (!EndsWithAsciiInsensitive(archiveName, ".zip"))
    {
        archiveName += ".zip";
    }

    return Unicode::PathFromUtf8(archiveName).lexically_normal();
}

std::filesystem::path BuildZipArchiveFileRelativePath(const std::filesystem::path& relativePath)
{
    if (HasZipArchiveExtension(relativePath))
    {
        return relativePath;
    }

    auto zippedRelativePath = relativePath;
    zippedRelativePath += ".zip";
    return zippedRelativePath;
}

} // namespace InpxWebReader::Foundation
