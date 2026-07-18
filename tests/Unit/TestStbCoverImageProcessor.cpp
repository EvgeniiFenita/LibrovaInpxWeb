#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <stb_image.h>
#include <stb_image_write.h>

#include "Storage/StbCoverImageProcessor.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <stop_token>
#include <vector>

namespace {

[[nodiscard]] std::vector<std::uint8_t> MakeRgbPixels(int width, int height)
{
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3u);
    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            const bool checker = (((x / 8) + (y / 8)) % 2 == 0);
            const int base = (y * width + x) * 3;
            pixels[static_cast<std::size_t>(base) + 0] = checker ? 200u : 50u;
            pixels[static_cast<std::size_t>(base) + 1] = checker ? 150u : 100u;
            pixels[static_cast<std::size_t>(base) + 2] = checker ? 100u : 200u;
        }
    }
    return pixels;
}

[[nodiscard]] std::vector<std::byte> CreateJpegBytes(int width, int height)
{
    const auto rgb = MakeRgbPixels(width, height);
    std::vector<std::byte> result;
    stbi_write_jpg_to_func(
        [](void* ctx, void* data, int size) {
            auto& buf = *static_cast<std::vector<std::byte>*>(ctx);
            const auto* src = static_cast<const std::byte*>(data);
            buf.insert(buf.end(), src, src + size);
        },
        &result, width, height, 3, rgb.data(), 90);
    return result;
}

[[nodiscard]] std::vector<std::byte> CreatePngBytes(int width, int height, int channels = 3)
{
    std::vector<std::uint8_t> pixels;
    if (channels == 4)
    {
        pixels.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u);
        for (int i = 0; i < width * height; ++i)
        {
            pixels[static_cast<std::size_t>(i) * 4 + 0] = 150u;
            pixels[static_cast<std::size_t>(i) * 4 + 1] = 100u;
            pixels[static_cast<std::size_t>(i) * 4 + 2] = 200u;
            pixels[static_cast<std::size_t>(i) * 4 + 3] = 128u;
        }
    }
    else
    {
        pixels = MakeRgbPixels(width, height);
    }

    std::vector<std::byte> result;
    stbi_write_png_to_func(
        [](void* ctx, void* data, int size) {
            auto& buf = *static_cast<std::vector<std::byte>*>(ctx);
            buf.insert(buf.end(),
                static_cast<const std::byte*>(data),
                static_cast<const std::byte*>(data) + size);
        },
        &result, width, height, channels, pixels.data(), 0);
    return result;
}

[[nodiscard]] std::vector<std::byte> CreateNoisyPngBytes(int width, int height)
{
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3u);
    std::uint32_t state = 0xC0FFEEu;
    for (auto& pixel : pixels)
    {
        state = state * 1664525u + 1013904223u;
        pixel = static_cast<std::uint8_t>((state >> 16) & 0xFFu);
    }

    std::vector<std::byte> result;
    stbi_write_png_to_func(
        [](void* ctx, void* data, int size) {
            auto& buf = *static_cast<std::vector<std::byte>*>(ctx);
            buf.insert(
                buf.end(),
                static_cast<const std::byte*>(data),
                static_cast<const std::byte*>(data) + size);
        },
        &result,
        width,
        height,
        3,
        pixels.data(),
        0);
    return result;
}

[[nodiscard]] bool StartsWithJpegMagic(const std::vector<std::byte>& bytes) noexcept
{
    return bytes.size() >= 3
        && bytes[0] == std::byte{0xFF}
        && bytes[1] == std::byte{0xD8}
        && bytes[2] == std::byte{0xFF};
}

} // namespace

TEST_CASE("Stb cover processor returns failed for invalid image data", "[cover-processing]")
{
    const InpxWebReader::CoverProcessingStb::CStbCoverImageProcessor proc;

    const auto result = proc.ProcessForCache({
        .Cover = {.Extension = "jpg", .Bytes = {std::byte{0x00}, std::byte{0xFF}, std::byte{0x42}}},
        .MaxWidth  = 512,
        .MaxHeight = 768
    });

    REQUIRE(result.Status == InpxWebReader::Domain::ECoverProcessingStatus::Failed);
    REQUIRE_FALSE(result.DiagnosticMessage.empty());
}

