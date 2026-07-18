#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include <zip.h>

#include "Inpx/InpxParser.hpp"
#include "Foundation/Sha256Fingerprint.hpp"
#include "TestWorkspace.hpp"

namespace {

const std::string GFieldSeparator(1, '\x04');

std::string MakeRecord(std::initializer_list<std::string> fields)
{
    std::string record;
    bool first = true;
    for (const auto& field : fields)
    {
        if (!first)
        {
            record += GFieldSeparator;
        }
        record += field;
        first = false;
    }
    record.push_back('\n');
    return record;
}

struct SFixtureRecordFields
{
    std::string Authors = "Last,First";
    std::string Genres = "genre";
    std::string Title = "Fixture";
    std::string Series;
    std::string SeriesNumber;
    std::string FileName = "book";
    std::string FileSize = "10";
    std::string LibId = "1";
    std::string Deleted = "0";
    std::string FileExtension = "fb2";
    std::string DateAdded;
    std::string Language = "en";
    std::string IgnoredField12;
    std::string Keywords;
    std::string Reserved;
};

std::string MakeFixtureRecord(const SFixtureRecordFields& fields)
{
    return MakeRecord({
        fields.Authors,
        fields.Genres,
        fields.Title,
        fields.Series,
        fields.SeriesNumber,
        fields.FileName,
        fields.FileSize,
        fields.LibId,
        fields.Deleted,
        fields.FileExtension,
        fields.DateAdded,
        fields.Language,
        fields.IgnoredField12,
        fields.Keywords,
        fields.Reserved
    });
}

std::string JoinRepeated(
    const std::string& value,
    const char separator,
    const std::size_t count)
{
    std::string result;
    if (count == 0)
    {
        return result;
    }

    result.reserve(value.size() * count + count - 1);
    for (std::size_t index = 0; index < count; ++index)
    {
        if (index != 0)
        {
            result.push_back(separator);
        }
        result.append(value);
    }
    return result;
}

void AddZipEntry(
    zip_t* archive,
    const std::string& entryName,
    const std::string& content,
    const bool storeUncompressed = false)
{
    zip_source_t* source = zip_source_buffer(archive, content.data(), content.size(), 0);
    if (source == nullptr)
    {
        throw std::runtime_error("Failed to allocate ZIP source.");
    }

    const zip_int64_t entryIndex = zip_file_add(
        archive,
        entryName.c_str(),
        source,
        ZIP_FL_OVERWRITE | ZIP_FL_ENC_UTF_8);
    if (entryIndex < 0)
    {
        zip_source_free(source);
        throw std::runtime_error("Failed to add ZIP entry.");
    }
    if (storeUncompressed
        && zip_set_file_compression(
               archive,
               static_cast<zip_uint64_t>(entryIndex),
               ZIP_CM_STORE,
               0)
            != 0)
    {
        throw std::runtime_error("Failed to store ZIP entry without compression.");
    }
}

std::filesystem::path WriteInpxArchive(
    const std::filesystem::path& archivePath,
    const std::vector<std::pair<std::string, std::string>>& entries,
    const bool storeUncompressed = false)
{
    std::filesystem::create_directories(archivePath.parent_path());

    int errorCode = ZIP_ER_OK;
    zip_t* archive = zip_open(archivePath.string().c_str(), ZIP_CREATE | ZIP_TRUNCATE, &errorCode);
    if (archive == nullptr)
    {
        throw std::runtime_error("Failed to create INPX fixture archive.");
    }

    for (const auto& [entryName, content] : entries)
    {
        AddZipEntry(archive, entryName, content, storeUncompressed);
    }

    if (zip_close(archive) != 0)
    {
        zip_discard(archive);
        throw std::runtime_error("Failed to finalize INPX fixture archive.");
    }

    return archivePath;
}

} // namespace

