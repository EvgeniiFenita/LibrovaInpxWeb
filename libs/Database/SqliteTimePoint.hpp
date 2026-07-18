#pragma once

#include <chrono>
#include <string>
#include <string_view>

namespace InpxWebReader::Sqlite {

[[nodiscard]] std::chrono::system_clock::time_point ParseTimePoint(std::string_view value);
[[nodiscard]] std::string SerializeTimePoint(std::chrono::system_clock::time_point value);

} // namespace InpxWebReader::Sqlite
