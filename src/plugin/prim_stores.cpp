#include "prim_stores.hpp"
#include "../core/json.hpp"
#include "../core/signature.hpp"

namespace cpuopt::prim_stores {
namespace {
bool read_pointer(std::uintptr_t at, std::uintptr_t& value) noexcept {
    __try {
        value = *reinterpret_cast<volatile const std::uintptr_t*>(at);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
}  // namespace

Discovery discover(std::uintptr_t base, const std::uintptr_t* matches, unsigned count) noexcept {
    Discovery d;
    d.matches = count;
    if (count != 1) {
        d.reason = count ? "signature not unique" : "signature not found";
        return d;
    }
    const auto site = matches[0];
    const auto* code = reinterpret_cast<const std::uint8_t*>(site);
    // The signature's fixed bytes at the change must be the expected bytes.
    for (unsigned i = 0; i < patch_length; ++i)
        if (code[patch_offset + i] != expected[i]) {
            d.reason = "changed bytes differ";
            return d;
        }
    const auto rva = static_cast<std::uint32_t>(site - base);
    const auto global = rip_target(rva + global_operand, 7, read_i32(code + global_operand + 3));
    if (global <= 0) {
        d.reason = "operand changed";
        return d;
    }
    d.found = true;
    d.reason = "ok";
    d.site_rva = rva;
    d.global_rva = static_cast<std::uint32_t>(global);
    return d;
}

std::string discovery_event(const Discovery& d, double t_ms, std::string_view scanner) {
    std::string out = "{";
    json::key(out, "event");
    json::string(out, "prim_stores");
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
    json::key(out, "global_rva");
    json::number(out, std::uint64_t(d.global_rva));
    out += '}';
    return out;
}

Buffer probe(std::uintptr_t global) noexcept {
    Buffer b;
    std::uintptr_t object{};
    if (!global || !read_pointer(global, object) || !object) return b;
    b.object = true;
    if (!read_pointer(object + ring_offset, b.address) || !b.address) return b;
    b.pointer = VirtualQuery(reinterpret_cast<const void*>(b.address), &b.info, sizeof(b.info)) == sizeof(b.info);
    return b;
}

std::string buffer_event(const Buffer& b, double t_ms) {
    std::string out = "{";
    json::key(out, "event");
    json::string(out, "prim_buffer");
    json::key(out, "t_ms");
    json::fixed(out, t_ms, 3);
    json::key(out, "write_combined");
    out += b.write_combined() ? "true" : "false";
    json::key(out, "protect");
    json::number(out, std::uint64_t(b.info.Protect));
    json::key(out, "allocation_protect");
    json::number(out, std::uint64_t(b.info.AllocationProtect));
    json::key(out, "type");
    json::number(out, std::uint64_t(b.info.Type));
    json::key(out, "state");
    json::number(out, std::uint64_t(b.info.State));
    json::key(out, "region_mb");
    json::fixed(out, double(b.info.RegionSize) / (1024.0 * 1024.0), 2);
    out += '}';
    return out;
}
}  // namespace cpuopt::prim_stores
