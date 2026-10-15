// The built cpuopt.dll under a stand-in Astera Loader host:
//   al_host_smoke <cpuopt.dll> <scratch folder>
// The host lays out <scratch>\host-<pid>\plugins\cpuopt\ with the DLL and its
// run-folder sidecar, serves al.game, al.scanner (over this exe, where a copy
// of the frame-counter signature points at a fake sRender), al.frame and
// al.d3d12, runs a game thread (4 ms frames, about 1 ms of update, the render
// counter advanced each frame) and a present thread (4 ms Presents taking
// about 0.3 ms), switches control.ini live, then sends EXITING and SHUTDOWN
// and checks the run log.
#include "al/al_plugin.h"
#include "../src/core/signature.hpp"
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace {
int failures = 0;
#define CHECK(x)                                                         \
    do {                                                                 \
        if (!(x)) {                                                      \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);     \
            ++failures;                                                  \
        }                                                                \
    } while (0)

// A copy of the frame-counter signature whose operand points at fake_srender.
#pragma section(".cpotest", read, execute)
__declspec(allocate(".cpotest")) unsigned char planted[] = {
    0x48, 0x8B, 0x05, 0x00, 0x00, 0x00, 0x00,                    // mov rax,[rip+fake_srender]
    0x49, 0xB9, 0xAB, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA,  // mov r9,0AAAAAAAAAAAAAAABh
    0x4C, 0x8B, 0x80, 0x10, 0xF2, 0x01, 0x00,                    // mov r8,[rax+1F210h]
    0xC3};
// sMain's job code: the runner's prologue (mov r8,[rip+fake_smain] at +21)
// and the three layout signatures, once each.
__declspec(allocate(".cpotest")) unsigned char planted_runner[] = {
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20, 0x41, 0x56,
    0x48, 0x83, 0xEC, 0x20, 0x4C, 0x8B, 0x05, 0x00, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xF1, 0x4C, 0x8B, 0x31, 0xC3};
__declspec(allocate(".cpotest")) unsigned char planted_layout[] = {
    0x8B, 0x83, 0xCC, 0x00, 0x00, 0x00, 0x8B, 0xBB, 0xB0, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x8B, 0xF0, 0xF1, 0x09, 0x00,
    0x44, 0x89, 0xA3, 0xF8, 0xF1, 0x09, 0x00, 0x44, 0x89, 0xA3, 0xFC, 0xF1, 0x09, 0x00, 0x3B, 0xF8, 0xCC,
    0x80, 0xB9, 0x8C, 0x00, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xD9, 0x8B, 0x81, 0xD0, 0x00, 0x00, 0x00, 0x89, 0x81, 0xCC, 0x00, 0x00, 0x00, 0xC3};
// executeJob's spin loop as a callable function: spin(sMain, released) waits
// until sMain's done counter [+9F1FCh] equals `released`, with the game's own
// 20 bytes (the spin signature) plus the loop's closing jnz.
__declspec(allocate(".cpotest")) unsigned char planted_spin[] = {
    0x53, 0x57, 0x48, 0x89, 0xCB, 0x89, 0xD7,                    // push rbx; push rdi; mov rbx,rcx; mov edi,edx
    0x8B, 0x83, 0xFC, 0xF1, 0x09, 0x00, 0x3B, 0xC7, 0x74, 0x0C,  // mov eax,[rbx+9F1FCh]; cmp eax,edi; jz done
    0xF3, 0x90, 0x8B, 0x83, 0xFC, 0xF1, 0x09, 0x00, 0x3B, 0xC7,  // pause; mov eax,[rbx+9F1FCh]; cmp eax,edi
    0x75, 0xF4,                                                  // jnz pause
    0x5F, 0x5B, 0xC3};                                           // done: pop rdi; pop rbx; ret
