#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "Parsing/Fb2Parser.hpp"

namespace InpxWebReader::Tests::Fb2ParserSupport {

void WriteTextFile(const std::filesystem::path& path, const std::string& text);

[[nodiscard]] Domain::SParsedBook ParseFb2File(
    const Fb2Parsing::CFb2Parser& parser,
    const std::filesystem::path& path,
    std::string_view logicalSourceLabel = {},
    const Domain::SBookParseOptions& options = {});

[[nodiscard]] std::string SampleMetadataFb2();

[[nodiscard]] Domain::SParsedBook ParseSampleMetadataFb2(std::string_view workspaceName);

} // namespace InpxWebReader::Tests::Fb2ParserSupport
