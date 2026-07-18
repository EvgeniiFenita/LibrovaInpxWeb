#pragma once

#include <string>

namespace InpxWebReader::Server {

struct SHttpError
{
    int Status = 500;
    std::string CodeUtf8;
    std::string MessageUtf8;
};

class CHttpErrorMapper final
{
public:
    [[nodiscard]] static SHttpError BadRequest(std::string messageUtf8);
    [[nodiscard]] static SHttpError Unauthorized(std::string messageUtf8);
    [[nodiscard]] static SHttpError NotFound(std::string messageUtf8);
    [[nodiscard]] static SHttpError Conflict(std::string messageUtf8);
    [[nodiscard]] static SHttpError PayloadTooLarge(std::string messageUtf8);
    [[nodiscard]] static SHttpError TooManyRequests(std::string messageUtf8);
    [[nodiscard]] static SHttpError BackendNotReady();
    [[nodiscard]] static SHttpError InternalServerError();
};

} // namespace InpxWebReader::Server
