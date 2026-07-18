#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include "TestFb2ParserSupport.hpp"
#include "TestWorkspace.hpp"

using namespace InpxWebReader::Tests::Fb2ParserSupport;

TEST_CASE("FB2 parser falls back to CP1251 for misdeclared UTF-8 files", "[fb2-parsing]")
{
    // Simulates a real-world file with no encoding declaration (or silently
    // misdeclared as UTF-8) but containing raw CP1251 bytes.
    // Affects ~100–180 books per lib.rus.ec 6000-book batch based on scan log analysis.
    CTestWorkspace sandbox("inpx-web-reader-fb2-parser-cp1251-fallback");
    const std::filesystem::path fb2Path = sandbox.GetPath() / "misdeclared.fb2";

    const std::string fb2Text =
        "<?xml version=\"1.0\"?>\r\n"
        "<FictionBook>\r\n"
        "  <description>\r\n"
        "    <title-info>\r\n"
        "      <book-title>\xC2\xEE\xE9\xED\xE0 \xE8 \xEC\xE8\xF0</book-title>\r\n"
        "      <author>\r\n"
        "        <first-name>\xcb\xe5\xe2</first-name>\r\n"
        "        <last-name>\xd2\xee\xeb\xf1\xf2\xee\xe9</last-name>\r\n"
        "      </author>\r\n"
        "      <lang>ru</lang>\r\n"
        "    </title-info>\r\n"
        "  </description>\r\n"
        "</FictionBook>";

    WriteTextFile(fb2Path, fb2Text);

    const InpxWebReader::Fb2Parsing::CFb2Parser parser;
    const InpxWebReader::Domain::SParsedBook parsedBook = ParseFb2File(parser, fb2Path);

    REQUIRE(parsedBook.Metadata.TitleUtf8 == "Война и мир");
    REQUIRE(parsedBook.Metadata.AuthorsUtf8 == std::vector<std::string>{"Лев Толстой"});
    REQUIRE(parsedBook.Metadata.Language == "ru");
}

TEST_CASE("FB2 parser recovers metadata from UTF-8 file with trailing incomplete character", "[fb2-parsing]")
{
    CTestWorkspace sandbox("inpx-web-reader-fb2-parser-trailing-incomplete-utf8");
    const std::filesystem::path fb2Path = sandbox.GetPath() / "trailing-incomplete-utf8.fb2";

    std::string fb2Text =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<FictionBook>\n"
        "  <description>\n"
        "    <title-info>\n"
        "      <book-title>\xD0\x98\xD0\xB3\xD1\x80\xD0\xB0</book-title>\n"
        "      <author>\n"
        "        <first-name>Test</first-name>\n"
        "        <last-name>Author</last-name>\n"
        "      </author>\n"
        "      <lang>ru</lang>\n"
        "    </title-info>\n"
        "  </description>\n"
        "  <body><section><p>\xD0\xA2\xD0\xB5\xD0\xBA\xD1\x81\xD1\x82 ";
    fb2Text.push_back('\xD0');

    WriteTextFile(fb2Path, fb2Text);

    const InpxWebReader::Fb2Parsing::CFb2Parser parser;
    const InpxWebReader::Domain::SParsedBook parsedBook = ParseFb2File(parser, fb2Path);

    REQUIRE(parsedBook.Metadata.TitleUtf8 == "Игра");
    REQUIRE(parsedBook.Metadata.AuthorsUtf8 == std::vector<std::string>{"Test Author"});
    REQUIRE(parsedBook.Metadata.Language == "ru");
}

TEST_CASE("FB2 parser recovers metadata from file with malformed body XM", "[fb2-parsing]")
{
    // Simulates lib.rus.ec files where body text contains unescaped '<' (e.g. "<...>" ellipsis,
    // "<:>" separators). Catalog parsing uses <description> and does not need a valid body DOM.
    CTestWorkspace sandbox("inpx-web-reader-fb2-parser-recover");
    const std::filesystem::path fb2Path = sandbox.GetPath() / "malformed-body.fb2";

    WriteTextFile(
        fb2Path,
        R"(<?xml version="1.0" encoding="UTF-8"?>
<FictionBook>
  <description>
    <title-info>
      <book-title>Роза мира</book-title>
      <author>
        <first-name>Даниил</first-name>
        <last-name>Андреев</last-name>
      </author>
      <lang>ru</lang>
    </title-info>
  </description>
  <body>
    <section>
      <p>Текст <...> с многоточием и <:> разделителем.</p>
    </section>
  </body>
</FictionBook>)");

    const InpxWebReader::Fb2Parsing::CFb2Parser parser;
    const InpxWebReader::Domain::SParsedBook parsedBook = ParseFb2File(parser, fb2Path);

    REQUIRE(parsedBook.Metadata.TitleUtf8 == "Роза мира");
    REQUIRE(parsedBook.Metadata.AuthorsUtf8 == std::vector<std::string>{"Даниил Андреев"});
    REQUIRE(parsedBook.Metadata.Language == "ru");
}

