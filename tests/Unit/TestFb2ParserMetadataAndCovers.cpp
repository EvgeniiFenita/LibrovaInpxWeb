#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "TestFb2ParserSupport.hpp"
#include "TestWorkspace.hpp"

using namespace InpxWebReader::Tests::Fb2ParserSupport;

TEST_CASE("FB2 parser extracts core metadata fields", "[fb2-parsing]")
{
    const InpxWebReader::Domain::SParsedBook parsedBook =
        ParseSampleMetadataFb2("inpx-web-reader-fb2-parser-core-metadata");

    REQUIRE(parsedBook.Metadata.TitleUtf8 == "Пикник на обочине");
    REQUIRE(parsedBook.Metadata.AuthorsUtf8 == std::vector<std::string>({"Аркадий Стругацкий", "Борис Стругацкий"}));
    REQUIRE(parsedBook.Metadata.Language == "ru");
    REQUIRE(parsedBook.Metadata.TagsUtf8.empty());
}

TEST_CASE("FB2 parser extracts metadata and cover directly from bytes", "[fb2-parsing]")
{
    const InpxWebReader::Fb2Parsing::CFb2Parser parser;
    const InpxWebReader::Domain::SParsedBook parsedBook = parser.ParseBytes(
        SampleMetadataFb2(),
        "memory-sample.fb2");

    REQUIRE(parsedBook.Metadata.TitleUtf8 == "Пикник на обочине");
    REQUIRE(parsedBook.Metadata.AuthorsUtf8 == std::vector<std::string>({"Аркадий Стругацкий", "Борис Стругацкий"}));
    REQUIRE(parsedBook.Metadata.Language == "ru");
    REQUIRE(parsedBook.CoverExtension == std::optional<std::string>{"jpg"});
    REQUIRE(parsedBook.CoverBytes == std::vector<std::byte>({std::byte{0x01}, std::byte{0x23}, std::byte{0x45}}));
}

TEST_CASE("FB2 parser byte input uses metadata-only parsing with XML-decoded cover binary ids", "[fb2-parsing]")
{
    const InpxWebReader::Fb2Parsing::CFb2Parser parser;
    const InpxWebReader::Domain::SParsedBook parsedBook = parser.ParseBytes(
        R"(<?xml version="1.0" encoding="UTF-8"?>
<FictionBook>
  <description>
    <title-info>
      <book-title>Escaped Cover</book-title>
      <author>
        <first-name>Test</first-name>
        <last-name>Author</last-name>
      </author>
      <lang>en</lang>
      <coverpage>
        <image href="#cover&amp;image.png"/>
      </coverpage>
    </title-info>
  </description>
  <body>
    <section>
      <p>Body contains malformed XML <...> that catalog parsing must ignore.</p>
    </section>
  </body>
  <binary id="cover&amp;image.png" content-type="image/png">AQID</binary>
</FictionBook>)",
        "memory-escaped-cover.fb2");

    REQUIRE(parsedBook.Metadata.TitleUtf8 == "Escaped Cover");
    REQUIRE(parsedBook.Metadata.AuthorsUtf8 == std::vector<std::string>{"Test Author"});
    REQUIRE(parsedBook.Metadata.Language == "en");
    REQUIRE(parsedBook.CoverExtension == std::optional<std::string>{"png"});
    REQUIRE(parsedBook.CoverBytes == std::vector<std::byte>({std::byte{0x01}, std::byte{0x02}, std::byte{0x03}}));
}

TEST_CASE("FB2 parser accepts greater-than signs inside XML attribute values", "[fb2-parsing]")
{
    const InpxWebReader::Fb2Parsing::CFb2Parser parser;
    const InpxWebReader::Domain::SParsedBook parsedBook = parser.ParseBytes(
        R"(<?xml version="1.0" encoding="UTF-8"?>
<FictionBook data-note="one > zero">
  <description>
    <title-info>
      <book-title>Quoted Greater Than</book-title>
      <author>
        <first-name>Test</first-name>
        <last-name>Author</last-name>
      </author>
      <lang>en</lang>
      <coverpage>
        <image href="#cover.png"/>
      </coverpage>
    </title-info>
  </description>
  <binary data-note='one > zero' id="cover.png" content-type="image/png">AQID</binary>
</FictionBook>)",
        "quoted-greater-than.fb2");

    REQUIRE(parsedBook.Metadata.TitleUtf8 == "Quoted Greater Than");
    REQUIRE(parsedBook.CoverExtension == std::optional<std::string>{"png"});
    REQUIRE(parsedBook.CoverBytes == std::vector<std::byte>({std::byte{0x01}, std::byte{0x02}, std::byte{0x03}}));
    REQUIRE_FALSE(parsedBook.CoverDiagnosticMessage.has_value());
}