TEST_CASE("Stb cover processor leaves small JPEG unchanged", "[cover-processing]")
{
    const InpxWebReader::CoverProcessingStb::CStbCoverImageProcessor proc;
    const auto jpegBytes = CreateJpegBytes(32, 48);

    const auto result = proc.ProcessForCache({
        .Cover             = {.Extension = "jpg", .Bytes = jpegBytes},
        .MaxWidth          = 512,
        .MaxHeight         = 768,
        .PreserveSmallerImages = true
    });

    REQUIRE(result.Status == InpxWebReader::Domain::ECoverProcessingStatus::Processed);
    REQUIRE(result.PixelWidth  == 32);
    REQUIRE(result.PixelHeight == 48);
    REQUIRE_FALSE(result.WasResized);
    REQUIRE(StartsWithJpegMagic(result.Cover.Bytes));
    REQUIRE(result.Cover.Bytes != jpegBytes);
}

TEST_CASE("Stb cover processor converts small PNG to JPEG", "[cover-processing]")
{
    // PNGs must always be converted to JPEG even when they fit within bounds,
    // so that cover cache is consistently compact.
    const InpxWebReader::CoverProcessingStb::CStbCoverImageProcessor proc;
    const auto pngBytes = CreatePngBytes(32, 48);

    const auto result = proc.ProcessForCache({
        .Cover             = {.Extension = "png", .Bytes = pngBytes},
        .MaxWidth          = 512,
        .MaxHeight         = 768,
        .PreserveSmallerImages = true
    });

    REQUIRE(result.Status == InpxWebReader::Domain::ECoverProcessingStatus::Processed);
    REQUIRE(result.PixelWidth  == 32);
    REQUIRE(result.PixelHeight == 48);
    REQUIRE_FALSE(result.WasResized);
    REQUIRE(result.Cover.Extension == "jpg");
    REQUIRE(StartsWithJpegMagic(result.Cover.Bytes));
}

TEST_CASE("Stb cover processor downscales oversized JPEG within bounds", "[cover-processing]")
{
    const InpxWebReader::CoverProcessingStb::CStbCoverImageProcessor proc;
    const auto jpegBytes = CreateJpegBytes(1024, 1536);

    const auto result = proc.ProcessForCache({
        .Cover     = {.Extension = "jpg", .Bytes = jpegBytes},
        .MaxWidth  = 512,
        .MaxHeight = 768
    });

    REQUIRE(result.Status == InpxWebReader::Domain::ECoverProcessingStatus::Processed);
    REQUIRE(result.WasResized);
    REQUIRE(result.PixelWidth  <= 512);
    REQUIRE(result.PixelHeight <= 768);
    REQUIRE(result.Cover.Extension == "jpg");
    REQUIRE(StartsWithJpegMagic(result.Cover.Bytes));
    // A 512x768 JPEG must be smaller on disk than the original 1024x1536.
    REQUIRE(result.Cover.Bytes.size() < jpegBytes.size());
}

TEST_CASE("Stb cover processor downscales oversized PNG within bounds", "[cover-processing]")
{
    const InpxWebReader::CoverProcessingStb::CStbCoverImageProcessor proc;
    const auto pngBytes = CreatePngBytes(1024, 1536);

    const auto result = proc.ProcessForCache({
        .Cover     = {.Extension = "png", .Bytes = pngBytes},
        .MaxWidth  = 512,
        .MaxHeight = 768
    });

    REQUIRE(result.Status == InpxWebReader::Domain::ECoverProcessingStatus::Processed);
    REQUIRE(result.WasResized);
    REQUIRE(result.PixelWidth  <= 512);
    REQUIRE(result.PixelHeight <= 768);
    REQUIRE(result.Cover.Extension == "jpg");
    REQUIRE(StartsWithJpegMagic(result.Cover.Bytes));
}

