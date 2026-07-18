#pragma once

#include <memory>

#include "App/IInpxCatalogApplication.hpp"

namespace InpxWebReader::Application {

class CInpxCatalogApplication final : public IInpxCatalogApplication
{
public:
    explicit CInpxCatalogApplication(const SInpxCatalogApplicationConfig& config);
    ~CInpxCatalogApplication() override;

    CInpxCatalogApplication(const CInpxCatalogApplication&) = delete;
    CInpxCatalogApplication& operator=(const CInpxCatalogApplication&) = delete;
    CInpxCatalogApplication(CInpxCatalogApplication&&) = delete;
    CInpxCatalogApplication& operator=(CInpxCatalogApplication&&) = delete;

    [[nodiscard]] SCatalogSessionInfo GetCatalogSessionInfo() override;
    [[nodiscard]] SBookListResult ListBooks(const SBookListRequest& request) override;
    [[nodiscard]] std::optional<SBookDetails> GetBookDetails(Domain::SBookId id) override;
    [[nodiscard]] SCatalogStatistics GetCatalogStatistics() override;
    [[nodiscard]] std::optional<SInpxSourceOverview> GetInpxSourceOverview() override;
    [[nodiscard]] ApplicationJobs::TInpxScanJobId StartInpxScan(
        const ApplicationJobs::SInpxScanRequest& request) override;
    [[nodiscard]] std::optional<ApplicationJobs::SInpxScanJobSnapshot> GetInpxScanJobSnapshot(
        ApplicationJobs::TInpxScanJobId jobId) override;
    [[nodiscard]] std::optional<ApplicationJobs::SInpxScanJobResult> GetInpxScanJobResult(
        ApplicationJobs::TInpxScanJobId jobId) override;
    [[nodiscard]] bool CancelInpxScanJob(ApplicationJobs::TInpxScanJobId jobId) override;
    [[nodiscard]] bool RemoveInpxScanJob(ApplicationJobs::TInpxScanJobId jobId) override;
    [[nodiscard]] std::optional<SPreparedBookDownload> PrepareDownload(
        const SBookDownloadRequest& request) override;

private:
    struct SImpl;
    std::unique_ptr<SImpl> m_impl;
};

} // namespace InpxWebReader::Application