TEST_CASE("FB2 parser rejects non-canonical base64 cover padding", "[fb2-parsing]")
{
    const InpxWebReader::Fb2Parsing::CFb2Parser parser;
    const std::vector<std::string_view> invalidEncodedValues{
        "AA=A",
        "AA==AAAA",
        "AB==",
        "AAF="
    };

    for (const std::string_view encodedValue : invalidEncodedValues)
    {
        CAPTURE(encodedValue);
        const std::string fb2 = std::string{R"(<?xml version="1.0" encoding="UTF-8"?>
<FictionBook>
  <description>
    <title-info>
      <book-title>Invalid Base64</book-title>
      <author><first-name>Test</first-name><last-name>Author</last-name></author>
      <lang>en</lang>
      <coverpage><image href="#cover.png"/></coverpage>
    </title-info>
  </description>
  <binary id="cover.png" content-type="image/png">)"}
            + std::string{encodedValue}
            + R"(</binary>
</FictionBook>)";

        const InpxWebReader::Domain::SParsedBook parsedBook = parser.ParseBytes(fb2, "invalid-base64.fb2");

        REQUIRE(parsedBook.CoverBytes.empty());
        REQUIRE(parsedBook.CoverDiagnosticMessage == std::optional<std::string>{"base64-decode-failed"});
    }
}

TEST_CASE("FB2 parser accepts canonical padded base64 covers", "[fb2-parsing]")
{
    const InpxWebReader::Fb2Parsing::CFb2Parser parser;
    const auto parseCover = [&parser](const std::string_view encodedValue) {
        const std::string fb2 = std::string{R"(<?xml version="1.0" encoding="UTF-8"?>
<FictionBook>
  <description>
    <title-info>
      <book-title>Padded Base64</book-title>
      <author><first-name>Test</first-name><last-name>Author</last-name></author>
      <lang>en</lang>
      <coverpage><image href="#cover.png"/></coverpage>
    </title-info>
  </description>
  <binary id="cover.png" content-type="image/png">)"}
            + std::string{encodedValue}
            + R"(</binary>
</FictionBook>)";
        return parser.ParseBytes(fb2, "padded-base64.fb2");
    };

    REQUIRE(parseCover("AQ==").CoverBytes == std::vector<std::byte>{std::byte{0x01}});
    REQUIRE(parseCover("AQI=").CoverBytes == std::vector<std::byte>{std::byte{0x01}, std::byte{0x02}});
}

TEST_CASE("FB2 parser ignores XML-like text inside comments before the root element", "[fb2-parsing]")
{
    const InpxWebReader::Fb2Parsing::CFb2Parser parser;
    const InpxWebReader::Domain::SParsedBook parsedBook = parser.ParseBytes(
        R"(<?xml version="1.0" encoding="UTF-8"?>
<!-- Look <document-info> for credits; ignore <FictionBook><description><title-info><book-title>Comment Title</book-title></title-info></description></FictionBook> fragments -->
<FictionBook xmlns="http://www.gribuser.ru/xml/fictionbook/2.0">
  <description>
    <title-info>
      <book-title>Commented Prelude</book-title>
      <author>
        <first-name>Test</first-name>
        <last-name>Author</last-name>
      </author>
      <lang>en</lang>
    </title-info>
  </description>
  <body>
    <section><p>Body text.</p></section>
  </body>
</FictionBook>)",
        "commented-prelude.fb2");

    REQUIRE(parsedBook.Metadata.TitleUtf8 == "Commented Prelude");
    REQUIRE(parsedBook.Metadata.AuthorsUtf8 == std::vector<std::string>{"Test Author"});
    REQUIRE(parsedBook.Metadata.Language == "en");
}

