#include "Server/AuthMiddleware.hpp"

#include <algorithm>
#include <string>

namespace InpxWebReader::Server {
namespace {

[[nodiscard]] bool ConstantTimeEquals(const std::string_view left, const std::string_view right) noexcept
{
    std::size_t difference = left.size() ^ right.size();
    const auto comparisonLength = std::max(left.size(), right.size());
    for (std::size_t index = 0; index < comparisonLength; ++index)
    {
        const auto leftByte = index < left.size() ? static_cast<unsigned char>(left[index]) : 0U;
        const auto rightByte = index < right.size() ? static_cast<unsigned char>(right[index]) : 0U;
        difference |= static_cast<std::size_t>(leftByte ^ rightByte);
    }

    return difference == 0;
}

} // namespace

SAuthResult CAuthMiddleware::Authorize(
    const SServerSecurityConfig& security,
    const std::optional<std::string>& authorizationHeader) noexcept
{
    if (security.TokenUtf8.empty())
    {
        return {.Authorized = true};
    }

    if (!authorizationHeader.has_value())
    {
        return {
            .Authorized = false,
            .ErrorCodeUtf8 = "unauthorized",
            .MessageUtf8 = "Authentication is required."
        };
    }

    const std::string expectedHeader = "Bearer " + security.TokenUtf8;
    if (!ConstantTimeEquals(*authorizationHeader, expectedHeader))
    {
        return {
            .Authorized = false,
            .ErrorCodeUtf8 = "unauthorized",
            .MessageUtf8 = "Authentication failed."
        };
    }

    return {.Authorized = true};
}

} // namespace InpxWebReader::Server
