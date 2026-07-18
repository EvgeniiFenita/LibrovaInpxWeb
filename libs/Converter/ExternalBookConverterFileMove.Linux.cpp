#include "Converter/ExternalBookConverterFileMove.hpp"

#include <cerrno>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "Foundation/FileSystemUtils.hpp"
#include "Foundation/UnicodeConversion.hpp"

namespace InpxWebReader::ConverterRuntime {
namespace {

class CScopedFileDescriptor final
{
public:
    explicit CScopedFileDescriptor(const int descriptor = -1) noexcept
        : m_descriptor(descriptor)
    {
    }

    ~CScopedFileDescriptor() noexcept
    {
        if (m_descriptor >= 0)
        {
            static_cast<void>(close(m_descriptor));
        }
    }

    CScopedFileDescriptor(const CScopedFileDescriptor&) = delete;
    CScopedFileDescriptor& operator=(const CScopedFileDescriptor&) = delete;

    CScopedFileDescriptor(CScopedFileDescriptor&& other) noexcept
        : m_descriptor(std::exchange(other.m_descriptor, -1))
    {
    }

    CScopedFileDescriptor& operator=(CScopedFileDescriptor&& other) noexcept
    {
        if (this != &other)
        {
            if (m_descriptor >= 0)
            {
                static_cast<void>(close(m_descriptor));
            }
            m_descriptor = std::exchange(other.m_descriptor, -1);
        }
        return *this;
    }

    [[nodiscard]] int Get() const noexcept
    {
        return m_descriptor;
    }

    [[nodiscard]] int Release() noexcept
    {
        return std::exchange(m_descriptor, -1);
    }

private:
    int m_descriptor = -1;
};

class CScopedDirectoryCleanup final
{
public:
    explicit CScopedDirectoryCleanup(std::filesystem::path path)
        : m_path(std::move(path))
    {
    }

    ~CScopedDirectoryCleanup() noexcept
    {
        InpxWebReader::Foundation::RemovePathBestEffortNoThrow(m_path);
    }

