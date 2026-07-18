#pragma once

#include <string>

#include "Server/HttpError.hpp"
#include "Server/ServerApplicationHost.hpp"
#include "Server/ServerScanCoordinator.hpp"

namespace InpxWebReader::Server {

class CJsonDtoMapper final
{
public:
    [[nodiscard]] static std::string BuildHealthJson(bool backendOpen);
    [[nodiscard]] static std::string BuildVersionJson();
    [[nodiscard]] static std::string BuildStatusJson(const SServerStatus& status);
    [[nodiscard]] static std::string BuildBooksJson(
        const Application::SBookListResult& result,
        const Application::SBookListRequest& request);
    [[nodiscard]] static std::string BuildBookDetailsJson(const Application::SBookDetails& details);
    [[nodiscard]] static std::string BuildStatisticsJson(const Application::SCatalogStatistics& statistics);
    [[nodiscard]] static std::string BuildSourceJson(
        const std::optional<Application::SInpxSourceOverview>& overview);
    [[nodiscard]] static std::string BuildScanStartJson(const SServerScanStartResult& result);
    [[nodiscard]] static std::string BuildScanProgressJson(const SServerScanProgress& progress);
    [[nodiscard]] static std::string BuildScanCancelJson(const SServerScanCancelResult& result);
    [[nodiscard]] static std::string BuildErrorJson(
        const SHttpError& error,
        const std::string& requestIdUtf8);
};

} // namespace InpxWebReader::Server
