add_library(InpxWebReaderConverter STATIC
    BuiltInConverterCliProbe.hpp
    BuiltInConverterCliProbe.Linux.cpp
    BuiltInConverterCliValidation.cpp
    BuiltInConverterCliValidation.hpp
    ConverterCommandBuilder.cpp
    ConverterConfiguration.cpp
    ExternalConverterProcessRunner.hpp
    ExternalBookConverter.cpp
    ExternalBookConverterFileMove.hpp
    ExternalBookConverterFileMove.Linux.cpp
    LinuxExternalConverterProcessRunner.cpp
    LinuxExternalConverterProcessRunner.hpp
    LinuxProcessUtils.cpp
    LinuxProcessUtils.hpp
)

target_include_directories(InpxWebReaderConverter PUBLIC
    ${CMAKE_SOURCE_DIR}/libs
)

target_link_libraries(InpxWebReaderConverter
    PUBLIC
        InpxWebReaderDomain
    PRIVATE
        InpxWebReaderFoundation
)

target_compile_features(InpxWebReaderConverter PUBLIC cxx_std_20)
