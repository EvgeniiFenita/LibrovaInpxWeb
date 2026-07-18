#include "App/InpxArchiveAccess.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <zip.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "App/FileReplacement.hpp"
#include "Foundation/BookPayloadLimits.hpp"
#include "Foundation/FileSystemUtils.hpp"
#include "Foundation/Sha256Fingerprint.hpp"
#include "Foundation/UnicodeConversion.hpp"
#include "Foundation/ZipArchivePaths.hpp"
#include "Storage/InpxArchivePathResolver.hpp"
#include "Storage/PathSafety.hpp"

namespace InpxWebReader::Application {
namespace {

constexpr std::size_t GZipStreamChunkBytes = 64ull * 1024ull;
constexpr std::size_t GManifestSortCheckpointInterval = 1'024;
constexpr std::uint64_t GApproximateManifestEntryBytes = 256;

[[noreturn]] void ThrowManifestMemoryBudgetExceeded(
    const std::uint64_t maxManifestMemoryBytes)
{
    throw std::runtime_error(
        "INPX archive manifest metadata exceeds the configured memory budget of "
        + std::to_string(maxManifestMemoryBytes) + " bytes.");
}

class CZipFileHandle final
{
public:
    explicit CZipFileHandle(zip_file_t* const file)
        : m_file(file)
    {
        if (m_file == nullptr)
        {
            throw std::runtime_error("Failed to open INPX archive entry.");
        }
    }

    ~CZipFileHandle()
    {
        if (m_file != nullptr)
        {
            zip_fclose(m_file);
        }
    }

    CZipFileHandle(const CZipFileHandle&) = delete;
    CZipFileHandle& operator=(const CZipFileHandle&) = delete;
    CZipFileHandle(CZipFileHandle&& other) noexcept
        : m_file(std::exchange(other.m_file, nullptr))
    {
    }

    CZipFileHandle& operator=(CZipFileHandle&& other) noexcept
    {
        if (this != &other)
        {
            if (m_file != nullptr)
            {
                zip_fclose(m_file);
            }
            m_file = std::exchange(other.m_file, nullptr);
        }

        return *this;
    }

    [[nodiscard]] zip_file_t* Get() const noexcept
    {
        return m_file;
    }

    void Close()
    {
        if (m_file == nullptr)
        {
            return;
        }

        zip_file_t* const file = std::exchange(m_file, nullptr);
        if (zip_fclose(file) != 0)
        {
            throw std::runtime_error("Failed to close INPX archive entry after reading.");
        }
    }

private:
    zip_file_t* m_file = nullptr;
};

class CScopedFileDescriptor final
{
public:
    explicit CScopedFileDescriptor(const int descriptor = -1) noexcept
        : m_descriptor(descriptor)
    {
    }

    ~CScopedFileDescriptor() noexcept
    {
        if (m_descriptor >= 0)
        {
            close(m_descriptor);
        }
    }

    CScopedFileDescriptor(const CScopedFileDescriptor&) = delete;
    CScopedFileDescriptor& operator=(const CScopedFileDescriptor&) = delete;

    [[nodiscard]] int Get() const noexcept
    {
        return m_descriptor;
    }

    [[nodiscard]] int Release() noexcept
    {
        return std::exchange(m_descriptor, -1);
    }

private:
    int m_descriptor = -1;
};

[[nodiscard]] SInpxArchiveFileState ReadArchiveFileStateFromDescriptor(
    const int descriptor)
{
    struct stat status = {};
    if (fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode) || status.st_size < 0)
    {
        throw std::runtime_error("INPX archive descriptor is not a regular file.");
    }

    std::error_code modifiedTimeError;
    const auto descriptorPath = std::filesystem::path{"/proc/self/fd"}
        / std::to_string(descriptor);
    const auto modifiedTime = std::filesystem::last_write_time(
        descriptorPath,
        modifiedTimeError);
    if (modifiedTimeError)
    {
        throw std::runtime_error("Failed to read INPX archive descriptor modification time.");
    }

