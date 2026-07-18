#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

struct sqlite3;
struct sqlite3_stmt;

namespace InpxWebReader::Sqlite {

class CSqliteStatement
{
public:
    CSqliteStatement(sqlite3* connection, std::string_view sql);

    void BindInt(int parameterIndex, int value) const;
    void BindInt64(int parameterIndex, std::int64_t value) const;
    void BindDouble(int parameterIndex, double value) const;
    void BindText(int parameterIndex, std::string_view value) const;
    void BindNull(int parameterIndex) const;

    [[nodiscard]] bool Step() const;
    void Reset() const;
    [[nodiscard]] int GetColumnInt(int columnIndex) const;
    [[nodiscard]] std::int64_t GetColumnInt64(int columnIndex) const;
    [[nodiscard]] double GetColumnDouble(int columnIndex) const;
    [[nodiscard]] std::string GetColumnText(int columnIndex) const;
    [[nodiscard]] bool IsColumnNull(int columnIndex) const noexcept;

private:
    struct SStatementFinalizer
    {
        void operator()(sqlite3_stmt* statement) const noexcept;
    };

    std::unique_ptr<sqlite3_stmt, SStatementFinalizer> m_statement;
};

} // namespace InpxWebReader::Sqlite
