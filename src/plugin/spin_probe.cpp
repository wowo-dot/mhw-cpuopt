#include "spin_probe.hpp"
#include "image_scan.hpp"
#include <windows.h>
#include <atomic>

namespace cpuopt::spin_probe {
namespace {
std::atomic<std::uintptr_t> checked{};  // sMain whose counters were found readable

bool read_counters(std::uintptr_t smain, Reading& out) noexcept {
    __try {
        out.started = *reinterpret_cast<volatile const std::uint32_t*>(smain + started_offset);
        out.done = *reinterpret_cast<volatile const std::uint32_t*>(smain + done_offset);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
}  // namespace

bool read(std::uintptr_t smain, std::uint32_t released, Reading& out) noexcept {
    out = Reading{};
    if (!smain || released > max_threads) return false;
    if (checked.load(std::memory_order_relaxed) != smain) {
        if (!image_scan::readable(smain + started_offset, 8)) return false;
        checked.store(smain, std::memory_order_relaxed);
    }
    out.released = released;
    if (!read_counters(smain, out)) return false;
    return out.started <= max_threads && out.done <= max_threads;
}
}  // namespace cpuopt::spin_probe
