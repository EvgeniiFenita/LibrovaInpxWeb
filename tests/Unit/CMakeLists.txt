include(CTest)

if(BUILD_TESTING)
    find_package(Catch2 CONFIG REQUIRED)
    find_package(Stb REQUIRED)
    include(Catch)

    set(INPX_WEB_READER_CORE_TEST_SOURCES
        TestFilenamePolicy.cpp
        TestFileSystemUtils.cpp
        TestConverterCommandBuilder.cpp
        TestBuiltInConverterCliValidation.cpp
        TestBookDownloadFacade.cpp
        TestConverterConfiguration.cpp
        TestExternalBookConverterBusiness.cpp
        TestExternalBookConverterLinux.cpp
        TestInpxArchiveAccess.cpp
        TestInpxScanJobServiceSupport.cpp
        TestInpxScanJobServiceCoversAndLogging.cpp
        TestInpxScanJobServiceFinalization.cpp
        TestInpxScanJobServiceLifecycle.cpp
        TestInpxScanJobServiceRescan.cpp
        TestFb2GenreMapper.cpp
        TestFb2ParserSupport.cpp
        TestFb2ParserDiagnosticsAndEncoding.cpp
        TestFb2ParserMetadataAndCovers.cpp
        TestFb2ParserRecoveryAndNormalization.cpp
        TestInpxParser.cpp
        TestInpxSourceConfiguration.cpp
        TestSha256Fingerprint.cpp
        TestInpxCacheBootstrap.cpp
        TestBookFormat.cpp
        TestInpxBookAvailability.cpp
        TestBookId.cpp
        TestLanguageNormalization.cpp
        TestScanPerfTracker.cpp
        TestLogging.cpp
        TestStringUtils.cpp
        TestUnicodeConversion.cpp
        TestSchemaInitializer.cpp
        TestDatabaseSchema.cpp
        TestSqliteBookQueryRepository.cpp
        TestSqliteConnection.cpp
        TestSqliteTransaction.cpp
        TestFileReplacement.cpp
        TestPathSafety.cpp
        TestMetadataNormalization.cpp
        TestSearchQuery.cpp
        TestServiceContracts.cpp
        TestBackendExecutor.cpp
        TestHttpServer.cpp
        TestServerApplicationHost.cpp
        TestServerCatalogApiSupport.cpp
        TestServerCatalogApiFilesAndIntegration.cpp
        TestServerCatalogApiQueriesAndAuth.cpp
        TestServerCommandLine.cpp
        TestServerConfig.cpp
        TestServerScanApi.cpp
        TestInpxCacheLayout.cpp
        TestSqliteTimePoint.cpp
        TestDomainError.cpp
        TestVersion.cpp
        TestStbCoverImageProcessor.cpp
    )

    add_executable(InpxWebReaderCoreTests
        ${INPX_WEB_READER_CORE_TEST_SOURCES}
    )
    target_link_libraries(InpxWebReaderCoreTests
        PRIVATE
            Catch2::Catch2WithMain
            InpxWebReaderApp
            InpxWebReaderDatabase
            InpxWebReaderDomain
            InpxWebReaderInpx
            InpxWebReaderParsing
            InpxWebReaderServer
    )

    target_compile_features(InpxWebReaderCoreTests PRIVATE cxx_std_20)
    target_compile_definitions(InpxWebReaderCoreTests PRIVATE
        INPX_WEB_READER_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
        INPX_WEB_READER_EXECUTABLE_PATH="$<TARGET_FILE:InpxWebReader>"
    )
    add_dependencies(InpxWebReaderCoreTests InpxWebReader)
    target_include_directories(InpxWebReaderCoreTests PRIVATE ${Stb_INCLUDE_DIR})
    catch_discover_tests(
        InpxWebReaderCoreTests
        DISCOVERY_MODE POST_BUILD
        DISCOVERY_TIMEOUT 60
        ADD_TAGS_AS_LABELS
        PROPERTIES
            LABELS "layer:native-mixed"
    )
endif()