TEST_CASE("FB2 parser recovers metadata from malformed body when the FB2 document uses prefixed namespaces", "[fb2-parsing]")
{
    CTestWorkspace sandbox("inpx-web-reader-fb2-parser-recover-prefixed-namespaces");
    const std::filesystem::path fb2Path = sandbox.GetPath() / "malformed-prefixed-body.fb2";

    WriteTextFile(
        fb2Path,
        R"(<?xml version="1.0" encoding="UTF-8"?>
<fb:FictionBook xmlns:fb="http://www.gribuser.ru/xml/fictionbook/2.0" xmlns:l="http://www.w3.org/1999/xlink">
  <fb:description>
    <fb:title-info>
      <fb:book-title>Пасынки вселенной</fb:book-title>
      <fb:author>
        <fb:first-name>Роберт</fb:first-name>
        <fb:last-name>Хайнлайн</fb:last-name>
      </fb:author>
      <fb:lang>ru</fb:lang>
      <fb:coverpage>
        <fb:image l:href="#cover.jpg"/>
      </fb:coverpage>
    </fb:title-info>
  </fb:description>
  <fb:body>
    <fb:section>
      <fb:p>Текст <...> с некорректным XML.</fb:p>
    </fb:section>
  </fb:body>
  <fb:binary id="cover.jpg" content-type="image/jpeg">AQID</fb:binary>
</fb:FictionBook>)");

    const InpxWebReader::Fb2Parsing::CFb2Parser parser;
    const InpxWebReader::Domain::SParsedBook parsedBook = ParseFb2File(parser, fb2Path);

    REQUIRE(parsedBook.Metadata.TitleUtf8 == "Пасынки вселенной");
    REQUIRE(parsedBook.Metadata.AuthorsUtf8 == std::vector<std::string>{"Роберт Хайнлайн"});
    REQUIRE(parsedBook.Metadata.Language == "ru");
    REQUIRE(parsedBook.CoverExtension == "jpg");
    REQUIRE(parsedBook.CoverBytes == std::vector<std::byte>({std::byte{0x01}, std::byte{0x02}, std::byte{0x03}}));
}

TEST_CASE("FB2 parser skips non-integer publish year with warning and still indexes book", "[fb2-parsing]")
{
    CTestWorkspace sandbox("inpx-web-reader-fb2-parser-invalid-year");
    const std::filesystem::path fb2Path = sandbox.GetPath() / "invalid-year.fb2";

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
    </title-info>
    <publish-info>
      <year>March 2004</year>
    </publish-info>
  </description>
</FictionBook>)");

    const InpxWebReader::Fb2Parsing::CFb2Parser parser;
    const auto result = ParseFb2File(parser, fb2Path);
    REQUIRE(result.Metadata.TitleUtf8 == "Test");
    REQUIRE_FALSE(result.Metadata.Year.has_value());
    REQUIRE(result.ParserWarningCount == 1);
}

