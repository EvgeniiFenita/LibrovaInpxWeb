#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace InpxWebReader::StoragePlanning {

struct SInpxArchiveRootIndexOptions
{
    std::function<void()> Checkpoint;
    std::size_t MaxFilesystemEntries = 1'000'000;
    std::function<void()> OnFallbackSnapshotBuild;
    std::function<void()> OnFilesystemEntryVisited;
};

class CInpxArchiveRootIndex final
{
public:
    CInpxArchiveRootIndex(
        const std::filesystem::path& archiveRoot,
        std::string canonicalizationErrorMessage,
        SInpxArchiveRootIndexOptions options = {});
    ~CInpxArchiveRootIndex();

    CInpxArchiveRootIndex(const CInpxArchiveRootIndex&) = delete;
    CInpxArchiveRootIndex& operator=(const CInpxArchiveRootIndex&) = delete;
    CInpxArchiveRootIndex(CInpxArchiveRootIndex&&) noexcept;
    CInpxArchiveRootIndex& operator=(CInpxArchiveRootIndex&&) noexcept;

    [[nodiscard]] std::optional<std::filesystem::path> TryResolveExistingZipArchivePath(
        std::string_view archiveNameUtf8,
        std::string_view unsafePathMessage);
    [[nodiscard]] const std::filesystem::path& GetCanonicalRoot() const noexcept;

private:
    class CImpl;
    std::unique_ptr<CImpl> m_impl;
};

[[nodiscard]] std::optional<std::filesystem::path> TryResolveExistingZipArchivePath(
    const std::filesystem::path& archiveRoot,
    std::string_view archiveNameUtf8,
    std::string_view unsafePathMessage,
    std::string_view canonicalizationErrorMessage);

} // namespace InpxWebReader::StoragePlanning