TEST_CASE("INPX parser reads multiple inp entries and builds archive and entry names", "[inpx]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-parser-multi");
    const auto inpxPath = WriteInpxArchive(
        sandbox.GetPath() / "catalog.inpx",
        {
            {
                "fb2-001.zip.inp",
                MakeRecord({"Strugatsky,Arkady", "sf:adventure", "Roadside Picnic", "Noon", "1", "picnic", "1234", "1001", "0", "fb2", "20240101", "en", "5", "zone:aliens", ""})
            },
            {
                "fb2-002.inp",
                MakeRecord({"Petrov,Пётр", "", "", "", "", "nameless", "55", "2002", "0", "", "20240202", "ru", "", "", ""})
            }
        });

    const auto result = InpxWebReader::Inpx::CInpxParser{}.ParseAll(inpxPath);

    REQUIRE(result.Summary.InpEntryCount == 2);
    REQUIRE(result.Summary.ParsedRecords == 2);
    REQUIRE(result.Records.size() == 2);
    REQUIRE(result.Records[0].ArchiveNameUtf8 == "fb2-001.zip");
    REQUIRE(result.Records[0].InpEntryNameUtf8 == "fb2-001.zip.inp");
    REQUIRE(result.Records[0].EntryNameUtf8 == "picnic.fb2");
    REQUIRE(result.Records[0].TitleUtf8 == "Roadside Picnic");
    REQUIRE(result.Records[0].Authors.size() == 1);
    REQUIRE(result.Records[0].Authors[0].DisplayNameUtf8 == "Arkady Strugatsky");
    REQUIRE(result.Records[1].ArchiveNameUtf8 == "fb2-002");
    REQUIRE(result.Records[1].FileExtensionUtf8 == "fb2");
    REQUIRE(result.Records[1].TitleUtf8 == "nameless");
    REQUIRE(result.Records[1].LanguageUtf8 == "ru");
}

TEST_CASE("INPX parser fingerprints and parses individual inp segments", "[inpx][fingerprint]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-parser-segments");
    const std::string firstRecord = MakeRecord(
        {"A,Author", "sf", "First", "", "", "first", "10", "1", "0", "fb2", "", "en", "", "", ""});
    const auto inpxPath = WriteInpxArchive(
        sandbox.GetPath() / "catalog.inpx",
        {
            {"fb2-first.zip.inp", firstRecord},
            {"fb2-second.zip.inp", MakeRecord({"B,Author", "sf", "Second", "", "", "second", "20", "2", "0", "fb2", "", "ru", "", "", ""})}
        });

    std::vector<InpxWebReader::Inpx::SInpxSegmentPayload> segments;
    const auto count = InpxWebReader::Inpx::CInpxParser{}.ReadSegments(
        inpxPath,
        [&segments](InpxWebReader::Inpx::SInpxSegmentPayload&& segment) {
            segments.push_back(std::move(segment));
        });

    REQUIRE(count == 2);
    REQUIRE(segments.size() == 2);
    REQUIRE(segments[0].InpEntryNameUtf8 == "fb2-first.zip.inp");
    REQUIRE(segments[0].ArchiveNameUtf8 == "fb2-first.zip");
    REQUIRE(segments[0].FingerprintUtf8
        == InpxWebReader::Foundation::CSha256Fingerprint::ComputeBytes(firstRecord));

    std::vector<InpxWebReader::Inpx::SInpxRecord> records;
    const auto summary = InpxWebReader::Inpx::CInpxParser{}.ParseSegment(
        segments[0],
        [&records](InpxWebReader::Inpx::SInpxRecord&& record) {
            records.push_back(std::move(record));
        });
    REQUIRE(summary.ParsedRecords == 1);
    REQUIRE(records.size() == 1);
    REQUIRE(records.front().LibId == "1");
    REQUIRE(records.front().InpEntryNameUtf8 == "fb2-first.zip.inp");
}

