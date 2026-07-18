#pragma once

#include <optional>
#include <string>

#include "Server/ServerConfig.hpp"

namespace InpxWebReader::Server {

struct SAuthResult
{
    bool Authorized = false;
    std::string ErrorCodeUtf8;
    std::string MessageUtf8;
};

class CAuthMiddleware final
{
public:
    [[nodiscard]] static SAuthResult Authorize(
        const SServerSecurityConfig& security,
        const std::optional<std::string>& authorizationHeader) noexcept;
};

} // namespace InpxWebReader::Server
