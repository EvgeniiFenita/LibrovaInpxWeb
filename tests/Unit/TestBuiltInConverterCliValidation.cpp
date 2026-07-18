#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

#include "Converter/BuiltInConverterCliValidation.hpp"
#include "Converter/BuiltInConverterCliProbe.hpp"
#include "Converter/ExternalConverterProcessRunner.hpp"
#include "TestWorkspace.hpp"

namespace {

class CScopedEnvironmentVariable final
{
public:
    CScopedEnvironmentVariable(std::string name, const std::string_view value)
        : m_name(std::move(name))
    {
        if (const char* const previous = std::getenv(m_name.c_str()))
        {
            m_previousValue = previous;
        }
        if (setenv(m_name.c_str(), std::string{value}.c_str(), 1) != 0)
        {
            throw std::runtime_error("Could not set test environment variable.");
        }
    }

    ~CScopedEnvironmentVariable()
    {
        if (m_previousValue.has_value())
        {
            static_cast<void>(setenv(m_name.c_str(), m_previousValue->c_str(), 1));
        }
        else
        {
            static_cast<void>(unsetenv(m_name.c_str()));
        }
    }

    CScopedEnvironmentVariable(const CScopedEnvironmentVariable&) = delete;
    CScopedEnvironmentVariable& operator=(const CScopedEnvironmentVariable&) = delete;

private:
    std::string m_name;
    std::optional<std::string> m_previousValue;
};

class CScopedFileDescriptor final
{
public:
    explicit CScopedFileDescriptor(const int value)
        : m_value(value)
    {
    }

    ~CScopedFileDescriptor()
    {
        if (m_value >= 0)
        {
            close(m_value);
        }
    }

    CScopedFileDescriptor(const CScopedFileDescriptor&) = delete;
    CScopedFileDescriptor& operator=(const CScopedFileDescriptor&) = delete;

    [[nodiscard]] int Get() const noexcept
    {
        return m_value;
    }

private:
    int m_value = -1;
};

void WriteTextFile(const std::filesystem::path& path, const std::string_view text)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output << text;
}

std::filesystem::path SupportedConverterFileName()
{
    return std::filesystem::path("fbc");
}

void MakeExecutable(const std::filesystem::path& path)
{
    std::filesystem::permissions(
        path,
        std::filesystem::perms::owner_exec
            | std::filesystem::perms::owner_read
            | std::filesystem::perms::owner_write
            | std::filesystem::perms::group_exec
            | std::filesystem::perms::group_read
            | std::filesystem::perms::others_exec
            | std::filesystem::perms::others_read,
        std::filesystem::perm_options::replace);
}

} // namespace

TEST_CASE("Built-in converter path validation accepts the Linux fbc executable", "[converter-validation]")
{
    CTestWorkspace sandbox("inpx-web-reader-converter-validation-path");
    const std::filesystem::path executablePath = sandbox.GetPath() / SupportedConverterFileName();
    WriteTextFile(executablePath, "binary");
    MakeExecutable(executablePath);

    CHECK(InpxWebReader::ConverterValidation::IsSupportedBuiltInConverterExecutablePath(executablePath));
}

TEST_CASE("Built-in converter path validation rejects unrelated files", "[converter-validation]")
{
    CTestWorkspace sandbox("inpx-web-reader-converter-validation-non-exe");
    const std::filesystem::path shortcutPath = sandbox.GetPath() / "converter.lnk";
    WriteTextFile(shortcutPath, "shortcut");

    CHECK_FALSE(InpxWebReader::ConverterValidation::IsSupportedBuiltInConverterExecutablePath(shortcutPath));
}

TEST_CASE("Built-in converter path validation rejects executable files with non-fbc name", "[converter-validation]")
{
    CTestWorkspace sandbox("inpx-web-reader-converter-validation-wrong-name");
    const std::filesystem::path executablePath = sandbox.GetPath() / "converter";
    WriteTextFile(executablePath, "binary");

    CHECK_FALSE(InpxWebReader::ConverterValidation::IsSupportedBuiltInConverterExecutablePath(executablePath));
}

TEST_CASE("Built-in converter path validation rejects missing paths", "[converter-validation]")
{
    CTestWorkspace sandbox("inpx-web-reader-converter-validation-missing");
    const std::filesystem::path missingPath = sandbox.GetPath() / "missing";

    CHECK_FALSE(InpxWebReader::ConverterValidation::IsSupportedBuiltInConverterExecutablePath(missingPath));
}

