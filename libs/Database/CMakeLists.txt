find_package(unofficial-sqlite3 CONFIG REQUIRED)

add_library(InpxWebReaderDatabase STATIC
    SqliteConnection.cpp
    SqliteStatement.cpp
    DatabaseSchema.cpp
    CatalogStatisticsMaintenance.cpp
    CatalogStatisticsMaintenance.hpp
    SchemaInitializer.cpp
    SearchIndexMaintenance.cpp
    SqliteBookQueryRepository.cpp
    SqliteEntityHelpers.cpp
    SqliteEntityHelpers.hpp
    SqliteGenreHelpers.cpp
    SqliteTimePoint.cpp
    SqliteTimePoint.hpp
    SqliteTransaction.hpp
)

target_include_directories(InpxWebReaderDatabase PUBLIC
    ${CMAKE_SOURCE_DIR}/libs
)

target_link_libraries(InpxWebReaderDatabase PUBLIC
    InpxWebReaderDomain
    InpxWebReaderFoundation
    unofficial::sqlite3::sqlite3
)

target_link_libraries(InpxWebReaderDatabase PRIVATE
    InpxWebReaderStorage
)

target_compile_features(InpxWebReaderDatabase PUBLIC cxx_std_20)
