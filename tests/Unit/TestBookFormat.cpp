#include <catch2/catch_test_macros.hpp>

#include "Domain/BookFormat.hpp"

TEST_CASE("Book format converts to stable storage strings", "[domain][book-format]")
{
    REQUIRE(InpxWebReader::Domain::ToString(InpxWebReader::Domain::EBookFormat::Epub) == "epub");
    REQUIRE(InpxWebReader::Domain::ToString(InpxWebReader::Domain::EBookFormat::Fb2) == "fb2");
}

TEST_CASE("Book format parsing accepts canonical MVP values", "[domain][book-format]")
{
    REQUIRE(InpxWebReader::Domain::TryParseBookFormat("epub") == InpxWebReader::Domain::EBookFormat::Epub);
    REQUIRE(InpxWebReader::Domain::TryParseBookFormat(".fb2") == InpxWebReader::Domain::EBookFormat::Fb2);
    REQUIRE(InpxWebReader::Domain::TryParseBookFormat("EPUB") == InpxWebReader::Domain::EBookFormat::Epub);
    REQUIRE_FALSE(InpxWebReader::Domain::TryParseBookFormat("zip").has_value());
}