    CScopedDirectoryCleanup(const CScopedDirectoryCleanup&) = delete;
    CScopedDirectoryCleanup& operator=(const CScopedDirectoryCleanup&) = delete;

private:
    std::filesystem::path m_path;
};

[[nodiscard]] std::string DescribeErrno(const std::string_view operation)
{
    const std::error_code error(errno, std::generic_category());
    return std::string{operation} + ": " + error.message();
}

[[nodiscard]] CScopedFileDescriptor OpenRegularFileNoFollow(
    const std::filesystem::path& path)
{
    const std::string pathUtf8 = InpxWebReader::Unicode::PathToUtf8(path);
    CScopedFileDescriptor descriptor(open(pathUtf8.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (descriptor.Get() < 0)
    {
        throw std::runtime_error(DescribeErrno("Failed to open generated converter output"));
    }

    struct stat fileState = {};
    if (fstat(descriptor.Get(), &fileState) != 0)
    {
        throw std::runtime_error(DescribeErrno("Failed to inspect generated converter output"));
    }
    if (!S_ISREG(fileState.st_mode))
    {
        throw std::runtime_error("Generated converter output is not a regular file.");
    }
    return descriptor;
}

void WriteAll(const int descriptor, const char* data, std::size_t remainingBytes)
{
    while (remainingBytes > 0)
    {
        const ssize_t written = write(descriptor, data, remainingBytes);
        if (written > 0)
        {
            data += written;
            remainingBytes -= static_cast<std::size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR)
        {
            continue;
        }
        throw std::runtime_error(DescribeErrno("Failed to write generated converter output"));
    }
}

[[nodiscard]] bool HasSameContentState(
    const struct stat& first,
    const struct stat& second) noexcept
{
#if defined(__APPLE__)
    const bool sameModifiedTime = first.st_mtimespec.tv_sec == second.st_mtimespec.tv_sec
        && first.st_mtimespec.tv_nsec == second.st_mtimespec.tv_nsec;
    const bool sameChangedTime = first.st_ctimespec.tv_sec == second.st_ctimespec.tv_sec
        && first.st_ctimespec.tv_nsec == second.st_ctimespec.tv_nsec;
#else
    const bool sameModifiedTime = first.st_mtim.tv_sec == second.st_mtim.tv_sec
        && first.st_mtim.tv_nsec == second.st_mtim.tv_nsec;
    const bool sameChangedTime = first.st_ctim.tv_sec == second.st_ctim.tv_sec
        && first.st_ctim.tv_nsec == second.st_ctim.tv_nsec;
#endif
    return first.st_dev == second.st_dev
        && first.st_ino == second.st_ino
        && first.st_size == second.st_size
        && sameModifiedTime
        && sameChangedTime;
}

void CopyValidatedOutputToPath(
    const std::filesystem::path& sourcePath,
    const std::filesystem::path& destinationPath)
{
    CScopedFileDescriptor sourceDescriptor = OpenRegularFileNoFollow(sourcePath);
    struct stat sourceStateBefore = {};
    if (fstat(sourceDescriptor.Get(), &sourceStateBefore) != 0)
    {
        throw std::runtime_error(DescribeErrno("Failed to inspect generated converter output"));
    }
    if (sourceStateBefore.st_size < 0)
    {
        throw std::runtime_error("Generated converter output has an invalid negative size.");
    }

    if (!destinationPath.parent_path().empty())
    {
        InpxWebReader::Foundation::EnsureDirectory(destinationPath.parent_path());
    }
    const auto validationDirectory = InpxWebReader::Foundation::CreateUniqueDirectory(
        destinationPath.parent_path(),
        ".inpx-web-reader-converter-output-");
    CScopedDirectoryCleanup validationDirectoryCleanup(validationDirectory);
    const auto validatedOutputPath = validationDirectory / "validated-output";
    const std::string validatedOutputUtf8 = InpxWebReader::Unicode::PathToUtf8(
        validatedOutputPath);
    CScopedFileDescriptor destinationDescriptor(open(
        validatedOutputUtf8.c_str(),
        O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
        S_IRUSR | S_IWUSR));
    if (destinationDescriptor.Get() < 0)
    {
        throw std::runtime_error(DescribeErrno("Failed to create validated converter output"));
    }

    std::uint64_t copiedBytes = 0;
    char buffer[64 * 1024];
    while (true)
    {
        const ssize_t bytesRead = read(sourceDescriptor.Get(), buffer, sizeof(buffer));
        if (bytesRead > 0)
        {
            WriteAll(destinationDescriptor.Get(), buffer, static_cast<std::size_t>(bytesRead));
            copiedBytes += static_cast<std::uint64_t>(bytesRead);
            continue;
        }
        if (bytesRead == 0)
        {
            break;
        }
        if (errno == EINTR)
        {
            continue;
        }
        throw std::runtime_error(DescribeErrno("Failed to read generated converter output"));
    }

    struct stat sourceStateAfter = {};
    if (fstat(sourceDescriptor.Get(), &sourceStateAfter) != 0)
    {
        throw std::runtime_error(DescribeErrno("Failed to revalidate generated converter output"));
    }
    if (!HasSameContentState(sourceStateBefore, sourceStateAfter)
        || copiedBytes != static_cast<std::uint64_t>(sourceStateBefore.st_size))
    {
        throw std::runtime_error("Generated converter output changed while it was being copied.");
    }
    if (fsync(destinationDescriptor.Get()) != 0)
    {
        throw std::runtime_error(DescribeErrno("Failed to finalize generated converter output"));
    }
    if (close(destinationDescriptor.Release()) != 0)
    {
        throw std::runtime_error(DescribeErrno("Failed to close generated converter output"));
    }

    std::error_code renameError;
    std::filesystem::rename(validatedOutputPath, destinationPath, renameError);
    if (renameError)
    {
        throw std::runtime_error(
            "Failed to publish validated converter output: " + renameError.message());
    }
}

} // namespace

bool IsGeneratedOutputRegularFile(const std::filesystem::path& path) noexcept
{
    try
    {
        static_cast<void>(OpenRegularFileNoFollow(path));
        return true;
    }
    catch (...)
    {
        return false;
    }
}

void MoveGeneratedOutputFile(
    const std::filesystem::path& sourcePath,
    const std::filesystem::path& destinationPath)
{
    CopyValidatedOutputToPath(sourcePath, destinationPath);
}

void SealGeneratedOutputFile(const std::filesystem::path& path)
{
    CopyValidatedOutputToPath(path, path);
}

} // namespace InpxWebReader::ConverterRuntime
