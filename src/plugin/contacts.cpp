#include "contacts.hpp"
#include "../core/json.hpp"
#include "contact_guard.hpp"
#include <windows.h>
#include <atomic>
#include <cstring>

namespace cpuopt::contacts {
namespace {
enum State : int { no_target, waiting, off, applied, engine, layout_mismatch, restored, no_guard };
constexpr const char* state_names[] = {"no target", "waiting", "off", "applied", "engine's own", "layout mismatch", "restored", "no guard"};
std::atomic<std::uintptr_t> global{}, vtable{};
std::atomic<int> state{no_target};
std::atomic<unsigned> requested{};
std::atomic<std::uint64_t> writes{};
std::atomic<int> seen[2]{-1, -1};  // contact, setup: the flags as last read (-1 unknown)
// Game thread only: the flags this switch set to 1 (0: none).
std::uintptr_t written_at[2]{};

bool read_ptr(std::uintptr_t at, std::uintptr_t& value) noexcept {
    __try {
        value = *reinterpret_cast<volatile const std::uintptr_t*>(at);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool read_u8(std::uintptr_t at, std::uint8_t& value) noexcept {
    __try {
        value = *reinterpret_cast<volatile const std::uint8_t*>(at);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool write_u8(std::uintptr_t at, std::uint8_t value) noexcept {
    __try {
        *reinterpret_cast<volatile std::uint8_t*>(at) = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool final_state(int s) noexcept { return s == no_target || s == layout_mismatch || s == restored; }
std::uintptr_t rel32(std::uintptr_t site, unsigned at, unsigned next) noexcept {
    std::int32_t rel{};
    std::memcpy(&rel, reinterpret_cast<const void*>(site + at), 4);
    return site + next + static_cast<std::intptr_t>(rel);
}

// Puts back what this switch set (only flags still at its 1).
void undo() noexcept {
    for (auto& at : written_at) {
        std::uint8_t v{};
        if (at && read_u8(at, v) && v == 1) write_u8(at, 0);
        at = 0;
    }
}

// The collider's flags (contact, setup) after the checks. False: not built
// yet, or (mismatch) not the object the signatures describe.
bool locate(std::uintptr_t (&flags)[2], bool& mismatch) noexcept {
    mismatch = false;
    std::uintptr_t smain{}, collision{}, vt{};
    if (!read_ptr(global.load(), smain) || !smain || !read_ptr(smain + collision_offset, collision) || !collision) return false;
    std::uint8_t active{};
    if (!read_ptr(collision, vt) || vt != vtable.load() || !read_u8(collision + active_offset, active) || active > 1) {
        mismatch = true;
        return false;
    }
    flags[0] = collision + collider_offset + contact_flag;
    flags[1] = collision + collider_offset + setup_flag;
    return true;
}

void apply(unsigned want) noexcept {
    // The contact pass runs the engine's callbacks on the job threads: only
    // with the contact guard's lock ready (contact_guard.hpp), else not at all.
    const bool unguarded = (want & contact_pass) && !contact_guard::ready();
    if (unguarded) want &= ~contact_pass;
    std::uintptr_t flags[2]{};
    bool mismatch{};
    if (!locate(flags, mismatch)) {
        if (mismatch) {
            undo();
            state.store(layout_mismatch);
        } else {
            state.store(waiting);
        }
        return;
    }
    std::uint8_t values[2]{};
    for (unsigned i = 0; i < 2; ++i)
        if (!read_u8(flags[i], values[i]) || values[i] > 1) {
            undo();
            state.store(layout_mismatch);
            return;
        }
    bool ours = false, engines = false;
    for (unsigned i = 0; i < 2; ++i) {
        if (written_at[i] && written_at[i] != flags[i]) written_at[i] = 0;  // another sCollision: the old one is gone
        if (want & (1U << i)) {
            if (values[i] == 0) {  // set, or set again after the engine's setup cleared it
                if (i == 0) contact_guard::set_locking(true);  // the lock before the flag
                if (!write_u8(flags[i], 1)) {
                    undo();
                    state.store(layout_mismatch);
                    return;
                }
                values[i] = 1;
                written_at[i] = flags[i];
                writes.fetch_add(1, std::memory_order_relaxed);
            }
            (written_at[i] ? ours : engines) = true;
        } else if (written_at[i]) {
            if (values[i] == 1) write_u8(flags[i], 0);
            values[i] = 0;
            written_at[i] = 0;
        }
        seen[i].store(values[i]);
    }
    // Locking while the contact flag is set, ours or the engine's; off after
    // it was cleared (here, at BEGIN: no collision runs now).
    contact_guard::set_locking(values[0] == 1);
    state.store(unguarded && !(want & setup_pass) ? no_guard : !want ? off : ours ? applied : engines ? engine : off);
}
}  // namespace

Discovery discover(const std::uintptr_t (&sites)[4], const unsigned (&counts)[4], std::uintptr_t smain_global) noexcept {
    Discovery d;
    for (unsigned i = 0; i < 4; ++i) d.counts[i] = counts[i];
    for (const auto c : counts)
        if (c != 1) {
            d.reason = c ? "signature not unique" : "signature not found";
            return d;
        }
    if (!smain_global) {
        d.reason = "no sMain global (job_cap)";
        return d;
    }
    d.global = rel32(sites[0], global_rel, global_next);
    if (d.global != smain_global) {
        d.reason = "the setup reads another global than sMain's";
        return d;
    }
    d.vtable = rel32(sites[3], vtable_rel, vtable_next);
    d.found = true;
    d.reason = "ok";
    return d;
}

std::string discovery_event(const Discovery& d, std::uintptr_t base, double t_ms, std::string_view scanner) {
    std::string out = "{";
    json::key(out, "event");
    json::string(out, "contacts");
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
    json::key(out, "vtable_rva");
    json::number(out, std::uint64_t(d.vtable ? d.vtable - base : 0));
    out += '}';
    return out;
}

void set_target(const Discovery& d) noexcept {
    written_at[0] = written_at[1] = 0;
    seen[0].store(-1);
    seen[1].store(-1);
    global.store(d.found ? d.global : 0);
    vtable.store(d.found ? d.vtable : 0);
    state.store(d.found ? waiting : no_target);
}
void request(unsigned mode) noexcept { requested.store(mode & (contact_pass | setup_pass)); }

void on_begin() noexcept {
    if (final_state(state.load(std::memory_order_relaxed))) return;
    const auto want = requested.load(std::memory_order_relaxed);
    if (!want && !written_at[0] && !written_at[1]) {
        if (state.load(std::memory_order_relaxed) == applied || state.load(std::memory_order_relaxed) == engine) state.store(off);
        return;  // nothing to do or undo
    }
    apply(want);
}

void restore() noexcept {
    if (final_state(state.load())) return;
    undo();
    state.store(restored);
}

void append(std::string& out) {
    json::key(out, "contacts");
    out += '{';
    json::key(out, "state");
    json::string(out, state_names[state.load()]);
    json::key(out, "mode");
    json::number(out, std::uint64_t(requested.load()));
    json::key(out, "contact");
    json::number(out, std::int64_t(seen[0].load()));
    json::key(out, "setup");
    json::number(out, std::int64_t(seen[1].load()));
    json::key(out, "writes");
    json::number(out, writes.exchange(0));
    out += '}';
}
}  // namespace cpuopt::contacts
