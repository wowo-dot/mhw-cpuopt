#include "log.hpp"
#include <cstring>

namespace cpuopt {
namespace {
const std::int64_t frequency = [] {
    LARGE_INTEGER f{};
    QueryPerformanceFrequency(&f);
    return f.QuadPart;
}();
const std::int64_t origin = qpc_now();
}  // namespace

std::int64_t qpc_now() noexcept {
    LARGE_INTEGER c{};
    QueryPerformanceCounter(&c);
    return c.QuadPart;
}
std::int64_t qpc_frequency() noexcept { return frequency; }
double now_ms() noexcept { return double(qpc_now() - origin) * 1000.0 / double(frequency); }

std::string utf8(std::wstring_view text) {
    if (text.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n, nullptr, nullptr);
    return out;
}

bool EventLog::open(const std::wstring& folder) {
    path_ = folder + L"\\cpuopt-" + std::to_wstring(GetCurrentProcessId()) + L".jsonl";
    file_ = CreateFileW(path_.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    return is_open();
}

void EventLog::append(std::string_view line) noexcept {
    if (full_) return;
    if (written_ + line.size() + 1 > limit) {
        constexpr std::string_view stop = "{\"event\":\"log_full\"}\n";
        DWORD n{};
        WriteFile(file_, stop.data(), static_cast<DWORD>(stop.size()), &n, nullptr);
        full_ = true;
        return;
    }
    // One WriteFile per line keeps lines whole when the file is read live.
    char small[4096];
    DWORD n{};
    if (line.size() < sizeof(small)) {
        memcpy(small, line.data(), line.size());
        small[line.size()] = '\n';
        WriteFile(file_, small, static_cast<DWORD>(line.size() + 1), &n, nullptr);
    } else {
        try {
            std::string copy;
            copy.reserve(line.size() + 1);
            copy.append(line);
            copy += '\n';
            WriteFile(file_, copy.data(), static_cast<DWORD>(copy.size()), &n, nullptr);
        } catch (...) {
            return;
        }
    }
    written_ += n;
}

void EventLog::write(std::string_view line) noexcept {
    if (!is_open()) return;
    AcquireSRWLockExclusive(&lock_);
    append(line);
    ReleaseSRWLockExclusive(&lock_);
}

void EventLog::write_at_exit(std::string_view line) noexcept {
    if (!is_open() || !TryAcquireSRWLockExclusive(&lock_)) return;
    append(line);
    ReleaseSRWLockExclusive(&lock_);
}
}  // namespace cpuopt