TEST_CASE("INPX parser transfers large records to its callback without cloning", "[inpx][cancellation][limits]")
{
    const std::string largeTitle(4ull * 64ull * 1024ull, 'T');
    const InpxWebReader::Inpx::SInpxSegmentPayload segment{
        .InpEntryNameUtf8 = "fb2-main.zip.inp",
        .ArchiveNameUtf8 = "fb2-main.zip",
        .Bytes = MakeRecord({
            "Author,Fixture",
            "sf",
            largeTitle,
            "",
            "",
            "book",
            "10",
            "1",
            "0",
            "fb2",
            "",
            "en",
            "",
            "",
            ""
        })
    };

    const char* callbackTitleStorage = nullptr;
    std::vector<InpxWebReader::Inpx::SInpxRecord> records;
    records.reserve(1);
    const auto summary = InpxWebReader::Inpx::CInpxParser{}.ParseSegment(
        segment,
        [&](InpxWebReader::Inpx::SInpxRecord&& record) {
            callbackTitleStorage = record.TitleUtf8.data();
            records.push_back(std::move(record));
        });

    REQUIRE(summary.ParsedRecords == 1);
    REQUIRE(records.size() == 1);
    REQUIRE(records.front().TitleUtf8.size() == largeTitle.size());
    REQUIRE(records.front().TitleUtf8.data() == callbackTitleStorage);
}

TEST_CASE("INPX parser accepts its line-size boundary and rejects a larger record with source context",
          "[inpx][limits]")
{
    const auto makeRecordWithLineBytes = [](const std::size_t lineBytes) {
        SFixtureRecordFields fields{.Title = ""};
        const std::size_t fixedLineBytes = MakeFixtureRecord(fields).size() - 1;
        REQUIRE(lineBytes >= fixedLineBytes);
        fields.Title.assign(lineBytes - fixedLineBytes, 'T');
        return MakeFixtureRecord(fields);
    };

    InpxWebReader::Inpx::SInpxSegmentPayload segment{
        .InpEntryNameUtf8 = "fb2-main.zip.inp",
        .ArchiveNameUtf8 = "fb2-main.zip",
        .Bytes = makeRecordWithLineBytes(InpxWebReader::Inpx::GMaxInpRecordLineBytes)
    };
    REQUIRE(segment.Bytes.size() == InpxWebReader::Inpx::GMaxInpRecordLineBytes + 1);

    const auto summary = InpxWebReader::Inpx::CInpxParser{}.ParseSegment(
        segment,
        {});

    REQUIRE(summary.ParsedRecords == 1);

    segment.Bytes = makeRecordWithLineBytes(InpxWebReader::Inpx::GMaxInpRecordLineBytes + 1);
    REQUIRE_THROWS_WITH(
        InpxWebReader::Inpx::CInpxParser{}.ParseSegment(segment, {}),
        "INP entry 'fb2-main.zip.inp' line 1 exceeds the 1048576-byte record limit.");
}

TEST_CASE("INPX parser checkpoints entry inflation and segment hashing", "[inpx][cancellation][fingerprint]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-parser-stream-cancellation");
    const std::string inpBytes(4ull * 64ull * 1024ull, 'x');
    const auto inpxPath = WriteInpxArchive(
        sandbox.GetPath() / "catalog.inpx",
        {{"fb2-main.zip.inp", inpBytes}});

    for (const std::size_t throwAt : {std::size_t{5}, std::size_t{9}})
    {
        std::size_t checkpointCount = 0;
        bool segmentPublished = false;
        REQUIRE_THROWS_WITH(
            InpxWebReader::Inpx::CInpxParser{}.ReadSegments(
                inpxPath,
                [&](InpxWebReader::Inpx::SInpxSegmentPayload&&) {
                    segmentPublished = true;
                },
                [&]() {
                    if (++checkpointCount == throwAt)
                    {
                        throw std::runtime_error("injected INP stream cancellation");
                    }
                }),
            "injected INP stream cancellation");
        REQUIRE(checkpointCount == throwAt);
        REQUIRE_FALSE(segmentPublished);
    }
}

