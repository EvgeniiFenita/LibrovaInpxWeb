#pragma once

#include <filesystem>
#include <system_error>

[[nodiscard]] inline bool TryCreateDirectorySymlink(
    const std::filesystem::path& target,
    const std::filesystem::path& linkPath)
{
    std::error_code errorCode;
    std::filesystem::create_directory_symlink(target, linkPath, errorCode);
    return !errorCode;
}
