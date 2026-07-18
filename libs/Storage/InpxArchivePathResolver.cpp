#include "Storage/InpxArchivePathResolver.hpp"

#include <algorithm>
#include <exception>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "Foundation/FileSystemUtils.hpp"
#include "Foundation/UnicodeConversion.hpp"
#include "Foundation/ZipArchivePaths.hpp"
#include "Storage/PathSafety.hpp"

namespace InpxWebReader::StoragePlanning {

class CInpxArchiveRootIndex::CImpl final
{
public:
    CImpl(
        const std::filesystem::path& archiveRoot,
        std::string canonicalizationErrorMessage,
        SInpxArchiveRootIndexOptions options)
        : m_canonicalizationErrorMessage(std::move(canonicalizationErrorMessage))
        , m_options(std::move(options))
    {
        RunCheckpoint();
        std::error_code errorCode;
        m_canonicalRoot = std::filesystem::weakly_canonical(archiveRoot, errorCode).lexically_normal();
        if (errorCode)
        {
            throw std::runtime_error(m_canonicalizationErrorMessage);
        }
    }

    [[nodiscard]] std::optional<std::filesystem::path> TryResolveExistingZipArchivePath(
        const std::string_view archiveNameUtf8,
        const std::string_view unsafePathMessage)
    {
        RunCheckpoint();
        const auto relativePath = Unicode::PathFromUtf8(std::string{archiveNameUtf8}).lexically_normal();
        if (!Foundation::IsSafeRelativePath(relativePath))
        {
            throw std::runtime_error(std::string{unsafePathMessage});
        }
        const auto archiveRelativePath = Foundation::BuildZipArchiveFileRelativePath(relativePath);

        std::exception_ptr directCandidateError;
        try
        {
            if (const auto directPath = TryResolveCandidate(archiveRelativePath, unsafePathMessage))
            {
                return directPath;
            }
        }
        catch (...)
        {
            directCandidateError = std::current_exception();
        }

        EnsureFallbackSnapshot(unsafePathMessage);
        const auto fallbackIterator = m_fallbackCandidates.find(archiveRelativePath);
        if (fallbackIterator != m_fallbackCandidates.end())
        {
            try
            {
                if (const auto fallbackPath = TryResolveCandidate(fallbackIterator->second, unsafePathMessage))
                {
                    return fallbackPath;
                }
            }
            catch (...)
            {
                // Unsafe or unreadable non-direct candidates have always been skipped.
                (void)0;
            }
        }

        if (directCandidateError != nullptr)
        {
            std::rethrow_exception(directCandidateError);
        }
        return std::nullopt;
    }

    [[nodiscard]] const std::filesystem::path& GetCanonicalRoot() const noexcept
    {
        return m_canonicalRoot;
    }

private:
    void RunCheckpoint() const
    {
        if (m_options.Checkpoint)
        {
            m_options.Checkpoint();
        }
    }

    void NotifyEntryVisited()
    {
        if (m_filesystemEntriesVisited == m_options.MaxFilesystemEntries)
        {
            throw std::runtime_error(
                "INPX archive fallback lookup exceeds the configured filesystem entry limit of "
                + std::to_string(m_options.MaxFilesystemEntries) + '.');
        }
        ++m_filesystemEntriesVisited;
        if (m_options.OnFilesystemEntryVisited)
        {
            m_options.OnFilesystemEntryVisited();
        }
    }

    [[nodiscard]] std::optional<std::filesystem::path> TryResolveCandidate(
        const std::filesystem::path& candidateRelativePath,
        const std::string_view unsafePathMessage) const
    {
        return SafePaths::TryResolvePathWithinCanonicalRoot(
            m_canonicalRoot,
            candidateRelativePath,
            unsafePathMessage,
            m_canonicalizationErrorMessage);
    }

