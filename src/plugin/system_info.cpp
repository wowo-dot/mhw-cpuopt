#include "system_info.hpp"
#include "../core/json.hpp"
#include "../core/toggles.hpp"
#include "log.hpp"
#include <bcrypt.h>
#include <intrin.h>
#include <tlhelp32.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>

#ifndef CPUOPT_VERSION
#define CPUOPT_VERSION "0.0.0"
#endif
#ifndef CPUOPT_COMMIT
#define CPUOPT_COMMIT "unknown"
#endif

namespace cpuopt::system_info {
namespace {
// The executable all offsets and signatures were derived from.
constexpr std::string_view tested_exe_sha256 = "c2ebbbd2c49f216d484e31a5219bed419eb1e5e7d206d02cba040a3ab79d90ea";

void hex64(std::string& out, std::uint64_t v) {
    char buffer[24];
    const auto n = std::snprintf(buffer, sizeof(buffer), "\"0x%llx\"", static_cast<unsigned long long>(v));
    out.append(buffer, n > 0 ? static_cast<std::size_t>(n) : 0);
}

std::string cpu_brand() {
    std::array<int, 4> r{};
    __cpuid(r.data(), static_cast<int>(0x80000000));
    if (static_cast<unsigned>(r[0]) < 0x80000004U) return {};
    std::array<char, 49> brand{};
    for (unsigned i = 0; i < 3; ++i) {
        __cpuid(r.data(), static_cast<int>(0x80000002U + i));
        std::memcpy(brand.data() + i * 16, r.data(), 16);
    }
    std::string s(brand.data());
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

void caches(std::string& out) {
    DWORD bytes{};
    GetLogicalProcessorInformationEx(RelationCache, nullptr, &bytes);
    auto buffer = std::make_unique<std::byte[]>(bytes);
    out += '[';
    if (bytes && GetLogicalProcessorInformationEx(RelationCache, reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.get()), &bytes)) {
        bool first = true;
        for (DWORD at = 0; at < bytes;) {
            const auto* info = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.get() + at);
            const auto& c = info->Cache;
            if (c.Level >= 2 && (c.Type == CacheUnified || c.Type == CacheData)) {
                out += first ? "{" : ",{";
                first = false;
                json::key(out, "level");
                json::number(out, std::uint64_t(c.Level));
                json::key(out, "bytes");
                json::number(out, std::uint64_t(c.CacheSize));
                json::key(out, "group");
                json::number(out, std::uint64_t(c.GroupMask.Group));
                json::key(out, "mask");
                hex64(out, c.GroupMask.Mask);
                out += '}';
            }
            at += info->Size;
        }
    }
    out += ']';
}

// Per logical processor: core, last-level cache, efficiency class, parked and
// allocated flags; and the process default CPU sets as logical indices.
void cpu_sets(std::string& out) {
    ULONG bytes{};
    const auto process = GetCurrentProcess();
    GetSystemCpuSetInformation(nullptr, 0, &bytes, process, 0);
    auto buffer = std::make_unique<std::byte[]>(bytes);
    std::vector<std::pair<ULONG, BYTE>> ids;  // id -> logical index
    out += '[';
    if (bytes && GetSystemCpuSetInformation(reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buffer.get()), bytes, &bytes, process, 0)) {
        bool first = true;
        for (ULONG at = 0; at < bytes;) {
            const auto* info = reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(buffer.get() + at);
            if (info->Type == CpuSetInformation) {
                const auto& s = info->CpuSet;
                ids.emplace_back(s.Id, s.LogicalProcessorIndex);
                out += first ? "[" : ",[";
                first = false;
                json::number(out, std::uint64_t(s.Group));
                out += ',';
                json::number(out, std::uint64_t(s.LogicalProcessorIndex));
                out += ',';
                json::number(out, std::uint64_t(s.CoreIndex));
                out += ',';
                json::number(out, std::uint64_t(s.LastLevelCacheIndex));
                out += ',';
                json::number(out, std::uint64_t(s.EfficiencyClass));
                out += ',';
                json::number(out, std::uint64_t(s.Parked));
                out += ',';
                json::number(out, std::uint64_t(s.Allocated));
                out += ',';
                json::number(out, std::uint64_t(s.AllocatedToTargetProcess));
                out += ']';
            }
            at += info->Size;
        }
    }
    out += ']';
    json::key(out, "cpu_set_columns");
    out += "[\"group\",\"logical\",\"core\",\"llc\",\"efficiency\",\"parked\",\"allocated\",\"allocated_to_process\"]";
    // Default CPU sets of the process (empty: no restriction set this way).
    ULONG count{};
    GetProcessDefaultCpuSets(process, nullptr, 0, &count);
    std::vector<ULONG> defaults(count);
    json::key(out, "process_default_cpu_sets");
    out += '[';
    if (count && GetProcessDefaultCpuSets(process, defaults.data(), count, &count)) {
        for (ULONG i = 0; i < count; ++i) {
            const auto it = std::find_if(ids.begin(), ids.end(), [&](const auto& p) { return p.first == defaults[i]; });
            if (i) out += ',';
            json::number(out, std::uint64_t(it == ids.end() ? 0xFFFF : it->second));
        }
    }
    out += ']';
}