TEST_CASE("INPX parser rejects total inp input before reading an oversized entry", "[inpx][limits]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-parser-total-input-limit");
    const auto inpxPath = WriteInpxArchive(
        sandbox.GetPath() / "catalog.inpx",
        {{"fb2-main.zip.inp", "four"}});

    std::size_t checkpointCount = 0;
    bool segmentPublished = false;
    REQUIRE_THROWS_WITH(
        InpxWebReader::Inpx::CInpxParser{}.ReadSegments(
            inpxPath,
            [&](InpxWebReader::Inpx::SInpxSegmentPayload&&) {
                segmentPublished = true;
            },
            [&]() {
                ++checkpointCount;
            },
            3),
        "INPX metadata input exceeds the configured scan planning limit of 3 bytes.");
    REQUIRE(checkpointCount == 2);
    REQUIRE_FALSE(segmentPublished);
}

TEST_CASE("INPX parser checkpoints decoding line search and field analysis", "[inpx][cancellation][unicode]")
{
    const std::string largeTitle(4ull * 64ull * 1024ull, 'T');
    const InpxWebReader::Inpx::SInpxSegmentPayload segment{
        .InpEntryNameUtf8 = "fb2-main.zip.inp",
        .ArchiveNameUtf8 = "fb2-main.zip",
        .Bytes = MakeRecord({
            "Author,Fixture",
            "sf",
            largeTitle,
            "",
            "",
            "book",
            "10",
            "1",
            "0",
            "fb2",
            "",
            "en",
            "",
            "",
            ""
        })
    };

    for (const std::size_t throwAt : {std::size_t{3}, std::size_t{14}, std::size_t{19}})
    {
        std::size_t checkpointCount = 0;
        REQUIRE_THROWS_WITH(
            InpxWebReader::Inpx::CInpxParser{}.ParseSegment(
                segment,
                {},
                {},
                [&]() {
                    if (++checkpointCount == throwAt)
                    {
                        throw std::runtime_error("injected INP parse cancellation");
                    }
                }),
            "injected INP parse cancellation");
        REQUIRE(checkpointCount == throwAt);
    }
}

TEST_CASE("INPX parser checkpoints cumulative small metadata passes", "[inpx][cancellation]")
{
    const InpxWebReader::Inpx::SInpxSegmentPayload segment{
        .InpEntryNameUtf8 = "fb2-main.zip.inp",
        .ArchiveNameUtf8 = "fb2-main.zip",
        .Bytes = MakeRecord({
            "Author,Fixture",
            "sf",
            "Checkpointed metadata",
            "Series",
            "1",
            "book",
            "10",
            "1",
            "0",
            "fb2",
            "20260715",
            "en",
            "",
            "keyword",
            ""
        })
    };

    std::size_t checkpointCount = 0;
    const auto summary = InpxWebReader::Inpx::CInpxParser{}.ParseSegment(
        segment,
        {},
        {},
        [&]() {
            ++checkpointCount;
        });

    REQUIRE(summary.ParsedRecords == 1);
    REQUIRE(checkpointCount >= 24);
}

TEST_CASE("INPX parser rejects a CRC-damaged inp entry before publishing it", "[inpx][security]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-parser-crc");
    const std::string inpBytes = MakeFixtureRecord({
        .Authors = "Author,Fixture",
        .Genres = "sf",
        .Title = "CRC fixture"
    });
    const auto inpxPath = WriteInpxArchive(
        sandbox.GetPath() / "catalog.inpx",
        {{"fb2-main.zip.inp", inpBytes}},
        true);

    std::string archiveBytes;
    {
        std::ifstream input(inpxPath, std::ios::binary);
        REQUIRE(input.good());
        archiveBytes.assign(
            std::istreambuf_iterator<char>{input},
            std::istreambuf_iterator<char>{});
    }
    const std::size_t payloadOffset = archiveBytes.find(inpBytes);
    REQUIRE(payloadOffset != std::string::npos);
    archiveBytes[payloadOffset + inpBytes.size() / 2] ^= static_cast<char>(0x01);
    {
        std::ofstream output(inpxPath, std::ios::binary | std::ios::trunc);
        REQUIRE(output.good());
        output.write(archiveBytes.data(), static_cast<std::streamsize>(archiveBytes.size()));
        REQUIRE(output.good());
    }

    bool segmentPublished = false;
    REQUIRE_THROWS(InpxWebReader::Inpx::CInpxParser{}.ReadSegments(
        inpxPath,
        [&](InpxWebReader::Inpx::SInpxSegmentPayload&&) {
            segmentPublished = true;
        }));
    REQUIRE_FALSE(segmentPublished);
}

