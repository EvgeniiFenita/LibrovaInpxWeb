#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <exception>
#include <filesystem>
#include <optional>
#include <string>
#include <stop_token>
#include <utility>
#include <vector>

#include "TestFb2ParserSupport.hpp"
#include "TestWorkspace.hpp"

using namespace InpxWebReader::Tests::Fb2ParserSupport;

TEST_CASE("FB2 parser rejects malformed metadata", "[fb2-parsing]")
{
    CTestWorkspace sandbox("inpx-web-reader-fb2-parser-invalid");
    const std::filesystem::path fb2Path = sandbox.GetPath() / "invalid.fb2";

    WriteTextFile(
        fb2Path,
        R"(<?xml version="1.0" encoding="UTF-8"?>
<FictionBook>
  <description>
    <title-info>
      <lang>ru</lang>
    </title-info>
  </description>
</FictionBook>)");

    const InpxWebReader::Fb2Parsing::CFb2Parser parser;
    REQUIRE_THROWS(ParseFb2File(parser, fb2Path));
}

TEST_CASE("FB2 parser includes XML preview when the file is empty", "[fb2-parsing]")
{
    CTestWorkspace sandbox("inpx-web-reader-fb2-parser-empty");
    const std::filesystem::path fb2Path = sandbox.GetPath() / "empty.fb2";
    WriteTextFile(fb2Path, "");

    const InpxWebReader::Fb2Parsing::CFb2Parser parser;

    try
    {
        static_cast<void>(ParseFb2File(parser, fb2Path));
        FAIL("Expected parser to reject empty FB2 input.");
    }
    catch (const std::exception& error)
    {
        const std::string message = error.what();
        REQUIRE(message.find("size_bytes=0") != std::string::npos);
        REQUIRE(message.find("xml_preview=\"<empty>\"") != std::string::npos);
    }
}

TEST_CASE("FB2 parser bounds previews for large malformed metadata", "[fb2-parsing]")
{
    CTestWorkspace sandbox("inpx-web-reader-fb2-parser-large-invalid-preview");
    const std::filesystem::path fb2Path = sandbox.GetPath() / "large-invalid-preview.fb2";
    std::string fb2Text =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<FictionBook><description><title-info><book-title>Broken</book-title>"
        "<annotation><p>";
    fb2Text.append(4U * 1024U * 1024U, 'x');
    fb2Text.append(
        "</broken></annotation><lang>en</lang></title-info></description></FictionBook>");
    WriteTextFile(fb2Path, fb2Text);

    const InpxWebReader::Fb2Parsing::CFb2Parser parser;
    try
    {
        static_cast<void>(ParseFb2File(parser, fb2Path));
        FAIL("Expected parser to reject malformed FB2 metadata.");
    }
    catch (const std::exception& error)
    {
        const std::string message = error.what();
        REQUIRE(message.find("size_bytes=" + std::to_string(fb2Text.size())) != std::string::npos);
        REQUIRE(message.find("xml_preview=\"") != std::string::npos);
        REQUIRE(message.find("...\"]") != std::string::npos);
        REQUIRE(message.size() < 1024);
    }
}

TEST_CASE("FB2 parser bounds author diagnostics for a large title-info subtree", "[fb2-parsing]")
{
    CTestWorkspace sandbox("inpx-web-reader-fb2-parser-large-author-preview");
    const std::filesystem::path fb2Path = sandbox.GetPath() / "large-author-preview.fb2";
    std::string fb2Text =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<FictionBook><description><title-info>"
        "<genre>sf</genre><book-title>Large Anonymous Book</book-title><custom>";
    fb2Text.append(4U * 1024U * 1024U, 'x');
    fb2Text.append("</custom><lang>en</lang></title-info></description></FictionBook>");
    WriteTextFile(fb2Path, fb2Text);

    const InpxWebReader::Fb2Parsing::CFb2Parser parser;
    const auto parsedBook = ParseFb2File(parser, fb2Path);

    REQUIRE(parsedBook.Metadata.TitleUtf8 == "Large Anonymous Book");
    REQUIRE(parsedBook.Metadata.AuthorsUtf8 == std::vector<std::string>{"Аноним"});
    REQUIRE(parsedBook.ParserWarningCount >= 1);
}

