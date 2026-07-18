#pragma once

#include <filesystem>
#include <string_view>

namespace InpxWebReader::Foundation {

[[nodiscard]] bool HasZipArchiveExtension(const std::filesystem::path& path);

[[nodiscard]] std::filesystem::path BuildZipArchiveFileRelativePath(std::string_view archiveNameUtf8);

[[nodiscard]] std::filesystem::path BuildZipArchiveFileRelativePath(const std::filesystem::path& relativePath);

} // namespace InpxWebReader::Foundation
