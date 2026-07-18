#include <catch2/catch_test_macros.hpp>

#include "Domain/BookId.hpp"

TEST_CASE("Book id is valid only for positive values", "[domain][book-id]")
{
    REQUIRE(InpxWebReader::Domain::SBookId{1}.IsValid());
    REQUIRE_FALSE(InpxWebReader::Domain::SBookId{0}.IsValid());
    REQUIRE_FALSE(InpxWebReader::Domain::SBookId{-42}.IsValid());
}

