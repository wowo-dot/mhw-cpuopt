#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include <string_view>

namespace cpuopt {
// One JSON object per line in <run folder>\cpuopt-<pid>.jsonl. Writes go
// straight to the file (no user-mode buffer), so a crash loses nothing that
// was logged. The file stops growing at `limit` bytes and records why.
class EventLog {
public:
    static constexpr std::uint64_t limit = 1ULL << 30;
    bool open(const std::wstring& folder);
    bool is_open() const noexcept { return file_ != INVALID_HANDLE_VALUE; }
    const std::wstring& path() const noexcept { return path_; }
    // `line` is a complete JSON object without the newline.
    void write(std::string_view line) noexcept;
    // Process exit (DllMain): only if no other thread holds the lock.
    void write_at_exit(std::string_view line) noexcept;

private:
    void append(std::string_view line) noexcept;
    HANDLE file_{INVALID_HANDLE_VALUE};
    std::wstring path_;
    SRWLOCK lock_ = SRWLOCK_INIT;
    std::uint64_t written_{};
    bool full_{};
};

// Milliseconds since the plugin started (QueryPerformanceCounter).
double now_ms() noexcept;
std::int64_t qpc_now() noexcept;
std::int64_t qpc_frequency() noexcept;
std::string utf8(std::wstring_view text);
}  // namespace cpuopt
