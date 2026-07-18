#include "Database/SqliteTimePoint.hpp"

#include <charconv>
#include <cctype>
#include <format>
#include <stdexcept>

namespace InpxWebReader::Sqlite {

namespace {

int ParseIntComponent(
    const std::string_view value,
    const std::size_t offset,
    const std::size_t length)
{
    int component = 0;
    const auto begin = value.data() + offset;
    const auto end = begin + length;
    const auto [pointer, error] = std::from_chars(begin, end, component);
    if (error != std::errc{} || pointer != end)
    {
        throw std::runtime_error(
            std::string{"Failed to parse sqlite timestamp: "} + std::string{value});
    }

    return component;
}

void ValidateTimestampShape(const std::string_view value)
{
    if (value.size() != 20
        || value[4] != '-'
        || value[7] != '-'
        || value[10] != 'T'
        || value[13] != ':'
        || value[16] != ':'
        || value[19] != 'Z')
    {
        throw std::runtime_error(
            std::string{"Failed to parse sqlite timestamp: "} + std::string{value});
    }

    for (std::size_t index = 0; index < value.size(); ++index)
    {
        if (index == 4 || index == 7 || index == 10 || index == 13 || index == 16 || index == 19)
        {
            continue;
        }

        const auto character = static_cast<unsigned char>(value[index]);
        if (std::isdigit(character) == 0)
        {
            throw std::runtime_error(
                std::string{"Failed to parse sqlite timestamp: "} + std::string{value});
        }
    }
}

} // namespace

std::chrono::system_clock::time_point ParseTimePoint(const std::string_view value)
{
    ValidateTimestampShape(value);

    const auto year = ParseIntComponent(value, 0, 4);
    const auto month = ParseIntComponent(value, 5, 2);
    const auto day = ParseIntComponent(value, 8, 2);
    const auto hour = ParseIntComponent(value, 11, 2);
    const auto minute = ParseIntComponent(value, 14, 2);
    const auto second = ParseIntComponent(value, 17, 2);

    const std::chrono::year_month_day date{
        std::chrono::year{year},
        std::chrono::month{static_cast<unsigned>(month)},
        std::chrono::day{static_cast<unsigned>(day)}
    };
    if (!date.ok()
        || hour < 0 || hour > 23
        || minute < 0 || minute > 59
        || second < 0 || second > 59)
    {
        throw std::runtime_error(
            std::string{"Failed to parse sqlite timestamp: "} + std::string{value});
    }

    return std::chrono::sys_days{date}
        + std::chrono::hours{hour}
        + std::chrono::minutes{minute}
        + std::chrono::seconds{second};
}

std::string SerializeTimePoint(const std::chrono::system_clock::time_point value)
{
    return std::format("{:%Y-%m-%dT%H:%M:%SZ}", std::chrono::floor<std::chrono::seconds>(value));
}

} // namespace InpxWebReader::Sqlite
