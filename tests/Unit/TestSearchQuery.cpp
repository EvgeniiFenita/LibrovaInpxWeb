#include <catch2/catch_test_macros.hpp>

#include "Domain/SearchQuery.hpp"

TEST_CASE("Search query reports text", "[domain][query]")
{
    InpxWebReader::Domain::SSearchQuery query;

    REQUIRE_FALSE(query.HasText());
    query.TextUtf8 = "strugatsky";
    REQUIRE(query.HasText());
}

TEST_CASE("Search query defaults to all full-text fields", "[domain][query]")
{
    const InpxWebReader::Domain::SSearchQuery query;

    REQUIRE(query.SearchFields.Title);
    REQUIRE(query.SearchFields.Authors);
    REQUIRE(query.SearchFields.Description);
    REQUIRE(query.SearchFields.HasAnyField());
}

TEST_CASE("Search query keeps MVP defaults for pagination", "[domain][query]")
{
    const InpxWebReader::Domain::SSearchQuery query;

    REQUIRE(query.Offset == 0);
    REQUIRE(query.Limit == 50);
}