void os_version(std::string& out) {
    using RtlGetVersion = LONG(WINAPI*)(OSVERSIONINFOW*);
    OSVERSIONINFOW v{};
    v.dwOSVersionInfoSize = sizeof(v);
    const auto get = reinterpret_cast<RtlGetVersion>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    if (get) get(&v);
    out += '[';
    json::number(out, std::uint64_t(v.dwMajorVersion));
    out += ',';
    json::number(out, std::uint64_t(v.dwMinorVersion));
    out += ',';
    json::number(out, std::uint64_t(v.dwBuildNumber));
    out += ']';
}

// Every key of one section of an ini file, as a JSON object.
void ini_section(std::string& out, const std::wstring& file, const wchar_t* section) {
    std::vector<wchar_t> buffer(32768);
    const auto n = GetPrivateProfileSectionW(section, buffer.data(), static_cast<DWORD>(buffer.size()), file.c_str());
    out += '{';
    for (DWORD at = 0; at < n;) {
        std::wstring_view entry(buffer.data() + at);
        at += static_cast<DWORD>(entry.size()) + 1;
        const auto eq = entry.find(L'=');
        if (entry.empty() || eq == std::wstring_view::npos) continue;
        json::key(out, utf8(entry.substr(0, eq)));
        json::string(out, utf8(entry.substr(eq + 1)));
    }
    out += '}';
}

void image_identity(std::string& out, std::uintptr_t base) {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + static_cast<std::uintptr_t>(dos->e_lfanew));
    json::key(out, "base");
    hex64(out, base);
    json::key(out, "image_bytes");
    json::number(out, std::uint64_t(nt->OptionalHeader.SizeOfImage));
    json::key(out, "pe_timestamp");
    hex64(out, nt->FileHeader.TimeDateStamp);
}

bool in_windows_folder(const std::wstring& path) {
    wchar_t windows[MAX_PATH]{};
    const auto n = GetWindowsDirectoryW(windows, MAX_PATH);
    return n && n < MAX_PATH && path.size() > n && CompareStringOrdinal(path.c_str(), static_cast<int>(n), windows, static_cast<int>(n), TRUE) == CSTR_EQUAL;
}
}  // namespace

std::string sha256_file(const std::wstring& path) {
    const auto file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                  FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    BCRYPT_HASH_HANDLE hash{};
    std::array<unsigned char, 32> digest{};
    bool ok = BCRYPT_SUCCESS(BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &hash, nullptr, 0, nullptr, 0, 0));
    if (ok) {
        std::vector<unsigned char> block(1 << 20);
        for (DWORD n{}; ok && ReadFile(file, block.data(), static_cast<DWORD>(block.size()), &n, nullptr) && n;)
            ok = BCRYPT_SUCCESS(BCryptHashData(hash, block.data(), n, 0));
        ok = ok && BCRYPT_SUCCESS(BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0));
        BCryptDestroyHash(hash);
    }
    CloseHandle(file);
    if (!ok) return {};
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    for (const auto b : digest) {
        out += digits[b >> 4];
        out += digits[b & 15];
    }
    return out;
}

std::wstring module_path(HMODULE module) {
    std::wstring path(1024, L'\0');
    for (;;) {
        const auto n = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
        if (n == 0) return {};
        if (n < path.size()) {
            path.resize(n);
            return path;
        }
        path.resize(path.size() * 2);
    }
}

std::wstring folder_of(const std::wstring& path) {
    const auto slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring{} : path.substr(0, slash);
}