using SpinFn = void (*)(void* smain, int released);
std::uintptr_t fake_smain;
std::uint8_t smain_object[0xAF680 + 64];
std::uint32_t& smain_field(std::uint32_t offset) { return *reinterpret_cast<std::uint32_t*>(smain_object + offset); }
std::uintptr_t& job_slot(unsigned i) { return *reinterpret_cast<std::uintptr_t*>(smain_object + 0x9E8F0 + i * 0x48 + 0x30); }
// The job a fake job thread "runs" for 0.5 ms each frame while the game thread spins.
__declspec(noinline) void fake_job_function(void*) {}
std::uintptr_t fake_job[4] = {0, reinterpret_cast<std::uintptr_t>(&fake_job_function), 0, 0};
std::uintptr_t fake_srender;
std::uint64_t srender_object[0x20000 / 8];
std::atomic<std::uint64_t>& render_counter() { return reinterpret_cast<std::atomic<std::uint64_t>&>(srender_object[0x1F210 / 8]); }

// The host's state.
std::mutex log_lock;
std::vector<std::string> plugin_log;
std::atomic<unsigned> last_status{99};
std::string last_reason;
al_frame_fn frame_callbacks[2]{};
al_d3d12_event_fn present_callback{};
std::uint32_t present_mask{};
std::atomic<DWORD> game_tid{};
std::atomic<bool> frames_stop{}, jobs_stop{};
char plugin_token;  // stands in for al_plugin_t
al_plugin_t* const self = reinterpret_cast<al_plugin_t*>(&plugin_token);

void AL_CALL host_log(al_plugin_t*, al_log_level_t, const char* message) {
    std::lock_guard<std::mutex> hold(log_lock);
    plugin_log.emplace_back(message);
}
void AL_CALL host_status(al_plugin_t*, al_status_t status, const char* reason) {
    last_status = status;
    std::lock_guard<std::mutex> hold(log_lock);
    last_reason = reason ? reason : "";
}

const al_game_t game = {sizeof(al_game_t), AL_GAME_VERSION, "c2ebbbd2c49f216d484e31a5219bed419eb1e5e7d206d02cba040a3ab79d90ea", "421810",
                        L"host.exe", L".", 0, AL_GRAPHICS_DX12};

