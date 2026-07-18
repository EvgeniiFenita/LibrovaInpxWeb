// Define stb implementations exactly once in this translation unit.
// All stb headers are included here; other TUs include them without the
// IMPLEMENTATION defines to get only extern declarations resolved from this lib.

#define STBI_NO_STDIO
#define STBI_FAILURE_USERMSG
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

// Use stb_image_resize2 (v2, sRGB-correct, Mitchell-Netravali default) when
// available; fall back to v1 for older vcpkg baselines.
#if __has_include(<stb_image_resize2.h>)
#    define STB_IMAGE_RESIZE_IMPLEMENTATION
#    include <stb_image_resize2.h>
#    define INPX_WEB_READER_STB_RESIZE_V2 1
#else
#    define STB_IMAGE_RESIZE_IMPLEMENTATION
#    include <stb_image_resize.h>
#    define INPX_WEB_READER_STB_RESIZE_V2 0
#endif

#include "Storage/StbCoverImageProcessor.hpp"

#include <array>
#include <climits>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr std::array<int, 3> GJpegQualityLadder = {85, 78, 72};

[[nodiscard]] std::pair<std::uint32_t, std::uint32_t> CalculateTargetSize(
    const std::uint32_t srcW, const std::uint32_t srcH,
    const std::uint32_t maxW, const std::uint32_t maxH)
{
    const double wRatio = static_cast<double>(maxW) / static_cast<double>(srcW);
    const double hRatio = static_cast<double>(maxH) / static_cast<double>(srcH);
    const double scale  = (std::min)(wRatio, hRatio);
    const auto dstW = static_cast<std::uint32_t>((std::max)(1.0, std::round(static_cast<double>(srcW) * scale)));
    const auto dstH = static_cast<std::uint32_t>((std::max)(1.0, std::round(static_cast<double>(srcH) * scale)));
    return {dstW, dstH};
}

void AppendToVector(void* ctx, void* data, int size)
{
    auto& buf = *static_cast<std::vector<std::byte>*>(ctx);
    const auto* src = static_cast<const std::byte*>(data);
    buf.insert(buf.end(), src, src + size);
}

[[nodiscard]] std::vector<stbi_uc> ResizeRgba(
    const stbi_uc* src, int srcW, int srcH, int dstW, int dstH)
{
    std::vector<stbi_uc> dst(static_cast<std::size_t>(dstW) * static_cast<std::size_t>(dstH) * 4u);

#if INPX_WEB_READER_STB_RESIZE_V2
    stbir_resize_uint8_srgb(src, srcW, srcH, 0, dst.data(), dstW, dstH, 0, STBIR_RGBA);
#else
    stbir_resize_uint8(src, srcW, srcH, 0, dst.data(), dstW, dstH, 0, 4);
#endif

    return dst;
}

// Composite RGBA pixels on a white background and pack to RGB (3 channels).
// When alpha == 255 (opaque), the result equals the original RGB values exactly.
[[nodiscard]] std::vector<stbi_uc> CompositeOnWhiteAndPackRgb(
    const stbi_uc* rgba, std::size_t pixelCount)
{
    std::vector<stbi_uc> rgb(pixelCount * 3u);
    for (std::size_t i = 0; i < pixelCount; ++i)
    {
        const std::uint32_t r = rgba[i * 4 + 0];
        const std::uint32_t g = rgba[i * 4 + 1];
        const std::uint32_t b = rgba[i * 4 + 2];
        const std::uint32_t a = rgba[i * 4 + 3];
        // Result = src * (a/255) + white * (1 - a/255)
        // Using integer arithmetic: (channel * a + 255 * (255 - a)) / 255
        rgb[i * 3 + 0] = static_cast<stbi_uc>((r * a + 255u * (255u - a)) / 255u);
        rgb[i * 3 + 1] = static_cast<stbi_uc>((g * a + 255u * (255u - a)) / 255u);
        rgb[i * 3 + 2] = static_cast<stbi_uc>((b * a + 255u * (255u - a)) / 255u);
    }
    return rgb;
}

struct SEncodedJpeg
{
    std::vector<std::byte> Bytes;
    int Quality = 0;
};