TEST_CASE("FB2 parser rejects missing book title unless a fallback is provided", "[fb2-parsing]")
{
    CTestWorkspace sandbox("inpx-web-reader-fb2-parser-missing-title");
    const std::filesystem::path fb2Path = sandbox.GetPath() / "missing-title.fb2";

    WriteTextFile(
        fb2Path,
        R"(<?xml version="1.0" encoding="UTF-8"?>
<FictionBook>
  <description>
    <title-info>
      <genre>science_fiction</genre>
      <author>
        <first-name>Test</first-name>
        <last-name>Author</last-name>
      </author>
      <lang>ru</lang>
    </title-info>
  </description>
</FictionBook>)");

    const InpxWebReader::Fb2Parsing::CFb2Parser parser;
    REQUIRE_THROWS_WITH(
        ParseFb2File(parser, fb2Path),
        Catch::Matchers::ContainsSubstring("Missing required FB2 node: book-title"));

    const InpxWebReader::Domain::SParsedBook parsedBook = ParseFb2File(parser,
        fb2Path,
        {},
        {.MissingTitleFallbackUtf8 = "Catalog Title"});

    REQUIRE(parsedBook.Metadata.TitleUtf8 == "Catalog Title");
    REQUIRE(parsedBook.Metadata.AuthorsUtf8 == std::vector<std::string>{"Test Author"});
    REQUIRE(parsedBook.Metadata.Language == "ru");
    REQUIRE(parsedBook.Metadata.GenresUtf8 == std::vector<std::string>{"Science Fiction"});
}

TEST_CASE("FB2 parser extracts genres, series, and publish metadata", "[fb2-parsing]")
{
    const InpxWebReader::Domain::SParsedBook parsedBook =
        ParseSampleMetadataFb2("inpx-web-reader-fb2-parser-extended-metadata");

    REQUIRE(parsedBook.Metadata.GenresUtf8 == std::vector<std::string>({"Science Fiction", "Adventure"}));
    REQUIRE(parsedBook.Metadata.SeriesUtf8 == std::optional<std::string>{"Миры"});
    REQUIRE(parsedBook.Metadata.SeriesIndex == std::optional<double>{1.5});
    REQUIRE(parsedBook.Metadata.DescriptionUtf8 == std::optional<std::string>{"Классика советской фантастики"});
    REQUIRE(parsedBook.Metadata.PublisherUtf8 == std::optional<std::string>{"АСТ"});
    REQUIRE(parsedBook.Metadata.Year == std::optional<int>{1972});
    REQUIRE(parsedBook.Metadata.Isbn == std::optional<std::string>{"978-5-17-090334-4"});
    REQUIRE(parsedBook.Metadata.Identifier == std::optional<std::string>{"roadside-picnic-fb2"});
}

TEST_CASE("FB2 parser extracts embedded cover bytes and extension", "[fb2-parsing]")
{
    const InpxWebReader::Domain::SParsedBook parsedBook =
        ParseSampleMetadataFb2("inpx-web-reader-fb2-parser-cover-metadata");

    REQUIRE(parsedBook.CoverExtension == std::optional<std::string>{"jpg"});
    REQUIRE(parsedBook.CoverBytes == std::vector<std::byte>({std::byte{0x01}, std::byte{0x23}, std::byte{0x45}}));
}

TEST_CASE("FB2 parser extracts embedded cover from xlink href", "[fb2-parsing]")
{
    CTestWorkspace sandbox("inpx-web-reader-fb2-parser-xlink-cover");
    const std::filesystem::path fb2Path = sandbox.GetPath() / "sample.fb2";

    WriteTextFile(
        fb2Path,
        R"(<?xml version="1.0" encoding="UTF-8"?>
<FictionBook xmlns:xlink="http://www.w3.org/1999/xlink">
  <description>
    <title-info>
      <book-title>Cover Regression</book-title>
      <author>
        <first-name>Test</first-name>
        <last-name>Author</last-name>
      </author>
      <lang>en</lang>
      <coverpage>
        <image xlink:href="#cover.png"/>
      </coverpage>
    </title-info>
  </description>
  <binary id="cover.png" content-type="image/png">AQID</binary>
</FictionBook>)");

    const InpxWebReader::Fb2Parsing::CFb2Parser parser;
    const InpxWebReader::Domain::SParsedBook parsedBook = ParseFb2File(parser, fb2Path);

    REQUIRE(parsedBook.Metadata.TitleUtf8 == "Cover Regression");
    REQUIRE(parsedBook.CoverExtension == std::optional<std::string>{"png"});
    REQUIRE(parsedBook.CoverBytes == std::vector<std::byte>({std::byte{0x01}, std::byte{0x02}, std::byte{0x03}}));
    REQUIRE_FALSE(parsedBook.CoverDiagnosticMessage.has_value());
}