TEST_CASE("INPX parser samples archive names from inp entries without reading records", "[inpx]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-parser-archive-samples");
    const auto inpxPath = WriteInpxArchive(
        sandbox.GetPath() / "catalog.inpx",
        {
            {"fb2-001.inp", "not a valid INPX record\n"},
            {"nested/fb2-001.inp", "duplicate archive sample\n"},
            {"fb2-002.zip.inp", "also not a valid INPX record\n"},
            {"notes.txt", "ignored"}
        });

    const auto oneSample = InpxWebReader::Inpx::CInpxParser{}.ReadArchiveNameSamples(inpxPath, 1);
    const std::vector<std::string> expectedOneSample = {"fb2-001"};
    REQUIRE(oneSample == expectedOneSample);

    const auto allSamples = InpxWebReader::Inpx::CInpxParser{}.ReadArchiveNameSamples(inpxPath, 16);
    const std::vector<std::string> expectedAllSamples = {"fb2-001", "fb2-002.zip"};
    REQUIRE(allSamples == expectedAllSamples);
}

TEST_CASE("INPX parser marks deleted records", "[inpx]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-parser-deleted");
    const auto inpxPath = WriteInpxArchive(
        sandbox.GetPath() / "catalog.inpx",
        {
            {
                "fb2-main.zip.inp",
                MakeRecord({"A,Author", "sf", "Alive", "", "", "alive", "10", "123", "0", "fb2", "", "en", "", "", ""})
                    + MakeRecord({"A,Author", "sf", "Deleted", "", "", "deleted", "10", "123", "1", "fb2", "", "en", "", "", ""})
                    + MakeRecord({"B,Author", "sf", "Again", "", "", "again", "10", "123", "0", "fb2", "", "en", "", "", ""})
            }
        });

    const auto result = InpxWebReader::Inpx::CInpxParser{}.ParseAll(inpxPath);
    REQUIRE(result.Summary.DeletedRecords == 1);
    REQUIRE(result.Records[1].Deleted);
}

TEST_CASE("INPX parser skips malformed records and keeps parsing", "[inpx]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-parser-warnings");
    const auto inpxPath = WriteInpxArchive(
        sandbox.GetPath() / "catalog.inpx",
        {
            {
                "fb2-main.zip.inp",
                std::string("Only") + GFieldSeparator + "three" + GFieldSeparator + "fields\n"
                    + MakeRecord({"A,Author", "sf", "Valid", "", "", "valid", "10", "500", "0", "fb2", "", "en", "", "", ""})
                    + MakeRecord({"A,Author", "sf", "Broken", "oops", "broken", "oops", "501", "0", "fb2", "", "en", "", "", ""})
            }
        });

    const auto result = InpxWebReader::Inpx::CInpxParser{}.ParseAll(inpxPath);

    REQUIRE(result.Summary.TotalRecords == 3);
    REQUIRE(result.Summary.ParsedRecords == 1);
    REQUIRE(result.Summary.SkippedRecords == 2);
    REQUIRE(result.Warnings.size() == 2);
    REQUIRE(result.Warnings[0].RecordSkipped);
    REQUIRE(result.Warnings[1].RecordSkipped);
    REQUIRE(result.Records[0].LibId == "500");
}