    return {
        .FileSizeBytes = static_cast<std::uint64_t>(status.st_size),
        .MtimeTicks = static_cast<std::int64_t>(modifiedTime.time_since_epoch().count())
    };
}

void ValidateEntryEndOfStream(CZipFileHandle& file)
{
    std::byte trailingByte{};
    const auto readCount = zip_fread(file.Get(), &trailingByte, 1);
    if (readCount != 0)
    {
        throw std::runtime_error("Failed to validate INPX archive entry payload checksum and size.");
    }
}

class CZipArchive final
{
public:
    explicit CZipArchive(const std::filesystem::path& archivePath)
    {
        const auto archiveUtf8 = Unicode::PathToUtf8(archivePath);
        CScopedFileDescriptor descriptor{
            open(archiveUtf8.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW)};
        if (descriptor.Get() < 0)
        {
            throw std::runtime_error("Failed to open INPX archive safely.");
        }
        m_fileState = ReadArchiveFileStateFromDescriptor(descriptor.Get());

        int errorCode = ZIP_ER_OK;
        // zip_fdopen is inherently read-only and takes ownership only on success.
        m_archive = zip_fdopen(descriptor.Get(), 0, &errorCode);
        if (m_archive == nullptr)
        {
            zip_error_t errorState;
            zip_error_init_with_code(&errorState, errorCode);
            const std::string message = zip_error_strerror(&errorState);
            zip_error_fini(&errorState);
            throw std::runtime_error("Failed to open INPX archive: " + message);
        }
        static_cast<void>(descriptor.Release());
    }

    ~CZipArchive()
    {
        if (m_archive != nullptr)
        {
            zip_close(m_archive);
        }
    }

    void Extract(
        std::string_view entryNameUtf8,
        const std::filesystem::path& destinationPath,
        const std::function<void()>& chunkCheckpoint) const
    {
        const SLocatedEntry entry = LocateEntry(entryNameUtf8);
        CZipFileHandle file = OpenEntry(entry);
        const auto preparedPath = BuildSiblingTemporaryPath(destinationPath);
        CScopedPathCleanup preparedCleanup(preparedPath);

        if (!preparedPath.parent_path().empty())
        {
            std::filesystem::create_directories(preparedPath.parent_path());
        }

        std::ofstream output(preparedPath, std::ios::binary | std::ios::trunc);
        if (!output)
        {
            throw std::runtime_error("Failed to create extracted INPX payload file.");
        }

        std::vector<std::byte> buffer(GZipStreamChunkBytes);
        zip_uint64_t remainingBytes = entry.Size;

        while (remainingBytes > 0)
        {
            if (chunkCheckpoint)
            {
                chunkCheckpoint();
            }
            const auto bytesToRead = static_cast<zip_uint64_t>((std::min<std::size_t>)(
                buffer.size(),
                static_cast<std::size_t>(remainingBytes)));
            const auto readCount = zip_fread(file.Get(), buffer.data(), bytesToRead);
            if (readCount <= 0 || static_cast<zip_uint64_t>(readCount) > bytesToRead)
            {
                output.close();
                throw std::runtime_error("Failed to read INPX archive entry payload.");
            }

            output.write(
                reinterpret_cast<const char*>(buffer.data()),
                static_cast<std::streamsize>(readCount));
            if (!output)
            {
                output.close();
                throw std::runtime_error("Failed to write extracted INPX payload file.");
            }

            remainingBytes -= static_cast<zip_uint64_t>(readCount);
        }

        ValidateEntryEndOfStream(file);
        file.Close();
        output.close();
        if (!output)
        {
            throw std::runtime_error("Failed to finalize extracted INPX payload file.");
        }

        ReplaceDestinationWithPreparedFile(
            preparedPath,
            destinationPath,
            "ExtractInpxArchiveEntry");
        preparedCleanup.Dismiss();
    }

    [[nodiscard]] std::string ReadBytes(
        std::string_view entryNameUtf8,
        const std::function<void()>& chunkCheckpoint) const
    {
        if (chunkCheckpoint)
        {
            chunkCheckpoint();
        }
        const SLocatedEntry entry = LocateEntry(entryNameUtf8);
        CZipFileHandle file = OpenEntry(entry);

        std::string payload;
        payload.resize(static_cast<std::size_t>(entry.Size));
        std::size_t offset = 0;

        while (offset < payload.size())
        {
            if (chunkCheckpoint)
            {
                chunkCheckpoint();
            }
            const auto bytesToRead = static_cast<zip_uint64_t>((std::min<std::size_t>)(
                GZipStreamChunkBytes,
                payload.size() - offset));
            const auto readCount = zip_fread(file.Get(), payload.data() + offset, bytesToRead);
            if (readCount <= 0 || static_cast<zip_uint64_t>(readCount) > bytesToRead)
            {
                throw std::runtime_error("Failed to read INPX archive entry payload.");
            }

            offset += static_cast<std::size_t>(readCount);
        }

        if (chunkCheckpoint)
        {
            chunkCheckpoint();
        }

        ValidateEntryEndOfStream(file);
        file.Close();

        return payload;
    }