TEST_CASE("FB2 parser admits metadata at its byte boundary and rejects the next byte", "[fb2-parsing][limits]")
{
    const std::string prefix =
        "<FictionBook><description><title-info><book-title>Boundary</book-title>"
        "<author><last-name>Author</last-name></author><lang>en</lang>"
        "<annotation><p>";
    const std::string suffix =
        "</p></annotation></title-info></description></FictionBook>";
    REQUIRE(prefix.size() + suffix.size() < InpxWebReader::Fb2Parsing::GMaxFb2MetadataXmlBytes);

    std::string atLimit = prefix;
    atLimit.append(
        static_cast<std::size_t>(InpxWebReader::Fb2Parsing::GMaxFb2MetadataXmlBytes)
            - prefix.size() - suffix.size(),
        'x');
    atLimit.append(suffix);
    REQUIRE(atLimit.size() == InpxWebReader::Fb2Parsing::GMaxFb2MetadataXmlBytes);
    REQUIRE_NOTHROW(
        InpxWebReader::Fb2Parsing::CFb2Parser{}.ParseBytes(
            atLimit,
            "metadata-at-limit.fb2"));

    atLimit.insert(atLimit.size() - suffix.size(), 1, 'x');
    REQUIRE_THROWS_WITH(
        InpxWebReader::Fb2Parsing::CFb2Parser{}.ParseBytes(
            std::move(atLimit),
            "metadata-over-limit.fb2"),
        Catch::Matchers::ContainsSubstring("byte metadata limit"));
}

TEST_CASE("FB2 parser rejects tiny-node metadata amplification before building a DOM", "[fb2-parsing][limits]")
{
    const std::string prefix =
        "<FictionBook><description><title-info><book-title>Nodes</book-title>"
        "<author><last-name>Author</last-name></author><lang>en</lang>";
    const std::string suffix = "</title-info></description></FictionBook>";
    const auto buildPayload = [&](const std::size_t tinyNodeCount) {
        std::string payload = prefix;
        payload.reserve(prefix.size() + tinyNodeCount * 4 + suffix.size());
        for (std::size_t index = 0; index < tinyNodeCount; ++index)
        {
            payload.append("<x/>");
        }
        payload.append(suffix);
        return payload;
    };

    const std::string nearLimit = buildPayload(
        static_cast<std::size_t>(InpxWebReader::Fb2Parsing::GMaxFb2MetadataXmlNodes) - 32);
    REQUIRE_NOTHROW(
        InpxWebReader::Fb2Parsing::CFb2Parser{}.ParseBytes(
            nearLimit,
            "nodes-near-limit.fb2"));

    REQUIRE_THROWS_WITH(
        InpxWebReader::Fb2Parsing::CFb2Parser{}.ParseBytes(
            buildPayload(static_cast<std::size_t>(InpxWebReader::Fb2Parsing::GMaxFb2MetadataXmlNodes)),
            "nodes-over-limit.fb2"),
        Catch::Matchers::ContainsSubstring("node limit"));
}

TEST_CASE("FB2 parser rejects attribute amplification independently of its node cap", "[fb2-parsing][limits]")
{
    std::string payload =
        "<FictionBook><description><title-info><book-title>Attributes</book-title>"
        "<author><last-name>Author</last-name></author><lang>en</lang>";
    const std::size_t repeatedNodes =
        static_cast<std::size_t>(InpxWebReader::Fb2Parsing::GMaxFb2MetadataXmlAttributes / 4) + 1;
    payload.reserve(payload.size() + repeatedNodes * 36);
    for (std::size_t index = 0; index < repeatedNodes; ++index)
    {
        payload.append("<x a='1' b='2' c='3' d='4'/>");
    }
    payload.append("</title-info></description></FictionBook>");

    REQUIRE_THROWS_WITH(
        InpxWebReader::Fb2Parsing::CFb2Parser{}.ParseBytes(
            payload,
            "attributes-over-limit.fb2"),
        Catch::Matchers::ContainsSubstring("attribute limit"));
}

