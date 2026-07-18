#pragma once

#include <filesystem>
#include <string_view>
#include <system_error>

namespace InpxWebReader::Foundation {

[[nodiscard]] bool IsSafeRelativePath(const std::filesystem::path& path);

// Creates directory hierarchy; throws std::runtime_error on failure.
void EnsureDirectory(const std::filesystem::path& path);

// Atomically creates and returns a uniquely named child directory. Existing
// candidates are retried, so uniqueness does not depend on clock resolution.
[[nodiscard]] std::filesystem::path CreateUniqueDirectory(
    const std::filesystem::path& parentDirectory,
    std::string_view directoryNamePrefix);

// Moves a file with a copy+delete fallback when atomic rename is unavailable
// across filesystem boundaries.
void MoveFileWithCopyFallback(
    const std::filesystem::path& sourcePath,
    const std::filesystem::path& destinationPath,
    std::string_view operationName);

// Removes path and all children recursively without throwing.
// Returns any filesystem error encountered by remove_all().
std::error_code RemovePathNoThrow(const std::filesystem::path& path) noexcept;

// Removes path recursively when the caller intentionally has no error handling path.
void RemovePathBestEffortNoThrow(const std::filesystem::path& path) noexcept;

} // namespace InpxWebReader::Foundation
