#pragma once

#include <filesystem>

namespace InpxWebReader::StoragePlanning {

struct SRuntimeWorkspaceLayoutPaths
{
    std::filesystem::path ScanDirectory;
    std::filesystem::path DownloadSourceDirectory;
    std::filesystem::path ServerDownloadDirectory;
    std::filesystem::path ConverterDirectory;
};

[[nodiscard]] inline SRuntimeWorkspaceLayoutPaths BuildRuntimeWorkspaceLayout(
    const std::filesystem::path& runtimeWorkspaceRoot)
{
    return {
        .ScanDirectory = runtimeWorkspaceRoot / "Scans",
        .DownloadSourceDirectory = runtimeWorkspaceRoot / "Downloads",
        .ServerDownloadDirectory = runtimeWorkspaceRoot / "ServerDownloads",
        .ConverterDirectory = runtimeWorkspaceRoot / "Converter"
    };
}

} // namespace InpxWebReader::StoragePlanning