TEST_CASE("FB2 parser skips non-numeric sequence number with warning and still indexes book", "[fb2-parsing]")
{
    CTestWorkspace sandbox("inpx-web-reader-fb2-parser-invalid-seq");
    const std::filesystem::path fb2Path = sandbox.GetPath() / "invalid-seq.fb2";

    WriteTextFile(
        fb2Path,
        R"(<?xml version="1.0" encoding="UTF-8"?>
<FictionBook>
  <description>
    <title-info>
      <book-title>Test Series</book-title>
      <author>
        <first-name>Author</first-name>
        <last-name>Name</last-name>
      </author>
      <lang>ru</lang>
      <sequence name="TestSeries" number="abc" />
    </title-info>
  </description>
</FictionBook>)");

    const InpxWebReader::Fb2Parsing::CFb2Parser parser;
    const auto result = ParseFb2File(parser, fb2Path);
    REQUIRE(result.Metadata.TitleUtf8 == "Test Series");
    REQUIRE(result.Metadata.SeriesUtf8.has_value());
    REQUIRE(*result.Metadata.SeriesUtf8 == "TestSeries");
    REQUIRE_FALSE(result.Metadata.SeriesIndex.has_value());
    REQUIRE(result.ParserWarningCount == 1);
}

TEST_CASE("FB2 parser accepts missing lang node and indexes book with empty language", "[fb2-parsing]")
{
    CTestWorkspace sandbox("inpx-web-reader-fb2-parser-no-lang");
    const std::filesystem::path fb2Path = sandbox.GetPath() / "no-lang.fb2";

    WriteTextFile(
        fb2Path,
        R"(<?xml version="1.0" encoding="UTF-8"?>
<FictionBook>
  <description>
    <title-info>
      <book-title>No Language Book</book-title>
      <author>
        <first-name>Author</first-name>
        <last-name>Name</last-name>
      </author>
    </title-info>
  </description>
</FictionBook>)");

    const InpxWebReader::Fb2Parsing::CFb2Parser parser;
    const auto result = ParseFb2File(parser, fb2Path);
    REQUIRE(result.Metadata.TitleUtf8 == "No Language Book");
    REQUIRE(result.Metadata.Language.empty());
    REQUIRE(result.ParserWarningCount == 1);

    const auto recoverableResult = ParseFb2File(parser,
        fb2Path,
        "",
        {.MissingLanguageMayBeRecoveredByCaller = true});
    REQUIRE(recoverableResult.Metadata.Language.empty());
    REQUIRE(recoverableResult.ParserWarningCount == 0);
}

TEST_CASE("FB2 parser splits comma-concatenated genre node value into multiple genres", "[fb2-parsing]")
{
    CTestWorkspace sandbox("inpx-web-reader-fb2-parser-comma-genres");
    const std::filesystem::path fb2Path = sandbox.GetPath() / "comma-genres.fb2";

    WriteTextFile(
        fb2Path,
        R"(<?xml version="1.0" encoding="UTF-8"?>
<FictionBook>
  <description>
    <title-info>
      <book-title>Comma Genre Test</book-title>
      <genre>sf_fantasy_city,sf_horror</genre>
      <author>
        <first-name>Test</first-name>
        <last-name>Author</last-name>
      </author>
      <lang>ru</lang>
    </title-info>
  </description>
</FictionBook>)");

    const InpxWebReader::Fb2Parsing::CFb2Parser parser;
    const auto result = ParseFb2File(parser, fb2Path);
    REQUIRE(result.Metadata.TitleUtf8 == "Comma Genre Test");
    REQUIRE(result.Metadata.GenresUtf8 == std::vector<std::string>{"Urban Fantasy", "Horror & Mystic"});
}

TEST_CASE("FB2 parser preserves unknown token as-is when splitting comma-concatenated genre value", "[fb2-parsing]")
{
    CTestWorkspace sandbox("inpx-web-reader-fb2-parser-comma-unknown-genre");
    const std::filesystem::path fb2Path = sandbox.GetPath() / "comma-unknown-genre.fb2";

    WriteTextFile(
        fb2Path,
        R"(<?xml version="1.0" encoding="UTF-8"?>
<FictionBook>
  <description>
    <title-info>
      <book-title>Mixed Genre Test</book-title>
      <genre>sf_horror,unknown_community_code</genre>
      <author>
        <first-name>Test</first-name>
        <last-name>Author</last-name>
      </author>
      <lang>ru</lang>
    </title-info>
  </description>
</FictionBook>)");

    const InpxWebReader::Fb2Parsing::CFb2Parser parser;
    const auto result = ParseFb2File(parser, fb2Path);
    REQUIRE(result.Metadata.TitleUtf8 == "Mixed Genre Test");
    // Known code resolves to display name; unknown code is stored as-is (raw).
    REQUIRE(result.Metadata.GenresUtf8 == std::vector<std::string>{"Horror & Mystic", "unknown_community_code"});
    REQUIRE(result.ParserWarningCount == 1);
}

