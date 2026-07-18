#include "App/FileReplacement.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <utility>

#include "Foundation/Logging.hpp"
#include "Foundation/UnicodeConversion.hpp"

namespace InpxWebReader::Application {
namespace {

std::atomic<std::uint64_t> GTemporaryPathSequence = 0;

void LogBackupRestoreFailure(
    const std::string_view operationName,
    const std::filesystem::path& backupPath,
    const std::filesystem::path& destinationPath,
    const std::error_code& restoreError) noexcept
{
    if (!restoreError)
    {
        return;
    }

    try
    {
        InpxWebReader::Logging::ErrorIfInitialized(
            "{}: failed to restore replaced file backup. BackupPath='{}' DestinationPath='{}' Error='{}'.",
            operationName,
            InpxWebReader::Unicode::PathToUtf8(backupPath),
            InpxWebReader::Unicode::PathToUtf8(destinationPath),
            restoreError.message());
    }
    catch (...)
    {
        // Intentionally ignored in best-effort cleanup/logging paths.
        (void)0;
    }
}

void LogBackupCleanupFailure(
    const std::string_view operationName,
    const std::filesystem::path& backupPath,
    const std::error_code& removeError) noexcept
{
    if (!removeError)
    {
        return;
    }

    try
    {
        InpxWebReader::Logging::WarnIfInitialized(
            "{}: failed to remove replacement backup after a successful replace. BackupPath='{}' Error='{}'.",
            operationName,
            InpxWebReader::Unicode::PathToUtf8(backupPath),
            removeError.message());
    }
    catch (...)
    {
        // Intentionally ignored in best-effort cleanup/logging paths.
        (void)0;
    }
}

void LogPublishedFileRemovalFailure(
    const std::string_view operationName,
    const std::filesystem::path& backupPath,
    const std::filesystem::path& destinationPath,
    const std::error_code& removeError) noexcept
{
    if (!removeError)
    {
        return;
    }

    try
    {
        InpxWebReader::Logging::ErrorIfInitialized(
            "{}: failed to remove a rejected replacement before restoring its backup. "
            "BackupPath='{}' DestinationPath='{}' Error='{}'.",
            operationName,
            InpxWebReader::Unicode::PathToUtf8(backupPath),
            InpxWebReader::Unicode::PathToUtf8(destinationPath),
            removeError.message());
    }
    catch (...)
    {
        // Intentionally ignored in best-effort cleanup/logging paths.
        (void)0;
    }
}

[[nodiscard]] bool PathExists(
    const std::filesystem::path& path,
    const SFileReplacementHooks& hooks)
{
    if (hooks.Exists)
    {
        return hooks.Exists(path);
    }

    return std::filesystem::exists(path);
}

void RenamePath(
    const std::filesystem::path& sourcePath,
    const std::filesystem::path& destinationPath,
    const SFileReplacementHooks& hooks)
{
    if (hooks.Rename)
    {
        hooks.Rename(sourcePath, destinationPath);
        return;
    }

    std::filesystem::rename(sourcePath, destinationPath);
}

void RenamePathNoThrow(
    const std::filesystem::path& sourcePath,
    const std::filesystem::path& destinationPath,
    std::error_code& errorCode,
    const SFileReplacementHooks& hooks) noexcept
{
    if (hooks.RenameNoThrow)
    {
        hooks.RenameNoThrow(sourcePath, destinationPath, errorCode);
        return;
    }

    std::filesystem::rename(sourcePath, destinationPath, errorCode);
}

void RemovePathNoThrow(
    const std::filesystem::path& path,
    std::error_code& errorCode,
    const SFileReplacementHooks& hooks) noexcept
{
    if (hooks.RemoveNoThrow)
    {
        hooks.RemoveNoThrow(path, errorCode);
        return;
    }

    std::filesystem::remove(path, errorCode);
}

[[noreturn]] void ThrowRollbackFailure(
    const std::string_view operationName,
    const std::filesystem::path& backupPath,
    const std::filesystem::path& destinationPath,
    const std::string_view replaceError,
    const std::string_view rollbackAction,
    const std::error_code& rollbackError)
{
    const auto backupDetail = backupPath.empty()
        ? std::string{"No previous destination backup exists."}
        : std::string{"Original content is preserved at BackupPath='"}
            + InpxWebReader::Unicode::PathToUtf8(backupPath)
            + "' and can be recovered manually.";
    throw CFileReplacementRestoreException(
        std::string(operationName)
            + ": failed to roll back a destination replacement while "
            + std::string(rollbackAction)
            + ". "
            + backupDetail
            + " DestinationPath='"
            + InpxWebReader::Unicode::PathToUtf8(destinationPath)
            + "' ReplaceError='"
            + std::string(replaceError)
            + "' RollbackError='"
            + rollbackError.message()
            + "'.",
        backupPath,
        destinationPath);
}

void RestoreBackupOrThrow(
    const std::string_view operationName,
    const std::filesystem::path& backupPath,
    const std::filesystem::path& destinationPath,
    const std::string_view replaceError,
    const SFileReplacementHooks& hooks)
{
    if (!PathExists(backupPath, hooks) || PathExists(destinationPath, hooks))
    {
        return;
    }

    std::error_code restoreError;
    RenamePathNoThrow(backupPath, destinationPath, restoreError, hooks);
    LogBackupRestoreFailure(operationName, backupPath, destinationPath, restoreError);
    if (restoreError)
    {
        ThrowRollbackFailure(
            operationName,
            backupPath,
            destinationPath,
            replaceError,
            "restoring the previous destination",
            restoreError);
    }
}

void RollbackPublishedFileOrThrow(
    const std::string_view operationName,
    const std::filesystem::path& backupPath,
    const std::filesystem::path& destinationPath,
    const bool destinationWasBackedUp,
    const bool preparedFileWasPublished,
    const std::string_view replaceError,
    const SFileReplacementHooks& hooks)
{
    if (preparedFileWasPublished)
    {
        std::error_code removeError;
        RemovePathNoThrow(destinationPath, removeError, hooks);
        if (removeError)
        {
            LogPublishedFileRemovalFailure(operationName, backupPath, destinationPath, removeError);
            ThrowRollbackFailure(
                operationName,
                backupPath,
                destinationPath,
                replaceError,
                "removing the rejected replacement",
                removeError);
        }
    }

    if (destinationWasBackedUp)
    {
        RestoreBackupOrThrow(
            operationName,
            backupPath,
            destinationPath,
            replaceError,
            hooks);
    }
}

} // namespace

CFileReplacementRestoreException::CFileReplacementRestoreException(
    const std::string& message,
    std::filesystem::path backupPath,
    std::filesystem::path destinationPath)
    : std::runtime_error(message)
    , m_backupPath(std::move(backupPath))
    , m_destinationPath(std::move(destinationPath))
{
}

const std::filesystem::path& CFileReplacementRestoreException::GetBackupPath() const noexcept
{
    return m_backupPath;
}

const std::filesystem::path& CFileReplacementRestoreException::GetDestinationPath() const noexcept
{
    return m_destinationPath;
}

std::filesystem::path BuildSiblingTemporaryPath(const std::filesystem::path& destinationPath)
{
    const auto clockValue = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto sequence = GTemporaryPathSequence.fetch_add(1, std::memory_order_relaxed);
    auto temporaryFileName = destinationPath.filename();
    const auto extension = temporaryFileName.extension();
    if (!extension.empty())
    {
        temporaryFileName.replace_extension();
    }

    temporaryFileName += ".inpx-web-reader-tmp-";
    temporaryFileName += std::to_string(clockValue);
    temporaryFileName += "-";
    temporaryFileName += std::to_string(sequence);
    temporaryFileName += extension;
    return destinationPath.parent_path() / temporaryFileName;
}

CScopedPathCleanup::CScopedPathCleanup(std::filesystem::path path)
    : m_path(std::move(path))
{
}

CScopedPathCleanup::~CScopedPathCleanup()
{
    if (m_path.empty())
    {
        return;
    }

    std::error_code errorCode;
    std::filesystem::remove(m_path, errorCode);
}

const std::filesystem::path& CScopedPathCleanup::GetPath() const noexcept
{
    return m_path;
}

void CScopedPathCleanup::Dismiss() noexcept
{
    m_path.clear();
}

void ReplaceDestinationWithPreparedFile(
    const std::filesystem::path& preparedPath,
    const std::filesystem::path& destinationPath,
    const std::string_view operationName,
    const SFileReplacementHooks& hooks,
    const std::function<void()>& validatePublishedFile)
{
    if (preparedPath == destinationPath)
    {
        if (validatePublishedFile)
        {
            validatePublishedFile();
        }
        return;
    }

    const bool destinationExists = PathExists(destinationPath, hooks);
    auto backupPath = std::filesystem::path{};
    if (destinationExists)
    {
        backupPath = BuildSiblingTemporaryPath(destinationPath);
        backupPath += ".inpx-web-reader-backup";
    }
    bool destinationWasBackedUp = false;
    bool preparedFileWasPublished = false;

    try
    {
        if (destinationExists)
        {
            RenamePath(destinationPath, backupPath, hooks);
            destinationWasBackedUp = true;
        }

        RenamePath(preparedPath, destinationPath, hooks);
        preparedFileWasPublished = true;

        if (validatePublishedFile)
        {
            validatePublishedFile();
        }

        if (destinationWasBackedUp)
        {
            std::error_code removeError;
            RemovePathNoThrow(backupPath, removeError, hooks);
            LogBackupCleanupFailure(operationName, backupPath, removeError);
        }
    }
    catch (const std::exception& replaceError)
    {
        RollbackPublishedFileOrThrow(
            operationName,
            backupPath,
            destinationPath,
            destinationWasBackedUp,
            preparedFileWasPublished,
            replaceError.what(),
            hooks);

        throw;
    }
    catch (...)
    {
        RollbackPublishedFileOrThrow(
            operationName,
            backupPath,
            destinationPath,
            destinationWasBackedUp,
            preparedFileWasPublished,
            "non-standard exception",
            hooks);

        throw;
    }
}

} // namespace InpxWebReader::Application
