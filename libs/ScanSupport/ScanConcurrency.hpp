#pragma once

#include <algorithm>
#include <cstddef>
#include <thread>

namespace InpxWebReader::ScanSupport {

enum class EScanWorkerPolicy
{
    LeaveOneThreadFree,
    UseAllAvailable
};

constexpr std::size_t GMaxScanWorkerCount = 8;

[[nodiscard]] inline std::size_t ResolveScanWorkerCountForHardware(
    const std::size_t hardwareConcurrency,
    const EScanWorkerPolicy policy,
    const std::size_t fallbackWorkerCount = 1) noexcept
{
    const std::size_t observedWorkerCount =
        hardwareConcurrency == 0 ? fallbackWorkerCount : hardwareConcurrency;
    const std::size_t usableWorkerCount =
        policy == EScanWorkerPolicy::LeaveOneThreadFree && observedWorkerCount > 1
        ? observedWorkerCount - 1
        : observedWorkerCount;

    return (std::max<std::size_t>)(
        1,
        (std::min<std::size_t>)(GMaxScanWorkerCount, usableWorkerCount));
}

[[nodiscard]] inline std::size_t ResolveScanWorkerCount(
    const EScanWorkerPolicy policy,
    const std::size_t fallbackWorkerCount = 1) noexcept
{
    return ResolveScanWorkerCountForHardware(
        std::thread::hardware_concurrency(),
        policy,
        fallbackWorkerCount);
}

} // namespace InpxWebReader::ScanSupport
