#pragma once

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

#include <zip.h>

#include "Foundation/UnicodeConversion.hpp"

namespace InpxWebReader::Tests::ServerFixtures {

inline const std::string GFieldSeparator(1, '\x04');

[[nodiscard]] inline std::string MakeMinimalInpxRecord()
{
    return std::string{"Author,Test"} + GFieldSeparator
        + "genre" + GFieldSeparator
        + "Title" + GFieldSeparator
        + GFieldSeparator
        + GFieldSeparator
        + "book" + GFieldSeparator
        + "0" + GFieldSeparator
        + "123" + GFieldSeparator
        + "0" + GFieldSeparator
        + "fb2" + GFieldSeparator
        + GFieldSeparator
        + "en" + GFieldSeparator
        + GFieldSeparator
        + GFieldSeparator
        + "\n";
}

inline void AddZipEntry(zip_t* archive, const std::string& entryName, const std::string& content)
{
    void* buffer = std::malloc(content.size());
    if (buffer == nullptr)
    {
        throw std::runtime_error("Failed to allocate ZIP fixture buffer.");
    }
    std::memcpy(buffer, content.data(), content.size());

    zip_source_t* source = zip_source_buffer(archive, buffer, content.size(), 1);
    if (source == nullptr)
    {
        std::free(buffer);
        throw std::runtime_error("Failed to allocate ZIP source.");
    }

    if (zip_file_add(archive, entryName.c_str(), source, ZIP_FL_OVERWRITE | ZIP_FL_ENC_UTF_8) < 0)
    {
        zip_source_free(source);
        throw std::runtime_error("Failed to add ZIP entry.");
    }
}

[[nodiscard]] inline std::filesystem::path WriteInpxArchive(const std::filesystem::path& archivePath)
{
    std::filesystem::create_directories(archivePath.parent_path());

    int errorCode = ZIP_ER_OK;
    const auto archivePathUtf8 = Unicode::PathToUtf8(archivePath);
    zip_t* archive = zip_open(archivePathUtf8.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &errorCode);
    if (archive == nullptr)
    {
        throw std::runtime_error("Failed to create INPX fixture archive.");
    }

    AddZipEntry(archive, "fb2-main.zip.inp", MakeMinimalInpxRecord());

    if (zip_close(archive) != 0)
    {
        zip_discard(archive);
        throw std::runtime_error("Failed to finalize INPX fixture archive.");
    }

    return archivePath;
}

[[nodiscard]] inline std::string JsonString(std::string_view value)
{
    std::string result = "\"";
    for (const char ch : value)
    {
        switch (ch)
        {
        case '\\':
            result += "\\\\";
            break;
        case '"':
            result += "\\\"";
            break;
        case '\n':
            result += "\\n";
            break;
        case '\r':
            result += "\\r";
            break;
        case '\t':
            result += "\\t";
            break;
        default:
            result.push_back(ch);
            break;
        }
    }
    result += "\"";
    return result;
}

[[nodiscard]] inline std::string JsonPath(const std::filesystem::path& path)
{
    return JsonString(Unicode::PathToUtf8(path));
}

[[nodiscard]] inline std::string MakeServerConfigJson(
    const std::filesystem::path& cacheRoot,
    const std::filesystem::path& runtimeWorkspaceRoot,
    const std::filesystem::path& inpxPath,
    const std::filesystem::path& archiveRoot,
    const std::string& serverJson = {},
    const std::string& securityJson = {},
    const std::string& limitsJson = {},
    const std::string& startupJson = {},
    const std::string& loggingJson = {})
{
    std::string json = "{";
    json += "\"cacheRoot\":" + JsonPath(cacheRoot);
    json += ",\"runtimeWorkspaceRoot\":" + JsonPath(runtimeWorkspaceRoot);
    json += ",\"inpxSource\":{";
    json += "\"inpxPath\":" + JsonPath(inpxPath);
    json += ",\"archiveRoot\":" + JsonPath(archiveRoot);
    json += "}";
    if (!serverJson.empty())
    {
        json += ",\"server\":" + serverJson;
    }
    if (!securityJson.empty())
    {
        json += ",\"security\":" + securityJson;
    }
    if (!limitsJson.empty())
    {
        json += ",\"limits\":" + limitsJson;
    }
    if (!startupJson.empty())
    {
        json += ",\"startup\":" + startupJson;
    }
    if (!loggingJson.empty())
    {
        json += ",\"logging\":" + loggingJson;
    }
    json += "}";
    return json;
}

} // namespace InpxWebReader::Tests::ServerFixtures
