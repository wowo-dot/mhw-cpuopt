#include "job_cap.hpp"
#include "../core/json.hpp"
#include "../core/signature.hpp"
#include "image_scan.hpp"
#include <windows.h>
#include <atomic>

namespace cpuopt::job_cap {
namespace {
constexpr std::uint32_t tested_instance = 0x225F620, tested_global = 0x51C4148;

enum State : int { no_target, waiting, native, capped, layout_mismatch, drift, restored };
constexpr const char* state_names[] = {"no target", "waiting", "native", "capped", "layout mismatch", "drift", "restored"};

std::atomic<std::uintptr_t> global{};  // address of the sMain pointer
std::atomic<unsigned> requested{};
std::atomic<int> state{no_target};
// Written on the game thread; read by restore() and append() elsewhere.
std::atomic<std::uintptr_t> instance{};
std::atomic<unsigned> native_count{}, applied{};  // applied: what we last wrote, 0 = never
std::atomic<std::uint64_t> applies{};

struct Fields {
    std::uint8_t created{};
    std::uint32_t participants{}, configured{}, detected{};
};
bool read_pointer(std::uintptr_t at, std::uintptr_t& value) noexcept {
    __try {
        value = *reinterpret_cast<volatile const std::uintptr_t*>(at);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool read_fields(std::uintptr_t object, Fields& f) noexcept {
    __try {
        f.created = *reinterpret_cast<volatile const std::uint8_t*>(object + threads_created_offset);
        f.participants = *reinterpret_cast<volatile const std::uint32_t*>(object + participants_offset);
        f.configured = *reinterpret_cast<volatile const std::uint32_t*>(object + configured_offset);
        f.detected = *reinterpret_cast<volatile const std::uint32_t*>(object + detected_offset);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// Configured first: the game copies it into participants every frame.
bool write_fields(std::uintptr_t object, std::uint32_t value) noexcept {
    __try {
        *reinterpret_cast<volatile std::uint32_t*>(object + configured_offset) = value;
        *reinterpret_cast<volatile std::uint32_t*>(object + participants_offset) = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool final_state(int s) noexcept { return s == no_target || s == layout_mismatch || s == drift || s == restored; }
}  // namespace

Discovery discover(std::uintptr_t base, const unsigned (&counts)[4], std::uintptr_t instance_site) noexcept {
    Discovery d;
    for (unsigned i = 0; i < 4; ++i) d.counts[i] = counts[i];
    for (unsigned i = 0; i < 4; ++i) {
        if (counts[i] != 1) {
            d.reason = counts[i] == 0 ? "signature not found" : "signature not unique";
            return d;
        }
    }
    const auto operand = instance_site + instance_operand;
    if (instance_site < base || !image_scan::readable(operand, 7)) {
        d.reason = "match unreadable";
        return d;
    }
    std::uint8_t code[7]{};
    for (unsigned i = 0; i < 7; ++i) code[i] = reinterpret_cast<const volatile std::uint8_t*>(operand)[i];
    if (code[0] != 0x4C || code[1] != 0x8B || code[2] != 0x05) {
        d.reason = "operand changed";
        return d;
    }
    d.instance_rva = static_cast<std::uint32_t>(instance_site - base);
    d.global_rva = static_cast<std::uint32_t>(rip_target(d.instance_rva + instance_operand, 7, read_i32(code + 3)));
    if (!image_scan::in_writable_section(base, d.global_rva, 8) || !image_scan::readable(base + d.global_rva, 8)) {
        d.reason = "global implausible";
        return d;
    }
    d.expected_values = d.instance_rva == tested_instance && d.global_rva == tested_global;
    d.found = true;
    d.reason = "ok";
    return d;
}

std::string discovery_event(const Discovery& d, double t_ms, std::string_view scanner) {
    std::string out = "{";
    json::key(out, "event");
    json::string(out, "job_cap");
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
    out += ']';
    json::key(out, "instance_rva");
    json::number(out, std::uint64_t(d.instance_rva));
    json::key(out, "global_rva");
    json::number(out, std::uint64_t(d.global_rva));
    json::key(out, "expected_values");
    out += d.expected_values ? "true" : "false";
    out += '}';
    return out;
}

void set_target(std::uintptr_t base, const Discovery& d) noexcept {
    if (!d.found) return;
    instance.store(0);  // a new target learns everything again
    native_count.store(0);
    applied.store(0);
    global.store(base + d.global_rva);
    state.store(waiting);
}

void request(unsigned participants) noexcept { requested.store(participants, std::memory_order_relaxed); }

void on_begin() noexcept {
    const int s = state.load(std::memory_order_relaxed);
    if (final_state(s)) return;
    std::uintptr_t object{};
    if (!read_pointer(global.load(std::memory_order_relaxed), object) || !object) {
        state.store(waiting, std::memory_order_relaxed);
        return;
    }
    if (object != instance.load(std::memory_order_relaxed)) {  // checked once per sMain pointer value
        if (!image_scan::readable(object + threads_created_offset, configured_offset + 4 - threads_created_offset) ||
            !image_scan::readable(object + detected_offset, 4)) {
            state.store(waiting, std::memory_order_relaxed);
            return;
        }
        instance.store(object, std::memory_order_relaxed);
        native_count.store(0, std::memory_order_relaxed);
        applied.store(0, std::memory_order_relaxed);
    }
    Fields f;
    if (!read_fields(object, f)) {
        instance.store(0, std::memory_order_relaxed);
        state.store(waiting, std::memory_order_relaxed);
        return;
    }
    auto native = native_count.load(std::memory_order_relaxed);
    if (!native) {
        if (!f.created) {  // executeJob creates its threads at its first parallel round
            state.store(waiting, std::memory_order_relaxed);
            return;
        }
        // The start-up formula (sub_14225BE20): min(processors, 32), minus one from 4 on.
        auto expected = f.detected > 32 ? 32U : f.detected;
        if (expected >= 4) --expected;
        if (f.configured != expected || f.participants != f.configured || f.configured < 2) {
            state.store(layout_mismatch, std::memory_order_relaxed);
            return;
        }
        native = f.configured;
        native_count.store(native, std::memory_order_relaxed);
    }
    const auto current = applied.load(std::memory_order_relaxed);
    if (f.configured != (current ? current : native)) {  // something else changed it: hands off for good
        state.store(drift, std::memory_order_relaxed);
        return;
    }
    const auto want = requested.load(std::memory_order_relaxed);
    const auto target = want == 0 || want >= native ? native : want;
    if (target != (current ? current : native)) {
        if (!write_fields(object, target)) {
            state.store(layout_mismatch, std::memory_order_relaxed);
            return;
        }
        applied.store(target == native ? 0 : target, std::memory_order_relaxed);
        applies.fetch_add(1, std::memory_order_relaxed);
    }
    state.store(target == native ? State::native : capped, std::memory_order_relaxed);
}

void restore() noexcept {
    requested.store(0);
    const auto object = instance.load();
    const auto native = native_count.load();
    const int s = state.load();
    // A plain 32-bit write: executeJob reads participants once into a register
    // per round, so a write from this thread while frames still run changes at
    // most whether the next round runs in parallel. Both values are valid.
    if (s == capped && object && native && write_fields(object, native)) applied.store(0);
    if (!final_state(s)) state.store(restored);
}

void append(std::string& out) {
    json::key(out, "job");
    out += '{';
    json::key(out, "state");
    json::string(out, state_names[state.load(std::memory_order_relaxed)]);
    json::key(out, "native");
    json::number(out, std::uint64_t(native_count.load(std::memory_order_relaxed)));
    json::key(out, "requested");
    json::number(out, std::uint64_t(requested.load(std::memory_order_relaxed)));
    const auto a = applied.load(std::memory_order_relaxed);
    json::key(out, "applied");
    json::number(out, std::uint64_t(a ? a : native_count.load(std::memory_order_relaxed)));
    json::key(out, "applies");
    json::number(out, applies.load(std::memory_order_relaxed));
    out += '}';
}
}  // namespace cpuopt::job_cap
