#include "Domain/DomainError.hpp"

namespace InpxWebReader::Domain {

std::string_view ToString(const EDomainErrorCode code) noexcept
{
    switch (code)
    {
    case EDomainErrorCode::Validation:
        return "validation";
    case EDomainErrorCode::ParserFailure:
        return "parser_failure";
    case EDomainErrorCode::ConverterUnavailable:
        return "converter_unavailable";
    case EDomainErrorCode::ConverterFailed:
        return "converter_failed";
    case EDomainErrorCode::ConverterTimeout:
        return "converter_timeout";
    case EDomainErrorCode::Cancellation:
        return "cancellation";
    case EDomainErrorCode::CatalogSnapshotChanged:
        return "catalog_snapshot_changed";
    case EDomainErrorCode::CatalogCursorExpired:
        return "catalog_cursor_expired";
    case EDomainErrorCode::CatalogCursorCapacityExceeded:
        return "catalog_cursor_capacity_exceeded";
    case EDomainErrorCode::CatalogCursorInvalid:
        return "catalog_cursor_invalid";
    case EDomainErrorCode::IntegrityIssue:
        return "integrity_issue";
    case EDomainErrorCode::NotFound:
        return "not_found";
    }

    return "unknown";
}

} // namespace InpxWebReader::Domain