TEST_CASE("Stb cover processor composites RGBA PNG alpha on white background", "[cover-processing]")
{
    const InpxWebReader::CoverProcessingStb::CStbCoverImageProcessor proc;
    const auto pngBytes = CreatePngBytes(64, 96, 4); // RGBA, alpha = 128

    const auto result = proc.ProcessForCache({
        .Cover             = {.Extension = "png", .Bytes = pngBytes},
        .MaxWidth          = 512,
        .MaxHeight         = 768,
        .PreserveSmallerImages = true
    });

    REQUIRE(result.Status == InpxWebReader::Domain::ECoverProcessingStatus::Processed);
    REQUIRE(result.Cover.Extension == "jpg");
    REQUIRE_FALSE(result.Cover.Bytes.empty());

    // Verify the output is a valid JPEG with the expected dimensions.
    int w = 0, h = 0, comp = 0;
    REQUIRE(stbi_info_from_memory(
        reinterpret_cast<const stbi_uc*>(result.Cover.Bytes.data()),
        static_cast<int>(result.Cover.Bytes.size()),
        &w, &h, &comp));
    REQUIRE(w == 64);
    REQUIRE(h == 96);
}

TEST_CASE("Stb cover processor output dimensions respect aspect ratio", "[cover-processing]")
{
    // Source: 800x400 (2:1), bounds: 512x768.
    // Width is the binding constraint: output must be 512x256.
    const InpxWebReader::CoverProcessingStb::CStbCoverImageProcessor proc;
    const auto jpegBytes = CreateJpegBytes(800, 400);

    const auto result = proc.ProcessForCache({
        .Cover     = {.Extension = "jpg", .Bytes = jpegBytes},
        .MaxWidth  = 512,
        .MaxHeight = 768
    });

    REQUIRE(result.Status == InpxWebReader::Domain::ECoverProcessingStatus::Processed);
    REQUIRE(result.WasResized);
    REQUIRE(result.PixelWidth  == 512);
    REQUIRE(result.PixelHeight == 256);
}

TEST_CASE("Stb cover processor returns failed for empty request", "[cover-processing]")
{
    const InpxWebReader::CoverProcessingStb::CStbCoverImageProcessor proc;

    const auto result = proc.ProcessForCache({
        .Cover     = {.Extension = "jpg", .Bytes = {}},
        .MaxWidth  = 512,
        .MaxHeight = 768
    });

    REQUIRE(result.Status == InpxWebReader::Domain::ECoverProcessingStatus::Failed);
}

TEST_CASE("Stb cover processor returns failed when MaxWidth is zero", "[cover-processing]")
{
    const InpxWebReader::CoverProcessingStb::CStbCoverImageProcessor proc;
    const auto jpegBytes = CreateJpegBytes(32, 48);

    const auto result = proc.ProcessForCache({
        .Cover     = {.Extension = "jpg", .Bytes = jpegBytes},
        .MaxWidth  = 0,   // invalid
        .MaxHeight = 768
    });

    REQUIRE(result.Status == InpxWebReader::Domain::ECoverProcessingStatus::Failed);
    REQUIRE_FALSE(result.DiagnosticMessage.empty());
}

TEST_CASE("Stb cover processor returns failed for truncated JPEG", "[cover-processing]")
{
    // A byte sequence that starts with the JPEG SOI marker but has no valid
    // image data at all.  stb_image cannot decode this even with its lenient
    // error recovery, so the processor must return Failed with a non-empty
    // diagnostic.  (Note: stb gracefully recovers many real truncated JPEGs by
    // filling the missing rows with the last decoded row — that behaviour is
    // intentional and NOT tested here as a failure case.)
    const InpxWebReader::CoverProcessingStb::CStbCoverImageProcessor proc;

    const std::vector<std::byte> truncatedJpeg = {
        std::byte{0xFF}, std::byte{0xD8}, std::byte{0xFF}  // JPEG SOI only, no image data
    };

    const auto result = proc.ProcessForCache({
        .Cover     = {.Extension = "jpg", .Bytes = truncatedJpeg},
        .MaxWidth  = 512,
        .MaxHeight = 768
    });

    REQUIRE(result.Status == InpxWebReader::Domain::ECoverProcessingStatus::Failed);
    REQUIRE_FALSE(result.DiagnosticMessage.empty());
}

