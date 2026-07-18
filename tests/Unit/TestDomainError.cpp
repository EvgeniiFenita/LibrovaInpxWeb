#include <catch2/catch_test_macros.hpp>

#include "Domain/DomainError.hpp"

TEST_CASE("Domain error codes convert to stable error strings", "[domain][error]")
{
    REQUIRE(InpxWebReader::Domain::ToString(InpxWebReader::Domain::EDomainErrorCode::Validation) == "validation");
    REQUIRE(InpxWebReader::Domain::ToString(InpxWebReader::Domain::EDomainErrorCode::IntegrityIssue) == "integrity_issue");
    REQUIRE(InpxWebReader::Domain::ToString(InpxWebReader::Domain::EDomainErrorCode::CatalogSnapshotChanged) == "catalog_snapshot_changed");
    REQUIRE(InpxWebReader::Domain::ToString(InpxWebReader::Domain::EDomainErrorCode::CatalogCursorExpired) == "catalog_cursor_expired");
    REQUIRE(InpxWebReader::Domain::ToString(InpxWebReader::Domain::EDomainErrorCode::NotFound) == "not_found");
}
