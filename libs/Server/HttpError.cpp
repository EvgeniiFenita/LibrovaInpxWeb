#include "Server/HttpError.hpp"

#include <utility>

namespace InpxWebReader::Server {

SHttpError CHttpErrorMapper::BadRequest(std::string messageUtf8)
{
    return {
        .Status = 400,
        .CodeUtf8 = "bad_request",
        .MessageUtf8 = std::move(messageUtf8)
    };
}

SHttpError CHttpErrorMapper::Unauthorized(std::string messageUtf8)
{
    return {
        .Status = 401,
        .CodeUtf8 = "unauthorized",
        .MessageUtf8 = std::move(messageUtf8)
    };
}

SHttpError CHttpErrorMapper::NotFound(std::string messageUtf8)
{
    return {
        .Status = 404,
        .CodeUtf8 = "not_found",
        .MessageUtf8 = std::move(messageUtf8)
    };
}

SHttpError CHttpErrorMapper::Conflict(std::string messageUtf8)
{
    return {
        .Status = 409,
        .CodeUtf8 = "conflict",
        .MessageUtf8 = std::move(messageUtf8)
    };
}

SHttpError CHttpErrorMapper::PayloadTooLarge(std::string messageUtf8)
{
    return {
        .Status = 413,
        .CodeUtf8 = "payload_too_large",
        .MessageUtf8 = std::move(messageUtf8)
    };
}

SHttpError CHttpErrorMapper::TooManyRequests(std::string messageUtf8)
{
    return {
        .Status = 429,
        .CodeUtf8 = "too_many_requests",
        .MessageUtf8 = std::move(messageUtf8)
    };
}

SHttpError CHttpErrorMapper::BackendNotReady()
{
    return {
        .Status = 503,
        .CodeUtf8 = "backend_not_ready",
        .MessageUtf8 = "The catalog service is not ready yet."
    };
}

SHttpError CHttpErrorMapper::InternalServerError()
{
    return {
        .Status = 500,
        .CodeUtf8 = "internal_error",
        .MessageUtf8 = "Internal server error."
    };
}

} // namespace InpxWebReader::Server
