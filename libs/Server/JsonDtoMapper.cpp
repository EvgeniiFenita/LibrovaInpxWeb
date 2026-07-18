#include "Server/JsonDtoMapper.hpp"

#include <chrono>
#include <format>
#include <optional>

#include <nlohmann/json.hpp>

#include "Domain/DomainError.hpp"
#include "Domain/BookFormat.hpp"
#include "Foundation/UnicodeConversion.hpp"
#include "Foundation/Version.hpp"
#include "Server/BookListCursor.hpp"

namespace InpxWebReader::Server {
namespace {

[[nodiscard]] nlohmann::json BuildCapabilitiesJson(const Application::SCatalogCapabilities& capabilities)
{
    return {
        {"canRescanInpxSource", capabilities.CanRescanInpxSource},
        {"canDownloadOriginal", capabilities.CanDownloadOriginal},
        {"canDownloadAsEpub", capabilities.CanDownloadAsEpub}
    };
}

[[nodiscard]] nlohmann::json BuildInpxSourceJson(const SServerInpxSourceStatus& source)
{
    return {
        {"available", source.IsSourceAvailable},
        {"requiresRescan", source.RequiresRescan},
        {"sourceWarning", source.SourceWarningUtf8},
        {"totalBookCount", source.TotalBookCount},
        {"availableBookCount", source.AvailableBookCount},
        {"unavailableBookCount", source.UnavailableBookCount},
        {"warningCount", source.WarningCount}
    };
}

[[nodiscard]] nlohmann::json BuildOptionalByteCountJson(const std::optional<std::uint64_t> value)
{
    return value.has_value()
        ? nlohmann::json(*value)
        : nlohmann::json(nullptr);
}

[[nodiscard]] nlohmann::json BuildRuntimeJson(const SServerRuntimeStatus& runtime)
{
    return {
        {"uptimeSeconds", runtime.UptimeSeconds},
        {"http", {
            {"activeWorkers", runtime.Http.ActiveWorkers},
            {"queuedRequests", runtime.Http.QueuedRequests},
            {"maxWorkers", runtime.Http.MaxWorkers},
            {"maxQueuedRequests", runtime.Http.MaxQueuedRequests}
        }},
        {"backend", {
            {"activeOperations", runtime.Backend.ActiveOperations},
            {"queuedOperations", runtime.Backend.QueuedOperations},
            {"maxQueueDepth", runtime.Backend.MaxQueueDepth}
        }},
        {"scan", {
            {"active", runtime.Scan.Active},
            {"activeJobs", runtime.Scan.ActiveJobs},
            {"maxConcurrentJobs", runtime.Scan.MaxConcurrentJobs},
            {"activeWorkers", runtime.Scan.ActiveWorkers},
            {"maxWorkers", runtime.Scan.MaxWorkers}
        }},
        {"downloads", {
            {"active", runtime.Downloads.Active},
            {"maxConcurrent", runtime.Downloads.MaxConcurrent}
        }},
        {"storage", {
            {"cacheRootPresent", runtime.Storage.CacheRootPresent},
            {"cacheDatabasePresent", runtime.Storage.CacheDatabasePresent},
            {"runtimeWorkspacePresent", runtime.Storage.RuntimeWorkspacePresent},
            {"coverCacheBytes", BuildOptionalByteCountJson(runtime.Storage.CoverCacheBytes)},
            {"inpxScanWorkspaceBytes", BuildOptionalByteCountJson(runtime.Storage.InpxScanWorkspaceBytes)},
            {"downloadWorkspaceBytes", BuildOptionalByteCountJson(runtime.Storage.DownloadWorkspaceBytes)}
        }},
        {"resources", {
            {"residentMemoryBytes", BuildOptionalByteCountJson(runtime.Resources.ResidentMemoryBytes)},
            {"peakResidentMemoryBytes", BuildOptionalByteCountJson(runtime.Resources.PeakResidentMemoryBytes)},
            {"maxCoverCacheBytes", runtime.Resources.MaxCoverCacheBytes},
            {"maxSteadyStateMemoryBytes", runtime.Resources.MaxSteadyStateMemoryBytes}
        }}
    };
}

[[nodiscard]] std::string FormatUtcTimestamp(const std::chrono::system_clock::time_point value)
{
    return std::format("{:%Y-%m-%dT%H:%M:%SZ}", std::chrono::floor<std::chrono::seconds>(value));
}

[[nodiscard]] nlohmann::json BuildStatisticsJsonObject(const Application::SCatalogStatistics& statistics)
{
    return {
        {"bookCount", statistics.BookCount},
        {"unavailableBookCount", statistics.UnavailableBookCount},
        {"inpxSourceSizeBytes", statistics.InpxSourceSizeBytes},
        {"coverCacheSizeBytes", statistics.CoverCacheSizeBytes},
        {"databaseSizeBytes", statistics.DatabaseSizeBytes},
        {"totalCatalogSizeBytes", statistics.TotalCatalogSizeBytes}
    };
}

[[nodiscard]] nlohmann::json BuildBookActionsJson(
    const bool canDownloadOriginal,
    const bool canDownloadAsEpub)
{
    return {
        {"canDownloadOriginal", canDownloadOriginal},
        {"canDownloadAsEpub", canDownloadAsEpub}
    };
}

template <typename TBookProjection>
[[nodiscard]] nlohmann::json BuildCommonBookJson(const TBookProjection& book)
{
    return {
        {"id", book.Id.Value},
        {"title", book.TitleUtf8},
        {"authors", book.AuthorsUtf8},
        {"language", book.Language},
        {"seriesName", book.SeriesUtf8.has_value() ? nlohmann::json(*book.SeriesUtf8) : nlohmann::json(nullptr)},
        {"seriesIndex", book.SeriesIndex.has_value() ? nlohmann::json(*book.SeriesIndex) : nlohmann::json(nullptr)},
        {"year", book.Year.has_value() ? nlohmann::json(*book.Year) : nlohmann::json(nullptr)},
        {"tags", book.TagsUtf8},
        {"genres", book.GenresUtf8},
        {"format", std::string{Domain::ToString(book.Format)}},
        {"sizeBytes", book.SizeBytes},
        {"addedAtUtc", FormatUtcTimestamp(book.AddedAtUtc)},
        {"coverUrl", book.CoverPath.has_value()
            ? nlohmann::json("/api/covers/" + std::to_string(book.Id.Value))
            : nlohmann::json(nullptr)},
        {"downloadUrl", "/api/books/" + std::to_string(book.Id.Value) + "/download?format=original"},
        {"epubDownloadUrl", book.CanDownloadAsEpub
            ? nlohmann::json("/api/books/" + std::to_string(book.Id.Value) + "/download?format=epub")
            : nlohmann::json(nullptr)},
        {"actions", BuildBookActionsJson(
            book.CanDownloadOriginal,
            book.CanDownloadAsEpub)},
        {"isAvailable", book.IsAvailable},
        {"availabilityLabel", book.AvailabilityLabelUtf8}
    };
}

[[nodiscard]] nlohmann::json BuildBookListItemJson(const Application::SBookListItem& item)
{
    return BuildCommonBookJson(item);
}

[[nodiscard]] nlohmann::json BuildBookDetailsJsonObject(const Application::SBookDetails& details)
{
    nlohmann::json result = BuildCommonBookJson(details);
    result["publisher"] = details.PublisherUtf8.has_value()
        ? nlohmann::json(*details.PublisherUtf8)
        : nlohmann::json(nullptr);
    result["isbn"] = details.Isbn.has_value()
        ? nlohmann::json(*details.Isbn)
        : nlohmann::json(nullptr);
    result["description"] = details.DescriptionUtf8.has_value()
        ? nlohmann::json(*details.DescriptionUtf8)
        : nlohmann::json(nullptr);
    result["identifier"] = details.Identifier.has_value()
        ? nlohmann::json(*details.Identifier)
        : nlohmann::json(nullptr);
    return result;
}

[[nodiscard]] nlohmann::json BuildFacetJson(const Domain::SFacetItem& item)
{
    return {
        {"value", item.Value},
        {"count", item.Count}
    };
}

[[nodiscard]] nlohmann::json BuildFacetsJson(const std::vector<Domain::SFacetItem>& items)
{
    nlohmann::json result = nlohmann::json::array();
    for (const auto& item : items)
    {
        result.push_back(BuildFacetJson(item));
    }
    return result;
}

[[nodiscard]] std::string ToScanStatusString(const ApplicationJobs::EInpxScanJobStatus status)
{
    switch (status)
    {
    case ApplicationJobs::EInpxScanJobStatus::Pending:
        return "pending";
    case ApplicationJobs::EInpxScanJobStatus::Running:
        return "running";
    case ApplicationJobs::EInpxScanJobStatus::Completed:
        return "completed";
    case ApplicationJobs::EInpxScanJobStatus::Failed:
        return "failed";
    case ApplicationJobs::EInpxScanJobStatus::Cancelled:
        return "cancelled";
    case ApplicationJobs::EInpxScanJobStatus::Cancelling:
        return "cancelling";
    }

    return "unknown";
}

[[nodiscard]] nlohmann::json BuildInpxSourceOverviewJson(const Application::SInpxSourceOverview& overview)
{
    return {
        {"inpxPath", Unicode::PathToUtf8(overview.Source.InpxPath)},
        {"archiveRoot", Unicode::PathToUtf8(overview.Source.ArchiveRoot)},
        {"displayName", overview.DisplayNameUtf8},
        {"available", overview.IsSourceAvailable},
        {"requiresRescan", overview.RequiresRescan},
        {"sourceWarning", overview.SourceWarningUtf8},
        {"lastScanStartedAtUtc", overview.LastScanStartedAtUtc},
        {"lastScanCompletedAtUtc", overview.LastScanCompletedAtUtc},
        {"lastSeenSnapshotId", overview.LastSeenSnapshotId},
        {"totalBookCount", overview.TotalBookCount},
        {"availableBookCount", overview.AvailableBookCount},
        {"unavailableBookCount", overview.UnavailableBookCount},
        {"warningCount", overview.WarningCount},
        {"recentWarnings", overview.RecentWarningsUtf8}
    };
}

template <typename TScanCounters>
void AppendScanCounters(nlohmann::json& dto, const TScanCounters& counters)
{
    dto["totalRecords"] = counters.TotalRecords;
    dto["scannedRecords"] = counters.ScannedRecords;
    dto["parsedFb2Records"] = counters.ParsedFb2Records;
    dto["addedRecords"] = counters.AddedRecords;
    dto["updatedRecords"] = counters.UpdatedRecords;
    dto["markedUnavailableRecords"] = counters.MarkedUnavailableRecords;
    dto["unavailableRecords"] = counters.MarkedUnavailableRecords;
    dto["skippedRecords"] = counters.SkippedRecords;
    dto["reusedRecords"] = counters.ReusedRecords;
    dto["segmentsTotal"] = counters.SegmentsTotal;
    dto["segmentsUnchanged"] = counters.SegmentsUnchanged;
    dto["segmentsAdded"] = counters.SegmentsAdded;
    dto["segmentsChanged"] = counters.SegmentsChanged;
    dto["segmentsRemoved"] = counters.SegmentsRemoved;
    dto["archivesSkipped"] = counters.ArchivesSkipped;
    dto["archivesOpened"] = counters.ArchivesOpened;
    dto["archiveBytesRead"] = counters.ArchiveBytesRead;
}

[[nodiscard]] nlohmann::json BuildScanResultJson(
    const ApplicationJobs::SInpxScanResult& result,
    const ApplicationJobs::SInpxScanJobSnapshot& snapshot)
{
    nlohmann::json dto = {
        {"warningCount", result.WarningCount},
        {"parserWarningCount", result.ParserWarningCount},
        {"coverWarningCount", result.CoverWarningCount},
        {"metadataFallbackCount", result.MetadataFallbackCount},
        {"titleFallbackCount", result.TitleFallbackCount},
        {"authorFallbackCount", result.AuthorFallbackCount},
        {"languageFallbackCount", result.LanguageFallbackCount}
    };
    AppendScanCounters(dto, snapshot);
    return dto;
}

[[nodiscard]] nlohmann::json BuildScanSnapshotJson(const ApplicationJobs::SInpxScanJobSnapshot& snapshot)
{
    nlohmann::json dto = {
        {"active", !snapshot.IsTerminal()},
        {"jobId", snapshot.JobId},
        {"status", ToScanStatusString(snapshot.Status)},
        {"percent", snapshot.Percent},
        {"message", snapshot.Message},
        {"warnings", snapshot.Warnings},
        {"current", {
            {"archiveName", snapshot.CurrentArchiveNameUtf8},
            {"entryName", snapshot.CurrentEntryNameUtf8}
        }}
    };
    AppendScanCounters(dto, snapshot);
    return dto;
}

[[nodiscard]] nlohmann::json BuildScanProgressJsonObject(const SServerScanProgress& progress)
{
    if (!progress.Snapshot.has_value())
    {
        return {
            {"active", false},
            {"status", "idle"}
        };
    }

    nlohmann::json dto = BuildScanSnapshotJson(*progress.Snapshot);
    if (progress.Result.has_value())
    {
        if (progress.Result->ScanResult.has_value())
        {
            dto["result"] = BuildScanResultJson(
                *progress.Result->ScanResult,
                progress.Result->Snapshot);
        }
        if (progress.Result->Error.has_value())
        {
            dto["error"] = {
                {"code", std::string{Domain::ToString(progress.Result->Error->Code)}},
                {"message", progress.Result->Error->Message}
            };
        }
    }

    return dto;
}

} // namespace

std::string CJsonDtoMapper::BuildHealthJson(const bool backendOpen)
{
    return nlohmann::json{
        {"ok", true},
        {"service", "InpxWebReader"},
        {"status", backendOpen ? "ready" : "starting"}
    }.dump();
}

std::string CJsonDtoMapper::BuildVersionJson()
{
    return nlohmann::json{
        {"name", "InpxWebReader"},
        {"version", std::string{Core::CVersion::GetValue()}}
    }.dump();
}

std::string CJsonDtoMapper::BuildStatusJson(const SServerStatus& status)
{
    nlohmann::json dto{
        {"version", status.VersionUtf8},
        {"status", status.IsOpen ? "open" : "closed"},
        {"capabilities", BuildCapabilitiesJson(status.Capabilities)},
        {"converter", {
            {"available", status.Capabilities.CanDownloadAsEpub},
            {"canDownloadAsEpub", status.Capabilities.CanDownloadAsEpub}
        }},
        {"scan", BuildScanProgressJsonObject({
            .Snapshot = status.ActiveScan,
            .Result = std::nullopt
        })},
        {"runtime", BuildRuntimeJson(status.Runtime)}
    };

    if (status.InpxSource.has_value())
    {
        dto["inpxSource"] = BuildInpxSourceJson(status.InpxSource.value());
    }

    return dto.dump();
}

std::string CJsonDtoMapper::BuildBooksJson(
    const Application::SBookListResult& result,
    const Application::SBookListRequest& request)
{
    nlohmann::json items = nlohmann::json::array();
    for (const auto& item : result.Items)
    {
        items.push_back(BuildBookListItemJson(item));
    }

    nlohmann::json facets = nullptr;
    if (result.TotalCount.has_value())
    {
        facets = {
            {"languages", BuildFacetsJson(result.AvailableLanguages)},
            {"genres", BuildFacetsJson(result.AvailableGenres)}
        };
    }
    nlohmann::json totalCount = nullptr;
    if (result.TotalCount.has_value())
    {
        totalCount = *result.TotalCount;
    }
    nlohmann::json nextCursor = nullptr;
    if (result.NextCursor.has_value())
    {
        nextCursor = CBookListCursorCodec::Encode(*result.NextCursor, request);
    }

    return nlohmann::json{
        {"items", std::move(items)},
        {"totalCount", totalCount},
        {"catalogSnapshotId", result.CatalogSnapshotIdUtf8},
        {"offset", request.Cursor.has_value() ? request.Cursor->Position : request.Offset},
        {"limit", request.Limit},
        {"nextCursor", nextCursor},
        {"facets", std::move(facets)}
    }.dump();
}

std::string CJsonDtoMapper::BuildBookDetailsJson(const Application::SBookDetails& details)
{
    return nlohmann::json{
        {"book", BuildBookDetailsJsonObject(details)}
    }.dump();
}

std::string CJsonDtoMapper::BuildStatisticsJson(const Application::SCatalogStatistics& statistics)
{
    return BuildStatisticsJsonObject(statistics).dump();
}

std::string CJsonDtoMapper::BuildSourceJson(
    const std::optional<Application::SInpxSourceOverview>& overview)
{
    return nlohmann::json{
        {"source", overview.has_value()
            ? BuildInpxSourceOverviewJson(*overview)
            : nlohmann::json(nullptr)}
    }.dump();
}

std::string CJsonDtoMapper::BuildScanStartJson(const SServerScanStartResult& result)
{
    return nlohmann::json{
        {"jobId", result.JobId},
        {"scan", BuildScanProgressJsonObject(result.Progress)}
    }.dump();
}

std::string CJsonDtoMapper::BuildScanProgressJson(const SServerScanProgress& progress)
{
    return BuildScanProgressJsonObject(progress).dump();
}

std::string CJsonDtoMapper::BuildScanCancelJson(const SServerScanCancelResult& result)
{
    return nlohmann::json{
        {"accepted", result.Accepted},
        {"scan", BuildScanProgressJsonObject(result.Progress)}
    }.dump();
}

std::string CJsonDtoMapper::BuildErrorJson(
    const SHttpError& error,
    const std::string& requestIdUtf8)
{
    return nlohmann::json{
        {"error", {
            {"code", error.CodeUtf8},
            {"message", error.MessageUtf8},
            {"requestId", requestIdUtf8}
        }}
    }.dump();
}

} // namespace InpxWebReader::Server
