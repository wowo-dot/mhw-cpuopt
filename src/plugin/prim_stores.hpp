#pragma once
#include <windows.h>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace cpuopt::prim_stores {
// Switch prim_stores (toggles.hpp): sPrimitive's record loop writes without
// reading.
//
// sub_142570A40 (sPrimitive, called from the effect draw jobs of the camera
// round) takes room for one 16-byte record per primitive from a ring buffer
// (sPrimitive +0x138, the mapped pointer; +0x144 the atomic fill offset) and
// fills the records in a loop. The first two dwords of each record are bit
// fields, and the compiler writes them with eight read-modify-write steps on
// memory (and/or on [rax-4] and [rax]); their final values do not depend on
// what was there before:
//   dword 0 = v (0, 1 or 2, in edx) | r12d << 4
//   dword 1 = (r14d & 0xF) | r13d << 4
// In the al-05 profile this one function had 5,793 samples (6.0% of the game
// thread), ten times any other game function outside executeJob's spin. If
// the ring is GPU upload memory (write-combined), each of the eight reads per
// record is an uncached read; `probe` checks that, read-only.
//
// The change (56 bytes from 0x2570C1A, a same-length CODE patch through
// al.patch): the loop preheader's 6-byte NOP becomes `or r9d,edx` and
// `or r10d,r14d`, which complete the two dwords once (r9d = r12d << 4 and
// r10d = r13d << 4 are already computed there), and the loop stores them with
// two plain movs. The other three stores, the counter and the loop's exit
// stay as they were; 20 bytes of NOPs fill the rest of the old body. The
// memory written is the same, byte for byte. After the loop, edx is unchanged
// (the next call's second argument); r9d and r10d differ but the call that
// follows clobbers them (volatile in the x64 ABI), and nothing jumps into the
// changed bytes but the loop's own branch, still at 0x2570C20.
//
// Signature (tested exe c2ebbbd2..., clean image a78ea69b...): from
// 0x2570BE7 to the loop's end, matched once; it holds the sPrimitive global
// (mov rax,[rip+0x51CC1F8] at +2).
inline constexpr std::string_view signature =
    "33 C9 48 8B 05 ?? ?? ?? ?? 44 89 44 24 40 2B 98 48 01 00 00 45 85 C0 74 52 45 8B CC 48 8D 45 04 41 C1 E1 04 "
    "41 83 E6 0F 45 8B D5 45 0F B7 D8 41 C1 E2 04 66 0F 1F 44 00 00 83 60 FC F0 09 50 FC 83 20 F0 44 09 30 83 60 FC "
    "0F 44 09 48 FC 83 20 0F 44 09 10 66 89 48 04 FF C1 66 44 89 58 06 89 58 08 48 8D 40 10 41 3B C8 72 CE";
inline constexpr unsigned global_operand = 2;   // mov rax,[rip+sPrimitive]: 7 bytes at match + 2
inline constexpr unsigned preheader = 25;       // the loop preheader (0x2570C00) in the match
inline constexpr unsigned patch_offset = 51;    // the change (0x2570C1A) in the match
inline constexpr unsigned patch_length = 56;    // to the loop's end (0x2570C52)
inline constexpr std::uint32_t ring_offset = 0x138;  // sPrimitive: the record ring's mapped pointer

// The bytes there now (from the signature) and the replacement.
inline constexpr std::array<std::uint8_t, patch_length> expected = {
    0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00,  // nop word [rax+rax+0]
    0x83, 0x60, 0xFC, 0xF0,              // loop: and dword [rax-4],0FFFFFFF0h
    0x09, 0x50, 0xFC,                    // or [rax-4],edx
    0x83, 0x20, 0xF0,                    // and dword [rax],0FFFFFFF0h
    0x44, 0x09, 0x30,                    // or [rax],r14d
    0x83, 0x60, 0xFC, 0x0F,              // and dword [rax-4],0Fh
    0x44, 0x09, 0x48, 0xFC,              // or [rax-4],r9d
    0x83, 0x20, 0x0F,                    // and dword [rax],0Fh
    0x44, 0x09, 0x10,                    // or [rax],r10d
    0x66, 0x89, 0x48, 0x04,              // mov [rax+4],cx
    0xFF, 0xC1,                          // inc ecx
    0x66, 0x44, 0x89, 0x58, 0x06,        // mov [rax+6],r11w
    0x89, 0x58, 0x08,                    // mov [rax+8],ebx
    0x48, 0x8D, 0x40, 0x10,              // lea rax,[rax+10h]
    0x41, 0x3B, 0xC8,                    // cmp ecx,r8d
    0x72, 0xCE};                         // jb loop
inline constexpr std::array<std::uint8_t, patch_length> replacement = {
    0x41, 0x09, 0xD1,                                // or r9d,edx       dword 0, whole
    0x45, 0x09, 0xF2,                                // or r10d,r14d     dword 1, whole
    0x44, 0x89, 0x48, 0xFC,                          // loop: mov [rax-4],r9d
    0x44, 0x89, 0x10,                                // mov [rax],r10d
    0x66, 0x89, 0x48, 0x04,                          // mov [rax+4],cx
    0xFF, 0xC1,                                      // inc ecx
    0x66, 0x44, 0x89, 0x58, 0x06,                    // mov [rax+6],r11w
    0x89, 0x58, 0x08,                                // mov [rax+8],ebx
    0x48, 0x8D, 0x40, 0x10,                          // lea rax,[rax+10h]
    0x41, 0x3B, 0xC8,                                // cmp ecx,r8d
    0x72, 0xE2,                                      // jb loop (still 0x2570C20)
    0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00,  // nop dword [rax+rax+0]
    0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00,  // nop dword [rax+rax+0]
    0x0F, 0x1F, 0x40, 0x00};                         // nop dword [rax+0]

struct Discovery {
    bool found{};
    const char* reason{"not searched"};
    unsigned matches{};
    std::uint32_t site_rva{}, global_rva{};  // the match; sPrimitive's global
};
// From the scanner's matches for `signature`.
Discovery discover(std::uintptr_t image_base, const std::uintptr_t* matches, unsigned count) noexcept;
std::string discovery_event(const Discovery&, double t_ms, std::string_view scanner);

// The ring's memory, read-only: sPrimitive from its global, the pointer at
// +0x138, VirtualQuery on it.
struct Buffer {
    bool object{}, pointer{};  // sPrimitive exists; the ring is mapped
    std::uintptr_t address{};
    MEMORY_BASIC_INFORMATION info{};
    bool write_combined() const noexcept { return pointer && (info.Protect & PAGE_WRITECOMBINE); }
};
Buffer probe(std::uintptr_t global) noexcept;
// "prim_buffer" event, once the ring is mapped.
std::string buffer_event(const Buffer&, double t_ms);
}  // namespace cpuopt::prim_stores
