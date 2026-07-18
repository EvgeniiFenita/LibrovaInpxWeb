#pragma once

#include <exception>

#include "Database/SqliteConnection.hpp"
#include "Foundation/Logging.hpp"

namespace InpxWebReader::Sqlite {

enum class ESqliteTransactionMode
{
    Immediate,
    Deferred
};

class CSqliteTransaction final
{
public:
    explicit CSqliteTransaction(
        const CSqliteConnection& connection,
        const ESqliteTransactionMode mode = ESqliteTransactionMode::Immediate)
        : m_connection(connection)
    {
        m_connection.Execute(mode == ESqliteTransactionMode::Immediate
            ? "BEGIN IMMEDIATE;"
            : "BEGIN;");
    }

    ~CSqliteTransaction()
    {
        if (!m_committed)
        {
            try
            {
                m_connection.Execute("ROLLBACK;");
            }
            catch (const std::exception& ex)
            {
                InpxWebReader::Logging::ErrorIfInitialized("SQLite transaction rollback failed: {}", ex.what());
            }
            catch (...)
            {
                InpxWebReader::Logging::ErrorIfInitialized(
                    "SQLite transaction rollback failed with a non-standard exception.");
            }
        }
    }

    CSqliteTransaction(const CSqliteTransaction&) = delete;
    CSqliteTransaction& operator=(const CSqliteTransaction&) = delete;

    void Commit()
    {
        m_connection.Execute("COMMIT;");
        m_committed = true;
    }

private:
    const CSqliteConnection& m_connection;
    bool m_committed = false;
};

} // namespace InpxWebReader::Sqlite
