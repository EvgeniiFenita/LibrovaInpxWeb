#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>

#include "Foundation/UnicodeConversion.hpp"
#include "Foundation/Sha256Fingerprint.hpp"
#include "TestWorkspace.hpp"

namespace {

void WriteBytes(const std::filesystem::path& path, const std::string_view bytes)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

} // namespace

TEST_CASE("SHA-256 fingerprint hashes every file byte and excludes the path", "[foundation][fingerprint]")
{
    CTestWorkspace workspace("inpx-web-reader-inpx-fingerprint");
    const auto firstPath = workspace.GetPath() / "first" / "catalog.inpx";
    const auto movedPath = workspace.GetPath()
        / InpxWebReader::Unicode::PathFromUtf8("перенесённый")
        / "renamed.inpx";
    WriteBytes(firstPath, "abc");
    WriteBytes(movedPath, "abc");

    const auto firstFingerprint = InpxWebReader::Foundation::CSha256Fingerprint::ComputeFile(firstPath);
    REQUIRE(firstFingerprint == "sha256:ba7816bf8f01cfea414140de5dae2223"
                                "b00361a396177a9cb410ff61f20015ad");
    REQUIRE(InpxWebReader::Foundation::CSha256Fingerprint::ComputeFile(movedPath) == firstFingerprint);

    WriteBytes(movedPath, "abcd");
    REQUIRE(InpxWebReader::Foundation::CSha256Fingerprint::ComputeFile(movedPath) != firstFingerprint);
}

TEST_CASE("SHA-256 file fingerprint checkpoints empty input and every read chunk", "[foundation][fingerprint][cancellation]")
{
    CTestWorkspace workspace("inpx-web-reader-file-fingerprint-cancellation");
    const auto emptyPath = workspace.GetPath() / "empty.inpx";
    const auto largePath = workspace.GetPath() / "large.inpx";
    WriteBytes(emptyPath, {});
    WriteBytes(largePath, std::string((2ull * 1024ull * 1024ull) + 1ull, 'x'));

    std::size_t emptyCheckpointCount = 0;
    REQUIRE(InpxWebReader::Foundation::CSha256Fingerprint::ComputeFile(
        emptyPath,
        [&]() { ++emptyCheckpointCount; })
        == "sha256:e3b0c44298fc1c149afbf4c8996fb924"
           "27ae41e4649b934ca495991b7852b855");
    REQUIRE(emptyCheckpointCount >= 1);

    std::size_t checkpointCount = 0;
    constexpr std::size_t throwAtCheckpoint = 3;
    REQUIRE_THROWS_WITH(
        InpxWebReader::Foundation::CSha256Fingerprint::ComputeFile(
            largePath,
            [&]() {
                if (++checkpointCount == throwAtCheckpoint)
                {
                    throw std::runtime_error("cancelled during SHA-256 file read");
                }
            }),
        "cancelled during SHA-256 file read");
    REQUIRE(checkpointCount == throwAtCheckpoint);
}

TEST_CASE("SHA-256 fingerprint hashes in-memory bytes and normalizes its text form", "[foundation][fingerprint]")
{
    REQUIRE(InpxWebReader::Foundation::CSha256Fingerprint::ComputeBytes("abc")
        == "sha256:ba7816bf8f01cfea414140de5dae2223"
           "b00361a396177a9cb410ff61f20015ad");
    const std::string uppercase(64, 'A');
    REQUIRE(InpxWebReader::Foundation::CSha256Fingerprint::Normalize(uppercase)
        == "sha256:" + std::string(64, 'a'));
    REQUIRE(InpxWebReader::Foundation::CSha256Fingerprint::Normalize("sha256:" + uppercase)
        == "sha256:" + std::string(64, 'a'));
    REQUIRE_THROWS_AS(
        InpxWebReader::Foundation::CSha256Fingerprint::Normalize("short"),
        std::invalid_argument);
}

TEST_CASE("SHA-256 fingerprint builder preserves the byte stream across updates", "[foundation][fingerprint]")
{
    InpxWebReader::Foundation::CSha256FingerprintBuilder builder;
    builder.Update("a");
    builder.Update("bc");

    REQUIRE(builder.Finalize()
        == "sha256:ba7816bf8f01cfea414140de5dae2223"
           "b00361a396177a9cb410ff61f20015ad");
}
