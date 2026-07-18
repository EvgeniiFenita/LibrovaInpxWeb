#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <string>

#include "Foundation/Version.hpp"

TEST_CASE("Core version matches the repository product version", "[core][bootstrap]")
{
    std::ifstream versionFile(std::string{INPX_WEB_READER_SOURCE_DIR} + "/VERSION.txt");
    REQUIRE(versionFile.is_open());

    std::string expectedVersion;
    REQUIRE(static_cast<bool>(std::getline(versionFile, expectedVersion)));
    REQUIRE(InpxWebReader::Core::CVersion::GetValue() == expectedVersion);
}