std::uint32_t scans{};
uint32_t AL_CALL host_find(al_plugin_t*, const char* pattern, uint32_t where, void** matches, uint32_t capacity) {
    ++scans;
    const auto p = cpuopt::Pattern::parse(pattern);
    if (!p || where != AL_SCAN_CODE) return AL_SCAN_INVALID;
    const auto base = reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(nullptr));
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const auto* s = IMAGE_FIRST_SECTION(nt);
    uint32_t total{};
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++s) {
        if (!(s->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        std::size_t hits[16]{};
        const auto found = p->find(base + s->VirtualAddress, s->Misc.VirtualSize, hits, 16);
        for (std::size_t h = 0; h < found && h < 16; ++h)
            if (total + h < capacity) matches[total + h] = const_cast<std::uint8_t*>(base + s->VirtualAddress + hits[h]);
        total += static_cast<uint32_t>(found);
    }
    return total;
}
void* AL_CALL host_rip(const void*, uint32_t, uint32_t) { return nullptr; }
const al_scanner_t scanner = {sizeof(al_scanner_t), AL_SCANNER_VERSION, host_find, host_rip};

uint32_t AL_CALL frame_subscribe(al_plugin_t*, al_frame_event_t event, al_frame_fn callback, void*) {
    if (event > AL_FRAME_END || !callback) return 0;
    frame_callbacks[event] = callback;
    return event + 1;
}
al_result_t AL_CALL frame_unsubscribe(al_plugin_t*, uint32_t) { return AL_OK; }
al_result_t AL_CALL frame_post(al_plugin_t*, al_task_fn, void*) { return AL_ERROR; }
void AL_CALL frame_status(al_plugin_t*, al_frame_status_t* status) {
    status->state = game_tid.load() ? AL_FRAME_RUNNING : AL_FRAME_WAITING;
    status->game_thread = game_tid.load();
    status->reason = "";
}
const al_frame_t frame = {sizeof(al_frame_t), AL_FRAME_VERSION, frame_subscribe, frame_unsubscribe, frame_post, frame_status};

al_result_t AL_CALL d3d12_subscribe(al_plugin_t*, uint32_t events, int32_t, al_d3d12_event_fn callback, void*) {
    present_callback = callback;
    present_mask = events;
    return AL_OK;
}
al_result_t AL_CALL d3d12_hook(al_plugin_t*, const al_d3d12_hook_t*, uint32_t) { return AL_ERROR; }
al_result_t AL_CALL d3d12_claim(al_plugin_t*) { return AL_ERROR; }
void AL_CALL d3d12_internal(al_plugin_t*) {}
void AL_CALL d3d12_status(al_plugin_t*, al_d3d12_status_t* status) { status->state = AL_D3D12_RUNNING; }
const al_d3d12_t d3d12 = {sizeof(al_d3d12_t), AL_D3D12_VERSION, d3d12_subscribe, d3d12_hook, d3d12_claim, d3d12_internal, d3d12_internal,
                          d3d12_status};

const void* AL_CALL host_interface(al_plugin_t*, const char* name, uint32_t) {
    const std::string n(name);
    if (n == AL_GAME_INTERFACE) return &game;
    if (n == AL_SCANNER_INTERFACE) return &scanner;
    if (n == AL_FRAME_INTERFACE) return &frame;
    if (n == AL_D3D12_INTERFACE) return &d3d12;
    return nullptr;
}
al_result_t AL_CALL host_publish(al_plugin_t*, const char*, uint32_t, const void*) { return AL_ERROR; }
const al_api_t api = {sizeof(al_api_t), AL_API_VERSION, host_log, host_status, host_interface, host_publish};

std::int64_t qpc() {
    LARGE_INTEGER c{};
    QueryPerformanceCounter(&c);
    return c.QuadPart;
}
std::int64_t frequency() {
    LARGE_INTEGER f{};
    QueryPerformanceFrequency(&f);
    return f.QuadPart;
}
void spin_until(std::int64_t due) {
    while (qpc() < due) Sleep(0);
}

DWORD WINAPI game_thread(void*) {
    game_tid = GetCurrentThreadId();
    const auto f = frequency();
    const auto t0 = qpc();
    for (std::uint64_t i = 0; !frames_stop.load(); ++i) {
        spin_until(t0 + f * static_cast<std::int64_t>(i) * 4 / 1000);
        al_frame_info_t info{sizeof(al_frame_info_t), 0, i, qpc()};
        if (frame_callbacks[AL_FRAME_BEGIN]) frame_callbacks[AL_FRAME_BEGIN](self, AL_FRAME_BEGIN, &info, nullptr);
        spin_until(info.qpc + f / 1000);  // about 1 ms of "update"
        // A job round: job thread 1 holds a job for about 0.5 ms while the
        // game thread waits in the game's spin loop.
        smain_field(0x9F1FC) = 0;
        job_slot(1) = reinterpret_cast<std::uintptr_t>(fake_job);
        reinterpret_cast<SpinFn>(static_cast<void*>(planted_spin))(smain_object, 1);
        render_counter().fetch_add(1);
        info.qpc = qpc();
        if (frame_callbacks[AL_FRAME_END]) frame_callbacks[AL_FRAME_END](self, AL_FRAME_END, &info, nullptr);
    }
    return 0;
}

DWORD WINAPI job_thread(void*) {
    const auto f = frequency();
    while (!jobs_stop.load()) {
        if (!reinterpret_cast<std::atomic<std::uintptr_t>&>(job_slot(1)).load()) {
            YieldProcessor();
            continue;
        }
        spin_until(qpc() + f / 2000);  // 0.5 ms of "job"
        reinterpret_cast<std::atomic<std::uintptr_t>&>(job_slot(1)).store(0);
        reinterpret_cast<std::atomic<std::uint32_t>&>(smain_field(0x9F1FC)).fetch_add(1);
    }
    return 0;
}

DWORD WINAPI present_thread(void*) {
    const auto f = frequency();
    const auto t0 = qpc() + f / 500;  // 2 ms behind the game thread
    al_d3d12_swapchain_t chain{};
    chain.size = sizeof(chain);
    std::int64_t previous{};
    for (std::uint64_t i = 0; !frames_stop.load(); ++i) {
        spin_until(t0 + f * static_cast<std::int64_t>(i) * 4 / 1000);
        al_d3d12_present_t p{};
        p.size = sizeof(p);
        p.swapchain = &chain;
        p.frame = i;
        p.present_qpc = qpc();
        p.previous_qpc = previous;
        previous = p.present_qpc;
        if (present_callback) present_callback(self, AL_D3D12_PRE_PRESENT, &p, nullptr);
        spin_until(p.present_qpc + f * 3 / 10000);  // 0.3 ms inside Present
        if (present_callback) present_callback(self, AL_D3D12_POST_PRESENT, &p, nullptr);
    }
    return 0;
}

std::string read_all(const std::wstring& path) {
    std::ifstream file(path, std::ios::binary);
    std::stringstream s;
    s << file.rdbuf();
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
std::vector<std::string> lines_with(const std::string& text, const std::string& what) {
    std::vector<std::string> out;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);)
        if (line.find(what) != std::string::npos) out.push_back(line);
    return out;
}
// The number after `key` inside the object that starts at `object` in `line`.
double number_in(const std::string& line, const std::string& object, const std::string& key) {
    const auto at = line.find(object);
    if (at == std::string::npos) return -1;
    const auto k = line.find("\"" + key + "\":", at);
    return k == std::string::npos ? -1 : std::atof(line.c_str() + k + key.size() + 3);
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) return 2;
    const std::wstring source = argv[1], scratch = argv[2];
    CreateDirectoryW(scratch.c_str(), nullptr);
    const auto root = scratch + L"\\host-" + std::to_wstring(GetCurrentProcessId());
    const auto plugins = root + L"\\plugins", folder = plugins + L"\\cpuopt", run = root + L"\\run";
    for (const auto& d : {root, plugins, folder, run}) CreateDirectoryW(d.c_str(), nullptr);
    const auto dll = folder + L"\\cpuopt.dll";
    CHECK(CopyFileW(source.c_str(), dll.c_str(), FALSE));
    write_all(dll + L".output-dir", utf8(run) + "\r\n");
    write_all(run + L"\\control.ini", "window_ms=1000\nmarker=smoke-a\n");

    // The planted signature points at fake_srender, which points at the object.
    const auto site = reinterpret_cast<std::uintptr_t>(planted);
    const auto displacement = static_cast<std::int32_t>(reinterpret_cast<std::intptr_t>(&fake_srender) - static_cast<std::intptr_t>(site + 7));
    DWORD old{};
    VirtualProtect(planted, sizeof(planted), PAGE_EXECUTE_READWRITE, &old);
    std::memcpy(planted + 3, &displacement, 4);
    VirtualProtect(planted, sizeof(planted), old, &old);
    fake_srender = reinterpret_cast<std::uintptr_t>(srender_object);
    {
        const auto at = reinterpret_cast<std::uintptr_t>(planted_runner) + 21;
        const auto d = static_cast<std::int32_t>(reinterpret_cast<std::intptr_t>(&fake_smain) - static_cast<std::intptr_t>(at + 7));
        VirtualProtect(planted_runner, sizeof(planted_runner), PAGE_EXECUTE_READWRITE, &old);
        std::memcpy(planted_runner + 24, &d, 4);
        VirtualProtect(planted_runner, sizeof(planted_runner), old, &old);
    }
    // A 32-thread machine as the game sets it up: 31 per round, threads created.
    fake_smain = reinterpret_cast<std::uintptr_t>(smain_object);
    smain_field(0xAF680) = 32;
    smain_field(0xD0) = smain_field(0xCC) = 31;
    smain_object[0x90] = 1;

    const auto module = LoadLibraryW(dll.c_str());
    CHECK(module != nullptr);
    if (!module) return 1;
    using InfoFn = const al_plugin_info_t*(AL_CALL*)(void);
    using InitFn = al_result_t(AL_CALL*)(const al_api_t*, const al_init_t*);
    const auto get_info = reinterpret_cast<InfoFn>(GetProcAddress(module, "al_plugin_info"));
    const auto init = reinterpret_cast<InitFn>(GetProcAddress(module, "al_plugin_init"));
    CHECK(get_info && init);
    if (!get_info || !init) return 1;
    const auto* info = get_info();
    CHECK(info->size == sizeof(al_plugin_info_t) && AL_VERSION_MAJOR(info->api_version) == 1);
    CHECK(std::string(info->name) == "cpuopt");
    CHECK((info->phases & AL_PHASE_BIT(AL_PHASE_EXITING)) && (info->phases & AL_PHASE_BIT(AL_PHASE_SHUTDOWN)) && info->on_phase);

    al_init_t start{sizeof(al_init_t), 0, self, folder.c_str(), folder.c_str(), nullptr};
    CHECK(init(&api, &start) == AL_OK);
    CHECK(last_status == AL_STATUS_ACTIVE);
    CHECK(scans == 5);  // the frame counter and the four job_cap signatures
    CHECK(frame_callbacks[AL_FRAME_BEGIN] && frame_callbacks[AL_FRAME_END] && present_callback);
    CHECK(present_mask == (AL_D3D12_EVENT_BIT(AL_D3D12_PRE_PRESENT) | AL_D3D12_EVENT_BIT(AL_D3D12_POST_PRESENT)));

    const HANDLE jobs = CreateThread(nullptr, 0, job_thread, nullptr, 0, nullptr);
    const HANDLE threads[] = {CreateThread(nullptr, 0, game_thread, nullptr, 0, nullptr),
                              CreateThread(nullptr, 0, present_thread, nullptr, 0, nullptr)};
    Sleep(300);
    const al_phase_game_ready_t ready{sizeof(al_phase_game_ready_t), AL_GAME_READY_TITLE, 75};
    info->on_phase(self, AL_PHASE_GAME_READY, &ready);
    Sleep(2300);
    const auto native_before = smain_field(0xD0);
    write_all(run + L"\\control.ini", "window_ms=1000\nmarker=smoke-b\ntoggles=3\njob_threads=12\n");
    Sleep(2600);
    const auto capped = smain_field(0xD0), capped_participants = smain_field(0xCC);
    write_all(run + L"\\control.ini", "toggles=banana\n");  // rejected, nothing changes
    Sleep(1200);

    // The game quits: EXITING while frames still run, then the loop ends.
    const auto t_exit = qpc();
    info->on_phase(self, AL_PHASE_EXITING, nullptr);
    const double exiting_ms = double(qpc() - t_exit) * 1000.0 / double(frequency());
    frames_stop = true;
    WaitForMultipleObjects(2, threads, TRUE, 2000);
    jobs_stop = true;  // after the game thread: it may be waiting for a job
    WaitForSingleObject(jobs, 2000);
    const auto after_exit = smain_field(0xD0);
    const al_phase_shutdown_t shutdown{sizeof(al_phase_shutdown_t), 0};
    info->on_phase(self, AL_PHASE_SHUTDOWN, &shutdown);

    const auto log = run + L"\\cpuopt-" + std::to_wstring(GetCurrentProcessId()) + L".jsonl";
    const auto text = read_all(log);
    const auto windows = lines_with(text, "\"event\":\"window\"");
    const auto profiles = lines_with(text, "\"event\":\"profile\"");
    std::printf("al_host_smoke: %zu bytes, %zu windows, %zu profiles, EXITING took %.1f ms\n", text.size(), windows.size(), profiles.size(),
                exiting_ms);
    CHECK(text.rfind("{\"event\":\"start\"", 0) == 0 || text.find("\n{\"event\":\"start\"") != std::string::npos);
    CHECK(text.find("\"loader\":{\"name\":\"Astera Loader\",\"api\":\"1.8\",\"game_version\":\"421810\"") != std::string::npos);
    CHECK(text.find("\"event\":\"frame_clock\",") != std::string::npos);
    CHECK(text.find("\"found\":true,\"reason\":\"ok\",\"scanner\":\"al.scanner\",\"matches\":1") != std::string::npos);
    CHECK(text.find("\"event\":\"al_frame_subscribed\"") != std::string::npos);
    // job_cap: found, 31 before, 12 while on, the game's 31 back at EXITING.
    CHECK(text.find("\"event\":\"job_cap\",") != std::string::npos);
    CHECK(text.find("\"reason\":\"ok\",\"scanner\":\"al.scanner\",\"counts\":[1,1,1,1]") != std::string::npos);
    CHECK(native_before == 31 && capped == 12 && capped_participants == 12 && after_exit == 31);
    CHECK(text.find("\"job\":{\"state\":\"native\",\"native\":31,\"requested\":0,\"applied\":31") != std::string::npos);
    CHECK(text.find("\"job\":{\"state\":\"capped\",\"native\":31,\"requested\":12,\"applied\":12") != std::string::npos);
    CHECK(text.find("\"job_threads\":12") != std::string::npos);
    std::printf("  job_cap: native %u, capped %u/%u, after EXITING %u\n", native_before, capped, capped_participants, after_exit);
    CHECK(text.find("\"event\":\"game_ready\"") != std::string::npos && text.find("\"frame\":75") != std::string::npos);
    CHECK(text.find("\"marker\":\"smoke-b\",\"window_ms\":1000") != std::string::npos);
    CHECK(text.find("\"rejected\":true") != std::string::npos);
    CHECK(windows.size() >= 5);
    CHECK(profiles.size() >= 1);
    // Full windows: 250 game frames and Presents a second, update about 1 ms,
    // Present calls about 0.3 ms, the render counter in step with the updates.
    unsigned checked = 0;
    const auto game_id = std::to_string(game_tid.load());
    for (const auto& w : windows) {
        const double ms = number_in(w, "\"event\":\"window\"", "ms");
        if (ms < 950 || ms > 1100) continue;
        const double fps = number_in(w, "\"update\":{", "fps"), present_fps = number_in(w, "\"present\":{", "fps");
        const double work = number_in(w, "\"work\":{", "p50"), call = number_in(w, "\"call\":{", "p50");
        const double interval = number_in(w, "\"update\":{", "p50");
        const double updates = number_in(w, "\"update\":{", "frames"), rendered = number_in(w, "\"render\":{", "frames");
        if (checked++ == 0)
            std::printf("  window: %.0f ms, update %.1f fps p50 %.2f ms work %.2f ms, present %.1f fps call %.2f ms, render %.0f of %.0f\n",
                        ms, fps, interval, work, present_fps, call, rendered, updates);
        CHECK(fps > 200 && fps < 260);
        CHECK(present_fps > 200 && present_fps < 260);
        CHECK(interval >= 3.5 && interval <= 4.6);
        CHECK(work >= 1.3 && work <= 2.3);  // 1 ms of update and about 0.5 ms waiting for the job
        CHECK(call >= 0.25 && call <= 0.8);
        CHECK(rendered >= updates - 2 && rendered <= updates + 2);
        CHECK(w.find("\"update\":{\"frames\":") != std::string::npos && w.find("\"thread\":" + game_id + ",") != std::string::npos);
        CHECK(w.find("\"frame_state\":1") != std::string::npos);
    }
    CHECK(checked >= 3);
    // The sampler followed the game thread.
    CHECK(!profiles.empty() && profiles.back().find("\"tracked\":[" + game_id) != std::string::npos);
    // The spin probe caught the game thread in the planted spin and saw job
    // thread 1 busy with fake_job_function.
    {
        long long spin_samples = 0, one_busy = 0;
        bool job_seen = false;
        const auto exe = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        const auto job_rva = std::to_string(reinterpret_cast<std::uintptr_t>(&fake_job_function) - exe);
        for (const auto& line : profiles) {
            const auto at = line.find("\"spin_probe\":{\"samples\":");
            if (at == std::string::npos) continue;
            spin_samples += std::atoll(line.c_str() + at + 24);
            const auto busy = line.find("\"busy\":[", at);
            const auto comma = line.find(',', busy + 8);
            one_busy += std::atoll(line.c_str() + comma + 1);  // samples with exactly one busy slot
            job_seen = job_seen || line.find(",1," + job_rva + ",") != std::string::npos;
        }
        std::printf("  spin probe: %lld samples, %lld with one busy slot, job function seen: %s\n", spin_samples, one_busy,
                    job_seen ? "yes" : "no");
        CHECK(spin_samples > 50);
        CHECK(one_busy > spin_samples / 2);
        CHECK(job_seen);
    }
    // EXITING stopped everything in time; SHUTDOWN still logged.
    CHECK(exiting_ms < 2500);
    CHECK(text.find("\"event\":\"stopping\"") != std::string::npos);
    CHECK(text.find("\"sampler_stopped\":true") != std::string::npos);
    CHECK(text.find("\"event\":\"shutdown\"") != std::string::npos);
    CHECK(text.find("stop_timeout") == std::string::npos);
    {
        std::lock_guard<std::mutex> hold(log_lock);
        bool told = false;
        for (const auto& line : plugin_log) told = told || line.find("logging to") != std::string::npos;
        CHECK(told);
    }
    std::printf(failures ? "al_host_smoke: %d failure(s)\n" : "al_host_smoke: ok\n", failures);
    std::fflush(stdout);
    ExitProcess(failures ? 1 : 0);
}
