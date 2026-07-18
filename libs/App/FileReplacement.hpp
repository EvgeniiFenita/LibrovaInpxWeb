#pragma once

#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace InpxWebReader::Application {

struct SFileReplacementHooks
{
    std::function<bool(const std::filesystem::path&)> Exists;
    std::function<void(const std::filesystem::path&, const std::filesystem::path&)> Rename;
    std::function<void(const std::filesystem::path&, const std::filesystem::path&, std::error_code&)> RenameNoThrow;
    std::function<void(const std::filesystem::path&, std::error_code&)> RemoveNoThrow;
};

class CFileReplacementRestoreException final : public std::runtime_error
{
public:
    CFileReplacementRestoreException(
        const std::string& message,
        std::filesystem::path backupPath,
        std::filesystem::path destinationPath);

    [[nodiscard]] const std::filesystem::path& GetBackupPath() const noexcept;
    [[nodiscard]] const std::filesystem::path& GetDestinationPath() const noexcept;

private:
    std::filesystem::path m_backupPath;
    std::filesystem::path m_destinationPath;
};

class CScopedPathCleanup final
{
public:
    explicit CScopedPathCleanup(std::filesystem::path path);
    ~CScopedPathCleanup();

    CScopedPathCleanup(const CScopedPathCleanup&) = delete;
    CScopedPathCleanup& operator=(const CScopedPathCleanup&) = delete;
    CScopedPathCleanup(CScopedPathCleanup&&) noexcept = default;
    CScopedPathCleanup& operator=(CScopedPathCleanup&&) noexcept = default;

    [[nodiscard]] const std::filesystem::path& GetPath() const noexcept;
    void Dismiss() noexcept;

private:
    std::filesystem::path m_path;
};

[[nodiscard]] std::filesystem::path BuildSiblingTemporaryPath(const std::filesystem::path& destinationPath);

// Keeps an existing destination in a backup until validation of the published
// file succeeds. A validation exception rolls the replacement back.
void ReplaceDestinationWithPreparedFile(
    const std::filesystem::path& preparedPath,
    const std::filesystem::path& destinationPath,
    std::string_view operationName,
    const SFileReplacementHooks& hooks = {},
    const std::function<void()>& validatePublishedFile = {});

} // namespace InpxWebReader::Application
