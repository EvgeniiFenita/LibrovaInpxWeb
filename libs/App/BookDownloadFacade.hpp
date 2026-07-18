#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include "App/FileReplacement.hpp"
#include "Domain/ServiceContracts.hpp"

namespace InpxWebReader::Application {

struct SBookDownloadRequest
{
    InpxWebReader::Domain::SBookId BookId;
    std::filesystem::path DestinationPath;
    std::optional<InpxWebReader::Domain::EBookFormat> RequestedFormat = std::nullopt;
    std::stop_token StopToken;
};

struct SBookDownloadSource
{
    std::filesystem::path InpxPath;
    std::filesystem::path ArchiveRoot;
};

struct SPreparedBookDownload
{
    std::filesystem::path Path;
    std::string TitleUtf8;
    std::vector<std::string> AuthorsUtf8;
    InpxWebReader::Domain::EBookFormat Format = InpxWebReader::Domain::EBookFormat::Fb2;
};

struct SBookDownloadHooks
{
    std::function<void()> AfterSnapshotLoaded;
    std::function<void()> BeforeSourceFingerprintCheckpoint;
    std::function<void()> BeforeArchiveManifestCheckpoint;
    std::function<void()> AfterArchiveSnapshotValidated;
    std::function<void()> BeforeArchiveExtractionCheckpoint;
};

class CBookDownloadFacade final
{
public:
    CBookDownloadFacade(
        std::filesystem::path cacheRoot,
        std::optional<SBookDownloadSource> source,
        const InpxWebReader::Domain::IBookConverter* converter,
        std::filesystem::path runtimeWorkspaceRoot,
        std::filesystem::path databasePath,
        std::uint64_t maxArchiveManifestMemoryBytes,
        SFileReplacementHooks replacementHooks = {},
        SBookDownloadHooks downloadHooks = {});

    [[nodiscard]] std::optional<SPreparedBookDownload> PrepareDownload(
        const SBookDownloadRequest& request) const;

private:
    [[nodiscard]] std::filesystem::path PrepareConvertedDownload(
        InpxWebReader::Domain::EBookFormat sourceFormat,
        const std::filesystem::path& sourcePath,
        const std::filesystem::path& preparedDestinationPath,
        const std::filesystem::path& destinationPath,
        std::stop_token stopToken,
        const std::function<void()>& validateSourceSnapshot) const;

    std::filesystem::path m_cacheRoot;
    std::optional<SBookDownloadSource> m_source;
    const InpxWebReader::Domain::IBookConverter* m_converter;
    std::filesystem::path m_runtimeWorkspaceRoot;
    std::filesystem::path m_databasePath;
    SFileReplacementHooks m_replacementHooks;
    SBookDownloadHooks m_downloadHooks;
    std::uint64_t m_maxArchiveManifestMemoryBytes = 0;
};

} // namespace InpxWebReader::Application