[[nodiscard]] std::optional<SEncodedJpeg> TryEncodeJpeg(
    const stbi_uc* rgb,
    const std::uint32_t width,
    const std::uint32_t height,
    const int quality)
{
    std::vector<std::byte> jpegBytes;
    jpegBytes.reserve(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    const int ok = stbi_write_jpg_to_func(
        AppendToVector,
        &jpegBytes,
        static_cast<int>(width),
        static_cast<int>(height),
        3,
        rgb,
        quality);

    if (ok == 0 || jpegBytes.empty())
    {
        return std::nullopt;
    }

    return SEncodedJpeg{
        .Bytes = std::move(jpegBytes),
        .Quality = quality
    };
}

[[nodiscard]] std::vector<stbi_uc> BuildRgbBuffer(
    const stbi_uc* rawPixels,
    const int srcW,
    const int srcH,
    const std::uint32_t dstW,
    const std::uint32_t dstH)
{
    if (static_cast<std::uint32_t>(srcW) == dstW && static_cast<std::uint32_t>(srcH) == dstH)
    {
        return CompositeOnWhiteAndPackRgb(
            rawPixels,
            static_cast<std::size_t>(dstW) * static_cast<std::size_t>(dstH));
    }

    const auto resizedRgba = ResizeRgba(rawPixels, srcW, srcH, static_cast<int>(dstW), static_cast<int>(dstH));
    return CompositeOnWhiteAndPackRgb(
        resizedRgba.data(),
        static_cast<std::size_t>(dstW) * static_cast<std::size_t>(dstH));
}

[[nodiscard]] std::pair<std::uint32_t, std::uint32_t> ResolveOutputSize(
    const std::uint32_t srcW,
    const std::uint32_t srcH,
    const std::uint32_t maxW,
    const std::uint32_t maxH,
    const bool preserveSmallerImages)
{
    if (preserveSmallerImages && srcW <= maxW && srcH <= maxH)
    {
        return {srcW, srcH};
    }

    return CalculateTargetSize(srcW, srcH, maxW, maxH);
}

} // namespace

