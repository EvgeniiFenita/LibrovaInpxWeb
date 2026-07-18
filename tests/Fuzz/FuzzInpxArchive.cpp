#include "App/InpxArchiveAccess.hpp"
#include "Inpx/InpxParser.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

constexpr std::uint64_t GManifestMemoryBudgetBytes = 1ull * 1024ull * 1024ull;

[[nodiscard]] const std::filesystem::path& InputPath()
{
    static const std::filesystem::path path = []() {
        const char* const configuredRoot = std::getenv("INPX_WEB_READER_FUZZ_RUNTIME_ROOT");
        const std::filesystem::path root = configuredRoot != nullptr
            ? std::filesystem::path{configuredRoot}
            : std::filesystem::current_path() / "out" / "fuzz" / "runtime";
        std::filesystem::create_directories(root);
        return root / "payload.zip";
    }();
    return path;
}

void WriteInput(const std::uint8_t* const data, const std::size_t size)
{
    std::ofstream output(InputPath(), std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, const std::size_t size)
{
    try
    {
        WriteInput(data, size);
    }
    catch (const std::exception&)
    {
        return 0;
    }

    try
    {
        static_cast<void>(InpxWebReader::Inpx::CInpxParser{}.ParseAll(InputPath()));
    }
    catch (const std::exception&)
    {
        // Invalid INPX containers and records are expected fuzzer outcomes.
    }

    try
    {
        const InpxWebReader::Application::SInpxSourceInfo source{
            .InpxPath = InputPath(),
            .ArchiveRoot = InputPath().parent_path()
        };
        const InpxWebReader::Application::CInpxArchiveReader reader(source, "payload");
        static_cast<void>(reader.ReadValidatedManifest(GManifestMemoryBudgetBytes));
    }
    catch (const std::exception&)
    {
        // Invalid ZIP metadata, entry names, sizes, and payloads are expected.
    }
    return 0;
}