TEST_CASE("INPX parser bounds repeated metadata tokens before materializing records", "[inpx][limits]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-parser-token-limits");
    const std::string maxAuthors = JoinRepeated("Last,First", ':', 256);
    const std::string maxGenres = JoinRepeated("genre", ':', 512);
    const std::string maxKeywords = JoinRepeated("keyword", ':', 512);
    const auto inpxPath = WriteInpxArchive(
        sandbox.GetPath() / "catalog.inpx",
        {
            {
                "fb2-main.zip.inp",
                MakeFixtureRecord({
                    .Authors = maxAuthors,
                    .Genres = maxGenres,
                    .Title = "At limits",
                    .FileName = "valid",
                    .Keywords = maxKeywords
                })
                    + MakeFixtureRecord({
                        .Authors = JoinRepeated("Last,First", ':', 257),
                        .Title = "Too many authors",
                        .FileName = "authors",
                        .LibId = "2",
                        .Keywords = "keyword"
                    })
                    + MakeFixtureRecord({
                        .Genres = JoinRepeated("genre", ':', 513),
                        .Title = "Too many genres",
                        .FileName = "genres",
                        .LibId = "3",
                        .Keywords = "keyword"
                    })
                    + MakeFixtureRecord({
                        .Title = "Too many keywords",
                        .FileName = "keywords",
                        .LibId = "4",
                        .Keywords = JoinRepeated("keyword", ':', 513)
                    })
                    + MakeFixtureRecord({
                        .Authors = "Last,First,Middle,Extra",
                        .Title = "Too many name parts",
                        .FileName = "name-parts",
                        .LibId = "5",
                        .Keywords = "keyword"
                    })
            }
        });

    const auto result = InpxWebReader::Inpx::CInpxParser{}.ParseAll(inpxPath);

    REQUIRE(result.Summary.TotalRecords == 5);
    REQUIRE(result.Summary.ParsedRecords == 1);
    REQUIRE(result.Summary.SkippedRecords == 4);
    REQUIRE(result.Records.size() == 1);
    REQUIRE(result.Records[0].Authors.size() == 256);
    REQUIRE(result.Records[0].GenresUtf8.size() == 512);
    REQUIRE(result.Records[0].KeywordsUtf8.size() == 512);
    REQUIRE(result.Warnings.size() == 4);
    REQUIRE_THAT(result.Warnings[0].MessageUtf8, Catch::Matchers::ContainsSubstring("too many authors"));
    REQUIRE_THAT(result.Warnings[1].MessageUtf8, Catch::Matchers::ContainsSubstring("too many genres"));
    REQUIRE_THAT(result.Warnings[2].MessageUtf8, Catch::Matchers::ContainsSubstring("too many keywords"));
    REQUIRE_THAT(result.Warnings[3].MessageUtf8, Catch::Matchers::ContainsSubstring("three name components"));
}

TEST_CASE("INPX parser counts empty raw components in defensive token limits", "[inpx][limits]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-parser-raw-token-limits");
    const auto inpxPath = WriteInpxArchive(
        sandbox.GetPath() / "catalog.inpx",
        {
            {
                "fb2-main.zip.inp",
                MakeFixtureRecord({
                    .Authors = JoinRepeated("Last,First", ':', 256) + ":",
                    .Title = "Authors",
                    .FileName = "authors",
                    .Keywords = "keyword"
                })
                    + MakeFixtureRecord({
                        .Genres = JoinRepeated("genre", ':', 512) + ":",
                        .Title = "Genres",
                        .FileName = "genres",
                        .LibId = "2",
                        .Keywords = "keyword"
                    })
                    + MakeFixtureRecord({
                        .Title = "Keywords",
                        .FileName = "keywords",
                        .LibId = "3",
                        .Keywords = JoinRepeated("keyword", ':', 512) + ":"
                    })
                    + MakeFixtureRecord({
                        .Authors = "Last,First,Middle,",
                        .Title = "Name parts",
                        .FileName = "name-parts",
                        .LibId = "4",
                        .Keywords = "keyword"
                    })
            }
        });

    const auto result = InpxWebReader::Inpx::CInpxParser{}.ParseAll(inpxPath);

    REQUIRE(result.Summary.TotalRecords == 4);
    REQUIRE(result.Summary.ParsedRecords == 0);
    REQUIRE(result.Summary.SkippedRecords == 4);
    REQUIRE(result.Warnings.size() == 4);
}

