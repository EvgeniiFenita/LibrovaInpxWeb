#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>

struct sqlite3;

namespace InpxWebReader::Sqlite {

class CSqliteConnection
{
public:
    explicit CSqliteConnection(const std::filesystem::path& databasePath);

    void Execute(std::string_view sql) const;
    [[nodiscard]] std::int64_t GetLastInsertRowId() const noexcept;
    [[nodiscard]] sqlite3* GetNativeHandle() const noexcept;

private:
    struct SConnectionCloser
    {
        void operator()(sqlite3* connection) const noexcept;
    };

    std::unique_ptr<sqlite3, SConnectionCloser> m_connection;
};

} // namespace InpxWebReader::Sqlite