    [[nodiscard]] std::uint64_t ReadSize(const std::string_view entryNameUtf8) const
    {
        return LocateEntry(entryNameUtf8).Size;
    }

    [[nodiscard]] SInpxArchiveFileState GetFileState() const noexcept
    {
        return m_fileState;
    }

    [[nodiscard]] std::string ComputeManifestFingerprint(
        const std::uint64_t maxManifestMemoryBytes,
        const std::function<void()>& checkpoint) const
    {
        return ReadManifest(false, maxManifestMemoryBytes, checkpoint).FingerprintUtf8;
    }

    [[nodiscard]] SInpxArchiveManifest ReadValidatedManifest(
        const std::uint64_t maxManifestMemoryBytes,
        const std::function<void()>& checkpoint) const
    {
        return ReadManifest(true, maxManifestMemoryBytes, checkpoint);
    }

    [[nodiscard]] SInpxArchiveManifest ReadManifest(
        const bool validatePayloads,
        const std::uint64_t maxManifestMemoryBytes,
        const std::function<void()>& checkpoint) const
    {
        struct SManifestEntry
        {
            std::size_t NameOffset = 0;
            std::size_t NameLength = 0;
            std::uint64_t Size = 0;
            std::uint32_t Crc = 0;
        };

        const auto runCheckpoint = [&]() {
            if (checkpoint)
            {
                checkpoint();
            }
        };
        runCheckpoint();

        // libzip materializes its central directory in zip_open(). This is the first
        // libzip API boundary where the entry count is available for admission.
        const zip_int64_t entryCount = zip_get_num_entries(m_archive, 0);
        if (entryCount < 0)
        {
            throw std::runtime_error("Failed to read INPX archive entry count.");
        }
        const auto unsignedEntryCount = static_cast<std::uint64_t>(entryCount);

        // This is an intentionally coarse input-volume guard, not allocator
        // accounting. Half of the budget allows one compact manifest entry per
        // conservative 256 bytes; the other half bounds aggregate entry-name data.
        const std::uint64_t entryBudgetBytes = maxManifestMemoryBytes / 2;
        const std::uint64_t nameBudgetBytes = maxManifestMemoryBytes / 2;
        const std::uint64_t maxManifestEntries = entryBudgetBytes / GApproximateManifestEntryBytes;
        if (unsignedEntryCount > maxManifestEntries
            || unsignedEntryCount > std::numeric_limits<std::size_t>::max())
        {
            ThrowManifestMemoryBudgetExceeded(maxManifestMemoryBytes);
        }
        const auto manifestEntryCount = static_cast<std::size_t>(unsignedEntryCount);

        const auto readManifestStat = [&](const zip_uint64_t index) {
            zip_stat_t stat = {};
            zip_stat_init(&stat);
            if (zip_stat_index(m_archive, index, ZIP_FL_ENC_GUESS, &stat) != 0
                || (stat.valid & ZIP_STAT_NAME) == 0
                || (stat.valid & ZIP_STAT_SIZE) == 0
                || (stat.valid & ZIP_STAT_CRC) == 0
                || stat.name == nullptr)
            {
                throw std::runtime_error("Failed to stat INPX archive manifest entry.");
            }
            return stat;
        };

        std::uint64_t requestedNameBytes = 0;
        for (zip_uint64_t index = 0; index < unsignedEntryCount; ++index)
        {
            runCheckpoint();
            const zip_stat_t stat = readManifestStat(index);
            const auto nameSize = static_cast<std::uint64_t>(std::strlen(stat.name));
            if (nameSize > nameBudgetBytes - requestedNameBytes)
            {
                ThrowManifestMemoryBudgetExceeded(maxManifestMemoryBytes);
            }
            requestedNameBytes += nameSize;
        }
        runCheckpoint();
        if (requestedNameBytes > std::numeric_limits<std::size_t>::max())
        {
            ThrowManifestMemoryBudgetExceeded(maxManifestMemoryBytes);
        }

        std::vector<SManifestEntry> entries;
        entries.reserve(manifestEntryCount);
        std::vector<char> ownedNames;
        ownedNames.reserve(static_cast<std::size_t>(requestedNameBytes));
        std::vector<std::byte> validationBuffer;
        if (validatePayloads)
        {
            validationBuffer.resize(GZipStreamChunkBytes);
        }

        std::uint64_t payloadBytesValidated = 0;
        for (zip_uint64_t index = 0; index < unsignedEntryCount; ++index)
        {
            runCheckpoint();
            const zip_stat_t stat = readManifestStat(index);

            if (validatePayloads)
            {
                if (!Foundation::IsBookPayloadSizeAllowed(stat.size))
                {
                    throw std::runtime_error(
                        "INPX archive entry payload too large: " + std::to_string(stat.size)
                        + " bytes exceeds " + std::to_string(Foundation::GMaxBookPayloadBytes)
                        + " byte limit.");
                }
                if (stat.size > std::numeric_limits<std::uint64_t>::max() - payloadBytesValidated)
                {
                    throw std::runtime_error("INPX archive manifest payload size overflow.");
                }

                CZipFileHandle file = OpenEntry({.Index = index, .Size = stat.size});
                zip_uint64_t remainingBytes = stat.size;
                while (remainingBytes > 0)
                {
                    runCheckpoint();
                    const auto bytesToRead = static_cast<zip_uint64_t>((std::min<std::size_t>)(
                        validationBuffer.size(),
                        static_cast<std::size_t>(remainingBytes)));
                    const auto readCount = zip_fread(
                        file.Get(),
                        validationBuffer.data(),
                        bytesToRead);
                    if (readCount <= 0 || static_cast<zip_uint64_t>(readCount) > bytesToRead)
                    {
                        throw std::runtime_error("Failed to validate INPX archive entry payload.");
                    }
                    remainingBytes -= static_cast<zip_uint64_t>(readCount);
                }
                ValidateEntryEndOfStream(file);
                file.Close();
                payloadBytesValidated += stat.size;
            }

            const std::size_t nameLength = std::strlen(stat.name);
            const std::size_t nameOffset = ownedNames.size();
            ownedNames.insert(ownedNames.end(), stat.name, stat.name + nameLength);
            entries.push_back({
                .NameOffset = nameOffset,
                .NameLength = nameLength,
                .Size = stat.size,
                .Crc = stat.crc
            });
        }

        const auto readOwnedName = [&](const SManifestEntry& entry) {
            return entry.NameLength == 0
                ? std::string_view{}
                : std::string_view{ownedNames.data() + entry.NameOffset, entry.NameLength};
        };
        const auto compareNames = [&](const SManifestEntry& left, const SManifestEntry& right) {
            return readOwnedName(left) < readOwnedName(right);
        };
        std::size_t sortComparisonCount = 0;
        runCheckpoint();
        std::ranges::sort(entries, [&](const SManifestEntry& left, const SManifestEntry& right) {
            if (sortComparisonCount % GManifestSortCheckpointInterval == 0)
            {
                runCheckpoint();
            }
            ++sortComparisonCount;
            return compareNames(left, right);
        });
        runCheckpoint();
        for (std::size_t index = 1; index < entries.size(); ++index)
        {
            runCheckpoint();
            if (readOwnedName(entries[index - 1]) == readOwnedName(entries[index]))
            {
                throw std::runtime_error("INPX archive contains duplicate entry names.");
            }
        }

        Foundation::CSha256FingerprintBuilder fingerprintBuilder;
        const auto appendNumber = [&](const auto value) {
            std::array<char, 32> buffer{};
            const auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
            if (error != std::errc{})
            {
                throw std::runtime_error("Failed to serialize INPX archive manifest metadata.");
            }
            fingerprintBuilder.Update(std::string_view{
                buffer.data(),
                static_cast<std::size_t>(end - buffer.data())});
        };
        runCheckpoint();
        for (const auto& entry : entries)
        {
            runCheckpoint();
            appendNumber(entry.NameLength);
            fingerprintBuilder.Update(":");
            fingerprintBuilder.Update(readOwnedName(entry));
            fingerprintBuilder.Update(":");
            appendNumber(entry.Size);
            fingerprintBuilder.Update(":");
            appendNumber(entry.Crc);
            fingerprintBuilder.Update("\n");
        }
        runCheckpoint();

        return {
            .FingerprintUtf8 = fingerprintBuilder.Finalize(),
            .PayloadBytesValidated = payloadBytesValidated
        };
    }

private:
    struct SLocatedEntry
    {
        zip_uint64_t Index = 0;
        zip_uint64_t Size = 0;
    };

