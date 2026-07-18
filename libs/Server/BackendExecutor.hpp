#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <type_traits>

namespace InpxWebReader::Server {

struct SBackendExecutorMetrics
{
    std::size_t ActiveOperations = 0;
    std::size_t QueuedOperations = 0;
    std::size_t MaxQueueDepth = 0;
};

class CBackendOverloadError final : public std::runtime_error
{
public:
    CBackendOverloadError();
};

class CBackendExecutor final
{
public:
    explicit CBackendExecutor(std::size_t maxQueueDepth);
    ~CBackendExecutor();

    CBackendExecutor(const CBackendExecutor&) = delete;
    CBackendExecutor& operator=(const CBackendExecutor&) = delete;
    CBackendExecutor(CBackendExecutor&&) = delete;
    CBackendExecutor& operator=(CBackendExecutor&&) = delete;

    [[nodiscard]] SBackendExecutorMetrics GetMetrics() const;

    template <typename TOperation>
    [[nodiscard]] auto Submit(TOperation&& operation)
        -> std::future<std::invoke_result_t<std::decay_t<TOperation>&>>
    {
        using TResult = std::invoke_result_t<std::decay_t<TOperation>&>;

        auto task = std::make_shared<std::packaged_task<TResult()>>(
            std::forward<TOperation>(operation));
        auto future = task->get_future();

        {
            std::scoped_lock lock(m_mutex);
            if (m_stopping)
            {
                throw std::runtime_error("Backend executor is stopped.");
            }

            if (m_tasks.size() >= m_maxQueueDepth)
            {
                throw CBackendOverloadError();
            }

            m_tasks.push([task]() { (*task)(); });
        }

        m_wakeWorker.notify_one();
        return future;
    }

private:
    void Run();

    const std::size_t m_maxQueueDepth;
    mutable std::mutex m_mutex;
    std::condition_variable m_wakeWorker;
    std::queue<std::function<void()>> m_tasks;
    bool m_stopping = false;
    std::size_t m_activeOperations = 0;
    std::thread m_worker;
};

} // namespace InpxWebReader::Server
