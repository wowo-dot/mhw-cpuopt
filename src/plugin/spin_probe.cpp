#include "spin_probe.hpp"
#include "image_scan.hpp"
#include <windows.h>
#include <atomic>

namespace cpuopt::spin_probe {
namespace {
std::atomic<std::uintptr_t> checked{};  // sMain whose slot range was found readable

bool read_slots(std::uintptr_t smain, Reading& out) noexcept {
    __try {
        for (unsigned i = 0; i < max_slots; ++i) {
            const auto job = *reinterpret_cast<volatile const std::uintptr_t*>(smain + slots_offset + i * slot_stride + slot_job);
            if (!job) continue;
            // A job entry lives in sMain's job array or in a list the caller
            // passed; read its function only where it is readable.
            std::uintptr_t function{};
            if (image_scan::readable(job + job_function, sizeof(function)))
                function = *reinterpret_cast<volatile const std::uintptr_t*>(job + job_function);
            out.functions[out.busy++] = function;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
}  // namespace

bool read(std::uintptr_t smain, Reading& out) noexcept {
    out = Reading{};
    if (!smain) return false;
    if (checked.load(std::memory_order_relaxed) != smain) {
        if (!image_scan::readable(smain + slots_offset, max_slots * slot_stride)) return false;
        checked.store(smain, std::memory_order_relaxed);
    }
    return read_slots(smain, out);
}
}  // namespace cpuopt::spin_probe
