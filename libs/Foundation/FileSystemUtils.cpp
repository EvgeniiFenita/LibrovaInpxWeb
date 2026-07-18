#include "Foundation/FileSystemUtils.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#include "Foundation/Logging.hpp"
#include "Foundation/UnicodeConversion.hpp"

namespace InpxWebReader::Foundation {
namespace {

std::atomic<std::uint64_t> GUniqueDirectorySequence = 0;

} // namespace

bool IsSafeRelativePath(const std::filesystem::path& path)
{
    if (path.empty() || path.is_absolute() || path.has_root_name() || path.has_root_directory())
    {
        return false;
    }

    const auto normalized = path.lexically_normal();
    if (normalized.empty() || normalized.is_absolute())
    {
        return false;
    }

    for (const auto& part : normalized)
    {
        if (part == "..")
        {
            return false;
        }
    }

    return true;
}

void EnsureDirectory(const std::filesystem::path& path)
{
    std::error_code errorCode;
    std::filesystem::create_directories(path, errorCode);

    if (errorCode)
    {
        throw std::runtime_error(
            std::string{"Failed to create directory: "} + InpxWebReader::Unicode::PathToUtf8(path)
            + ": " + errorCode.message());
    }
}

std::filesystem::path CreateUniqueDirectory(
    const std::filesystem::path& parentDirectory,
    const std::string_view directoryNamePrefix)
{
    const std::filesystem::path prefixPath{directoryNamePrefix};
    if (parentDirectory.empty()
        || prefixPath.empty()
        || prefixPath.has_parent_path()
        || prefixPath.filename() != prefixPath)
    {
        throw std::invalid_argument("Unique directory creation requires a parent and a filename prefix.");
    }

    EnsureDirectory(parentDirectory);
    for (;;)
    {
        const auto clockValue = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto sequence = GUniqueDirectorySequence.fetch_add(1, std::memory_order_relaxed);
        const auto candidate = parentDirectory
            / (std::string{directoryNamePrefix}
                + std::to_string(clockValue)
                + "-"
                + std::to_string(sequence));

        std::error_code errorCode;
        if (std::filesystem::create_directory(candidate, errorCode))
        {
            return candidate;
        }
        if (!errorCode || errorCode == std::errc::file_exists)
        {
            continue;
        }

        throw std::runtime_error(
            std::string{"Failed to create unique directory: "}
            + InpxWebReader::Unicode::PathToUtf8(candidate)
            + ": "
            + errorCode.message());
    }
}

void MoveFileWithCopyFallback(
    const std::filesystem::path& sourcePath,
    const std::filesystem::path& destinationPath,
    const std::string_view operationName)
{
    std::error_code renameError;
    std::filesystem::rename(sourcePath, destinationPath, renameError);

    if (!renameError)
    {
        return;
    }

    try
    {
        InpxWebReader::Logging::InfoIfInitialized(
            "{}: atomic rename failed ({}), falling back to copy+delete. src='{}' dst='{}'.",
            operationName,
            renameError.message(),
            InpxWebReader::Unicode::PathToUtf8(sourcePath),
            InpxWebReader::Unicode::PathToUtf8(destinationPath));
    }
    catch (...)
    {
        // Intentionally ignored in best-effort cleanup/logging paths.
        (void)0;
    }

    if (!destinationPath.parent_path().empty())
    {
        EnsureDirectory(destinationPath.parent_path());
    }

    std::error_code copyError;
    const bool copied = std::filesystem::copy_file(
        sourcePath,
        destinationPath,
        std::filesystem::copy_options::overwrite_existing,
        copyError);

    if (!copied || copyError)
    {
        throw std::runtime_error(
            std::string{"Failed to move file from "}
            + InpxWebReader::Unicode::PathToUtf8(sourcePath)
            + " to "
            + InpxWebReader::Unicode::PathToUtf8(destinationPath)
            + ": rename failed ("
            + renameError.message()
            + "), copy fallback also failed ("
            + copyError.message()
            + ")");
    }

    std::error_code removeError;
    std::filesystem::remove(sourcePath, removeError);
    if (removeError)
    {
        try
        {
            InpxWebReader::Logging::WarnIfInitialized(
                "{}: copy fallback succeeded but source removal failed. src='{}' err='{}'.",
                operationName,
                InpxWebReader::Unicode::PathToUtf8(sourcePath),
                removeError.message());
        }
        catch (...)
        {
            // Intentionally ignored in best-effort cleanup/logging paths.
            (void)0;
        }
    }
}

std::error_code RemovePathNoThrow(const std::filesystem::path& path) noexcept
{
    if (path.empty())
    {
        return {};
    }

    std::error_code errorCode;
    std::filesystem::remove_all(path, errorCode);
    return errorCode;
}

void RemovePathBestEffortNoThrow(const std::filesystem::path& path) noexcept
{
    const std::error_code errorCode = RemovePathNoThrow(path);
    (void)errorCode;
}

} // namespace InpxWebReader::Foundation
