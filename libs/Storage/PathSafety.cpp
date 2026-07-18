#include "Storage/PathSafety.hpp"

#include <stdexcept>
#include <system_error>

#include "Foundation/FileSystemUtils.hpp"

namespace InpxWebReader::SafePaths {

namespace {

[[nodiscard]] std::filesystem::path CanonicalizeExistingPath(
    const std::filesystem::path& path,
    std::string_view errorMessage)
{
    std::error_code errorCode;
    const auto canonicalPath = std::filesystem::weakly_canonical(path, errorCode);
    if (errorCode)
    {
        throw std::runtime_error(std::string{errorMessage});
    }

    return canonicalPath.lexically_normal();
}

[[nodiscard]] std::filesystem::path BuildValidatedCandidatePath(
    const std::filesystem::path& root,
    const std::filesystem::path& path,
    const std::string_view unsafePathMessage)
{
    auto normalizedPath = path.lexically_normal();
    if (normalizedPath.is_absolute())
    {
        return normalizedPath;
    }
    if (!Foundation::IsSafeRelativePath(normalizedPath))
    {
        throw std::runtime_error(std::string{unsafePathMessage});
    }
    return (root / normalizedPath).lexically_normal();
}

} // namespace

bool IsPathWithinRoot(
    const std::filesystem::path& root,
    const std::filesystem::path& candidate)
{
    const auto normalizedRoot = root.lexically_normal();
    const auto normalizedCandidate = candidate.lexically_normal();

    auto rootIt = normalizedRoot.begin();
    auto candidateIt = normalizedCandidate.begin();

    for (; rootIt != normalizedRoot.end(); ++rootIt, ++candidateIt)
    {
        if (candidateIt == normalizedCandidate.end() || *rootIt != *candidateIt)
        {
            return false;
        }
    }

    return true;
}

std::optional<std::filesystem::path> TryResolvePathWithinRoot(
    const std::filesystem::path& root,
    const std::filesystem::path& path,
    const std::string_view unsafePathMessage,
    const std::string_view canonicalizationErrorMessage)
{
    const auto canonicalRoot = CanonicalizeExistingPath(root, canonicalizationErrorMessage);
    return TryResolvePathWithinCanonicalRoot(canonicalRoot, path, unsafePathMessage, canonicalizationErrorMessage);
}

std::optional<std::filesystem::path> TryResolvePathWithinCanonicalRoot(
    const std::filesystem::path& canonicalRoot,
    const std::filesystem::path& path,
    const std::string_view unsafePathMessage,
    const std::string_view canonicalizationErrorMessage)
{
    const auto candidatePath = BuildValidatedCandidatePath(canonicalRoot, path, unsafePathMessage);
    auto canonicalCandidate = CanonicalizeExistingPath(candidatePath, canonicalizationErrorMessage);
    if (!IsPathWithinRoot(canonicalRoot, canonicalCandidate))
    {
        throw std::runtime_error(std::string{unsafePathMessage});
    }

    if (!std::filesystem::exists(candidatePath))
    {
        return std::nullopt;
    }

    return canonicalCandidate;
}

} // namespace InpxWebReader::SafePaths