TEST_CASE("FB2 parser handles UTF-16 LE encoded file", "[fb2-parsing]")
{
    // Simulates lib.rus.ec files that begin with 0xFF 0xFE BOM and contain raw UTF-16 LE bytes.
    // pugixml receives raw UTF-16 and fails with "Could not determine tag type" without this fix.
    CTestWorkspace sandbox("inpx-web-reader-fb2-parser-utf16le");
    const std::filesystem::path fb2Path = sandbox.GetPath() / "utf16le.fb2";

    const std::u16string utf16Content =
        u"<?xml version=\"1.0\" encoding=\"utf-16\"?>\n"
        u"<FictionBook>\n"
        u"  <description>\n"
        u"    <title-info>\n"
        u"      <book-title>\u041F\u0438\u043A\u043D\u0438\u043A \u043D\u0430 \u043E\u0431\u043E\u0447\u0438\u043D\u0435</book-title>\n"
        u"      <author>\n"
        u"        <first-name>\u0410\u0440\u043A\u0430\u0434\u0438\u0439</first-name>\n"
        u"        <last-name>\u0421\u0442\u0440\u0443\u0433\u0430\u0446\u043A\u0438\u0439</last-name>\n"
        u"      </author>\n"
        u"      <lang>ru</lang>\n"
        u"    </title-info>\n"
        u"  </description>\n"
        u"</FictionBook>";

    std::string bytes;
    bytes += '\xFF';
    bytes += '\xFE';
    for (const char16_t ch : utf16Content)
    {
        bytes += static_cast<char>(static_cast<unsigned>(ch) & 0xFFu);
        bytes += static_cast<char>((static_cast<unsigned>(ch) >> 8u) & 0xFFu);
    }

    WriteTextFile(fb2Path, bytes);

    const InpxWebReader::Fb2Parsing::CFb2Parser parser;
    const InpxWebReader::Domain::SParsedBook parsedBook = ParseFb2File(parser, fb2Path);

    REQUIRE(parsedBook.Metadata.TitleUtf8 == "Пикник на обочине");
    REQUIRE(parsedBook.Metadata.AuthorsUtf8 == std::vector<std::string>{"Аркадий Стругацкий"});
    REQUIRE(parsedBook.Metadata.Language == "ru");
}

TEST_CASE("FB2 parser handles UTF-16 BE encoded file", "[fb2-parsing]")
{
    CTestWorkspace sandbox("inpx-web-reader-fb2-parser-utf16be");
    const std::filesystem::path fb2Path = sandbox.GetPath() / "utf16be.fb2";

    const std::u16string utf16Content =
        u"<?xml version=\"1.0\" encoding=\"utf-16be\"?>\n"
        u"<FictionBook>\n"
        u"  <description>\n"
        u"    <title-info>\n"
        u"      <book-title>\u0410\u043D\u0434\u0440\u043E\u043C\u0435\u0434\u0430</book-title>\n"
        u"      <author>\n"
        u"        <first-name>\u0418\u0432\u0430\u043D</first-name>\n"
        u"        <last-name>\u0415\u0444\u0440\u0435\u043C\u043E\u0432</last-name>\n"
        u"      </author>\n"
        u"      <lang>ru</lang>\n"
        u"    </title-info>\n"
        u"  </description>\n"
        u"</FictionBook>";

    std::string bytes;
    bytes += '\xFE';
    bytes += '\xFF';
    for (const char16_t ch : utf16Content)
    {
        bytes += static_cast<char>((static_cast<unsigned>(ch) >> 8u) & 0xFFu);
        bytes += static_cast<char>(static_cast<unsigned>(ch) & 0xFFu);
    }

    WriteTextFile(fb2Path, bytes);

    const InpxWebReader::Fb2Parsing::CFb2Parser parser;
    const InpxWebReader::Domain::SParsedBook parsedBook = ParseFb2File(parser, fb2Path);

    REQUIRE(parsedBook.Metadata.TitleUtf8 == "Андромеда");
    REQUIRE(parsedBook.Metadata.AuthorsUtf8 == std::vector<std::string>{"Иван Ефремов"});
    REQUIRE(parsedBook.Metadata.Language == "ru");
}
