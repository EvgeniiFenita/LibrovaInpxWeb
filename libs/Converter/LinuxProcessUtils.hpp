#pragma once

#include <string>
#include <vector>

#include <sys/types.h>

namespace InpxWebReader::ConverterLinux {

[[nodiscard]] pid_t WaitForProcessNoInterrupt(pid_t processId, int& status, int options) noexcept;

[[nodiscard]] long ResolveFileDescriptorCloseLimit() noexcept;
void CloseInheritedFileDescriptors(long closeLimit, int preservedDescriptor = -1) noexcept;

[[nodiscard]] std::vector<char*> BuildArgv(std::vector<std::string>& arguments);
[[nodiscard]] std::vector<std::string> BuildSanitizedEnvironment();
[[nodiscard]] std::vector<char*> BuildEnvp(std::vector<std::string>& environment);

} // namespace InpxWebReader::ConverterLinux
