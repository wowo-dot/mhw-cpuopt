// Loads the built plugin into this stand-in process the way Stracker's Loader
// does, with a run folder sidecar, and checks the log: start, frame_clock
// (no game here, so it must fail closed), modules, windows, a live control
// change with sampling and the exit line.
//   runtime_smoke <mhw_cpuopt.dll> <scratch folder>
#include <windows.h>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace {
int failures = 0;
#define CHECK(x)                                                         \
    do {                                                                 \
        if (!(x)) {                                                      \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);     \
            ++failures;                                                  \
        }                                                                \
    } while (0)

std::string read_all(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}
void write_all(const std::wstring& path, const std::string& text) {
    const auto tmp = path + L".tmp";
    std::ofstream(tmp, std::ios::binary) << text;
    MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}
std::string utf8(const std::wstring& w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}
std::size_t count(const std::string& text, const std::string& what) {
    std::size_t n = 0;
    for (auto at = text.find(what); at != std::string::npos; at = text.find(what, at + 1)) ++n;
    return n;
}
volatile unsigned long long sink;
DWORD WINAPI busy(void*) {
    for (;;) sink = sink * 3 + 1;
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) return 2;
    const std::wstring source = argv[1], folder = argv[2];
    CreateDirectoryW(folder.c_str(), nullptr);
    const auto run = folder + L"\\run-" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(run.c_str(), nullptr);
    const auto dll = run + L"\\mhw_cpuopt.dll";
    CHECK(CopyFileW(source.c_str(), dll.c_str(), FALSE));
    write_all(dll + L".output-dir", utf8(run) + "\r\n");
    write_all(run + L"\\control.ini", "window_ms=1000\nmarker=smoke-a\n");
    if (const auto t = CreateThread(nullptr, 0, busy, nullptr, 0, nullptr)) CloseHandle(t);

    const auto module = LoadLibraryW(dll.c_str());
    CHECK(module != nullptr);
    Sleep(2600);
    write_all(run + L"\\control.ini", "window_ms=1000\nmarker=smoke-b\ntoggles=1\n");
    Sleep(2600);
    write_all(run + L"\\control.ini", "toggles=banana\n");  // rejected, nothing changes
    Sleep(1500);
    const auto log = run + L"\\cpuopt-" + std::to_wstring(GetCurrentProcessId()) + L".jsonl";
    const auto text = read_all(log);
    std::printf("runtime_smoke: %zu bytes, %zu windows, %zu profiles\n", text.size(), count(text, "\"event\":\"window\""),
                count(text, "\"event\":\"profile\""));
    CHECK(text.rfind("{\"event\":\"start\"", 0) == 0);
    CHECK(count(text, "\"event\":\"frame_clock\",") == 1);
    CHECK(text.find("\"found\":false,\"reason\":\"signature not found\"") != std::string::npos);
    CHECK(count(text, "\"event\":\"modules\"") >= 1);
    CHECK(count(text, "\"event\":\"window\"") >= 4);
    CHECK(text.find("\"marker\":\"smoke-a\"") != std::string::npos);
    CHECK(text.find("\"event\":\"window\",\"t_ms\"") != std::string::npos);
    CHECK(text.find("\"marker\":\"smoke-b\",\"window_ms\":1000") != std::string::npos);
    CHECK(count(text, "\"event\":\"profile\"") >= 1);
    CHECK(text.find("\"rejected\":true") != std::string::npos);
    CHECK(text.find("\"renderer\":false") != std::string::npos);
    // The sidecar's trailing CRLF must not end up in the path.
    CHECK(text.find("control.ini\\r") == std::string::npos);
    std::printf(failures ? "runtime_smoke: %d failure(s)\n" : "runtime_smoke: ok\n", failures);
    std::fflush(stdout);
    // Exit with the plugin still loaded, as the game does; the exit line is
    // written from DllMain.
    ExitProcess(failures ? 1 : 0);
}