    void EnsureFallbackSnapshot(const std::string_view unsafePathMessage)
    {
        if (m_fallbackSnapshotBuilt)
        {
            return;
        }
        if (m_options.OnFallbackSnapshotBuild)
        {
            m_options.OnFallbackSnapshotBuild();
        }

        std::vector<std::filesystem::path> childDirectories;
        std::error_code errorCode;
        std::filesystem::directory_iterator iterator(
            m_canonicalRoot, std::filesystem::directory_options::skip_permission_denied, errorCode);
        const std::filesystem::directory_iterator end;
        if (!errorCode)
        {
            for (; iterator != end; iterator.increment(errorCode))
            {
                RunCheckpoint();
                NotifyEntryVisited();
                if (errorCode)
                {
                    errorCode.clear();
                    continue;
                }

                std::error_code directoryError;
                if (!iterator->is_directory(directoryError) || directoryError)
                {
                    continue;
                }
                try
                {
                    if (!SafePaths::TryResolvePathWithinCanonicalRoot(
                            m_canonicalRoot,
                            iterator->path(),
                            unsafePathMessage,
                            m_canonicalizationErrorMessage))
                    {
                        continue;
                    }
                }
                catch (...)
                {
                    continue;
                }

                const auto childName = iterator->path().filename();
                childDirectories.push_back(childName);
            }
        }

        // The fallback snapshot turns repeated segment lookups into one O(F log F)
        // root walk plus O(log F) lookups, where F is the number of filesystem
        // entries beneath the root.
        std::size_t sortComparisonCount = 0;
        std::sort(
            childDirectories.begin(),
            childDirectories.end(),
            [&](const std::filesystem::path& left, const std::filesystem::path& right) {
                if (sortComparisonCount % 1'024 == 0)
                {
                    RunCheckpoint();
                }
                ++sortComparisonCount;
                return left < right;
            });
        RunCheckpoint();
        for (const auto& childDirectory : childDirectories)
        {
            RunCheckpoint();
            IndexFallbackChild(childDirectory, unsafePathMessage);
        }
        m_fallbackSnapshotBuilt = true;
    }

    void IndexFallbackChild(
        const std::filesystem::path& childDirectory,
        const std::string_view unsafePathMessage)
    {
        const auto visibleChildPath = (m_canonicalRoot / childDirectory).lexically_normal();
        std::optional<std::filesystem::path> canonicalChildPath;
        try
        {
            canonicalChildPath = SafePaths::TryResolvePathWithinCanonicalRoot(
                m_canonicalRoot,
                childDirectory,
                unsafePathMessage,
                m_canonicalizationErrorMessage);
        }
        catch (...)
        {
            return;
        }
        if (!canonicalChildPath.has_value())
        {
            return;
        }

        std::error_code errorCode;
        std::filesystem::recursive_directory_iterator iterator(
            visibleChildPath,
            std::filesystem::directory_options::follow_directory_symlink
                | std::filesystem::directory_options::skip_permission_denied,
            errorCode);
        const std::filesystem::recursive_directory_iterator end;
        if (errorCode)
        {
            return;
        }

        std::vector<std::filesystem::path> ancestorDirectories = {*canonicalChildPath};
        std::set<std::filesystem::path> activeDirectories = {*canonicalChildPath};
        while (iterator != end)
        {
            RunCheckpoint();
            NotifyEntryVisited();
            const auto depth = static_cast<std::size_t>(iterator.depth());
            while (ancestorDirectories.size() > depth + 1)
            {
                activeDirectories.erase(ancestorDirectories.back());
                ancestorDirectories.pop_back();
            }

            std::error_code directoryError;
            const bool isDirectory = iterator->is_directory(directoryError);
            if (directoryError)
            {
                iterator.disable_recursion_pending();
            }
            else if (isDirectory)
            {
                try
                {
                    const auto canonicalDirectory = SafePaths::TryResolvePathWithinCanonicalRoot(
                        m_canonicalRoot,
                        iterator->path(),
                        unsafePathMessage,
                        m_canonicalizationErrorMessage);
                    if (!canonicalDirectory.has_value() || activeDirectories.contains(*canonicalDirectory))
                    {
                        iterator.disable_recursion_pending();
                    }
                    else
                    {
                        ancestorDirectories.push_back(*canonicalDirectory);
                        activeDirectories.insert(*canonicalDirectory);
                    }
                }
                catch (...)
                {
                    iterator.disable_recursion_pending();
                }
            }
            else
            {
                IndexFallbackEntry(
                    childDirectory,
                    visibleChildPath,
                    iterator->path(),
                    unsafePathMessage);
            }

            iterator.increment(errorCode);
            if (errorCode)
            {
                return;
            }
        }
    }

    void IndexFallbackEntry(const std::filesystem::path& childDirectory,
                            const std::filesystem::path& visibleChildPath,
                            const std::filesystem::path& entryPath,
                            const std::string_view unsafePathMessage)
    {
        const auto key = entryPath.lexically_relative(visibleChildPath).lexically_normal();
        // Children are processed lexicographically, so the first insertion keeps
        // the same immediate-child precedence as the one-shot resolver.
        if (!Foundation::IsSafeRelativePath(key) || !Foundation::HasZipArchiveExtension(key)
            || m_fallbackCandidates.contains(key))
        {
            return;
        }

        const auto candidate = (childDirectory / key).lexically_normal();
        try
        {
            if (!TryResolveCandidate(candidate, unsafePathMessage))
            {
                return;
            }
        }
        catch (...)
        {
            return;
        }

        static_cast<void>(m_fallbackCandidates.emplace(key, candidate));
    }

    std::filesystem::path m_canonicalRoot;
    std::string m_canonicalizationErrorMessage;
    SInpxArchiveRootIndexOptions m_options;
    std::map<std::filesystem::path, std::filesystem::path> m_fallbackCandidates;
    std::size_t m_filesystemEntriesVisited = 0;
    bool m_fallbackSnapshotBuilt = false;
};

CInpxArchiveRootIndex::CInpxArchiveRootIndex(
    const std::filesystem::path& archiveRoot,
    std::string canonicalizationErrorMessage,
    SInpxArchiveRootIndexOptions options)
    : m_impl(std::make_unique<CImpl>(
        archiveRoot,
        std::move(canonicalizationErrorMessage),
        std::move(options)))
{
}

CInpxArchiveRootIndex::~CInpxArchiveRootIndex() = default;

CInpxArchiveRootIndex::CInpxArchiveRootIndex(CInpxArchiveRootIndex&&) noexcept = default;

CInpxArchiveRootIndex& CInpxArchiveRootIndex::operator=(CInpxArchiveRootIndex&&) noexcept = default;

std::optional<std::filesystem::path> CInpxArchiveRootIndex::TryResolveExistingZipArchivePath(
    const std::string_view archiveNameUtf8,
    const std::string_view unsafePathMessage)
{
    return m_impl->TryResolveExistingZipArchivePath(archiveNameUtf8, unsafePathMessage);
}

const std::filesystem::path& CInpxArchiveRootIndex::GetCanonicalRoot() const noexcept
{
    return m_impl->GetCanonicalRoot();
}

std::optional<std::filesystem::path> TryResolveExistingZipArchivePath(
    const std::filesystem::path& archiveRoot,
    const std::string_view archiveNameUtf8,
    const std::string_view unsafePathMessage,
    const std::string_view canonicalizationErrorMessage)
{
    CInpxArchiveRootIndex index(archiveRoot, std::string{canonicalizationErrorMessage});
    return index.TryResolveExistingZipArchivePath(archiveNameUtf8, unsafePathMessage);
}

} // namespace InpxWebReader::StoragePlanning