std::string start_event(HMODULE self, const std::wstring& control_path, const LoaderInfo& loader) {
    std::string out = "{";
    json::key(out, "event");
    json::string(out, "start");
    json::key(out, "t_ms");
    json::fixed(out, now_ms(), 3);
    json::key(out, "schema");
    json::number(out, std::uint64_t(1));
    json::key(out, "version");
    json::string(out, CPUOPT_VERSION);
    json::key(out, "commit");
    json::string(out, CPUOPT_COMMIT);
    SYSTEMTIME st{};
    GetSystemTime(&st);
    char utc[40];
    std::snprintf(utc, sizeof(utc), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                  st.wMilliseconds);
    json::key(out, "utc");
    json::string(out, utc);
    json::key(out, "qpc_hz");
    json::number(out, std::uint64_t(qpc_frequency()));
    json::key(out, "pid");
    json::number(out, std::uint64_t(GetCurrentProcessId()));

    json::key(out, "loader");
    out += '{';
    json::key(out, "name");
    json::string(out, loader.name);
    json::key(out, "api");
    char api[24];
    std::snprintf(api, sizeof(api), "%u.%u", loader.api_version >> 16, loader.api_version & 0xFFFFu);
    json::string(out, api);
    json::key(out, "game_version");
    json::string(out, loader.game_version);
    json::key(out, "exe_sha256");
    json::string(out, loader.exe_sha256);
    out += '}';

    const auto exe_path = module_path(nullptr);
    const auto given = loader_hash(loader);
    const auto exe_hash = given.empty() ? sha256_file(exe_path) : given;
    json::key(out, "exe");
    out += '{';
    json::key(out, "path");
    json::string(out, utf8(exe_path));
    json::key(out, "sha256");
    json::string(out, exe_hash);
    json::key(out, "sha256_from");
    json::string(out, given.empty() ? "file" : "loader");
    json::key(out, "tested_build");
    out += exe_hash == tested_exe_sha256 ? "true" : "false";
    image_identity(out, reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)));
    out += '}';

    const auto dll_path = module_path(self);
    json::key(out, "dll");
    out += '{';
    json::key(out, "path");
    json::string(out, utf8(dll_path));
    json::key(out, "sha256");
    json::string(out, sha256_file(dll_path));
    image_identity(out, reinterpret_cast<std::uintptr_t>(self));
    out += '}';

    json::key(out, "cpu");
    out += '{';
    json::key(out, "brand");
    json::string(out, cpu_brand());
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    json::key(out, "logical");
    json::number(out, std::uint64_t(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)));
    json::key(out, "groups");
    json::number(out, std::uint64_t(GetActiveProcessorGroupCount()));
    json::key(out, "system_info_processors");
    json::number(out, std::uint64_t(si.dwNumberOfProcessors));
    DWORD_PTR process_mask{}, system_mask{};
    GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask);
    json::key(out, "process_affinity");
    hex64(out, process_mask);
    json::key(out, "system_affinity");
    hex64(out, system_mask);
    json::key(out, "priority_class");
    hex64(out, GetPriorityClass(GetCurrentProcess()));
    json::key(out, "caches");
    caches(out);
    json::key(out, "cpu_sets");
    cpu_sets(out);
    out += '}';

    json::key(out, "os");
    os_version(out);

    const auto game_folder = folder_of(exe_path);
    json::key(out, "game_config");
    out += '{';
    json::key(out, "graphics_option");
    ini_section(out, game_folder + L"\\graphics_option.ini", L"GraphicsOption");
    json::key(out, "config_cpu");
    ini_section(out, game_folder + L"\\config.ini", L"CPU");
    json::key(out, "config_graphics");
    ini_section(out, game_folder + L"\\config.ini", L"GRAPHICS");
    out += '}';

    json::key(out, "control");
    json::string(out, utf8(control_path));
    json::key(out, "toggles");
    json::number(out, std::uint64_t(toggles.load()));
    out += '}';
    return out;
}

std::string loader_hash(const LoaderInfo& loader) {
    if (loader.exe_sha256.size() != 64) return {};
    std::string out;
    for (const char c : loader.exe_sha256) {
        if (c >= '0' && c <= '9') out += c;
        else if (c >= 'a' && c <= 'f') out += c;
        else if (c >= 'A' && c <= 'F') out += static_cast<char>(c - 'A' + 'a');
        else return {};
    }
    return out;
}

std::string wine_version() {
    using Fn = const char*(CDECL*)();
    const auto ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto fn = ntdll ? reinterpret_cast<Fn>(GetProcAddress(ntdll, "wine_get_version")) : nullptr;
    const char* v = fn ? fn() : nullptr;
    return v ? std::string(v) : std::string{};
}