    [[nodiscard]] SLocatedEntry LocateEntry(std::string_view entryNameUtf8) const
    {
        const auto index = zip_name_locate(
            m_archive,
            std::string(entryNameUtf8).c_str(),
            ZIP_FL_ENC_GUESS);
        if (index < 0)
        {
            throw std::runtime_error("INPX archive entry is missing: " + std::string(entryNameUtf8));
        }

        zip_stat_t stat = {};
        if (zip_stat_index(m_archive, static_cast<zip_uint64_t>(index), 0, &stat) != 0)
        {
            throw std::runtime_error("Failed to stat INPX archive entry.");
        }

        if (!Foundation::IsBookPayloadSizeAllowed(stat.size))
        {
            throw std::runtime_error(
                "INPX archive entry payload too large: " + std::to_string(stat.size)
                + " bytes exceeds " + std::to_string(Foundation::GMaxBookPayloadBytes) + " byte limit.");
        }

        return SLocatedEntry{
            .Index = static_cast<zip_uint64_t>(index),
            .Size = stat.size
        };
    }

    [[nodiscard]] CZipFileHandle OpenEntry(const SLocatedEntry& entry) const
    {
        return CZipFileHandle{zip_fopen_index(m_archive, entry.Index, 0)};
    }

    zip_t* m_archive = nullptr;
    SInpxArchiveFileState m_fileState;
};

[[nodiscard]] std::filesystem::path BuildArchivePathForOpen(
    StoragePlanning::CInpxArchiveRootIndex& archiveRootIndex,
    const std::string_view archiveNameUtf8)
{
    if (const auto resolved = archiveRootIndex.TryResolveExistingZipArchivePath(
            archiveNameUtf8,
            "INPX archive path is unsafe."))
    {
        return *resolved;
    }

    const auto relativePath = Unicode::PathFromUtf8(std::string{archiveNameUtf8}).lexically_normal();
    if (!Foundation::IsSafeRelativePath(relativePath))
    {
        throw std::runtime_error("INPX archive path is unsafe.");
    }
    return (archiveRootIndex.GetCanonicalRoot()
        / Foundation::BuildZipArchiveFileRelativePath(relativePath)).lexically_normal();
}

} // namespace

