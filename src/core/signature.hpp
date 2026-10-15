#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace cpuopt {
// A code signature such as "48 8B 05 ?? ?? ?? ?? 49 B9". "??" matches any
// byte. Fixed storage and no allocation, so a scan can run under SEH.
class Pattern {
public:
    static constexpr std::size_t capacity = 256;  // as Astera Loader's scanner
    static std::optional<Pattern> parse(std::string_view text) noexcept;
    std::size_t size() const noexcept { return size_; }
    bool matches(const std::uint8_t* at) const noexcept;
    // Offsets of matches in [data, data + length), in order; returns how many
    // were found in total (only the first `out_capacity` are stored).
    std::size_t find(const std::uint8_t* data, std::size_t length, std::size_t* out,
                     std::size_t out_capacity) const noexcept;

private:
    std::array<std::uint8_t, capacity> bytes_{};
    std::array<bool, capacity> fixed_{};
    std::size_t size_{}, anchor_{};
};

// Target RVA of a RIP-relative operand: the instruction at `instruction_rva`
// is `length` bytes long and its 32-bit displacement is `displacement`.
constexpr std::int64_t rip_target(std::uint32_t instruction_rva, std::uint32_t length, std::int32_t displacement) noexcept {
    return std::int64_t(instruction_rva) + length + displacement;
}

inline std::int32_t read_i32(const std::uint8_t* p) noexcept {
    return std::int32_t(std::uint32_t(p[0]) | std::uint32_t(p[1]) << 8 | std::uint32_t(p[2]) << 16 | std::uint32_t(p[3]) << 24);
}
}  // namespace cpuopt