std::string init_event(const char* mode, const LoaderInfo& loader) {
    std::string out = "{";
    json::key(out, "event");
    json::string(out, "init");
    json::key(out, "t_ms");
    json::fixed(out, now_ms(), 3);
    json::key(out, "version");
    json::string(out, CPUOPT_VERSION);
    json::key(out, "commit");
    json::string(out, CPUOPT_COMMIT);
    SYSTEMTIME st{};
    GetSystemTime(&st);
    char utc[40];
    std::snprintf(utc, sizeof(utc), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                  st.wMilliseconds);
    json::key(out, "utc");
    json::string(out, utc);
    json::key(out, "pid");
    json::number(out, std::uint64_t(GetCurrentProcessId()));
    json::key(out, "mode");
    json::string(out, mode);
    json::key(out, "loader_api");
    char api[24];
    std::snprintf(api, sizeof(api), "%u.%u", loader.api_version >> 16, loader.api_version & 0xFFFFu);
    json::string(out, api);
    json::key(out, "game_version");
    json::string(out, loader.game_version);
    json::key(out, "exe_sha256_from_loader");
    out += loader_hash(loader).empty() ? "false" : "true";
    json::key(out, "wine");
    json::string(out, wine_version());
    out += '}';
    return out;
}

std::string cpu_name() { return cpu_brand(); }

std::string gpu_name() {
    DISPLAY_DEVICEW device{};
    device.cb = sizeof(device);
    std::wstring first;
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &device, 0); ++i) {
        if (device.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) return utf8(device.DeviceString);
        if (first.empty() && (device.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) first = device.DeviceString;
        device.cb = sizeof(device);
    }
    return utf8(first);
}

std::string windows_version() {
    std::string v;
    os_version(v);  // [major,minor,build]
    for (auto& c : v)
        if (c == ',') c = '.';
    return v.size() > 2 ? v.substr(1, v.size() - 2) : v;
}

bool tested_exe(std::string_view sha256) noexcept { return sha256 == tested_exe_sha256; }

std::string ini_value(const std::wstring& file, const wchar_t* section, const wchar_t* key) {
    wchar_t value[256]{};
    GetPrivateProfileStringW(section, key, L"", value, static_cast<DWORD>(std::size(value)), file.c_str());
    return utf8(value);
}

std::string other_modules(HMODULE self) {
    std::vector<std::wstring> names;
    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return {};
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    const auto exe = GetModuleHandleW(nullptr);
    for (BOOL ok = Module32FirstW(snapshot, &entry); ok; ok = Module32NextW(snapshot, &entry))
        if (entry.hModule != exe && entry.hModule != self && !in_windows_folder(entry.szExePath)) {
            // Not NVIDIA's NGX model files (.bin, mapped as images by the driver, even with DLSS off).
            const std::wstring_view name(entry.szModule);
            if (name.size() > 4 && CompareStringOrdinal(name.data() + name.size() - 4, 4, L".bin", 4, TRUE) == CSTR_EQUAL) continue;
            names.emplace_back(entry.szModule);
        }
    CloseHandle(snapshot);
    std::sort(names.begin(), names.end(), [](const std::wstring& a, const std::wstring& b) {
        return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN;
    });
    names.erase(std::unique(names.begin(), names.end(),
                            [](const std::wstring& a, const std::wstring& b) {
                                return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
                            }),
                names.end());
    std::string out;
    for (const auto& n : names) {
        if (!out.empty()) out += ", ";
        out += utf8(n);
    }
    return out;
}

std::string ModuleCensus::changed_event(double t_ms) {
    std::vector<Entry> now;
    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return {};
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL ok = Module32FirstW(snapshot, &entry); ok; ok = Module32NextW(snapshot, &entry))
        now.push_back({entry.szExePath, reinterpret_cast<std::uintptr_t>(entry.modBaseAddr), entry.modBaseSize});
    CloseHandle(snapshot);
    std::sort(now.begin(), now.end(), [](const Entry& a, const Entry& b) { return a.base < b.base; });
    if (now == last_) return {};
    std::string out = "{";
    json::key(out, "event");
    json::string(out, "modules");
    json::key(out, "t_ms");
    json::fixed(out, t_ms, 3);
    json::key(out, "columns");
    out += "[\"path\",\"base\",\"bytes\",\"sha256\",\"loaded\"]";
    json::key(out, "modules");
    out += '[';
    for (std::size_t i = 0; i < now.size(); ++i) {
        const auto& m = now[i];
        std::string hash;
        if (!in_windows_folder(m.path)) {
            auto it = hashes_.find(m.path);
            if (it == hashes_.end()) it = hashes_.emplace(m.path, sha256_file(m.path)).first;
            hash = it->second;
        }
        const bool fresh = std::find(last_.begin(), last_.end(), m) == last_.end();
        out += i ? ",[" : "[";
        json::string(out, utf8(m.path));
        out += ',';
        hex64(out, m.base);
        out += ',';
        json::number(out, std::uint64_t(m.bytes));
        out += ',';
        json::string(out, hash);
        out += ',';
        out += fresh && !last_.empty() ? "\"new\"" : "\"\"";
        out += ']';
    }
    out += "]}";
    last_ = std::move(now);
    return out;
}
}  // namespace cpuopt::system_info
