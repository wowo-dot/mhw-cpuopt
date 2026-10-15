#pragma once
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace cpuopt::image_scan {
// Our own search of a loaded image's executable sections, region by region
// (readable runs only, under SEH), for when Astera Loader's scanner is not
// there, and for the tests. Returns how many matches there are (0 for an
// unreadable pattern); stores the first `capacity` addresses in order.
unsigned find(std::uintptr_t image_base, std::string_view pattern, std::uintptr_t* matches, unsigned capacity) noexcept;

// Whether [address, address + bytes) is committed, readable memory.
bool readable(std::uintptr_t address, std::size_t bytes) noexcept;
// Whether the RVA range lies in a writable section of the image.
bool in_writable_section(std::uintptr_t image_base, std::uint32_t rva, std::uint32_t bytes) noexcept;
}  // namespace cpuopt::image_scan
