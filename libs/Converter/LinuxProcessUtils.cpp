#include "Converter/LinuxProcessUtils.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <string_view>

#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

namespace InpxWebReader::ConverterLinux {
namespace {

[[nodiscard]] std::vector<char*> BuildNullTerminatedPointers(std::vector<std::string>& values)
{
    std::vector<char*> pointers;
    pointers.reserve(values.size() + 1);
    for (std::string& value : values)
    {
        pointers.push_back(value.data());
    }
    pointers.push_back(nullptr);
    return pointers;
}

} // namespace

pid_t WaitForProcessNoInterrupt(const pid_t processId, int& status, const int options) noexcept
{
    while (true)
    {
        const pid_t waitResult = waitpid(processId, &status, options);
        if (waitResult < 0 && errno == EINTR)
        {
            continue;
        }
        return waitResult;
    }
}

long ResolveFileDescriptorCloseLimit() noexcept
{
    rlimit limit = {};
    if (getrlimit(RLIMIT_NOFILE, &limit) == 0 && limit.rlim_cur != RLIM_INFINITY)
    {
        return static_cast<long>((std::min)(limit.rlim_cur, static_cast<rlim_t>(INT_MAX)));
    }

    const long openMax = sysconf(_SC_OPEN_MAX);
    return openMax > 0 ? (std::min)(openMax, static_cast<long>(INT_MAX)) : 256;
}

void CloseInheritedFileDescriptors(
    const long closeLimit,
    const int preservedDescriptor) noexcept
{
    for (long fd = STDERR_FILENO + 1; fd < closeLimit; ++fd)
    {
        if (fd != preservedDescriptor)
        {
            close(static_cast<int>(fd));
        }
    }
}

std::vector<char*> BuildArgv(std::vector<std::string>& arguments)
{
    return BuildNullTerminatedPointers(arguments);
}

std::vector<std::string> BuildSanitizedEnvironment()
{
    constexpr std::array allowedNames = {
        std::string_view{"PATH"},
        std::string_view{"LANG"},
        std::string_view{"LANGUAGE"},
        std::string_view{"LC_ALL"},
        std::string_view{"LC_CTYPE"},
        std::string_view{"LC_MESSAGES"},
        std::string_view{"TZ"}
    };

    std::vector<std::string> environment;
    environment.reserve(allowedNames.size());
    bool hasPath = false;
    for (const std::string_view name : allowedNames)
    {
        const std::string nameText{name};
        const char* const value = std::getenv(nameText.c_str());
        if (value == nullptr || *value == '\0')
        {
            continue;
        }
        environment.push_back(nameText + "=" + value);
        hasPath = hasPath || name == "PATH";
    }
    if (!hasPath)
    {
        environment.emplace_back("PATH=/usr/local/bin:/usr/bin:/bin");
    }
    return environment;
}

std::vector<char*> BuildEnvp(std::vector<std::string>& environment)
{
    return BuildNullTerminatedPointers(environment);
}

} // namespace InpxWebReader::ConverterLinux
