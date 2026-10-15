#include "frame_clock.hpp"
#include "../core/json.hpp"
#include "../core/signature.hpp"
#include "log.hpp"
#include <atomic>

namespace cpuopt::frame_clock {
namespace {
constexpr std::uint32_t tested_site = 0x23922C2, tested_singleton = 0x51C4480, tested_offset = 0x1F210;
constexpr std::size_t site_bytes = 24;  // the signature's length

bool readable_protection(DWORD protect) noexcept {
    if (protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    return (protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                       PAGE_EXECUTE_WRITECOPY)) != 0;
}
bool readable(std::uintptr_t address, std::size_t bytes) noexcept {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<const void*>(address), &mbi, sizeof(mbi))) return false;
    const auto end = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return mbi.State == MEM_COMMIT && readable_protection(mbi.Protect) && address + bytes <= end;
}

struct Section {
    std::uint32_t rva{}, bytes{}, characteristics{};
};
// Sections of the loaded image; false if the headers are not readable.
bool sections(std::uintptr_t base, Section* out, unsigned capacity, unsigned& count) noexcept {
    count = 0;
    __try {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + static_cast<std::uintptr_t>(dos->e_lfanew));
        if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
        const auto* s = IMAGE_FIRST_SECTION(nt);
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections && count < capacity; ++i, ++s)
            out[count++] = {s->VirtualAddress, s->Misc.VirtualSize, s->Characteristics};
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// Scans one readable run; false if it faulted.
bool scan_run(const Pattern& p, const std::uint8_t* data, std::size_t bytes, std::size_t* hits, std::size_t& found) noexcept {
    __try {
        found = p.find(data, bytes, hits, match_capacity);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// Copies a match's bytes; false if they are not readable.
bool copy_site(std::uintptr_t at, std::uint8_t (&out)[site_bytes]) noexcept {
    if (!readable(at, site_bytes)) return false;
    __try {
        for (std::size_t i = 0; i < site_bytes; ++i) out[i] = reinterpret_cast<const volatile std::uint8_t*>(at)[i];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

struct Shared {
    std::atomic<std::uintptr_t> singleton{};  // absolute address of the sRender pointer
    std::atomic<std::uint32_t> offset{};
    std::atomic<DWORD> thread{};
    std::atomic<bool> stop{}, renderer{};
    std::atomic<std::uint64_t> counter{}, frames{}, polls{}, multi_step{}, max_gap_us{}, gap_total_us{};
    IntervalHistogram intervals;
} shared;

DWORD WINAPI poll(void*) noexcept {
    shared.thread.store(GetCurrentThreadId());
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    const auto timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    const auto frequency = qpc_frequency();
    const auto us = [frequency](std::int64_t ticks) { return static_cast<std::uint64_t>(ticks) * 1000000ULL / static_cast<std::uint64_t>(frequency); };
    std::uintptr_t validated{};
    std::uint64_t last{};
    std::int64_t last_poll = qpc_now(), last_change{};
    bool have_last = false, have_change = false;
    while (!shared.stop.load(std::memory_order_relaxed)) {
        const auto now = qpc_now();
        const auto gap = us(now - last_poll);
        last_poll = now;
        shared.polls.fetch_add(1, std::memory_order_relaxed);
        shared.gap_total_us.fetch_add(gap, std::memory_order_relaxed);
        auto seen = shared.max_gap_us.load(std::memory_order_relaxed);
        while (gap > seen && !shared.max_gap_us.compare_exchange_weak(seen, gap, std::memory_order_relaxed)) {}
        std::uint64_t value{};
        if (read(validated, value)) {
            shared.renderer.store(true, std::memory_order_relaxed);
            shared.counter.store(value, std::memory_order_relaxed);
            if (have_last && value > last) {
                const auto steps = value - last;
                shared.frames.fetch_add(steps, std::memory_order_relaxed);
                if (steps == 1 && have_change) shared.intervals.add(us(now - last_change));
                if (steps > 1) shared.multi_step.fetch_add(1, std::memory_order_relaxed);
                last_change = now;
                have_change = true;
            } else if (have_last && value < last) {
                have_change = false;  // counter reset: a new renderer
            }
            last = value;
            have_last = true;
        } else {
            shared.renderer.store(false, std::memory_order_relaxed);
            have_last = have_change = false;
        }
        if (timer) {
            LARGE_INTEGER due{};
            due.QuadPart = -5000;  // asks for 500 us; Windows delivers about 1 ms
            SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(timer, 20);
        } else {
            Sleep(1);
        }
    }
    if (timer) CloseHandle(timer);
    return 0;
}
}  // namespace

unsigned scan(std::uintptr_t base, std::uintptr_t* matches, unsigned capacity) noexcept {
    const auto pattern = Pattern::parse(signature);
    Section list[32];
    unsigned count{}, total{};
    if (!pattern || !sections(base, list, 32, count)) return 0;
    for (unsigned i = 0; i < count; ++i) {
        if (!(list[i].characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        // Contiguous readable runs inside the section, region by region.
        auto at = base + list[i].rva;
        const auto end = at + list[i].bytes;
        while (at < end) {
            MEMORY_BASIC_INFORMATION mbi{};
            if (!VirtualQuery(reinterpret_cast<const void*>(at), &mbi, sizeof(mbi))) break;
            auto run_end = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
            if (mbi.State == MEM_COMMIT && readable_protection(mbi.Protect)) {
                // Extend over following readable regions so no match is split.
                while (run_end < end) {
                    MEMORY_BASIC_INFORMATION next{};
                    if (!VirtualQuery(reinterpret_cast<const void*>(run_end), &next, sizeof(next)) || next.State != MEM_COMMIT ||
                        !readable_protection(next.Protect))
                        break;
                    run_end = reinterpret_cast<std::uintptr_t>(next.BaseAddress) + next.RegionSize;
                }
                if (run_end > end) run_end = end;
                std::size_t hits[match_capacity]{}, found{};
                if (scan_run(*pattern, reinterpret_cast<const std::uint8_t*>(at), run_end - at, hits, found)) {
                    for (std::size_t h = 0; h < found && h < match_capacity; ++h)
                        if (total + h < capacity) matches[total + h] = at + hits[h];
                    total += static_cast<unsigned>(found);
                }
            }
            at = run_end;
        }
    }
    return total;
}

Discovery discover(std::uintptr_t base, const std::uintptr_t* matches, unsigned count) noexcept {
    Discovery d;
    d.matches = count;
    Section list[32];
    unsigned sections_found{};
    if (!sections(base, list, 32, sections_found)) {
        d.reason = "image headers unreadable";
        return d;
    }
    if (count == 0) {
        d.reason = "signature not found";
        return d;
    }
    if (count > match_capacity) {  // matches past the first ones cannot be checked
        d.reason = "too many matches";
        return d;
    }
    std::uint32_t singleton{}, offset{}, site{};
    for (unsigned i = 0; i < count; ++i) {
        std::uint8_t code[site_bytes];
        if (matches[i] < base || !copy_site(matches[i], code)) {
            d.reason = "match unreadable";
            return d;
        }
        const auto rva = static_cast<std::uint32_t>(matches[i] - base);
        const auto target = static_cast<std::uint32_t>(rip_target(rva, 7, read_i32(code + 3)));
        const auto field = static_cast<std::uint32_t>(read_i32(code + 20));
        if (i == 0) {
            site = rva;
            singleton = target;
            offset = field;
        } else if (target != singleton || field != offset) {
            d.reason = "matches disagree";
            return d;
        }
    }
    // The singleton must be in a writable data section; the counter field
    // must be 8-byte aligned and inside a plausible object.
    bool in_data = false;
    for (unsigned i = 0; i < sections_found; ++i)
        in_data = in_data || ((list[i].characteristics & IMAGE_SCN_MEM_WRITE) && singleton >= list[i].rva &&
                              singleton + 8 <= list[i].rva + list[i].bytes);
    if (!in_data || offset % 8 != 0 || offset >= 0x100000) {
        d.reason = "operands implausible";
        return d;
    }
    if (!readable(base + singleton, 8)) {
        d.reason = "singleton unreadable";
        return d;
    }
    d.site_rva = site;
    d.singleton_rva = singleton;
    d.counter_offset = offset;
    d.expected_values = site == tested_site && singleton == tested_singleton && offset == tested_offset;
    d.found = true;
    d.reason = "ok";
    return d;
}

std::string discovery_event(const Discovery& d, double t_ms, std::string_view scanner) {
    std::string out = "{";
    json::key(out, "event");
    json::string(out, "frame_clock");
    json::key(out, "t_ms");
    json::fixed(out, t_ms, 3);
    json::key(out, "found");
    out += d.found ? "true" : "false";
    json::key(out, "reason");
    json::string(out, d.reason);
    json::key(out, "scanner");
    json::string(out, scanner);
    json::key(out, "matches");
    json::number(out, std::uint64_t(d.matches));
    json::key(out, "site_rva");
    json::number(out, std::uint64_t(d.site_rva));
    json::key(out, "singleton_rva");
    json::number(out, std::uint64_t(d.singleton_rva));
    json::key(out, "counter_offset");
    json::number(out, std::uint64_t(d.counter_offset));
    json::key(out, "expected_values");
    out += d.expected_values ? "true" : "false";
    out += '}';
    return out;
}

void set_target(std::uintptr_t base, const Discovery& d) noexcept {
    if (!d.found) return;
    shared.offset.store(d.counter_offset);
    shared.singleton.store(base + d.singleton_rva);
}

bool has_target() noexcept { return shared.singleton.load() != 0; }

bool read(std::uintptr_t& validated, std::uint64_t& value) noexcept {
    const auto singleton = shared.singleton.load(std::memory_order_relaxed);
    const auto offset = shared.offset.load(std::memory_order_relaxed);
    if (!singleton) return false;
    __try {
        const auto renderer = *reinterpret_cast<volatile const std::uintptr_t*>(singleton);
        if (!renderer) return false;
        if (renderer != validated) {  // checked once per renderer pointer value
            if (!readable(renderer + offset, sizeof(std::uint64_t))) return false;
            validated = renderer;
        }
        value = *reinterpret_cast<volatile const std::uint64_t*>(renderer + offset);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        validated = 0;
        return false;
    }
}

void start_poller() noexcept {
    if (!has_target() || shared.thread.load()) return;
    shared.stop.store(false);
    if (const auto thread = CreateThread(nullptr, 0, poll, nullptr, 0, nullptr)) CloseHandle(thread);
    for (int i = 0; i < 100 && !shared.thread.load(); ++i) Sleep(5);
}

void stop_poller() noexcept { shared.stop.store(true); }

DWORD poller_thread() noexcept { return shared.thread.load(); }

Window take() noexcept {
    Window w;
    w.renderer = shared.renderer.load(std::memory_order_relaxed);
    w.counter = shared.counter.load(std::memory_order_relaxed);
    w.frames = shared.frames.exchange(0, std::memory_order_relaxed);
    w.polls = shared.polls.exchange(0, std::memory_order_relaxed);
    w.multi_step = shared.multi_step.exchange(0, std::memory_order_relaxed);
    w.max_poll_gap_us = shared.max_gap_us.exchange(0, std::memory_order_relaxed);
    w.poll_gap_total_us = shared.gap_total_us.exchange(0, std::memory_order_relaxed);
    w.intervals = shared.intervals.take();
    return w;
}
}  // namespace cpuopt::frame_clock