TEST_CASE("Built-in converter path validation rejects directories", "[converter-validation]")
{
    CTestWorkspace sandbox("inpx-web-reader-converter-validation-directory");

    CHECK_FALSE(InpxWebReader::ConverterValidation::IsSupportedBuiltInConverterExecutablePath(sandbox.GetPath()));
}

TEST_CASE("Built-in converter path validation accepts the extensionless Linux executable", "[converter-validation]")
{
    CTestWorkspace sandbox("inpx-web-reader-converter-validation-no-extension");
    const std::filesystem::path filePath = sandbox.GetPath() / "fbc";
    WriteTextFile(filePath, "binary");
    MakeExecutable(filePath);
    CHECK(InpxWebReader::ConverterValidation::IsSupportedBuiltInConverterExecutablePath(filePath));
}

TEST_CASE("Built-in converter help validation accepts fbc-like output and extracts version", "[converter-validation]")
{
    const auto result = InpxWebReader::ConverterValidation::ValidateBuiltInFbcHelpOutput(
        "NAME:\n"
        "   fbc - conversion engine for fiction book (FB2) files\n"
        "USAGE:\n"
        "   fbc [global options] [command [command options]]\n"
        "VERSION:\n"
        "   1.2.3-test\n"
        "COMMANDS:\n"
        "   convert     Converts FB2 file(s) to specified format\n"
        "   dumpconfig  Dumps either default or actual configuration (YAML)\n");

    CHECK(result.IsValid);
    CHECK(result.VersionString == "1.2.3-test");
}

TEST_CASE("Built-in converter help validation rejects empty output", "[converter-validation]")
{
    const auto result = InpxWebReader::ConverterValidation::ValidateBuiltInFbcHelpOutput("");

    CHECK_FALSE(result.IsValid);
    CHECK(result.VersionString.empty());
}

TEST_CASE("Built-in converter help validation accepts fbc-like output without version", "[converter-validation]")
{
    const auto result = InpxWebReader::ConverterValidation::ValidateBuiltInFbcHelpOutput(
        "NAME:\n"
        "   fbc - conversion engine for fiction book (FB2) files\n"
        "USAGE:\n"
        "   fbc [global options] [command [command options]]\n"
        "COMMANDS:\n"
        "   convert     Converts FB2 file(s) to specified format\n"
        "   dumpconfig  Dumps either default or actual configuration (YAML)\n");

    CHECK(result.IsValid);
    CHECK(result.VersionString.empty());
}

TEST_CASE("Built-in converter help validation rejects unrelated help output", "[converter-validation]")
{
    const auto result = InpxWebReader::ConverterValidation::ValidateBuiltInFbcHelpOutput(
        "NAME:\n"
        "   not-a-converter - unrelated helper tool\n"
        "USAGE:\n"
        "   not-a-converter [options]\n"
        "COMMANDS:\n"
        "   inspect\n");

    CHECK_FALSE(result.IsValid);
    CHECK(result.VersionString.empty());
}

TEST_CASE("Built-in converter executable validation accepts valid fbc-like CLI", "[converter-validation]")
{
    CTestWorkspace sandbox("inpx-web-reader-valid-built-in-converter-probe");
    const std::filesystem::path executablePath = sandbox.GetPath() / SupportedConverterFileName();
    WriteTextFile(
        executablePath,
        "#!/bin/sh\n"
        "cat <<'EOF'\n"
        "NAME:\n"
        "   fbc - conversion engine for fiction book (FB2) files\n"
        "USAGE:\n"
        "   fbc [global options] [command [command options]]\n"
        "VERSION:\n"
        "   1.2.3-test\n"
        "COMMANDS:\n"
        "   convert     Converts FB2 file(s) to specified format\n"
        "   dumpconfig  Dumps either default or actual configuration (YAML)\n"
        "EOF\n");
    MakeExecutable(executablePath);

    const auto result = InpxWebReader::ConverterValidation::ValidateBuiltInConverterExecutable(
        executablePath);

    CHECK(result.IsValid);
    CHECK(result.VersionString == "1.2.3-test");
}

TEST_CASE("Built-in converter probe executes an identity-bound shebang descriptor", "[converter-validation][security]")
{
    CTestWorkspace sandbox("inpx-web-reader-built-in-converter-probe-script-fd");
    const auto executablePath = sandbox.GetPath() / SupportedConverterFileName();
    WriteTextFile(
        executablePath,
        "#!/bin/sh\n"
        "echo 'USAGE: fbc convert FB2'\n");
    MakeExecutable(executablePath);
    const auto identity = InpxWebReader::ConverterRuntime::ReadExternalConverterExecutableIdentity(
        executablePath);

    const auto output = InpxWebReader::ConverterValidation::ProbeBuiltInConverterHelpOutput(
        identity.CanonicalPath,
        std::chrono::seconds{5},
        identity);

    REQUIRE(output.has_value());
    REQUIRE(output->find("USAGE: fbc convert FB2") != std::string::npos);
}

