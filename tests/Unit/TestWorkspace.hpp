#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>

[[nodiscard]] inline std::filesystem::path TestWorkspaceRoot()
{
    const auto root = std::filesystem::path(INPX_WEB_READER_SOURCE_DIR) / "out" / "test-workspaces" / "unit";
    std::filesystem::create_directories(root);
    return root;
}

// Generates a unique temp path under repository out/: <out>/test-workspaces/unit/<prefix>.<timestamp>.<counter>
// The path is not created on disk.
[[nodiscard]] inline std::filesystem::path MakeUniqueTestPath(std::string_view prefix)
{
    static std::atomic<unsigned long long> s_counter{0};
    const auto timestamp = static_cast<unsigned long long>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto seq = s_counter.fetch_add(1, std::memory_order_relaxed);
    const std::string name =
        std::string(prefix) + "." + std::to_string(timestamp) + "." + std::to_string(seq);
    return TestWorkspaceRoot() / name;
}

// RAII unique sandbox directory.
// Creates a fresh directory on construction, removes it (and all contents) on destruction.
class CTestWorkspace final
{
public:
    explicit CTestWorkspace(std::string_view prefix)
        : m_path(MakeUniqueTestPath(prefix))
    {
        std::filesystem::remove_all(m_path);
        std::filesystem::create_directories(m_path);
    }

    ~CTestWorkspace()
    {
        std::error_code ec;
        std::filesystem::remove_all(m_path, ec);
    }

    CTestWorkspace(const CTestWorkspace&) = delete;
    CTestWorkspace& operator=(const CTestWorkspace&) = delete;

    [[nodiscard]] const std::filesystem::path& GetPath() const noexcept { return m_path; }

private:
    std::filesystem::path m_path;
};
