#include "image_scan.hpp"
#include "../core/signature.hpp"
#include <windows.h>

namespace cpuopt::image_scan {
namespace {
constexpr unsigned run_capacity = 64;  // matches kept per readable run

bool readable_protection(DWORD protect) noexcept {
    if (protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    return (protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                       PAGE_EXECUTE_WRITECOPY)) != 0;
}

struct Section {
    std::uint32_t rva{}, bytes{}, characteristics{};
};
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
bool scan_run(const Pattern& p, const std::uint8_t* data, std::size_t bytes, std::size_t* hits, std::size_t& found) noexcept {
    __try {
        found = p.find(data, bytes, hits, run_capacity);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
}  // namespace

bool readable(std::uintptr_t address, std::size_t bytes) noexcept {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<const void*>(address), &mbi, sizeof(mbi))) return false;
    const auto end = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return mbi.State == MEM_COMMIT && readable_protection(mbi.Protect) && address + bytes <= end;
}

bool in_writable_section(std::uintptr_t base, std::uint32_t rva, std::uint32_t bytes) noexcept {
    Section list[32];
    unsigned count{};
    if (!sections(base, list, 32, count)) return false;
    for (unsigned i = 0; i < count; ++i)
        if ((list[i].characteristics & IMAGE_SCN_MEM_WRITE) && rva >= list[i].rva && rva + bytes <= list[i].rva + list[i].bytes) return true;
    return false;
}

unsigned find(std::uintptr_t base, std::string_view text, std::uintptr_t* matches, unsigned capacity) noexcept {
    const auto pattern = Pattern::parse(text);
    Section list[32];
    unsigned count{}, total{};
    if (!pattern || !sections(base, list, 32, count)) return 0;
    for (unsigned i = 0; i < count; ++i) {
        if (!(list[i].characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
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
                std::size_t hits[run_capacity]{}, found{};
                if (scan_run(*pattern, reinterpret_cast<const std::uint8_t*>(at), run_end - at, hits, found)) {
                    for (std::size_t h = 0; h < found && h < run_capacity; ++h)
                        if (total + h < capacity) matches[total + h] = at + hits[h];
                    total += static_cast<unsigned>(found);
                }
            }
            at = run_end;
        }
    }
    return total;
}
}  // namespace cpuopt::image_scan
