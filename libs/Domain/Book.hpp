#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "Domain/BookFormat.hpp"
#include "Domain/BookId.hpp"

namespace InpxWebReader::Domain {

struct SBookMetadata
{
    std::string TitleUtf8;
    std::vector<std::string> AuthorsUtf8;
    std::string Language;
    std::optional<std::string> SeriesUtf8 = std::nullopt;
    std::optional<double> SeriesIndex = std::nullopt;
    std::optional<std::string> PublisherUtf8 = std::nullopt;
    std::optional<int> Year = std::nullopt;
    std::optional<std::string> Isbn = std::nullopt;
    std::vector<std::string> TagsUtf8;
    std::vector<std::string> GenresUtf8;
    std::optional<std::string> DescriptionUtf8 = std::nullopt;
    std::optional<std::string> Identifier = std::nullopt;

};

struct SBookFileInfo
{
    EBookFormat Format = EBookFormat::Fb2;
    std::uintmax_t SizeBytes = 0;
};

struct SBook
{
    SBookId Id;
    SBookMetadata Metadata;
    SBookFileInfo File;
    std::optional<std::string> CoverPathUtf8 = std::nullopt;
    std::chrono::system_clock::time_point AddedAtUtc = {};
    bool IsAvailable = true;

};

} // namespace InpxWebReader::Domain