TEST_CASE("Built-in converter probe rejects an ABA executable identity replacement", "[converter-validation][security]")
{
    CTestWorkspace sandbox("inpx-web-reader-built-in-converter-probe-aba");
    const auto executablePath = sandbox.GetPath() / SupportedConverterFileName();
    const auto originalBackupPath = sandbox.GetPath() / "fbc-original";
    const auto replacementPath = sandbox.GetPath() / "fbc-replacement";
    WriteTextFile(
        executablePath,
        "#!/bin/sh\n"
        "echo 'original executable must remain bound'\n");
    WriteTextFile(
        replacementPath,
        "#!/bin/sh\n"
        "echo 'USAGE: fbc convert FB2'\n");
    MakeExecutable(executablePath);
    MakeExecutable(replacementPath);
    const auto expectedIdentity =
        InpxWebReader::ConverterRuntime::ReadExternalConverterExecutableIdentity(
            executablePath);
    bool replacementObserved = false;

    const auto output = InpxWebReader::ConverterValidation::ProbeBuiltInConverterHelpOutput(
        expectedIdentity.CanonicalPath,
        std::chrono::seconds{5},
        expectedIdentity,
        {
            .BeforeExecutableOpen = [&]() {
                std::filesystem::rename(executablePath, originalBackupPath);
                std::filesystem::rename(replacementPath, executablePath);
                replacementObserved = true;
            }
        });
    std::filesystem::rename(executablePath, replacementPath);
    std::filesystem::rename(originalBackupPath, executablePath);

    REQUIRE(replacementObserved);
    REQUIRE_FALSE(output.has_value());
}

TEST_CASE("Built-in converter executable validation accepts valid help written to stderr", "[converter-validation]")
{
    CTestWorkspace sandbox("inpx-web-reader-valid-built-in-converter-probe-stderr");
    const std::filesystem::path executablePath = sandbox.GetPath() / SupportedConverterFileName();
    WriteTextFile(
        executablePath,
        "#!/bin/sh\n"
        "cat >&2 <<'EOF'\n"
        "NAME:\n"
        "   fbc - conversion engine for fiction book (FB2) files\n"
        "USAGE:\n"
        "   fbc [global options] [command [command options]]\n"
        "COMMANDS:\n"
        "   convert     Converts FB2 file(s) to specified format\n"
        "EOF\n");
    MakeExecutable(executablePath);

    const auto result = InpxWebReader::ConverterValidation::ValidateBuiltInConverterExecutable(
        executablePath);

    CHECK(result.IsValid);
}

TEST_CASE("Built-in converter probe does not inherit server secrets", "[converter-validation][security]")
{
    CTestWorkspace sandbox("inpx-web-reader-built-in-converter-sanitized-environment");
    const std::filesystem::path executablePath = sandbox.GetPath() / SupportedConverterFileName();
    const auto inheritedFilePath = sandbox.GetPath() / "parent-only.txt";
    WriteTextFile(inheritedFilePath, "parent-only");
    const CScopedFileDescriptor inheritedDescriptor(
        open(inheritedFilePath.c_str(), O_RDONLY));
    REQUIRE(inheritedDescriptor.Get() > STDERR_FILENO);
    WriteTextFile(
        executablePath,
        "#!/bin/sh\n"
        "if [ \"${INPX_WEB_READER_AUTH_TOKEN+x}\" = x ]; then exit 91; fi\n"
        "if [ -e /proc/self/fd/" + std::to_string(inheritedDescriptor.Get()) + " ]; then exit 92; fi\n"
        "cat <<'EOF'\n"
        "NAME:\n"
        "   fbc - conversion engine for fiction book (FB2) files\n"
        "USAGE:\n"
        "   fbc [global options] [command [command options]]\n"
        "COMMANDS:\n"
        "   convert     Converts FB2 file(s) to specified format\n"
        "EOF\n");
    MakeExecutable(executablePath);
    CScopedEnvironmentVariable token("INPX_WEB_READER_AUTH_TOKEN", "sentinel-server-secret");

    const auto result = InpxWebReader::ConverterValidation::ValidateBuiltInConverterExecutable(
        executablePath);

    CHECK(result.IsValid);
}

