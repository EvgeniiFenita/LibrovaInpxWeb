#include <catch2/catch_test_macros.hpp>

#include "Domain/ServiceContracts.hpp"

TEST_CASE("Domain service contract value types validate required fields", "[domain][services]")
{
    const InpxWebReader::Domain::SConversionRequest request{
        .SourcePath = "source.fb2",
        .DestinationPath = "book.epub",
        .SourceFormat = InpxWebReader::Domain::EBookFormat::Fb2,
        .DestinationFormat = InpxWebReader::Domain::EBookFormat::Epub
    };

    REQUIRE(request.IsValid());
    REQUIRE_FALSE(InpxWebReader::Domain::SConversionRequest{}.IsValid());
}