TEST_CASE("FB2 parser uses 'Anonimous' fallback when all title-info author nodes are empty", "[fb2-parsing]")
{
    CTestWorkspace sandbox("inpx-web-reader-fb2-parser-empty-author");
    const std::filesystem::path fb2Path = sandbox.GetPath() / "empty-author.fb2";

    WriteTextFile(
        fb2Path,
        R"(<?xml version="1.0" encoding="UTF-8"?>
<FictionBook>
  <description>
    <title-info>
      <book-title>Magazine Issue</book-title>
      <author>
        <first-name></first-name>
        <last-name></last-name>
      </author>
      <lang>ru</lang>
    </title-info>
    <document-info>
      <author>
        <nickname>Scanner Team</nickname>
      </author>
    </document-info>
  </description>
</FictionBook>)");

    const InpxWebReader::Fb2Parsing::CFb2Parser parser;
    const auto result = ParseFb2File(parser, fb2Path);

    REQUIRE(result.Metadata.AuthorsUtf8 == std::vector<std::string>{"Аноним"});
    REQUIRE(result.Metadata.TitleUtf8 == "Magazine Issue");
}

TEST_CASE("FB2 parser uses 'Anonimous' fallback when title-info has no author node at all", "[fb2-parsing]")
{
    CTestWorkspace sandbox("inpx-web-reader-fb2-parser-no-author-node");
    const std::filesystem::path fb2Path = sandbox.GetPath() / "no-author-node.fb2";

    WriteTextFile(
        fb2Path,
        R"(<?xml version="1.0" encoding="UTF-8"?>
<FictionBook>
  <description>
    <title-info>
      <genre>religion</genre>
      <book-title>Anonymous Religious Text</book-title>
      <lang>ru</lang>
    </title-info>
  </description>
</FictionBook>)");

    const InpxWebReader::Fb2Parsing::CFb2Parser parser;
    const auto result = ParseFb2File(parser, fb2Path);

    REQUIRE(result.Metadata.AuthorsUtf8 == std::vector<std::string>{"Аноним"});
    REQUIRE(result.Metadata.TitleUtf8 == "Anonymous Religious Text");
}


TEST_CASE("FB2 parser decodes windows-1251 metadata as UTF-8", "[fb2-parsing]")
{
    CTestWorkspace sandbox("inpx-web-reader-fb2-parser-cp1251");
    const std::filesystem::path fb2Path = sandbox.GetPath() / "cp1251.fb2";

    const std::string fb2Text =
        "<?xml version=\"1.0\" encoding=\"windows-1251\"?>\r\n"
        "<FictionBook>\r\n"
        "  <description>\r\n"
        "    <title-info>\r\n"
        "      <book-title>\xC0\xED\xE3\xE5\xEB\xFB \xE8 \xE4\xE5\xEC\xEE\xED\xFB</book-title>\r\n"
        "      <author>\r\n"
        "        <first-name>\xC4\xFD\xED</first-name>\r\n"
        "        <last-name>\xC1\xF0\xE0\xF3\xED</last-name>\r\n"
        "      </author>\r\n"
        "      <lang>ru</lang>\r\n"
        "    </title-info>\r\n"
        "  </description>\r\n"
        "</FictionBook>";

    WriteTextFile(fb2Path, fb2Text);

    const InpxWebReader::Fb2Parsing::CFb2Parser parser;
    const InpxWebReader::Domain::SParsedBook parsedBook = ParseFb2File(parser, fb2Path);

    REQUIRE(parsedBook.Metadata.TitleUtf8 == "Ангелы и демоны");
    REQUIRE(parsedBook.Metadata.AuthorsUtf8 == std::vector<std::string>({"Дэн Браун"}));
    REQUIRE(parsedBook.Metadata.Language == "ru");
}

TEST_CASE("FB2 parser accepts the CP1251 alias and whitespace around the encoding separator", "[fb2-parsing]")
{
    CTestWorkspace sandbox("inpx-web-reader-fb2-parser-cp1251-alias");
    const std::filesystem::path fb2Path = sandbox.GetPath() / "cp1251-alias.fb2";
    const std::string fb2Text =
        "<?xml version=\"1.0\" encoding \t=\t 'Cp1251'?>"
        "<FictionBook><description><title-info>"
        "<book-title>\xCA\xED\xE8\xE3\xE0</book-title>"
        "<author><first-name>\xCF\xE5\xF2\xF0</first-name>"
        "<last-name>\xCF\xE5\xF2\xF0\xEE\xE2</last-name></author>"
        "<lang>ru</lang></title-info></description><body/></FictionBook>";
    WriteTextFile(fb2Path, fb2Text);

    const InpxWebReader::Fb2Parsing::CFb2Parser parser;
    const auto parsedBook = ParseFb2File(parser, fb2Path);

    REQUIRE(parsedBook.Metadata.TitleUtf8 == "Книга");
    REQUIRE(parsedBook.Metadata.AuthorsUtf8 == std::vector<std::string>{"Петр Петров"});
}

