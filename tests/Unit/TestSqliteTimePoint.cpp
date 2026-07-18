#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <stdexcept>
#include <string>

#include "Database/SqliteTimePoint.hpp"

namespace {

void RequireInvalidTimestamp(const std::string& value)
{
    REQUIRE_THROWS_AS(InpxWebReader::Sqlite::ParseTimePoint(value), std::runtime_error);
}

} // namespace

TEST_CASE("SQLite time point parser accepts valid UTC boundary timestamps", "[sqlite-time]")
{
    const auto midnight = InpxWebReader::Sqlite::ParseTimePoint("2026-01-01T00:00:00Z");
    CHECK(InpxWebReader::Sqlite::SerializeTimePoint(midnight) == "2026-01-01T00:00:00Z");

    const auto leapDay = InpxWebReader::Sqlite::ParseTimePoint("2000-02-29T23:59:59Z");
    CHECK(InpxWebReader::Sqlite::SerializeTimePoint(leapDay) == "2000-02-29T23:59:59Z");

    const auto yearEnd = InpxWebReader::Sqlite::ParseTimePoint("2026-12-31T23:59:59Z");
    CHECK(InpxWebReader::Sqlite::SerializeTimePoint(yearEnd) == "2026-12-31T23:59:59Z");
}

TEST_CASE("SQLite time point parser rejects malformed numeric components", "[sqlite-time]")
{
    RequireInvalidTimestamp("-001-01-01T00:00:00Z");
    RequireInvalidTimestamp("2026--1-01T00:00:00Z");
    RequireInvalidTimestamp("2026-01--1T00:00:00Z");
    RequireInvalidTimestamp("2026-01-01T-1:00:00Z");
    RequireInvalidTimestamp("2026-01-01T00:-1:00Z");
    RequireInvalidTimestamp("2026-01-01T00:00:-1Z");
}

TEST_CASE("SQLite time point parser rejects out-of-range components", "[sqlite-time]")
{
    RequireInvalidTimestamp("2026-00-01T00:00:00Z");
    RequireInvalidTimestamp("2026-13-01T00:00:00Z");
    RequireInvalidTimestamp("2026-01-00T00:00:00Z");
    RequireInvalidTimestamp("2026-02-29T00:00:00Z");
    RequireInvalidTimestamp("2026-01-01T24:00:00Z");
    RequireInvalidTimestamp("2026-01-01T00:60:00Z");
    RequireInvalidTimestamp("2026-01-01T00:00:60Z");
}