TEST_CASE("Built-in converter executable validation rejects non-zero exit with valid output", "[converter-validation]")
{
    CTestWorkspace sandbox("inpx-web-reader-invalid-built-in-converter-probe-exit");
    const std::filesystem::path executablePath = sandbox.GetPath() / SupportedConverterFileName();
    WriteTextFile(
        executablePath,
        "#!/bin/sh\n"
        "echo 'NAME:'\n"
        "echo '   fbc - conversion engine for fiction book (FB2) files'\n"
        "echo 'USAGE:'\n"
        "echo '   fbc [global options] [command [command options]]'\n"
        "exit 42\n");
    MakeExecutable(executablePath);

    const auto result = InpxWebReader::ConverterValidation::ValidateBuiltInConverterExecutable(
        executablePath);

    CHECK_FALSE(result.IsValid);
}

TEST_CASE("Built-in converter Linux probe times out hanging help", "[converter-validation]")
{
    CTestWorkspace sandbox("inpx-web-reader-invalid-built-in-converter-probe-timeout");
    const std::filesystem::path executablePath = sandbox.GetPath() / SupportedConverterFileName();
    WriteTextFile(
        executablePath,
        "#!/bin/sh\n"
        "sleep 30\n");
    MakeExecutable(executablePath);

    const auto output = InpxWebReader::ConverterValidation::ProbeBuiltInConverterHelpOutput(
        executablePath,
        std::chrono::milliseconds{20});

    CHECK_FALSE(output.has_value());
}

TEST_CASE("Built-in converter Linux probe bounds help output capture", "[converter-validation][limits]")
{
    CTestWorkspace sandbox("inpx-web-reader-built-in-converter-probe-output-limit");
    const auto executablePath = sandbox.GetPath() / SupportedConverterFileName();
    WriteTextFile(
        executablePath,
        "#!/bin/sh\n"
        "while :; do\n"
        "  printf '0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef'\n"
        "done\n");
    MakeExecutable(executablePath);
    const auto startedAt = std::chrono::steady_clock::now();

    const auto output = InpxWebReader::ConverterValidation::ProbeBuiltInConverterHelpOutput(
        executablePath,
        std::chrono::seconds{5});
    const auto elapsed = std::chrono::steady_clock::now() - startedAt;

    REQUIRE_FALSE(output.has_value());
    REQUIRE(elapsed < std::chrono::seconds{3});
}

TEST_CASE("Built-in converter Linux probe timeout kills descendant process group", "[converter-validation][security]")
{
    CTestWorkspace sandbox("inpx-web-reader-built-in-converter-probe-descendant");
    const auto executablePath = sandbox.GetPath() / SupportedConverterFileName();
    const auto readyPath = sandbox.GetPath() / "descendant-ready";
    const auto survivorSentinelPath = sandbox.GetPath() / "descendant-survived";
    WriteTextFile(
        executablePath,
        "#!/bin/sh\n"
        "(sleep 1; printf survived > '" + survivorSentinelPath.generic_string() + "') &\n"
        "printf ready > '" + readyPath.generic_string() + "'\n"
        "sleep 30\n");
    MakeExecutable(executablePath);

    const auto output = InpxWebReader::ConverterValidation::ProbeBuiltInConverterHelpOutput(
        executablePath,
        std::chrono::milliseconds{250});

    REQUIRE_FALSE(output.has_value());
    REQUIRE(std::filesystem::exists(readyPath));
    std::this_thread::sleep_for(std::chrono::milliseconds{1100});
    REQUIRE_FALSE(std::filesystem::exists(survivorSentinelPath));
}

TEST_CASE("Built-in converter executable validation rejects unrelated CLI", "[converter-validation]")
{
    CTestWorkspace sandbox("inpx-web-reader-invalid-built-in-converter-probe");
    const std::filesystem::path executablePath = sandbox.GetPath() / SupportedConverterFileName();
    WriteTextFile(
        executablePath,
        "#!/bin/sh\n"
        "echo 'NAME:'\n"
        "echo '   not-a-converter - unrelated helper tool'\n"
        "echo 'USAGE:'\n"
        "echo '   not-a-converter [options]'\n");
    MakeExecutable(executablePath);

    const auto result = InpxWebReader::ConverterValidation::ValidateBuiltInConverterExecutable(
        executablePath);

    CHECK_FALSE(result.IsValid);
    CHECK(result.VersionString.empty());
}
