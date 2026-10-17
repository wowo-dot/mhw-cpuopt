// The built cpuopt.dll under a stand-in Astera Loader host:
//   al_host_smoke <cpuopt.dll> <scratch folder>
// The host lays out <scratch>\host-<pid>\plugins\cpuopt\ with the DLL and its
// run-folder sidecar, serves al.game, al.scanner (over this exe, where a copy
// of the frame-counter signature points at a fake sRender), al.frame and
// al.d3d12, runs a game thread (4 ms frames, about 1 ms of update, the render
// counter advanced each frame) and a present thread (4 ms Presents taking
// about 0.3 ms), switches control.ini live (every switch on in the last
// phase: the ones this exe has no code for say so, inline rounds run the
// host's small round alone), then sends EXITING and SHUTDOWN and checks the
// run log.
//   al_host_smoke <cpuopt.dll> <scratch folder> player
// Player mode instead: no run folder; the host also serves al.input and a
// stand-in CMFM (cmfm.menu), and the test checks cpuopt.ini (made with the
// defaults), the Mods page's switches and hotkey, a hand edit of the file,
// and cpuopt-session.log.
#include "al/al_plugin.h"
#include "cmfm/cmfm.h"
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
std::atomic<DWORD> game_tid{}, job_tid{};
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

// al.input: the hotkeys the plugin holds.
std::mutex input_lock;
std::uint32_t hotkey_key{}, hotkey_mods{}, hotkey_next{1}, hotkey_live{};
al_hotkey_fn hotkey_fn{};
void* hotkey_user{};
uint32_t AL_CALL input_add_hotkey(al_plugin_t*, const al_hotkey_t* h, al_hotkey_fn fn, void* user) {
    std::lock_guard<std::mutex> hold(input_lock);
    hotkey_key = h->key;
    hotkey_mods = h->modifiers;
    hotkey_fn = fn;
    hotkey_user = user;
    return hotkey_live = hotkey_next++;
}
al_result_t AL_CALL input_remove_hotkey(al_plugin_t*, uint32_t handle) {
    std::lock_guard<std::mutex> hold(input_lock);
    if (handle != hotkey_live) return AL_ERROR;
    hotkey_live = 0;
    return AL_OK;
}
al_input_t make_input() {
    al_input_t in{};
    in.size = sizeof(in);
    in.version = AL_INPUT_VERSION;
    in.add_hotkey = input_add_hotkey;
    in.remove_hotkey = input_remove_hotkey;
    return in;
}
const al_input_t input = make_input();
// cmfm.menu: the page the plugin adds (its callbacks stay valid; its rows are
// counted), with CMFM's text limits (CMFM src/menu/menu.hpp: title 24 bytes,
// labels 64, help lines and questions 200, button texts 32).
cmfm_page_t added_page{};
std::string added_title;
std::uint32_t added_rows{};
std::string too_long;  // the first text over its limit
void fits(const char* text, std::size_t limit, const char* what) {
    if (text && std::strlen(text) > limit && too_long.empty()) too_long = std::string(what) + ": " + text;
}
uint32_t CMFM_CALL cmfm_add_page(const cmfm_page_t* page, char*, uint32_t) {
    added_page = *page;
    added_title = page->title ? page->title : "";
    added_rows = page->setting_count;
    fits(page->title, 24, "title");
    fits(page->help, 200, "page help");
    for (std::uint32_t i = 0; i < page->setting_count; ++i) {
        const auto& row = page->settings[i];
        fits(row.label, 64, "label");
        fits(row.help, 200, "help");
        if (row.type == CMFM_TYPE_BUTTON) {
            fits(row.button, 32, "button");
            fits(row.confirm, 200, "question");
        }
    }
    return 1;
}
const cmfm_menu_t cmfm = {sizeof(cmfm_menu_t), CMFM_MENU_VERSION, cmfm_add_page};

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

