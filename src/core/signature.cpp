#include "signature.hpp"
#include <cstring>

namespace cpuopt {
namespace {
int hex(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
}  // namespace

std::optional<Pattern> Pattern::parse(std::string_view text) noexcept {
    Pattern p;
    std::size_t i = 0;
    while (i < text.size()) {
        if (text[i] == ' ') {
            ++i;
            continue;
        }
        if (i + 1 >= text.size() || p.size_ == capacity) return std::nullopt;
        const char a = text[i], b = text[i + 1];
        if (i + 2 < text.size() && text[i + 2] != ' ') return std::nullopt;
        if (a == '?' && b == '?') {
            p.fixed_[p.size_++] = false;
        } else {
            const int hi = hex(a), lo = hex(b);
            if (hi < 0 || lo < 0) return std::nullopt;
            p.bytes_[p.size_] = static_cast<std::uint8_t>(hi << 4 | lo);
            p.fixed_[p.size_++] = true;
        }
        i += 2;
    }
    // The first fixed byte drives the memchr search; a pattern of wildcards only is useless.
    for (p.anchor_ = 0; p.anchor_ < p.size_ && !p.fixed_[p.anchor_]; ++p.anchor_) {}
    if (p.anchor_ == p.size_) return std::nullopt;
    return p;
}

bool Pattern::matches(const std::uint8_t* at) const noexcept {
    for (std::size_t i = 0; i < size_; ++i)
        if (fixed_[i] && at[i] != bytes_[i]) return false;
    return true;
}

std::size_t Pattern::find(const std::uint8_t* data, std::size_t length, std::size_t* out, std::size_t out_capacity) const noexcept {
    if (length < size_) return 0;
    std::size_t found = 0;
    const std::size_t last = length - size_;  // last start offset
    std::size_t start = 0;
    while (start <= last) {
        const auto* hit = static_cast<const std::uint8_t*>(std::memchr(data + start + anchor_, bytes_[anchor_], last - start + 1));
        if (!hit) break;
        const auto at = static_cast<std::size_t>(hit - data) - anchor_;
        if (matches(data + at)) {
            if (found < out_capacity) out[found] = at;
            ++found;
        }
        start = at + 1;
    }
    return found;
}
}  // namespace cpuopt
