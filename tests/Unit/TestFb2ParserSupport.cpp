#include "TestFb2ParserSupport.hpp"

#include <fstream>
#include <iterator>
#include <stdexcept>
#include <utility>

#include "Foundation/UnicodeConversion.hpp"
#include "TestWorkspace.hpp"

namespace InpxWebReader::Tests::Fb2ParserSupport {

void WriteTextFile(const std::filesystem::path& path, const std::string& text)
{
    std::ofstream output(path, std::ios::binary);
    output << text;
}

[[nodiscard]] InpxWebReader::Domain::SParsedBook ParseFb2File(
    const InpxWebReader::Fb2Parsing::CFb2Parser& parser,
    const std::filesystem::path& path,
    const std::string_view logicalSourceLabel,
    const InpxWebReader::Domain::SBookParseOptions& options)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        throw std::runtime_error("Failed to open FB2 test fixture.");
    }
    std::string rawBytes{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    return parser.ParseBytes(
        std::move(rawBytes),
        logicalSourceLabel.empty() ? InpxWebReader::Unicode::PathToUtf8(path) : std::string{logicalSourceLabel},
        options);
}

[[nodiscard]] std::string SampleMetadataFb2()
{
    return R"(<?xml version="1.0" encoding="UTF-8"?>
<FictionBook xmlns:l="http://www.w3.org/1999/xlink">
  <description>
    <title-info>
      <book-title>Пикник на обочине</book-title>
      <genre>science_fiction</genre>
      <genre> adventure </genre>
      <author>
        <first-name>Аркадий</first-name>
        <last-name>Стругацкий</last-name>
      </author>
      <author>
        <first-name>Борис</first-name>
        <last-name>Стругацкий</last-name>
      </author>
      <lang>ru</lang>
      <sequence name="Миры" number="1.5" />
      <annotation>
        <p>Классика советской фантастики</p>
      </annotation>
      <coverpage>
        <image l:href="#cover-image"/>
      </coverpage>
    </title-info>
    <publish-info>
      <publisher>АСТ</publisher>
      <year>1972</year>
      <isbn>978-5-17-090334-4</isbn>
    </publish-info>
    <document-info>
      <id>roadside-picnic-fb2</id>
    </document-info>
  </description>
  <body>
    <section><p>Test body</p></section>
  </body>
  <binary id="cover-image" content-type="image/jpeg">ASNF</binary>
</FictionBook>)";
}

[[nodiscard]] InpxWebReader::Domain::SParsedBook ParseSampleMetadataFb2(std::string_view workspaceName)
{
    CTestWorkspace sandbox(workspaceName);
    const std::filesystem::path fb2Path = sandbox.GetPath() / "sample.fb2";
    WriteTextFile(fb2Path, SampleMetadataFb2());

    const InpxWebReader::Fb2Parsing::CFb2Parser parser;
    return ParseFb2File(parser, fb2Path);
}

} // namespace InpxWebReader::Tests::Fb2ParserSupport