// A stand-in executeJob and al.hooks: the game thread runs a three-job round
// every frame through the detour while it is registered and enabled.
using ExecuteJobFn = void(__fastcall*)(std::uintptr_t, int, std::uintptr_t, std::uint32_t, std::uintptr_t);
__declspec(noinline) std::uintptr_t host_job(std::uintptr_t context, std::uintptr_t, std::uintptr_t, std::uintptr_t) {
    return context;
}
std::atomic<unsigned> alone_rounds{};  // rounds the detour ran with participants 1
void __fastcall host_execute_job(std::uintptr_t smain, int, std::uintptr_t jobs, std::uint32_t count, std::uintptr_t) {
    if (*reinterpret_cast<const std::uint32_t*>(smain + 0xCC) == 1) ++alone_rounds;
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto* entry = reinterpret_cast<const std::uintptr_t*>(jobs + 32ULL * i);
        reinterpret_cast<std::uintptr_t (*)(std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t)>(entry[1])(entry[0], 0, 0, 0);
    }
}
std::atomic<ExecuteJobFn> hooked_detour{};
std::atomic<bool> hook_enabled{};
std::atomic<unsigned> hook_registrations{};
std::uintptr_t host_jobs[3][4];
uint32_t AL_CALL hooks_hook(al_plugin_t*, const al_hook_t* h) {
    if (!h || h->size < sizeof(al_hook_t) || !h->pattern || !h->detour || !h->next) return 0;
    *h->next = reinterpret_cast<void*>(&host_execute_job);
    hooked_detour = reinterpret_cast<ExecuteJobFn>(h->detour);
    hook_enabled = true;
    ++hook_registrations;
    return 7;
}
al_result_t AL_CALL hooks_set_enabled(al_plugin_t*, uint32_t handle, uint32_t on) {
    if (handle != 7) return AL_ERROR;
    hook_enabled = on != 0;
    return AL_OK;
}
al_hook_state_t AL_CALL hooks_state(al_plugin_t*, uint32_t handle) {
    return handle != 7 ? AL_HOOK_FAILED : hook_enabled ? AL_HOOK_ACTIVE : AL_HOOK_DISABLED;
}
const al_hooks_t hooks = {sizeof(al_hooks_t), AL_HOOKS_VERSION, hooks_hook, hooks_set_enabled, hooks_state};

const void* AL_CALL host_interface(al_plugin_t*, const char* name, uint32_t) {
    const std::string n(name);
    if (n == AL_GAME_INTERFACE) return &game;
    if (n == AL_SCANNER_INTERFACE) return &scanner;
    if (n == AL_FRAME_INTERFACE) return &frame;
    if (n == AL_D3D12_INTERFACE) return &d3d12;
    if (n == AL_HOOKS_INTERFACE) return &hooks;
    if (n == AL_INPUT_INTERFACE) return &input;
    if (n == CMFM_MENU_INTERFACE) return &cmfm;
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
        smain_field(0x9F1F8) = 0;  // started and done, zeroed before the release as executeJob does
        smain_field(0x9F1FC) = 0;
        job_slot(1) = reinterpret_cast<std::uintptr_t>(fake_job);
        reinterpret_cast<SpinFn>(static_cast<void*>(planted_spin))(smain_object, 1);
        const auto execute = hook_enabled.load() && hooked_detour.load() ? hooked_detour.load() : &host_execute_job;
        execute(reinterpret_cast<std::uintptr_t>(smain_object), 0, reinterpret_cast<std::uintptr_t>(host_jobs), 3, 0);
        render_counter().fetch_add(1);
        info.qpc = qpc();
        if (frame_callbacks[AL_FRAME_END]) frame_callbacks[AL_FRAME_END](self, AL_FRAME_END, &info, nullptr);
    }
    return 0;
}

