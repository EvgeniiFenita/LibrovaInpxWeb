#include "Parsing/Fb2Parser.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, const std::size_t size)
{
    const InpxWebReader::Fb2Parsing::CFb2Parser parser;
    try
    {
        static_cast<void>(parser.ParseBytes(
            std::string(reinterpret_cast<const char*>(data), size),
            "libFuzzer input"));
    }
    catch (const std::exception&)
    {
        // Invalid or unsupported FB2 is an expected parser outcome. Sanitizers
        // still report memory, undefined-behaviour, and process-level failures.
    }
    return 0;
}
