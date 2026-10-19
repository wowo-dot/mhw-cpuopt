#pragma once
#include <windows.h>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace cpuopt::system_info {
// Lower-case hex SHA-256 of a file, or empty if it cannot be read.
std::string sha256_file(const std::wstring& path);
std::wstring module_path(HMODULE module);
std::wstring folder_of(const std::wstring& path);

// What the loader reported about itself and the game.
struct LoaderInfo {
    std::string name;
    std::uint32_t api_version{};
    std::string game_version, exe_sha256;  // empty when the loader did not say
};

// The "start" event: build identity, loader, executable and DLL hashes, CPU
// and cache layout, CPU sets and affinity, OS build and the game's own
// graphics and CPU settings files (read only). The executable's hash is the
// loader's when it gave one (Astera Loader hashes the exe before plugins
// start), else read from the file (84 MB: slow on some disks).
std::string start_event(HMODULE self, const std::wstring& control_path, const LoaderInfo& loader);
// The "init" event, the log's first line, written before anything slow:
// build identity, mode ("player" or "research"), the loader's API and game
// version, and Wine's version when running under Wine or Proton, so a log
// from a game that ends early still says what ran.
std::string init_event(const char* mode, const LoaderInfo& loader);
// Wine's version ("9.0") when this process runs under Wine or Proton
// (ntdll exports wine_get_version there), else empty.
std::string wine_version();
// The loader's exe hash in lower case if it is a SHA-256 (64 hex digits), else empty.
std::string loader_hash(const LoaderInfo& loader);

// For the benchmark's report: plain text, no paths.
std::string cpu_name();                         // the CPU's brand string
std::string gpu_name();                         // the primary display's adapter
std::string windows_version();                  // "10.0.26200"
bool tested_exe(std::string_view sha256) noexcept;  // the build cpuopt was made on
std::string ini_value(const std::wstring& file, const wchar_t* section, const wchar_t* key);
// File names of the loaded DLLs from outside Windows but the exe and `self`, sorted, ", " between.
std::string other_modules(HMODULE self);

// Loaded modules; hashes are computed once per path for modules outside the
// Windows directory (game folder, mods, overlays).
class ModuleCensus {
public:
    // The "modules" event if the set of loaded modules changed since the last
    // call, else empty.
    std::string changed_event(double t_ms);
    // A hash already known (the exe's from the loader), so it is not read again.
    void known(const std::wstring& path, const std::string& sha256) {
        if (!sha256.empty()) hashes_[path] = sha256;
    }

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
