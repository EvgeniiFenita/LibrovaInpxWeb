#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

namespace InpxWebReader::Domain {

enum class EDomainErrorCode
{
    Validation,
    ParserFailure,
    ConverterUnavailable,
    ConverterFailed,
    ConverterTimeout,
    Cancellation,
    CatalogSnapshotChanged,
    CatalogCursorExpired,
    CatalogCursorCapacityExceeded,
    CatalogCursorInvalid,
    IntegrityIssue,
    NotFound
};

struct SDomainError
{
    EDomainErrorCode Code = EDomainErrorCode::Validation;
    std::string Message;
};

class CDomainException : public std::runtime_error
{
public:
    CDomainException(EDomainErrorCode code, const std::string& message)
        : std::runtime_error(message)
        , m_code(code)
    {
    }

    [[nodiscard]] EDomainErrorCode Code() const noexcept
    {
        return m_code;
    }

private:
    EDomainErrorCode m_code;
};

[[nodiscard]] std::string_view ToString(EDomainErrorCode code) noexcept;

} // namespace InpxWebReader::Domain