TEST_CASE("Stb cover processor re-encodes PNG bytes even when extension says jpg", "[cover-processing]")
{
    // Extension says "jpg" but the actual bytes are PNG.
    // Fast path must be skipped (magic bytes check), and the PNG must be
    // decoded and re-encoded as JPEG.  The output must be a valid JPEG.
    const InpxWebReader::CoverProcessingStb::CStbCoverImageProcessor proc;
    const auto pngBytes = CreatePngBytes(32, 48); // PNG magic: 89 50 4E 47

    const auto result = proc.ProcessForCache({
        .Cover             = {.Extension = "jpg", .Bytes = pngBytes},
        .MaxWidth          = 512,
        .MaxHeight         = 768,
        .PreserveSmallerImages = true
    });

    // Must be Processed (re-encoded), never Unchanged — the stored bytes
    // must actually be JPEG, not the original PNG.
    REQUIRE(result.Status == InpxWebReader::Domain::ECoverProcessingStatus::Processed);
    REQUIRE(result.Cover.Extension == "jpg");
    REQUIRE(StartsWithJpegMagic(result.Cover.Bytes));
    REQUIRE(result.Cover.Bytes != pngBytes); // output differs from PNG input
}

TEST_CASE("Stb cover processor applies byte-budget fallback ladder for noisy covers", "[cover-processing]")
{
    const InpxWebReader::CoverProcessingStb::CStbCoverImageProcessor proc;
    const auto noisyPngBytes = CreateNoisyPngBytes(456, 684);

    const auto result = proc.ProcessForCache({
        .Cover = {.Extension = "png", .Bytes = noisyPngBytes},
        .MaxWidth = 456,
        .MaxHeight = 684,
        .FallbackMaxWidth = 400,
        .FallbackMaxHeight = 600,
        .TargetMaxBytes = 120u * 1024u,
        .PreserveSmallerImages = true
    });

    REQUIRE(result.Status == InpxWebReader::Domain::ECoverProcessingStatus::Processed);
    REQUIRE(result.PixelWidth <= 456);
    REQUIRE(result.PixelHeight <= 684);
    REQUIRE(result.Cover.Extension == "jpg");
    REQUIRE(StartsWithJpegMagic(result.Cover.Bytes));
    REQUIRE(result.Cover.Bytes.size() <= 120u * 1024u);
}

TEST_CASE("Stb cover processor rejects images above the decoded-memory budget", "[cover-processing][limits]")
{
    const InpxWebReader::CoverProcessingStb::CStbCoverImageProcessor proc;
    const auto pngBytes = CreatePngBytes(32, 48);

    const auto result = proc.ProcessForCache({
        .Cover = {.Extension = "png", .Bytes = pngBytes},
        .MaxWidth = 32,
        .MaxHeight = 48,
        .MaxDecodedBytes = 1024
    });

    REQUIRE(result.Status == InpxWebReader::Domain::ECoverProcessingStatus::Failed);
    REQUIRE(result.DiagnosticMessage.find("exceeding the 1024 byte limit") != std::string::npos);
}

TEST_CASE("Stb cover processor honors a requested cooperative cancellation", "[cover-processing][cancellation]")
{
    std::stop_source stopSource;
    stopSource.request_stop();
    const InpxWebReader::CoverProcessingStb::CStbCoverImageProcessor proc;

    REQUIRE_THROWS_WITH(
        proc.ProcessForCache({
            .Cover = {.Extension = "png", .Bytes = CreatePngBytes(2, 2)},
            .MaxWidth = 2,
            .MaxHeight = 2,
            .StopToken = stopSource.get_token()
        }),
        "Cover processing cancelled.");
}

TEST_CASE("Stb cover processor rejects output when the byte budget is impossible", "[cover-processing][limits]")
{
    const InpxWebReader::CoverProcessingStb::CStbCoverImageProcessor proc;
    const auto noisyPngBytes = CreateNoisyPngBytes(456, 684);

    const auto result = proc.ProcessForCache({
        .Cover = {.Extension = "png", .Bytes = noisyPngBytes},
        .MaxWidth = 456,
        .MaxHeight = 684,
        .FallbackMaxWidth = 400,
        .FallbackMaxHeight = 600,
        .TargetMaxBytes = 1,
        .PreserveSmallerImages = true
    });

    REQUIRE(result.Status == InpxWebReader::Domain::ECoverProcessingStatus::Failed);
    REQUIRE_FALSE(result.HasOutputCover());
    REQUIRE(std::string_view{result.DiagnosticMessage}.find("exceeds the 1 byte cache-entry limit") != std::string_view::npos);
}
