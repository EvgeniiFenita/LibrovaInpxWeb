#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "App/IInpxCatalogApplication.hpp"
#include "App/InpxScanJobService.hpp"
#include "Domain/ServiceContracts.hpp"

class CTestWorkspace;

namespace InpxWebReader::Tests::InpxScanJobServiceSupport {

[[nodiscard]] std::chrono::seconds InpxScanWaitTimeout();

[[nodiscard]] bool WaitForApplicationScan(
    Application::IInpxCatalogApplication& application,
    ApplicationJobs::TInpxScanJobId jobId,
    std::chrono::milliseconds timeout);

[[nodiscard]] std::string MakeInpxRecord(
    const std::string& fileName,
    const std::string& libId,
    const std::string& language = "ru",
    const std::string& unusedField12 = "",
    bool deleted = false,
    const std::string& fileExtension = "fb2",
    std::size_t fileSizeBytes = 0);

std::filesystem::path WriteInpxArchiveEntries(
    const std::filesystem::path& archivePath,
    const std::vector<std::pair<std::string, std::string>>& entries);

std::filesystem::path WriteInpxArchive(
    const std::filesystem::path& archivePath,
    const std::string& content);

[[nodiscard]] std::string MakeFb2Payload(
    const std::string& title,
    bool includeCover,
    const std::string& language = "ru");

std::filesystem::path WriteInpxArchive(
    const std::filesystem::path& archivePath,
    std::size_t recordCount,
    bool includeCover = false);

std::filesystem::path WriteInpxArchive(
    const std::filesystem::path& archivePath,
    const std::vector<std::string>& titles,
    bool includeCover = false);

std::filesystem::path WriteSinglePayloadInpxArchive(
    const std::filesystem::path& archivePath,
    const std::string& payload);

[[nodiscard]] std::string ReadInpxBookAvailability(
    const std::filesystem::path& databasePath,
    const std::string& libId);

[[nodiscard]] bool ReadInpxPresence(
    const std::filesystem::path& databasePath,
    const std::string& libId);

[[nodiscard]] std::int64_t CountInpxDeletionMarkers(
    const std::filesystem::path& databasePath,
    const std::string& libId);

[[nodiscard]] bool ReadSegmentRequiresArchive(
    const std::filesystem::path& databasePath,
    const std::string& inpEntryNameUtf8);

[[nodiscard]] std::int64_t CountPersistedWarnings(const std::filesystem::path& databasePath);
[[nodiscard]] std::int64_t CountPersistedWarningScans(const std::filesystem::path& databasePath);
[[nodiscard]] std::string ReadSourceFingerprint(const std::filesystem::path& databasePath);
[[nodiscard]] std::string ReadSourceDisplayName(const std::filesystem::path& databasePath);
[[nodiscard]] std::int64_t CountBooksWithCoverPath(const std::filesystem::path& databasePath);
[[nodiscard]] std::int64_t CountBookRows(const std::filesystem::path& databasePath);
[[nodiscard]] std::size_t CountCoverFiles(const std::filesystem::path& cacheRoot);
[[nodiscard]] std::uint64_t SumCoverFileBytes(const std::filesystem::path& cacheRoot);

[[nodiscard]] std::optional<std::string> ReadCoverPath(
    const std::filesystem::path& databasePath,
    const std::string& libId);

void UpdateCoverPath(
    const std::filesystem::path& databasePath,
    const std::string& libId,
    const std::optional<std::string>& coverPathUtf8);

[[nodiscard]] std::string ReadAllText(const std::filesystem::path& path);

class CRecordingCoverImageProcessor final : public Domain::ICoverImageProcessor
{
public:
    [[nodiscard]] Domain::SCoverProcessingResult ProcessForCache(
        const Domain::SCoverProcessingRequest& request) const override;

    [[nodiscard]] std::vector<std::thread::id> GetThreadIds() const;

private:
    mutable std::mutex m_mutex;
    mutable std::vector<std::thread::id> m_threadIds;
};

class CBlockingCoverImageProcessor final : public Domain::ICoverImageProcessor
{
public:
    [[nodiscard]] Domain::SCoverProcessingResult ProcessForCache(
        const Domain::SCoverProcessingRequest& request) const override;

    [[nodiscard]] bool WaitUntilStarted(std::size_t expectedCount, std::chrono::milliseconds timeout) const;
    [[nodiscard]] bool WaitUntilFinished(std::size_t expectedCount, std::chrono::milliseconds timeout) const;
    void Release() const;

private:
    mutable std::mutex m_mutex;
    mutable std::condition_variable m_condition;
    mutable std::size_t m_startedCount = 0;
    mutable std::size_t m_finishedCount = 0;
    mutable bool m_release = false;
};

[[nodiscard]] Application::SInpxCatalogApplicationConfig MakeBaseConfig(const CTestWorkspace& sandbox);

[[nodiscard]] Application::SInpxCatalogApplicationConfig MakeInpxConfig(
    const CTestWorkspace& sandbox,
    std::size_t recordCount = 2);

} // namespace InpxWebReader::Tests::InpxScanJobServiceSupport