TEST_CASE("FB2 parser extracts embedded cover from plain href", "[fb2-parsing]")
{
    CTestWorkspace sandbox("inpx-web-reader-fb2-parser-plain-href-cover");
    const std::filesystem::path fb2Path = sandbox.GetPath() / "sample.fb2";

    WriteTextFile(
        fb2Path,
        R"(<?xml version="1.0" encoding="UTF-8"?>
<FictionBook>
  <description>
    <title-info>
      <book-title>Plain Cover Regression</book-title>
      <author>
        <first-name>Test</first-name>
        <last-name>Author</last-name>
      </author>
      <lang>en</lang>
      <coverpage>
        <image href="#cover.jpg"/>
      </coverpage>
    </title-info>
  </description>
  <binary id="cover.jpg" content-type="image/jpeg">AQID</binary>
</FictionBook>)");

    const InpxWebReader::Fb2Parsing::CFb2Parser parser;
    const InpxWebReader::Domain::SParsedBook parsedBook = ParseFb2File(parser, fb2Path);

    REQUIRE(parsedBook.Metadata.TitleUtf8 == "Plain Cover Regression");
    REQUIRE(parsedBook.CoverExtension == std::optional<std::string>{"jpg"});
    REQUIRE(parsedBook.CoverBytes == std::vector<std::byte>({std::byte{0x01}, std::byte{0x02}, std::byte{0x03}}));
    REQUIRE_FALSE(parsedBook.CoverDiagnosticMessage.has_value());
}

TEST_CASE("FB2 parser extracts cover extension from Cyrillic binary id without filesystem path conversion", "[fb2-parsing]")
{
    CTestWorkspace sandbox("inpx-web-reader-fb2-parser-cyrillic-binary-id");
    const std::filesystem::path fb2Path = sandbox.GetPath() / "sample.fb2";

    WriteTextFile(
        fb2Path,
        R"(<?xml version="1.0" encoding="UTF-8"?>
<FictionBook xmlns:l="http://www.w3.org/1999/xlink">
  <description>
    <title-info>
      <book-title>Cyrillic Binary Id</book-title>
      <author>
        <first-name>Test</first-name>
        <last-name>Author</last-name>
      </author>
      <lang>en</lang>
      <coverpage>
        <image l:href="#обложка.PNG"/>
      </coverpage>
    </title-info>
  </description>
  <binary id="обложка.PNG">AQID</binary>
</FictionBook>)");

    const InpxWebReader::Fb2Parsing::CFb2Parser parser;
    const InpxWebReader::Domain::SParsedBook parsedBook = ParseFb2File(parser, fb2Path);

    REQUIRE(parsedBook.Metadata.TitleUtf8 == "Cyrillic Binary Id");
    REQUIRE(parsedBook.CoverExtension == std::optional<std::string>{"png"});
    REQUIRE(parsedBook.CoverBytes == std::vector<std::byte>({std::byte{0x01}, std::byte{0x02}, std::byte{0x03}}));
    REQUIRE_FALSE(parsedBook.CoverDiagnosticMessage.has_value());
}

TEST_CASE("FB2 parser skips embedded cover extraction when disabled", "[fb2-parsing]")
{
    CTestWorkspace sandbox("inpx-web-reader-fb2-parser-no-cover-extract");
    const std::filesystem::path fb2Path = sandbox.GetPath() / "sample.fb2";

    WriteTextFile(
        fb2Path,
        R"(<?xml version="1.0" encoding="UTF-8"?>
<FictionBook xmlns:l="http://www.w3.org/1999/xlink">
  <description>
    <title-info>
      <book-title>Roadside Picnic</book-title>
      <author>
        <first-name>Arkady</first-name>
        <last-name>Strugatsky</last-name>
      </author>
      <lang>en</lang>
      <coverpage>
        <image l:href="#cover-image"/>
      </coverpage>
    </title-info>
  </description>
  <binary id="cover-image" content-type="image/jpeg">ASNF</binary>
</FictionBook>)");

    const InpxWebReader::Fb2Parsing::CFb2Parser parser;
    const InpxWebReader::Domain::SParsedBook parsedBook = ParseFb2File(parser,
        fb2Path,
        {},
        {.ExtractCover = false});

    REQUIRE(parsedBook.Metadata.TitleUtf8 == "Roadside Picnic");
    REQUIRE_FALSE(parsedBook.CoverExtension.has_value());
    REQUIRE(parsedBook.CoverBytes.empty());
    REQUIRE_FALSE(parsedBook.CoverDiagnosticMessage.has_value());
}