namespace InpxWebReader::CoverProcessingStb {

InpxWebReader::Domain::SCoverProcessingResult CStbCoverImageProcessor::ProcessForCache(
    const InpxWebReader::Domain::SCoverProcessingRequest& request) const
{
    const auto throwIfCancelled = [&request]() {
        if (request.StopToken.stop_requested())
        {
            throw std::runtime_error("Cover processing cancelled.");
        }
    };
    throwIfCancelled();
    if (!request.IsValid())
    {
        return {
            .Status = InpxWebReader::Domain::ECoverProcessingStatus::Failed,
            .DiagnosticMessage = "Cover processing request must contain non-empty bytes and positive target bounds."
        };
    }

    // Guard against stb_image's int-sized length API.
    if (request.Cover.Bytes.size() > static_cast<std::size_t>(INT_MAX))
    {
        return {
            .Status = InpxWebReader::Domain::ECoverProcessingStatus::Failed,
            .DiagnosticMessage = "Cover image data too large: "
                + std::to_string(request.Cover.Bytes.size()) + " bytes"
        };
    }

    // Decode image to RGBA pixels regardless of source format.
    // stb_image handles JPEG (baseline + progressive), PNG, BMP, GIF (first
    // frame), TGA, PSD, PNM, HDR, PIC, and JPEG with unusual subsampling or
    // CMYK colour space — all cases that caused WIC to fail.
    int srcW = 0;
    int srcH = 0;
    int srcChans = 0;
    if (stbi_info_from_memory(
            reinterpret_cast<const stbi_uc*>(request.Cover.Bytes.data()),
            static_cast<int>(request.Cover.Bytes.size()),
            &srcW,
            &srcH,
            &srcChans) == 0
        || srcW <= 0
        || srcH <= 0)
    {
        const char* const stbReason = stbi_failure_reason();
        return {
            .Status = InpxWebReader::Domain::ECoverProcessingStatus::Failed,
            .DiagnosticMessage = std::string("Failed to inspect cover image dimensions: ")
                + (stbReason != nullptr ? stbReason : "unknown error")
        };
    }

    constexpr std::uint64_t rgbaBytesPerPixel = 4;
    const auto width = static_cast<std::uint64_t>(srcW);
    const auto height = static_cast<std::uint64_t>(srcH);
    if (height > std::numeric_limits<std::uint64_t>::max() / width
        || width * height > std::numeric_limits<std::uint64_t>::max() / rgbaBytesPerPixel)
    {
        return {
            .Status = InpxWebReader::Domain::ECoverProcessingStatus::Failed,
            .DiagnosticMessage = "Cover image dimensions overflow the decoded-size calculation."
        };
    }
    const std::uint64_t decodedBytes = width * height * rgbaBytesPerPixel;
    if (request.MaxDecodedBytes > 0 && decodedBytes > request.MaxDecodedBytes)
    {
        return {
            .Status = InpxWebReader::Domain::ECoverProcessingStatus::Failed,
            .DiagnosticMessage = "Decoded cover image would require " + std::to_string(decodedBytes)
                + " bytes, exceeding the " + std::to_string(request.MaxDecodedBytes) + " byte limit."
        };
    }

    // The bounded dimensions above make stb_image's RGBA allocation predictable.
    throwIfCancelled();
    stbi_uc* rawPixels = stbi_load_from_memory(
        reinterpret_cast<const stbi_uc*>(request.Cover.Bytes.data()),
        static_cast<int>(request.Cover.Bytes.size()),
        &srcW, &srcH, &srcChans,
        STBI_rgb_alpha); // always decode to RGBA

    if (rawPixels == nullptr)
    {
        const char* const stbReason = stbi_failure_reason();
        return {
            .Status = InpxWebReader::Domain::ECoverProcessingStatus::Failed,
            .DiagnosticMessage = std::string("Failed to decode cover image: ")
                + (stbReason != nullptr ? stbReason : "unknown error")
        };
    }

    // RAII: free stb-allocated pixel buffer on scope exit.
    struct StbGuard
    {
        stbi_uc* data;
        ~StbGuard() { stbi_image_free(data); }
    } guard{rawPixels};

    // Defend against a decoder result that disagrees with the inspected header.
    if (srcW <= 0 || srcH <= 0)
    {
        return {
            .Status = InpxWebReader::Domain::ECoverProcessingStatus::Failed,
            .DiagnosticMessage = "Decoded image has invalid dimensions: "
                + std::to_string(srcW) + "x" + std::to_string(srcH)
        };
    }

    const auto w = static_cast<std::uint32_t>(srcW);
    const auto h = static_cast<std::uint32_t>(srcH);

    const auto [primaryWidth, primaryHeight] = ResolveOutputSize(
        w,
        h,
        request.MaxWidth,
        request.MaxHeight,
        request.PreserveSmallerImages);
    throwIfCancelled();
    const auto primaryRgb = BuildRgbBuffer(rawPixels, srcW, srcH, primaryWidth, primaryHeight);

    std::optional<std::vector<stbi_uc>> fallbackRgb;
    std::uint32_t fallbackWidth = 0;
    std::uint32_t fallbackHeight = 0;

    if (request.FallbackMaxWidth > 0 && request.FallbackMaxHeight > 0)
    {
        std::tie(fallbackWidth, fallbackHeight) = ResolveOutputSize(
            w,
            h,
            request.FallbackMaxWidth,
            request.FallbackMaxHeight,
            request.PreserveSmallerImages);

        if (fallbackWidth != primaryWidth || fallbackHeight != primaryHeight)
        {
            fallbackRgb = BuildRgbBuffer(rawPixels, srcW, srcH, fallbackWidth, fallbackHeight);
        }
    }

    std::optional<SEncodedJpeg> lastAttempt = std::nullopt;
    std::uint32_t lastWidth = primaryWidth;
    std::uint32_t lastHeight = primaryHeight;

    const auto tryCandidate =
        [&](const std::vector<stbi_uc>& rgb,
            const std::uint32_t candidateWidth,
            const std::uint32_t candidateHeight) -> std::optional<InpxWebReader::Domain::SCoverProcessingResult> {
            for (const int quality : GJpegQualityLadder)
            {
                throwIfCancelled();
                lastAttempt = TryEncodeJpeg(rgb.data(), candidateWidth, candidateHeight, quality);
                lastWidth = candidateWidth;
                lastHeight = candidateHeight;

                if (!lastAttempt.has_value())
                {
                    return std::nullopt;
                }

                if (request.TargetMaxBytes == 0 || lastAttempt->Bytes.size() <= request.TargetMaxBytes)
                {
                    return InpxWebReader::Domain::SCoverProcessingResult{
                        .Status      = InpxWebReader::Domain::ECoverProcessingStatus::Processed,
                        .Cover       = {.Extension = "jpg", .Bytes = std::move(lastAttempt->Bytes)},
                        .PixelWidth  = candidateWidth,
                        .PixelHeight = candidateHeight,
                        .WasResized  = candidateWidth != w || candidateHeight != h,
                        .DiagnosticMessage = "quality=" + std::to_string(quality)
                    };
                }
            }

            return std::nullopt;
        };

    if (const auto processed = tryCandidate(primaryRgb, primaryWidth, primaryHeight))
    {
        return *processed;
    }

    if (fallbackRgb.has_value())
    {
        if (const auto processed = tryCandidate(*fallbackRgb, fallbackWidth, fallbackHeight))
        {
            return *processed;
        }
    }

    if (!lastAttempt.has_value())
    {
        return {
            .Status = InpxWebReader::Domain::ECoverProcessingStatus::Failed,
            .DiagnosticMessage = "JPEG encoding failed for "
                + std::to_string(lastWidth) + "x" + std::to_string(lastHeight)
                + " image (source " + std::to_string(srcW) + "x" + std::to_string(srcH)
                + ", channels=" + std::to_string(srcChans) + ")."
        };
    }

    return {
        .Status = InpxWebReader::Domain::ECoverProcessingStatus::Failed,
        .DiagnosticMessage = "Encoded cover exceeds the " + std::to_string(request.TargetMaxBytes)
            + " byte cache-entry limit after the final fallback."
    };
}

} // namespace InpxWebReader::CoverProcessingStb
