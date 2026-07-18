#include "Inpx/InpxSourceConfiguration.hpp"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "Foundation/FileSystemUtils.hpp"
#include "Foundation/StringUtils.hpp"
#include "Foundation/UnicodeConversion.hpp"
#include "Foundation/ZipArchivePaths.hpp"
#include "Inpx/InpxParser.hpp"

namespace InpxWebReader::Inpx {
namespace {

// Source auto-detection deliberately keeps its duplicate/path cross-checks on
// this hard small sample; catalog-sized segment processing uses ordered sets.
constexpr std::size_t GMaxArchiveRootProbeSamples = 16;

[[nodiscard]] bool HasExtensionAsciiInsensitive(
    const std::filesystem::path& path,
    const std::string_view expectedExtension)
{
    const auto extensionUtf8 = Unicode::PathToUtf8(path.extension());
    return extensionUtf8.size() == expectedExtension.size()
        && Foundation::EndsWithAsciiInsensitive(extensionUtf8, expectedExtension);
}

[[nodiscard]] bool ContainsPath(
    const std::vector<std::filesystem::path>& paths,
    const std::filesystem::path& value)
{
    return std::find(paths.begin(), paths.end(), value) != paths.end();
}

[[nodiscard]] std::vector<std::filesystem::path> BuildArchiveRootProbePaths(
    const std::vector<std::string>& archiveNamesUtf8)
{
    std::vector<std::filesystem::path> archivePaths;
    archivePaths.reserve(archiveNamesUtf8.size());
    for (const std::string& archiveNameUtf8 : archiveNamesUtf8)
    {
        const auto archivePath =
            Foundation::BuildZipArchiveFileRelativePath(std::string_view{archiveNameUtf8});
        if (Foundation::IsSafeRelativePath(archivePath) && !ContainsPath(archivePaths, archivePath))
        {
            archivePaths.push_back(archivePath);
        }
    }

    return archivePaths;
}

[[nodiscard]] std::vector<std::filesystem::path> BuildArchiveRootCandidates(
    const std::filesystem::path& parentPath)
{
    std::vector<std::filesystem::path> candidates;
    candidates.push_back(parentPath.lexically_normal());

    std::error_code errorCode;
    std::filesystem::directory_iterator iterator(parentPath, errorCode);
    if (errorCode)
    {
        return candidates;
    }

    std::vector<std::filesystem::path> childDirectories;
    for (const auto& entry : iterator)
    {
        if (entry.is_directory(errorCode) && !errorCode)
        {
            childDirectories.push_back(entry.path().lexically_normal());
        }
        errorCode.clear();
    }

    std::sort(childDirectories.begin(), childDirectories.end());
    candidates.insert(candidates.end(), childDirectories.begin(), childDirectories.end());
    return candidates;
}

[[nodiscard]] std::size_t CountExistingArchiveSamples(
    const std::filesystem::path& root,
    const std::vector<std::filesystem::path>& archivePaths)
{
    std::size_t count = 0;
    for (const auto& archivePath : archivePaths)
    {
        std::error_code errorCode;
        if (std::filesystem::is_regular_file(root / archivePath, errorCode))
        {
            ++count;
        }
    }

    return count;
}

[[nodiscard]] std::filesystem::path InferArchiveRoot(
    const std::filesystem::path& inpxPath,
    const std::vector<std::filesystem::path>& archivePaths)
{
    const auto parentPath = inpxPath.parent_path();
    if (archivePaths.empty())
    {
        return parentPath.lexically_normal();
    }

    std::filesystem::path bestRoot = parentPath.lexically_normal();
    std::size_t bestScore = 0;
    for (const auto& candidate : BuildArchiveRootCandidates(parentPath))
    {
        const std::size_t score = CountExistingArchiveSamples(candidate, archivePaths);
        if (score == archivePaths.size())
        {
            return candidate.lexically_normal();
        }

        if (score > bestScore)
        {
            bestRoot = candidate.lexically_normal();
            bestScore = score;
        }
    }

    return bestScore > 0 ? bestRoot : parentPath.lexically_normal();
}

} // namespace

SInpxSourceValidationResult CInpxSourceConfiguration::Validate(
    const std::filesystem::path& inpxPath,
    std::optional<std::filesystem::path> archiveRootOverride)
{
    SInpxSourceValidationResult result;
    result.InpxPath = inpxPath.lexically_normal();
    result.ArchiveRoot = archiveRootOverride.has_value()
        ? archiveRootOverride->lexically_normal()
        : result.InpxPath.parent_path().lexically_normal();

    if (result.InpxPath.empty() || !result.InpxPath.is_absolute())
    {
        result.ErrorUtf8 = "INPX path must be an absolute path.";
        return result;
    }

    std::error_code fileError;
    if (!std::filesystem::is_regular_file(result.InpxPath, fileError) || fileError)
    {
        result.ErrorUtf8 = "INPX file does not exist.";
        return result;
    }

    if (!HasExtensionAsciiInsensitive(result.InpxPath, ".inpx"))
    {
        result.ErrorUtf8 = "INPX file must use the .inpx extension.";
        return result;
    }

    try
    {
        const auto archiveNames = CInpxParser{}.ReadArchiveNameSamples(
            result.InpxPath,
            GMaxArchiveRootProbeSamples);
        if (archiveNames.empty())
        {
            result.ErrorUtf8 = "INPX archive does not contain any .inp entries.";
            return result;
        }

        const auto archivePaths = BuildArchiveRootProbePaths(archiveNames);
        if (!archiveRootOverride.has_value())
        {
            result.ArchiveRoot = InferArchiveRoot(result.InpxPath, archivePaths);
        }
    }
    catch (const std::exception& ex)
    {
        result.ErrorUtf8 = ex.what();
        return result;
    }

    if (result.ArchiveRoot.empty() || !result.ArchiveRoot.is_absolute())
    {
        result.ErrorUtf8 = "Archive root path must be an absolute path.";
        return result;
    }

    std::error_code archiveRootError;
    if (!std::filesystem::exists(result.ArchiveRoot, archiveRootError) || archiveRootError)
    {
        result.ErrorUtf8 = "Archive root folder does not exist.";
        return result;
    }

    if (!std::filesystem::is_directory(result.ArchiveRoot, archiveRootError) || archiveRootError)
    {
        result.ErrorUtf8 = "Archive root path points to a file, not a folder.";
        return result;
    }

    result.IsValid = true;
    return result;
}

} // namespace InpxWebReader::Inpx