TEST_CASE("INPX parser bounds numeric and extension lexical analysis", "[inpx][limits]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-parser-lexical-limits");
    const std::string overlongInteger(33, '1');
    const std::string overlongExtension(33, 'e');
    const auto inpxPath = WriteInpxArchive(
        sandbox.GetPath() / "catalog.inpx",
        {
            {
                "fb2-main.zip.inp",
                MakeFixtureRecord({
                    .Title = "File size",
                    .FileName = "file-size",
                    .FileSize = overlongInteger
                })
                    + MakeFixtureRecord({
                        .Title = "Deleted",
                        .FileName = "deleted",
                        .LibId = "2",
                        .Deleted = overlongInteger
                    })
                    + MakeFixtureRecord({
                        .Title = "Extension",
                        .FileName = "extension",
                        .LibId = "3",
                        .FileExtension = overlongExtension
                    })
                    + MakeFixtureRecord({
                        .Title = "Series number",
                        .Series = "Series",
                        .SeriesNumber = overlongInteger,
                        .FileName = "series",
                        .LibId = "4"
                    })
            }
        });

    const auto result = InpxWebReader::Inpx::CInpxParser{}.ParseAll(inpxPath);

    REQUIRE(result.Summary.TotalRecords == 4);
    REQUIRE(result.Summary.ParsedRecords == 1);
    REQUIRE(result.Summary.SkippedRecords == 3);
    REQUIRE(result.Summary.WarningCount == 4);
    REQUIRE(result.Records.size() == 1);
    REQUIRE_FALSE(result.Records[0].SeriesNumber.has_value());
    REQUIRE_THAT(result.Warnings[0].MessageUtf8, Catch::Matchers::ContainsSubstring("FileSize"));
    REQUIRE_THAT(result.Warnings[1].MessageUtf8, Catch::Matchers::ContainsSubstring("Deleted"));
    REQUIRE_THAT(result.Warnings[2].MessageUtf8, Catch::Matchers::ContainsSubstring("FileExt"));
    REQUIRE_THAT(result.Warnings[3].MessageUtf8, Catch::Matchers::ContainsSubstring("SeriesNumber"));
}

TEST_CASE("INPX parser ignores unused field 12 without diagnostics", "[inpx]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-parser-non-skipping-warnings");
    const auto inpxPath = WriteInpxArchive(
        sandbox.GetPath() / "catalog.inpx",
        {
            {
                "fb2-main.zip.inp",
                MakeRecord({"A,Author", "sf", "Valid", "Series", "", "valid", "10", "500", "0", "fb2", "", "en", "unused", "", ""})
            }
        });

    const auto result = InpxWebReader::Inpx::CInpxParser{}.ParseAll(inpxPath);

    REQUIRE(result.Summary.TotalRecords == 1);
    REQUIRE(result.Summary.ParsedRecords == 1);
    REQUIRE(result.Summary.SkippedRecords == 0);
    REQUIRE(result.Warnings.empty());
}

TEST_CASE("INPX parser decodes windows-1251 payloads into UTF-8", "[inpx]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-parser-cp1251");
    const std::string cp1251Line =
        std::string("\xCF\xE5\xF2\xF0\xEE\xE2,\xCF\xE5\xF2\xF0") + GFieldSeparator
        + "\xF4\xE0\xED\xF2\xE0\xF1\xF2\xE8\xEA\xE0" + GFieldSeparator
        + "\xCA\xED\xE8\xE3\xE0" + GFieldSeparator + GFieldSeparator + GFieldSeparator + "book"
        + GFieldSeparator + "10" + GFieldSeparator + "999" + GFieldSeparator + "0" + GFieldSeparator + "fb2"
        + GFieldSeparator + GFieldSeparator + "ru" + GFieldSeparator + GFieldSeparator + GFieldSeparator + "\n";
    const auto inpxPath = WriteInpxArchive(
        sandbox.GetPath() / "catalog.inpx",
        {
            {"fb2-main.zip.inp", cp1251Line}
        });

    const auto result = InpxWebReader::Inpx::CInpxParser{}.ParseAll(inpxPath);

    REQUIRE(result.Summary.ParsedRecords == 1);
    REQUIRE(result.Records[0].TitleUtf8 == "Книга");
    REQUIRE(result.Records[0].Authors[0].DisplayNameUtf8 == "Петр Петров");
}