TEST_CASE("FB2 parser decodes explicitly declared CP866 metadata as UTF-8", "[fb2-parsing][unicode]")
{
    CTestWorkspace sandbox("inpx-web-reader-fb2-parser-cp866");
    const std::filesystem::path fb2Path = sandbox.GetPath() / "cp866.fb2";

    const std::string fb2Text =
        "<?xml version=\"1.0\" encoding=\"CP866\"?>"
        "<FictionBook><description><title-info>"
        "<book-title>\x8A\xAD\xA8\xA3\xA0</book-title>"
        "<author><first-name>\x8F\xA5\xE2\xE0</first-name>"
        "<last-name>\x8F\xA5\xE2\xE0\xAE\xA2</last-name></author>"
        "<lang>ru</lang></title-info></description><body/></FictionBook>";
    WriteTextFile(fb2Path, fb2Text);

    const InpxWebReader::Fb2Parsing::CFb2Parser parser;
    const auto parsedBook = ParseFb2File(parser, fb2Path);

    REQUIRE(parsedBook.Metadata.TitleUtf8 == "Книга");
    REQUIRE(parsedBook.Metadata.AuthorsUtf8 == std::vector<std::string>{"Петр Петров"});
    REQUIRE(parsedBook.Metadata.Language == "ru");
}

TEST_CASE("FB2 parser accepts the IBM866 encoding alias", "[fb2-parsing][unicode]")
{
    CTestWorkspace sandbox("inpx-web-reader-fb2-parser-ibm866");
    const std::filesystem::path fb2Path = sandbox.GetPath() / "ibm866.fb2";
    const std::string fb2Text =
        "<?xml version='1.0' encoding='IbM866'?>"
        "<FictionBook><description><title-info>"
        "<book-title>\x8A\xAD\xA8\xA3\xA0</book-title>"
        "<author><first-name>\x8F\xA5\xE2\xE0</first-name>"
        "<last-name>\x8F\xA5\xE2\xE0\xAE\xA2</last-name></author>"
        "<lang>ru</lang></title-info></description><body/></FictionBook>";
    WriteTextFile(fb2Path, fb2Text);

    const InpxWebReader::Fb2Parsing::CFb2Parser parser;
    const auto parsedBook = ParseFb2File(parser, fb2Path);

    REQUIRE(parsedBook.Metadata.TitleUtf8 == "Книга");
    REQUIRE(parsedBook.Metadata.AuthorsUtf8 == std::vector<std::string>{"Петр Петров"});
}

TEST_CASE("FB2 parser detects undeclared CP866 metadata", "[fb2-parsing][unicode]")
{
    CTestWorkspace sandbox("inpx-web-reader-fb2-parser-undeclared-cp866");
    const std::filesystem::path fb2Path = sandbox.GetPath() / "undeclared-cp866.fb2";
    const std::string fb2Text =
        "<FictionBook><description><title-info>"
        "<book-title>\x8A\xAD\xA8\xA3\xA0</book-title>"
        "<author><first-name>\x8F\xA5\xE2\xE0</first-name>"
        "<last-name>\x8F\xA5\xE2\xE0\xAE\xA2</last-name></author>"
        "<lang>ru</lang></title-info></description><body/></FictionBook>";
    WriteTextFile(fb2Path, fb2Text);

    const InpxWebReader::Fb2Parsing::CFb2Parser parser;
    const auto parsedBook = ParseFb2File(parser, fb2Path);

    REQUIRE(parsedBook.Metadata.TitleUtf8 == "Книга");
    REQUIRE(parsedBook.Metadata.AuthorsUtf8 == std::vector<std::string>{"Петр Петров"});
}