DWORD WINAPI job_thread(void*) {
    job_tid = GetCurrentThreadId();
    const auto f = frequency();
    while (!jobs_stop.load()) {
        if (!reinterpret_cast<std::atomic<std::uintptr_t>&>(job_slot(1)).load()) {
            YieldProcessor();
            continue;
        }
        reinterpret_cast<std::atomic<std::uint32_t>&>(smain_field(0x9F1F8)).fetch_add(1);  // started
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

std::string replaced(std::string text, const std::string& from, const std::string& to) {
    const auto at = text.find(from);
    if (at != std::string::npos) text.replace(at, from.size(), to);
    return text;
}

// Player mode: no run folder. The frames run as in the research flow.
int player_flow(const al_plugin_info_t* info, const std::wstring& folder) {
    CHECK(last_status == AL_STATUS_ACTIVE);
    CHECK(frame_callbacks[AL_FRAME_BEGIN] && frame_callbacks[AL_FRAME_END] && !present_callback);  // no Present timing
    CHECK(added_title == "CPU optimizations" && added_rows == 17 && added_page.get && added_page.set && added_page.get_text);
    CHECK(added_page.press && added_page.on_event && added_page.locked);
    if (!too_long.empty()) std::printf("  too long for CMFM: %s\n", too_long.c_str());
    CHECK(too_long.empty());
    const HANDLE jobs = CreateThread(nullptr, 0, job_thread, nullptr, 0, nullptr);
    const HANDLE threads[] = {CreateThread(nullptr, 0, game_thread, nullptr, 0, nullptr),
                              CreateThread(nullptr, 0, present_thread, nullptr, 0, nullptr)};
    Sleep(1500);
    const auto ini = folder + L"\\cpuopt.ini";
    auto text = read_all(ini);
    CHECK(text.find("enabled = on") != std::string::npos && text.find("remove_fps_cap = off") != std::string::npos);
    CHECK(text.find("toggle_key = None") != std::string::npos && hotkey_live == 0);
    // The Mods page: All optimizations off, Ctrl+F9 as the hotkey.
    cmfm_value_t v{};
    CHECK(added_page.get(added_page.user, 1, &v) == CMFM_OK && v.i == 1);
    v.i = 0;
    CHECK(added_page.set(added_page.user, 1, &v) == CMFM_OK);
    v.i = 0x100 | 0x78;
    CHECK(added_page.set(added_page.user, 2, &v) == CMFM_OK);
    v.i = 0x1B;  // plain Esc: refused
    char why[200] = "";
    CHECK(added_page.validate && added_page.validate(added_page.user, 2, &v, why, sizeof(why)) == CMFM_ERROR && why[0]);
    Sleep(1500);
    text = read_all(ini);
    CHECK(text.find("enabled = off") != std::string::npos && text.find("toggle_key = Ctrl+F9") != std::string::npos);
    CHECK(hotkey_live != 0 && hotkey_key == 0x78 && hotkey_mods == 1);
    char status[128] = "";
    added_page.get_text(added_page.user, 3, status, sizeof(status));
    std::printf("  status: %s\n", status);
    CHECK(std::string(status).find("optimizations off") != std::string::npos);
    // The hotkey turns them on again.
    CHECK(hotkey_fn != nullptr);
    if (hotkey_fn) hotkey_fn(self, hotkey_live, hotkey_user);
    Sleep(1500);
    CHECK(read_all(ini).find("enabled = on") != std::string::npos);
    // Every usable switch counted as on (the row names once shadowed four toggles).
    added_page.get_text(added_page.user, 3, status, sizeof(status));
    std::printf("  status: %s\n", status);
    CHECK(std::string(status).find("optimization") != std::string::npos && std::string(status).find(" on") != std::string::npos && std::string(status).find(" of ") == std::string::npos);
    // A PIX capture request: taken (through the layer when PIX is installed here), logged, the file gone.
    write_all(folder + L"\\pix-capture.txt", "2\r\n");
    Sleep(1500);
    CHECK(GetFileAttributesW((folder + L"\\pix-capture.txt").c_str()) == INVALID_FILE_ATTRIBUTES);
    // A hand edit while the game runs (and 1 s benchmark windows).
    write_all(ini, replaced(replaced(read_all(ini), "unit_us = 40", "unit_us = 50"), "benchmark_s = 6 ", "benchmark_s = 1 "));
    Sleep(2000);

    // The benchmark: Run on the page, then it starts once the page closes. The
    // page opening on the way pauses it, and that window runs again.
    char why_locked[200] = "";
    CHECK(added_page.locked(added_page.user, 31, why_locked, sizeof(why_locked)) == 0);
    added_page.on_event(added_page.user, CMFM_EVENT_OPENED);
    CHECK(added_page.press(added_page.user, 31) == CMFM_OK);
    Sleep(300);
    CHECK(added_page.locked(added_page.user, 31, why_locked, sizeof(why_locked)) == 1 && why_locked[0]);
    added_page.on_event(added_page.user, CMFM_EVENT_CLOSED);
    Sleep(3500);  // a 1 s countdown, then two windows and a half
    added_page.on_event(added_page.user, CMFM_EVENT_OPENED);
    Sleep(400);
    added_page.get_text(added_page.user, 32, status, sizeof(status));
    std::printf("  benchmark: %s\n", status);
    CHECK(std::string(status).find("Paused") == 0 && std::string(status).size() <= 30);
    added_page.get_text(added_page.user, 3, status, sizeof(status));
    CHECK(std::string(status).find("benchmark") != std::string::npos);
    added_page.on_event(added_page.user, CMFM_EVENT_CLOSED);
    const auto report_path = folder + L"\\cpuopt-benchmark.txt";
    for (int i = 0; i < 300 && read_all(report_path).empty(); ++i) Sleep(100);
    Sleep(300);
    const auto report = read_all(report_path);
    std::printf("%s", report.c_str());
    CHECK(report.find("== cpuopt benchmark, ") == 0 && report.find("\r\n") != std::string::npos);
    CHECK(report.find("Frame rate: ") != std::string::npos && report.find("1 window(s) measured again") != std::string::npos);
    CHECK(report.find("\n16  off ") != std::string::npos);
    added_page.get_text(added_page.user, 32, status, sizeof(status));
    std::printf("  benchmark: %s\n", status);
    CHECK(std::string(status).find(" fps, ") != std::string::npos && std::string(status).size() <= 30);
    added_page.get_text(added_page.user, 33, status, sizeof(status));
    std::printf("  benchmark: %s\n", status);
    CHECK(std::string(status) == "not measured" || std::string(status).find(" ms") != std::string::npos);
    CHECK(added_page.locked(added_page.user, 31, why_locked, sizeof(why_locked)) == 0);
    // Stopped by a change after a few pairs: those pairs are reported.
    added_page.on_event(added_page.user, CMFM_EVENT_OPENED);
    CHECK(added_page.press(added_page.user, 31) == CMFM_OK);
    added_page.on_event(added_page.user, CMFM_EVENT_CLOSED);
    Sleep(6500);  // the countdown and five windows or so
    v.i = 0;
    CHECK(added_page.set(added_page.user, 12, &v) == CMFM_OK);  // Draw job order off
    Sleep(1500);
    added_page.get_text(added_page.user, 32, status, sizeof(status));
    std::printf("  benchmark: %s\n", status);
    CHECK(std::string(status).find("/8)") != std::string::npos);
    CHECK(read_all(report_path).find("Stopped early (a setting changed): ") != std::string::npos);
    v.i = 1;
    CHECK(added_page.set(added_page.user, 12, &v) == CMFM_OK);
    Sleep(1500);
    // And one the game closes on (EXITING below).
    added_page.on_event(added_page.user, CMFM_EVENT_OPENED);
    CHECK(added_page.press(added_page.user, 31) == CMFM_OK);
    added_page.on_event(added_page.user, CMFM_EVENT_CLOSED);
    Sleep(6500);

    const auto t_exit = qpc();
    info->on_phase(self, AL_PHASE_EXITING, nullptr);
    const double exiting_ms = double(qpc() - t_exit) * 1000.0 / double(frequency());
    frames_stop = true;
    WaitForMultipleObjects(2, threads, TRUE, 2000);
    jobs_stop = true;
    WaitForSingleObject(jobs, 2000);
    const al_phase_shutdown_t shutdown{sizeof(al_phase_shutdown_t), 0};
    info->on_phase(self, AL_PHASE_SHUTDOWN, &shutdown);

    const auto log = read_all(folder + L"\\cpuopt-session.log");
    std::printf("al_host_smoke (player): %zu bytes of cpuopt-session.log, EXITING took %.1f ms\n", log.size(), exiting_ms);
    CHECK(log.find("{\"event\":\"start\"") == 0 || log.find("\n{\"event\":\"start\"") != std::string::npos);
    CHECK(log.find("\"toggles\":51512,\"requested_toggles\":51512,\"marker\":\"player\"") != std::string::npos);
    CHECK(log.find("\"toggles\":0,\"requested_toggles\":0,\"marker\":\"player\"") != std::string::npos);
    CHECK(log.find("\"unit_us\":50") != std::string::npos);  // the hand edit
    CHECK(log.find("\"event\":\"settings_saved\"") != std::string::npos && log.find("\"event\":\"settings_read\"") != std::string::npos);
    CHECK(log.find("\"event\":\"cmfm_page\",") != std::string::npos && log.find("\"added\":1") != std::string::npos);
    CHECK(log.find("\"event\":\"status\"") != std::string::npos && log.find("\"event\":\"stopped\"}") != std::string::npos);
    CHECK(log.find("\"event\":\"window\"") == std::string::npos && log.find("\"event\":\"profile\"") == std::string::npos);
    CHECK(read_all(ini).find("unit_us = 50") != std::string::npos);
    // The benchmark's windows, its result, and the player's settings after it.
    CHECK(log.find("\"marker\":\"benchmark-off\"") != std::string::npos && log.find("\"marker\":\"benchmark-on\"") != std::string::npos);
    CHECK(log.find("\"event\":\"benchmark\"") != std::string::npos && log.find("\"what\":\"paused\"") != std::string::npos);
    CHECK(log.find("\"marker\":\"player\"", log.find("\"event\":\"benchmark\"")) != std::string::npos);  // its own settings back
    CHECK(log.find("\"event\":\"modules\"") != std::string::npos && log.find("al_d3d12_subscribed") == std::string::npos);
    CHECK(log.find("\"event\":\"pix_capturer\"") != std::string::npos);
    CHECK(log.find("\"event\":\"pix_capture\"") != std::string::npos && log.find("\"frames\":2") != std::string::npos);
    CHECK(lines_with(log, "\"event\":\"bench_window\"").size() >= 16 + 4 + 4);
    CHECK(log.find("\"stopped\":\"a setting changed\"") != std::string::npos && log.find("\"stopped\":\"the game closed\"") != std::string::npos);
    CHECK(read_all(folder + L"\\cpuopt-benchmark.txt").find("Stopped early (the game closed): ") != std::string::npos);
    CHECK(exiting_ms < 2500);
    std::printf(failures ? "al_host_smoke (player): %d failure(s)\n" : "al_host_smoke (player): ok\n", failures);
    std::fflush(stdout);
    ExitProcess(failures ? 1 : 0);
}

int wmain(int argc, wchar_t** argv) {
    const bool player = argc == 4 && std::wstring(argv[3]) == L"player";
    if (argc != 3 && !player) return 2;
    const std::wstring source = argv[1], scratch = argv[2];
    CreateDirectoryW(scratch.c_str(), nullptr);
    const auto root = scratch + L"\\host-" + std::to_wstring(GetCurrentProcessId());
    const auto plugins = root + L"\\plugins", folder = plugins + L"\\cpuopt", run = root + L"\\run";
    for (const auto& d : {root, plugins, folder, run}) CreateDirectoryW(d.c_str(), nullptr);
    const auto dll = folder + L"\\cpuopt.dll";
    CHECK(CopyFileW(source.c_str(), dll.c_str(), FALSE));
    if (player) write_all(folder + L"\\load-pix-capturer.txt", "");  // the research switch: tried, logged either way
    if (!player) {
        write_all(dll + L".output-dir", utf8(run) + "\r\n");
        write_all(run + L"\\control.ini", "window_ms=1000\nmarker=smoke-a\n");
    }

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
    for (std::uintptr_t i = 0; i < 3; ++i) {
        host_jobs[i][0] = 0x1000 + i;  // contexts that are not objects
        host_jobs[i][1] = reinterpret_cast<std::uintptr_t>(&host_job);
        host_jobs[i][2] = host_jobs[i][3] = 0;
    }
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
    CHECK(scans == 24);  // the frame counter, the four job_cap signatures, executeJob, the camera call site,
                         // sPrimitive's loop, the two server time calls, the job threads' wait, the four
                         // sMhUnit signatures, the four sCollision ones, the frame budget's two functions
                         // and the three IsSteamRunning calls
    if (player) return player_flow(info, folder);
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
    std::uintptr_t jobs_before[3][4];
    std::memcpy(jobs_before, host_jobs, sizeof(host_jobs));
    write_all(run + L"\\control.ini", "window_ms=1000\nmarker=smoke-b\ntoggles=15\njob_threads=12\n");
    Sleep(2600);
    const auto capped = smain_field(0xD0), capped_participants = smain_field(0xCC);
    // Every switch: no al.patch here and no sPrimitive or server time code in
    // this exe, so those two say why; inline rounds learn the host's round.
    write_all(run + L"\\control.ini", "window_ms=1000\nmarker=smoke-c\ntoggles=8191\ninline_us=500\ntime_ms=500\nspin_us=300\nunit_us=20\n");
    Sleep(1600);
    const unsigned alone = alone_rounds.load();
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
    const auto after_exit = smain_field(0xD0), participants_after_exit = smain_field(0xCC);
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
    // job_timing: registered once when switched on, rounds timed, the list
    // back as it was; the detour left the chain when the control file's last
    // valid state had it on, so nothing changes at the rejected file.
    {
        const auto events = lines_with(text, "\"event\":\"job_timing\"");
        long long rounds = 0;
        for (const auto& line : events) rounds += std::atoll(line.c_str() + line.find("\"rounds\":") + 9);
        std::printf("  job_timing: %u registration(s), %zu event(s), %lld rounds\n", hook_registrations.load(), events.size(), rounds);
        CHECK(hook_registrations == 1);
        CHECK(text.find("\"event\":\"job_hook\",\"on\":true,\"reason\":\"hook registered\"") != std::string::npos);
        CHECK(!events.empty() && rounds > 200);
        CHECK(std::memcmp(jobs_before, host_jobs, sizeof(host_jobs)) == 0);
        // No camera call site in this exe: job order is unavailable and off.
        CHECK(text.find("\"event\":\"job_order_site\",\"reason\":\"executeJob not found once\"") != std::string::npos);
        CHECK(text.find("\"order\":{\"mode\":0,\"available\":false") != std::string::npos);
    }
    std::printf("  job_cap: native %u, capped %u/%u, after EXITING %u/%u\n", native_before, capped, capped_participants, after_exit,
                participants_after_exit);
    // (+0xCC may hold a round's value here: EXITING came from another thread mid-round, and this
    // host has no per-frame copy of +0xD0 into +0xCC as the game does.)
    // The last phase: every switch requested.
    std::printf("  all switches: %u rounds alone\n", alone);
    CHECK(text.find("\"event\":\"prim_stores\",") != std::string::npos && text.find("\"reason\":\"signature not found\",\"scanner\":\"al.scanner\",\"matches\":0") != std::string::npos);
    CHECK(text.find("\"event\":\"server_time\",") != std::string::npos && text.find("\"counts\":[0,0]") != std::string::npos);
    CHECK(text.find("\"event\":\"prim_patch\",\"on\":false,\"reason\":\"no al.patch\"") != std::string::npos);
    CHECK(text.find("\"event\":\"server_hooks\",\"on\":false,\"reason\":\"server time calls not found\"") != std::string::npos);
    CHECK(text.find("\"inline_us\":500,\"time_ms\":500,\"spin_us\":300,\"unit_us\":20") != std::string::npos);
    CHECK(text.find("\"event\":\"unit_lines\",\"t_ms\"") != std::string::npos && text.find("\"units\":{\"state\":\"no target\",") != std::string::npos);
    CHECK(text.find("\"event\":\"steam_running\",") != std::string::npos);
    CHECK(text.find("\"event\":\"running_patch\",\"on\":false,\"reason\":\"no al.patch\"") != std::string::npos);
    CHECK(text.find("\"event\":\"frame_budget\",") != std::string::npos);
    CHECK(text.find("\"event\":\"budget_hooks\",\"on\":false,\"reason\":\"the limiter's function or cD3DRender::present not found\"") !=
          std::string::npos);
    CHECK(text.find("\"budget\":{\"active\":false,") != std::string::npos);
    CHECK(text.find("\"event\":\"worker_spin\",") != std::string::npos);
    CHECK(text.find("\"event\":\"spin_patch\",\"on\":false,\"reason\":\"no al.patch\"") != std::string::npos);
    CHECK(text.find("\"spin\":{\"active\":false,") != std::string::npos);
    CHECK(text.find("\"inline\":{\"on\":true,\"threshold_us\":500,") != std::string::npos);
    CHECK(text.find("\"server_time\":{\"active\":false,") != std::string::npos);
    CHECK(alone > 100);  // learned within 8 rounds, then alone at 250 rounds a second
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
        if (ms < 950 || ms > 1250) continue;  // a loaded machine (the game running) stretches windows
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
    // The spin probe caught the game thread in the game's spin loop and read
    // the round: one thread released, started, still running its job. The job
    // thread's samples from those ticks carry the during-spin flag.
    {
        long long spin_samples = 0, round_samples = 0, released = 0, started_all = 0, one_running = 0, flagged = 0;
        const std::string job_prefix = "[" + std::to_string(job_tid.load()) + ",";
        for (const auto& line : profiles) {
            const auto at = line.find("\"spin_probe\":{\"samples\":");
            if (at == std::string::npos) continue;
            spin_samples += std::atoll(line.c_str() + at + 24);
            // spin_rounds: [caller, samples, released_sum, [not_started x33], [running x33]] per round
            const auto from = line.find("\"spin_rounds\":["), to = line.find(",\"frames\":", from);
            std::vector<long long> n;
            for (auto i = from + 15; i < to; ++i)
                if (line[i] >= '0' && line[i] <= '9') {
                    n.push_back(std::atoll(line.c_str() + i));
                    while (i < to && line[i] >= '0' && line[i] <= '9') ++i;
                }
            for (std::size_t r = 0; r + 69 <= n.size(); r += 69) {
                round_samples += n[r + 1];
                released += n[r + 2];
                started_all += n[r + 3];       // not started: 0
                one_running += n[r + 36 + 1];  // running: 1
            }
            for (auto i = line.find(job_prefix, line.find("\"stacks\":[")); i != std::string::npos; i = line.find(job_prefix, i + 1)) {
                const auto flags = line.find(',', line.find(',', i) + 1) + 1;  // [tid,count,flags,[...]]
                if (line[flags] == '2' || line[flags] == '3') ++flagged;
            }
        }
        std::printf("  spin probe: %lld samples, %lld in rounds, released %lld, all started %lld, one running %lld, "
                    "job thread stacks during the spin %lld\n",
                    spin_samples, round_samples, released, started_all, one_running, flagged);
        CHECK(spin_samples > 50 && round_samples == spin_samples);
        CHECK(released == round_samples);  // one thread per round
        CHECK(one_running > round_samples / 2);
        CHECK(flagged > 0);
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
        for (const auto& line : plugin_log) told = told || line.find("research run, log ") != std::string::npos;
        CHECK(told);
    }
    std::printf(failures ? "al_host_smoke: %d failure(s)\n" : "al_host_smoke: ok\n", failures);
    std::fflush(stdout);
    ExitProcess(failures ? 1 : 0);
}