TEST_CASE("INPX parser detects CP866 payloads and preserves Cyrillic metadata", "[inpx][unicode]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-parser-cp866");
    const std::string cp866Line =
        std::string("\x8F\xA5\xE2\xE0\xAE\xA2,\x8F\xA5\xE2\xE0") + GFieldSeparator
        + "\xE4\xA0\xAD\xE2\xA0\xE1\xE2\xA8\xAA\xA0" + GFieldSeparator
        + "\x8A\xAD\xA8\xA3\xA0" + GFieldSeparator + GFieldSeparator + GFieldSeparator + "book"
        + GFieldSeparator + "10" + GFieldSeparator + "999" + GFieldSeparator + "0" + GFieldSeparator + "fb2"
        + GFieldSeparator + GFieldSeparator + "ru" + GFieldSeparator + GFieldSeparator + GFieldSeparator + "\n";
    const auto inpxPath = WriteInpxArchive(
        sandbox.GetPath() / "catalog.inpx",
        {
            {"fb2-main.zip.inp", cp866Line}
        });

    const auto result = InpxWebReader::Inpx::CInpxParser{}.ParseAll(inpxPath);

    REQUIRE(result.Summary.ParsedRecords == 1);
    REQUIRE(result.Records[0].TitleUtf8 == "Книга");
    REQUIRE(result.Records[0].Authors[0].DisplayNameUtf8 == "Петр Петров");
    REQUIRE(result.Records[0].GenresUtf8 == std::vector<std::string>{"фантастика"});
}

TEST_CASE("INPX parser payload limit includes its exact boundary", "[inpx][limits]")
{
    REQUIRE(InpxWebReader::Inpx::IsInpEntryPayloadSizeAllowed(
        InpxWebReader::Inpx::GMaxInpEntryPayloadBytes - 1));
    REQUIRE(InpxWebReader::Inpx::IsInpEntryPayloadSizeAllowed(
        InpxWebReader::Inpx::GMaxInpEntryPayloadBytes));
    REQUIRE_FALSE(InpxWebReader::Inpx::IsInpEntryPayloadSizeAllowed(
        InpxWebReader::Inpx::GMaxInpEntryPayloadBytes + 1));
}

TEST_CASE("INPX parser handles a deterministic large catalog", "[inpx][scale]")
{
    CTestWorkspace sandbox("inpx-web-reader-inpx-parser-scale");
    constexpr std::size_t recordCount = 10'000;
    std::string records;
    records.reserve(recordCount * 96);
    for (std::size_t index = 0; index < recordCount; ++index)
    {
        records += MakeRecord({
            "Author,Fixture",
            "sf",
            "Scale Book " + std::to_string(index),
            "",
            "",
            "book-" + std::to_string(index),
            "1024",
            std::to_string(index + 1),
            "0",
            "fb2",
            "20260101",
            index % 2 == 0 ? "en" : "ru",
            "",
            "scale",
            ""
        });
    }
    const auto inpxPath = WriteInpxArchive(
        sandbox.GetPath() / "catalog.inpx",
        {{"fb2-scale.zip.inp", records}});

    const auto result = InpxWebReader::Inpx::CInpxParser{}.ParseAll(inpxPath);

    REQUIRE(result.Summary.TotalRecords == recordCount);
    REQUIRE(result.Summary.ParsedRecords == recordCount);
    REQUIRE(result.Summary.WarningCount == 0);
    REQUIRE(result.Records.size() == recordCount);
    REQUIRE(result.Records.front().LibId == "1");
    REQUIRE(result.Records.back().LibId == std::to_string(recordCount));
}