TEST_CASE("FB2 parser decodes windows-1251 metadata when XML declaration uses single quotes", "[fb2-parsing]")
{
    CTestWorkspace sandbox("inpx-web-reader-fb2-parser-cp1251-single-quotes");
    const std::filesystem::path fb2Path = sandbox.GetPath() / "cp1251-single-quotes.fb2";

    const std::string fb2Text =
        "<?xml version='1.0' encoding='windows-1251'?>\r\n"
        "<FictionBook>\r\n"
        "  <description>\r\n"
        "    <title-info>\r\n"
        "      <book-title>\xD2\xF0\xF3\xE4\xED\xEE \xE1\xFB\xF2\xFC \xE1\xEE\xE3\xEE\xEC</book-title>\r\n"
        "      <author>\r\n"
        "        <first-name>\xC0\xF0\xEA\xE0\xE4\xE8\xE9</first-name>\r\n"
        "        <last-name>\xD1\xF2\xF0\xF3\xE3\xE0\xF6\xEA\xE8\xE9</last-name>\r\n"
        "      </author>\r\n"
        "      <lang>ru</lang>\r\n"
        "    </title-info>\r\n"
        "  </description>\r\n"
        "</FictionBook>";

    WriteTextFile(fb2Path, fb2Text);

    const InpxWebReader::Fb2Parsing::CFb2Parser parser;
    const InpxWebReader::Domain::SParsedBook parsedBook = ParseFb2File(parser, fb2Path);

    REQUIRE(parsedBook.Metadata.TitleUtf8 == "Трудно быть богом");
    REQUIRE(parsedBook.Metadata.AuthorsUtf8 == std::vector<std::string>({"Аркадий Стругацкий"}));
    REQUIRE(parsedBook.Metadata.Language == "ru");
}

TEST_CASE("FB2 parser decodes windows-1251 metadata when XML declaration uses uppercase encoding", "[fb2-parsing]")
{
    CTestWorkspace sandbox("inpx-web-reader-fb2-parser-cp1251-uppercase");
    const std::filesystem::path fb2Path = sandbox.GetPath() / "cp1251-uppercase.fb2";

    const std::string fb2Text =
        "<?xml version=\"1.0\" encoding=\"WINDOWS-1251\"?>\r\n"
        "<FictionBook>\r\n"
        "  <description>\r\n"
        "    <title-info>\r\n"
        "      <book-title>\xCF\xE8\xEA\xED\xE8\xEA \xED\xE0 \xEE\xE1\xEE\xF7\xE8\xED\xE5</book-title>\r\n"
        "      <author>\r\n"
        "        <first-name>\xC0\xF0\xEA\xE0\xE4\xE8\xE9</first-name>\r\n"
        "        <last-name>\xD1\xF2\xF0\xF3\xE3\xE0\xF6\xEA\xE8\xE9</last-name>\r\n"
        "      </author>\r\n"
        "      <lang>ru</lang>\r\n"
        "    </title-info>\r\n"
        "  </description>\r\n"
        "</FictionBook>";

    WriteTextFile(fb2Path, fb2Text);

    const InpxWebReader::Fb2Parsing::CFb2Parser parser;
    const InpxWebReader::Domain::SParsedBook parsedBook = ParseFb2File(parser, fb2Path);

    REQUIRE(parsedBook.Metadata.TitleUtf8 == "Пикник на обочине");
    REQUIRE(parsedBook.Metadata.AuthorsUtf8 == std::vector<std::string>({"Аркадий Стругацкий"}));
    REQUIRE(parsedBook.Metadata.Language == "ru");
}

TEST_CASE("FB2 parser falls back to lossy windows-1251 decoding for undefined bytes", "[fb2-parsing]")
{
    CTestWorkspace sandbox("inpx-web-reader-fb2-parser-cp1251-lossy");
    const std::filesystem::path fb2Path = sandbox.GetPath() / "cp1251-lossy.fb2";

    const std::string fb2Text =
        "<?xml version=\"1.0\" encoding=\"windows-1251\"?>\r\n"
        "<FictionBook>\r\n"
        "  <description>\r\n"
        "    <title-info>\r\n"
        "      <book-title>\xCA\xED\xE8\xE3\xE0\x98</book-title>\r\n"
        "      <author>\r\n"
        "        <first-name>\xC0\xE2\xF2\xEE\xF0</first-name>\r\n"
        "      </author>\r\n"
        "      <lang>ru</lang>\r\n"
        "    </title-info>\r\n"
        "  </description>\r\n"
        "</FictionBook>";

    WriteTextFile(fb2Path, fb2Text);

    const InpxWebReader::Fb2Parsing::CFb2Parser parser;
    const InpxWebReader::Domain::SParsedBook parsedBook = ParseFb2File(parser, fb2Path);

    REQUIRE(parsedBook.Metadata.TitleUtf8 == std::string{"Книга"} + "\xEF\xBF\xBD");
    REQUIRE(parsedBook.Metadata.AuthorsUtf8 == std::vector<std::string>({"Автор"}));
    REQUIRE(parsedBook.Metadata.Language == "ru");
    REQUIRE(parsedBook.ParserWarningCount == 1);
}