class CInpxArchiveReader::CImpl final
{
public:
    explicit CImpl(const std::filesystem::path& archivePath);

    CZipArchive Archive;
};

CInpxArchiveReader::CImpl::CImpl(const std::filesystem::path& archivePath)
    : Archive(archivePath)
{
}

CInpxArchiveReader::CInpxArchiveReader(
    const SInpxSourceInfo& source,
    const std::string_view archiveNameUtf8)
    : m_impl(std::make_unique<CImpl>(ResolveInpxArchivePath(source, archiveNameUtf8)))
{
}

CInpxArchiveReader::CInpxArchiveReader(const std::filesystem::path& resolvedArchivePath)
    : m_impl(std::make_unique<CImpl>(resolvedArchivePath))
{
}

CInpxArchiveReader::~CInpxArchiveReader() = default;

CInpxArchiveReader::CInpxArchiveReader(CInpxArchiveReader&&) noexcept = default;

CInpxArchiveReader& CInpxArchiveReader::operator=(CInpxArchiveReader&&) noexcept = default;

void CInpxArchiveReader::ExtractEntryToPath(
    const std::string_view entryNameUtf8,
    const std::filesystem::path& destinationPath,
    const std::function<void()>& chunkCheckpoint) const
{
    m_impl->Archive.Extract(entryNameUtf8, destinationPath, chunkCheckpoint);
}

std::string CInpxArchiveReader::ReadEntryBytes(
    const std::string_view entryNameUtf8,
    const std::function<void()>& chunkCheckpoint) const
{
    return m_impl->Archive.ReadBytes(entryNameUtf8, chunkCheckpoint);
}

std::uint64_t CInpxArchiveReader::ReadEntrySize(const std::string_view entryNameUtf8) const
{
    return m_impl->Archive.ReadSize(entryNameUtf8);
}

