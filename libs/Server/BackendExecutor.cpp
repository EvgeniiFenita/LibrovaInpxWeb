#include "Server/BackendExecutor.hpp"

#include <stdexcept>
#include <utility>

namespace InpxWebReader::Server {

CBackendOverloadError::CBackendOverloadError()
    : std::runtime_error("Backend executor queue is full.")
{
}

CBackendExecutor::CBackendExecutor(const std::size_t maxQueueDepth)
    : m_maxQueueDepth(maxQueueDepth)
{
    if (maxQueueDepth == 0)
    {
        throw std::invalid_argument("Backend executor queue depth must be at least 1.");
    }

    m_worker = std::thread([this]() { Run(); });
}

CBackendExecutor::~CBackendExecutor()
{
    {
        std::scoped_lock lock(m_mutex);
        m_stopping = true;
    }

    m_wakeWorker.notify_one();
    if (m_worker.joinable())
    {
        m_worker.join();
    }
}

SBackendExecutorMetrics CBackendExecutor::GetMetrics() const
{
    std::scoped_lock lock(m_mutex);
    return {
        .ActiveOperations = m_activeOperations,
        .QueuedOperations = m_tasks.size(),
        .MaxQueueDepth = m_maxQueueDepth
    };
}

void CBackendExecutor::Run()
{
    for (;;)
    {
        std::function<void()> task;
        {
            std::unique_lock lock(m_mutex);
            m_wakeWorker.wait(lock, [this]() {
                return m_stopping || !m_tasks.empty();
            });

            if (m_stopping && m_tasks.empty())
            {
                return;
            }

            task = std::move(m_tasks.front());
            m_tasks.pop();
            ++m_activeOperations;
        }

        task();

        {
            std::scoped_lock lock(m_mutex);
            if (m_activeOperations > 0)
            {
                --m_activeOperations;
            }
        }
    }
}

} // namespace InpxWebReader::Server
