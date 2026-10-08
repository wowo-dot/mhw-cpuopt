#pragma once
#include <windows.h>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace cpuopt::system_info {
// Lower-case hex SHA-256 of a file, or empty if it cannot be read.
std::string sha256_file(const std::wstring& path);
std::wstring module_path(HMODULE module);
std::wstring folder_of(const std::wstring& path);

// The "start" event: build identity, executable and DLL hashes, CPU and cache
// layout, CPU sets and affinity, OS build and the game's own graphics and CPU
// settings files (read only).
std::string start_event(HMODULE self, const std::wstring& control_path);

// Loaded modules; hashes are computed once per path for modules outside the
// Windows directory (game folder, mods, overlays).
class ModuleCensus {
public:
    // The "modules" event if the set of loaded modules changed since the last
    // call, else empty.
    std::string changed_event(double t_ms);

private:
    struct Entry {
        std::wstring path;
        std::uintptr_t base{};
        std::uint32_t bytes{};
        bool operator==(const Entry&) const = default;
    };
    std::vector<Entry> last_;
    std::map<std::wstring, std::string> hashes_;
};
}  // namespace cpuopt::system_info