SInpxArchiveFileState CInpxArchiveReader::GetFileState() const noexcept
{
    return m_impl->Archive.GetFileState();
}

SInpxArchiveManifest CInpxArchiveReader::ReadValidatedManifest(
    const std::uint64_t maxManifestMemoryBytes,
    const std::function<void()>& checkpoint) const
{
    return m_impl->Archive.ReadValidatedManifest(maxManifestMemoryBytes, checkpoint);
}

std::string CInpxArchiveReader::ComputeManifestFingerprint(
    const std::uint64_t maxManifestMemoryBytes,
    const std::function<void()>& checkpoint) const
{
    return m_impl->Archive.ComputeManifestFingerprint(maxManifestMemoryBytes, checkpoint);
}

std::filesystem::path ResolveInpxArchivePath(
    const SInpxSourceInfo& source,
    const std::string_view archiveNameUtf8)
{
    StoragePlanning::CInpxArchiveRootIndex archiveRootIndex(
        source.ArchiveRoot,
        "INPX archive root could not be canonicalized.");
    return BuildArchivePathForOpen(archiveRootIndex, archiveNameUtf8);
}

SResolvedInpxArchivePath ResolvePortableInpxArchivePath(
    const SInpxSourceInfo& source,
    const std::string_view archiveNameUtf8)
{
    StoragePlanning::CInpxArchiveRootIndex archiveRootIndex(
        source.ArchiveRoot,
        "INPX archive root could not be canonicalized.");
    return ResolvePortableInpxArchivePath(archiveRootIndex, archiveNameUtf8);
}

SResolvedInpxArchivePath ResolvePortableInpxArchivePath(
    StoragePlanning::CInpxArchiveRootIndex& archiveRootIndex,
    const std::string_view archiveNameUtf8)
{
    const auto absolutePath = BuildArchivePathForOpen(archiveRootIndex, archiveNameUtf8);
    const auto relativePath = absolutePath.lexically_relative(archiveRootIndex.GetCanonicalRoot()).lexically_normal();
    if (!Foundation::IsSafeRelativePath(relativePath))
    {
        throw std::runtime_error("Resolved INPX archive path is outside the archive root.");
    }
    return {
        .AbsolutePath = absolutePath,
        .RelativePathUtf8 = Unicode::PathToUtf8(relativePath)
    };
}

std::filesystem::path ResolvePersistedInpxArchivePath(
    const SInpxSourceInfo& source,
    const std::string_view relativeArchivePathUtf8)
{
    const auto relativePath = Unicode::PathFromUtf8(std::string{relativeArchivePathUtf8}).lexically_normal();
    if (!Foundation::IsSafeRelativePath(relativePath))
    {
        throw std::runtime_error("Persisted INPX archive path is unsafe.");
    }

    const auto resolved = SafePaths::TryResolvePathWithinRoot(
        source.ArchiveRoot,
        relativePath,
        "Persisted INPX archive path is unsafe.",
        "INPX archive root could not be canonicalized.");
    if (!resolved.has_value())
    {
        throw std::runtime_error("Persisted INPX archive is missing.");
    }

    std::error_code errorCode;
    if (!std::filesystem::is_regular_file(*resolved, errorCode) || errorCode)
    {
        throw std::runtime_error("Persisted INPX archive path is not a regular file.");
    }
    return *resolved;
}

SInpxArchiveFileState ReadInpxArchiveFileState(
    const SInpxSourceInfo& source,
    const std::string_view archiveNameUtf8)
{
    return ReadInpxArchiveFileState(ResolveInpxArchivePath(source, archiveNameUtf8));
}

SInpxArchiveFileState ReadInpxArchiveFileState(
    const std::filesystem::path& resolvedArchivePath)
{
    return {
        .FileSizeBytes = static_cast<std::uint64_t>(std::filesystem::file_size(resolvedArchivePath)),
        .MtimeTicks = static_cast<std::int64_t>(
            std::filesystem::last_write_time(resolvedArchivePath).time_since_epoch().count())
    };
}

void ExtractInpxArchiveEntryToPath(
    const SInpxSourceInfo& source,
    const std::string_view archiveNameUtf8,
    const std::string_view entryNameUtf8,
    const std::filesystem::path& destinationPath)
{
    CInpxArchiveReader(source, archiveNameUtf8).ExtractEntryToPath(entryNameUtf8, destinationPath);
}

} // namespace InpxWebReader::Application
