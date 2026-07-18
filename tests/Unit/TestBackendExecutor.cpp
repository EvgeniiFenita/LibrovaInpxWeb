#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <stdexcept>
#include <thread>
#include <vector>

#include "Server/BackendExecutor.hpp"

namespace {

void RaiseAtomicMax(std::atomic<int>& target, const int value)
{
    int observed = target.load();
    while (value > observed && !target.compare_exchange_weak(observed, value))
    {
    }
}

class CPromiseReleaseGuard final
{
public:
    explicit CPromiseReleaseGuard(std::promise<void>& release) noexcept
        : m_release(release)
    {
    }

    ~CPromiseReleaseGuard()
    {
        ReleaseNow();
    }

    CPromiseReleaseGuard(const CPromiseReleaseGuard&) = delete;
    CPromiseReleaseGuard& operator=(const CPromiseReleaseGuard&) = delete;

    void ReleaseNow() noexcept
    {
        if (!m_active)
        {
            return;
        }

        try
        {
            m_release.set_value();
        }
        catch (const std::future_error&)
        {
        }
        m_active = false;
    }

private:
    std::promise<void>& m_release;
    bool m_active = true;
};

} // namespace

TEST_CASE("BackendExecutor serializes submitted backend operations", "[server][executor]")
{
    InpxWebReader::Server::CBackendExecutor executor(16);
    std::atomic<int> activeOperations = 0;
    std::atomic<int> maxActiveOperations = 0;

    std::vector<std::future<void>> futures;
    for (int index = 0; index < 12; ++index)
    {
        futures.push_back(executor.Submit([&activeOperations, &maxActiveOperations]() {
            const int active = activeOperations.fetch_add(1) + 1;
            RaiseAtomicMax(maxActiveOperations, active);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            activeOperations.fetch_sub(1);
        }));
    }

    for (auto& future : futures)
    {
        future.get();
    }

    REQUIRE(maxActiveOperations.load() == 1);
}

TEST_CASE("BackendExecutor propagates backend exceptions through futures", "[server][executor]")
{
    InpxWebReader::Server::CBackendExecutor executor(4);

    auto future = executor.Submit([]() -> int {
        throw std::runtime_error("backend failed");
    });

    REQUIRE_THROWS_AS(future.get(), std::runtime_error);
}

TEST_CASE("BackendExecutor rejects submissions over queue depth", "[server][executor]")
{
    InpxWebReader::Server::CBackendExecutor executor(1);
    std::promise<void> started;
    std::promise<void> release;
    auto releaseFuture = release.get_future().share();

    auto first = executor.Submit([&started, releaseFuture]() {
        started.set_value();
        releaseFuture.wait();
    });
    CPromiseReleaseGuard releaseGuard(release);
    started.get_future().wait();

    auto second = executor.Submit([]() {});
    REQUIRE_THROWS_AS(executor.Submit([]() {}), InpxWebReader::Server::CBackendOverloadError);

    releaseGuard.ReleaseNow();
    first.get();
    second.get();
}

TEST_CASE("BackendExecutor reports active and queued operation metrics", "[server][executor]")
{
    InpxWebReader::Server::CBackendExecutor executor(2);
    std::promise<void> started;
    std::promise<void> release;
    auto releaseFuture = release.get_future().share();

    auto first = executor.Submit([&started, releaseFuture]() {
        started.set_value();
        releaseFuture.wait();
    });
    CPromiseReleaseGuard releaseGuard(release);
    started.get_future().wait();

    auto second = executor.Submit([]() {});
    const auto metrics = executor.GetMetrics();

    REQUIRE(metrics.ActiveOperations == 1);
    REQUIRE(metrics.QueuedOperations == 1);
    REQUIRE(metrics.MaxQueueDepth == 2);

    releaseGuard.ReleaseNow();
    first.get();
    second.get();
}
