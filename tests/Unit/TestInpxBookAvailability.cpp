#include <catch2/catch_test_macros.hpp>

#include "Domain/InpxBookAvailability.hpp"

TEST_CASE("INPX availability tokens have one domain owner", "[domain][inpx]")
{
    using InpxWebReader::Domain::EInpxBookAvailability;

    REQUIRE(InpxWebReader::Domain::ToString(EInpxBookAvailability::Available) == "available");
    REQUIRE(
        InpxWebReader::Domain::ToString(EInpxBookAvailability::MissingFromIndex)
        == "missing_from_index");
    REQUIRE(InpxWebReader::Domain::IsInpxBookAvailable("available"));
    REQUIRE_FALSE(InpxWebReader::Domain::IsInpxBookAvailable("missing_from_index"));
    REQUIRE_FALSE(InpxWebReader::Domain::IsInpxBookAvailable("unknown"));
}
