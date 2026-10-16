#include "unit_lines.hpp"
#include "../core/json.hpp"
#include <windows.h>
#include <atomic>

namespace cpuopt::unit_lines {
namespace {
enum State : int { no_target, waiting, native, set, layout_mismatch, drift, restored };
constexpr const char* state_names[] = {"no target", "waiting", "native", "set", "layout mismatch", "drift", "restored"};
std::atomic<std::uintptr_t> global{};
std::atomic<int> state{no_target};
std::atomic<unsigned> requested{}, applied{};
std::uintptr_t unit{};       // the sMhUnit we checked
std::uint32_t written{};     // what we last wrote (0: nothing yet)
SRWLOCK lock = SRWLOCK_INIT;

bool read_ptr(std::uintptr_t at, std::uintptr_t& value) noexcept {
    __try {
        value = *reinterpret_cast<volatile const std::uintptr_t*>(at);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool read_u64(std::uintptr_t at, std::uint64_t& value) noexcept {
    __try {
        value = *reinterpret_cast<volatile const std::uint64_t*>(at);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool write_u32(std::uintptr_t at, std::uint32_t value) noexcept {
    __try {
        *reinterpret_cast<volatile std::uint32_t*>(at) = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool final_state(int s) noexcept { return s == no_target || s == layout_mismatch || s == drift || s == restored; }

// Under the lock: find and check sMhUnit, then write `value` (0 = native).
void write_locked(unsigned value) noexcept {
    const int s = state.load();
    if (final_state(s)) return;
    std::uintptr_t smain{}, u{};
    if (!read_ptr(global.load(), smain) || !smain || !read_ptr(smain + unit_offset, u) || !u) {
        state.store(waiting);
        return;
    }
    std::uint64_t field{};
    if (!read_u64(u + threshold_offset, field)) {
        state.store(waiting);
        return;
    }
    if (!unit) {
        if (field != native_threshold) {  // the constructor's qword 60
            state.store(layout_mismatch);
            return;
        }
        unit = u;
        written = native_threshold;
    } else if (u != unit || field != written) {
        state.store(drift);  // another sMhUnit, or someone else wrote the field
        return;
    }
    const std::uint32_t target = value ? value : native_threshold;
    if (target != written && !write_u32(u + threshold_offset, target)) {
        state.store(drift);
        return;
    }
    written = target;
    applied.store(target);
    state.store(value ? set : native);
}
}  // namespace

Discovery discover(const unsigned (&counts)[4]) noexcept {
    Discovery d;
    for (unsigned i = 0; i < 4; ++i) d.counts[i] = counts[i];
    for (const auto c : counts)
        if (c != 1) {
            d.reason = c ? "signature not unique" : "signature not found";
            return d;
        }
    d.found = true;
    d.reason = "ok";
    return d;
}

std::string discovery_event(const Discovery& d, double t_ms, std::string_view scanner) {
    std::string out = "{";
    json::key(out, "event");
    json::string(out, "unit_lines");
    json::key(out, "t_ms");
    json::fixed(out, t_ms, 3);
    json::key(out, "found");
    out += d.found ? "true" : "false";
    json::key(out, "reason");
    json::string(out, d.reason);
    json::key(out, "scanner");
    json::string(out, scanner);
    json::key(out, "counts");
    out += '[';
    for (unsigned i = 0; i < 4; ++i) {
        if (i) out += ',';
        json::number(out, std::uint64_t(d.counts[i]));
    }
    out += "]}";
    return out;
}

void set_target(std::uintptr_t smain_global) noexcept {
    AcquireSRWLockExclusive(&lock);
    global.store(smain_global);
    unit = 0;
    written = 0;
    applied.store(0);
    state.store(smain_global ? waiting : no_target);
    ReleaseSRWLockExclusive(&lock);
}

void request(unsigned us) noexcept {
    requested.store(us);
    apply();
}

void apply() noexcept {
    AcquireSRWLockExclusive(&lock);
    write_locked(requested.load());
    ReleaseSRWLockExclusive(&lock);
}

void restore() noexcept {
    AcquireSRWLockExclusive(&lock);
    if (unit && !final_state(state.load())) {
        write_locked(0);
        state.store(restored);
    }
    ReleaseSRWLockExclusive(&lock);
}

void append(std::string& out) {
    json::key(out, "units");
    out += '{';
    json::key(out, "state");
    json::string(out, state_names[state.load()]);
    json::key(out, "native");
    json::number(out, std::uint64_t(native_threshold));
    json::key(out, "requested");
    json::number(out, std::uint64_t(requested.load()));
    json::key(out, "applied");
    json::number(out, std::uint64_t(applied.load()));
    out += '}';
}
}  // namespace cpuopt::unit_lines
