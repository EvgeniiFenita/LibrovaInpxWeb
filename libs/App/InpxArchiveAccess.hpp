#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "App/IInpxCatalogApplication.hpp"

namespace InpxWebReader::StoragePlanning {
class CInpxArchiveRootIndex;
}

namespace InpxWebReader::Application {

struct SInpxArchiveFileState
{
    std::uint64_t FileSizeBytes = 0;
    std::int64_t MtimeTicks = 0;
};

struct SInpxArchiveManifest
{
    std::string FingerprintUtf8;
    std::uint64_t PayloadBytesValidated = 0;
};

struct SResolvedInpxArchivePath
{
    std::filesystem::path AbsolutePath;
    std::string RelativePathUtf8;
};

class CInpxArchiveReader final
{
public:
    CInpxArchiveReader(
        const SInpxSourceInfo& source,
        std::string_view archiveNameUtf8);
    explicit CInpxArchiveReader(const std::filesystem::path& resolvedArchivePath);
    ~CInpxArchiveReader();

    CInpxArchiveReader(const CInpxArchiveReader&) = delete;
    CInpxArchiveReader& operator=(const CInpxArchiveReader&) = delete;
    CInpxArchiveReader(CInpxArchiveReader&&) noexcept;
    CInpxArchiveReader& operator=(CInpxArchiveReader&&) noexcept;

    void ExtractEntryToPath(
        std::string_view entryNameUtf8,
        const std::filesystem::path& destinationPath,
        const std::function<void()>& chunkCheckpoint = {}) const;
    [[nodiscard]] std::string ReadEntryBytes(
        std::string_view entryNameUtf8,
        const std::function<void()>& chunkCheckpoint = {}) const;
    [[nodiscard]] std::uint64_t ReadEntrySize(std::string_view entryNameUtf8) const;
    [[nodiscard]] SInpxArchiveFileState GetFileState() const noexcept;
    [[nodiscard]] SInpxArchiveManifest ReadValidatedManifest(
        std::uint64_t maxManifestMemoryBytes,
        const std::function<void()>& checkpoint = {}) const;
    [[nodiscard]] std::string ComputeManifestFingerprint(
        std::uint64_t maxManifestMemoryBytes,
        const std::function<void()>& checkpoint = {}) const;

private:
    class CImpl;
    std::unique_ptr<CImpl> m_impl;
};

[[nodiscard]] std::filesystem::path ResolveInpxArchivePath(
    const SInpxSourceInfo& source,
    std::string_view archiveNameUtf8);

[[nodiscard]] SResolvedInpxArchivePath ResolvePortableInpxArchivePath(
    const SInpxSourceInfo& source,
    std::string_view archiveNameUtf8);

[[nodiscard]] SResolvedInpxArchivePath ResolvePortableInpxArchivePath(
    StoragePlanning::CInpxArchiveRootIndex& archiveRootIndex,
    std::string_view archiveNameUtf8);

[[nodiscard]] std::filesystem::path ResolvePersistedInpxArchivePath(
    const SInpxSourceInfo& source,
    std::string_view relativeArchivePathUtf8);

[[nodiscard]] SInpxArchiveFileState ReadInpxArchiveFileState(
    const SInpxSourceInfo& source,
    std::string_view archiveNameUtf8);

[[nodiscard]] SInpxArchiveFileState ReadInpxArchiveFileState(
    const std::filesystem::path& resolvedArchivePath);

void ExtractInpxArchiveEntryToPath(
    const SInpxSourceInfo& source,
    std::string_view archiveNameUtf8,
    std::string_view entryNameUtf8,
    const std::filesystem::path& destinationPath);

} // namespace InpxWebReader::Application
