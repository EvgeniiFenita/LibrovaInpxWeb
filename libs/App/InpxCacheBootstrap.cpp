#include "App/InpxCacheBootstrap.hpp"

#include <stdexcept>
#include <system_error>

#include "Foundation/FileSystemUtils.hpp"
#include "Storage/InpxCacheLayout.hpp"

namespace InpxWebReader::Application {
namespace {

[[nodiscard]] bool IsManagedDirectory(const std::filesystem::path& path)
{
    std::error_code errorCode;
    const auto status = std::filesystem::symlink_status(path, errorCode);
    return !errorCode && std::filesystem::is_directory(status) && !std::filesystem::is_symlink(status);
}

[[nodiscard]] bool IsManagedRegularFile(const std::filesystem::path& path)
{
    std::error_code errorCode;
    const auto status = std::filesystem::symlink_status(path, errorCode);
    return !errorCode && std::filesystem::is_regular_file(status) && !std::filesystem::is_symlink(status);
}

void ValidateManagedDirectories(const std::filesystem::path& cacheRoot)
{
    const auto layout = InpxWebReader::StoragePlanning::CInpxCacheLayout::Build(cacheRoot);
    if (!IsManagedDirectory(layout.Root)
        || !IsManagedDirectory(layout.DatabaseDirectory)
        || !IsManagedDirectory(layout.CoversDirectory))
    {
        throw std::runtime_error("INPX cache root is incomplete or missing.");
    }
}

void PrepareExistingCache(const std::filesystem::path& cacheRoot)
{
    ValidateManagedDirectories(cacheRoot);
    if (!IsManagedRegularFile(
            InpxWebReader::StoragePlanning::CInpxCacheLayout::GetDatabasePath(cacheRoot)))
    {
        throw std::runtime_error("INPX cache root is incomplete or missing.");
    }
}

void PrepareNewCache(const std::filesystem::path& cacheRoot)
{
    std::error_code statusError;
    auto rootStatus = std::filesystem::symlink_status(cacheRoot, statusError);
    if (statusError == std::errc::no_such_file_or_directory)
    {
        statusError.clear();
        rootStatus = std::filesystem::file_status{std::filesystem::file_type::not_found};
    }
    if (statusError)
    {
        throw std::runtime_error("New INPX cache root could not be inspected.");
    }
    if (std::filesystem::is_symlink(rootStatus))
    {
        throw std::runtime_error("New INPX cache root must not be a symbolic link.");
    }
    if (std::filesystem::exists(rootStatus))
    {
        std::error_code errorCode;
        const bool isEmpty = std::filesystem::is_empty(cacheRoot, errorCode);
        if (errorCode || !isEmpty)
        {
            throw std::runtime_error("New INPX cache root must be empty.");
        }
    }

    const auto layout = InpxWebReader::StoragePlanning::CInpxCacheLayout::Build(cacheRoot);
    std::filesystem::create_directories(layout.DatabaseDirectory);
    std::filesystem::create_directories(layout.CoversDirectory);
    ValidateManagedDirectories(cacheRoot);
}

} // namespace

void CInpxCacheBootstrap::PrepareCacheRoot(
    const std::filesystem::path& cacheRoot,
    const ECacheOpenMode cacheOpenMode)
{
    if (cacheOpenMode == ECacheOpenMode::Open)
    {
        PrepareExistingCache(cacheRoot);
        return;
    }
    PrepareNewCache(cacheRoot);
}

void CInpxCacheBootstrap::ValidateExistingCacheRoot(const std::filesystem::path& cacheRoot)
{
    PrepareExistingCache(cacheRoot);
}

} // namespace InpxWebReader::Application