TEST_CASE("FB2 parser accepts dot-separated sequence numbers independently from process locale", "[fb2-parsing]")
{
    CTestWorkspace sandbox("inpx-web-reader-fb2-parser-sequence-number");
    const std::filesystem::path fb2Path = sandbox.GetPath() / "sequence-number.fb2";

    WriteTextFile(
        fb2Path,
        R"(<?xml version="1.0" encoding="UTF-8"?>
<FictionBook>
  <description>
    <title-info>
      <book-title>Test</book-title>
      <author>
        <first-name>Arkady</first-name>
        <last-name>Strugatsky</last-name>
      </author>
      <lang>ru</lang>
      <sequence name="Cycle" number="1.5" />
    </title-info>
  </description>
</FictionBook>)");

    const InpxWebReader::Fb2Parsing::CFb2Parser parser;
    const InpxWebReader::Domain::SParsedBook parsedBook = ParseFb2File(parser, fb2Path);

    REQUIRE(parsedBook.Metadata.SeriesUtf8 == std::optional<std::string>{"Cycle"});
    REQUIRE(parsedBook.Metadata.SeriesIndex == std::optional<double>{1.5});
}

TEST_CASE("FB2 parser strips UTF-8 BOM and parses correctly", "[fb2-parsing]")
{
    CTestWorkspace sandbox("inpx-web-reader-fb2-parser-bom");
    const std::filesystem::path fb2Path = sandbox.GetPath() / "bom.fb2";

    // UTF-8 BOM (EF BB BF) prepended to a valid UTF-8 FB2 file.
    // Causes "Error parsing document declaration" in pugixml without BOM stripping.
    std::string fb2Text;
    fb2Text += '\xEF';
    fb2Text += '\xBB';
    fb2Text += '\xBF';
    fb2Text +=
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<FictionBook>\n"
        "  <description>\n"
        "    <title-info>\n"
        "      <book-title>\xD0\x9C\xD0\xB0\xD1\x81\xD1\x82\xD0\xB5\xD1\x80 \xD0\xB8 \xD0\x9C\xD0\xB0\xD1\x80\xD0\xB3\xD0\xB0\xD1\x80\xD0\xB8\xD1\x82\xD0\xB0</book-title>\n"
        "      <author>\n"
        "        <first-name>\xD0\x9C\xD0\xB8\xD1\x85\xD0\xB0\xD0\xB8\xD0\xBB</first-name>\n"
        "        <last-name>\xD0\x91\xD1\x83\xD0\xBB\xD0\xB3\xD0\xB0\xD0\xBA\xD0\xBE\xD0\xB2</last-name>\n"
        "      </author>\n"
        "      <lang>ru</lang>\n"
        "    </title-info>\n"
        "  </description>\n"
        "</FictionBook>";

    WriteTextFile(fb2Path, fb2Text);

    const InpxWebReader::Fb2Parsing::CFb2Parser parser;
    const InpxWebReader::Domain::SParsedBook parsedBook = ParseFb2File(parser, fb2Path);

    REQUIRE(parsedBook.Metadata.TitleUtf8 == "Мастер и Маргарита");
    REQUIRE(parsedBook.Metadata.AuthorsUtf8 == std::vector<std::string>{"Михаил Булгаков"});
    REQUIRE(parsedBook.Metadata.Language == "ru");
}

TEST_CASE("FB2 parser honors a requested cooperative cancellation", "[fb2][cancellation]")
{
    std::stop_source stopSource;
    stopSource.request_stop();
    const InpxWebReader::Fb2Parsing::CFb2Parser parser;

    REQUIRE_THROWS_WITH(
        parser.ParseBytes(
            "<FictionBook><description><title-info><book-title>Title</book-title></title-info></description>"
            "</FictionBook>",
            "cancelled.fb2",
            {.StopToken = stopSource.get_token()}),
        "FB2 parsing cancelled.");
}
