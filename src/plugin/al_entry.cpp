// Astera Loader entry: <game>\AsteraLoader\plugins\cpuopt\cpuopt.dll.
//
// al_plugin_init runs at PRE_MAIN on the game's main thread, with the game's
// code already unpacked: the frame-counter signature is searched there
// (al.scanner, cached per exe by AL), the run folder is read, and the window
// thread starts. Per-frame work is limited to the callbacks below, which only
// count (frame_events.hpp). EXITING stops every thread of ours.
#include "al/al_plugin.h"
#include "frame_clock.hpp"
#include "frame_events.hpp"
#include "image_scan.hpp"
#include "job_cap.hpp"
#include "job_timing.hpp"
#include "log.hpp"
#include "prim_stores.hpp"
#include "runtime.hpp"
#include "sampler.hpp"
#include "server_time.hpp"
#include "steam_running.hpp"
#include "unit_lines.hpp"
#include "worker_spin.hpp"
#include <atomic>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <string>

#ifndef CPUOPT_VERSION
#define CPUOPT_VERSION "0.0.0"
#endif

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace {
const al_api_t* api{};
al_plugin_t* self{};
const al_frame_t* frame{};
const al_hooks_t* hooks{};
std::atomic<std::uint32_t> timing_hook{};  // al.hooks handle, 0 until first switched on
SRWLOCK timing_lock = SRWLOCK_INIT;
const al_patches_t* patches{};
std::uint32_t prim_handle{};                 // al.patch handle, 0 until first switched on
std::uint32_t query_hook{}, date_hook{};     // al.hooks handles, 0 until first switched on
std::uintptr_t prim_target{}, query_target{}, date_target{};  // found at PRE_MAIN
std::uintptr_t spin_target{};                // the job threads' wait call (match + 7), found at PRE_MAIN
std::uint32_t spin_handle{};                 // al.patch handle, 0 until first switched on
std::uintptr_t spin_stub{};                  // its jump stub, made at PRE_MAIN
cpuopt::steam_running::Discovery running{};  // the IsSteamRunning sites and stub, found at PRE_MAIN
std::uint32_t running_handles[2]{};          // al.patch handles, 0 until first switched on
SRWLOCK switch_lock = SRWLOCK_INIT;
std::uintptr_t validated_renderer{};  // game thread only

void say(al_log_level_t level, const std::string& text) noexcept {
    if (api && self) api->log(self, level, text.c_str());
}

int frame_state() noexcept {
    if (!frame) return -1;
    al_frame_status_t status{};
    status.size = sizeof(status);
    frame->status(self, &status);
    return static_cast<int>(status.state);
}

// The detour on sMain::executeJob (job timing and job order). It is
// registered the first time either switch is on, so sessions that use
// neither carry no hook there; afterwards set_enabled takes it in and out of
// the chain.
bool job_hook(bool on, const char** reason) noexcept {
    AcquireSRWLockExclusive(&timing_lock);
    bool ok = true;
    if (!hooks) {
        ok = false;
        *reason = "no al.hooks";
    } else if (!timing_hook.load()) {
        if (on) {
            al_hook_t h{};
            h.size = sizeof(h);
            h.name = "sMain::executeJob (cpuopt job_timing)";
            static const std::string pattern(cpuopt::job_timing::execute_job_signature);
            h.pattern = pattern.c_str();
            h.detour = reinterpret_cast<void*>(&cpuopt::job_timing::detour);
            h.next = reinterpret_cast<void**>(cpuopt::job_timing::next_slot());
            timing_hook.store(hooks->hook(self, &h));
            ok = timing_hook.load() != 0;
            *reason = ok ? "hook registered" : "al.hooks refused the hook (see the plugin log)";
        }
    } else {
        ok = hooks->set_enabled(self, timing_hook.load(), on ? 1 : 0) == AL_OK;
        *reason = ok ? (on ? "hook enabled" : "hook disabled") : "set_enabled failed";
    }
    ReleaseSRWLockExclusive(&timing_lock);
    return ok;
}

// sPrimitive's record loop (prim_stores.hpp): applied the first time the
// switch is on (AL pauses the game's threads for a code change and checks
// none is inside the bytes), then switched with set_enabled.
bool prim_patch(bool on, const char** reason) noexcept {
    AcquireSRWLockExclusive(&switch_lock);
    bool ok = true;
    if (!patches) {
        ok = false;
        *reason = "no al.patch";
    } else if (!prim_target) {
        ok = false;
        *reason = "sPrimitive's loop not found";
    } else if (!prim_handle) {
        if (on) {
            static const std::string pattern(cpuopt::prim_stores::signature);
            al_patch_t p{};
            p.size = sizeof(p);
            p.kind = AL_PATCH_CODE;
            p.name = "sPrimitive record loop: stores without reads (cpuopt prim_stores)";
            p.pattern = pattern.c_str();
            p.offset = static_cast<std::int32_t>(cpuopt::prim_stores::patch_offset);
            p.length = cpuopt::prim_stores::patch_length;
            p.address = reinterpret_cast<void*>(prim_target);
            p.expected = cpuopt::prim_stores::expected.data();
            p.replacement = cpuopt::prim_stores::replacement.data();
            prim_handle = patches->apply(self, &p);
            ok = prim_handle != 0;
            *reason = ok ? "change applied" : "al.patch refused the change (see the plugin log)";
        }
    } else {
        ok = patches->set_enabled(self, prim_handle, on ? 1 : 0) == AL_OK;
        *reason = ok ? (on ? "change enabled" : "change disabled") : "set_enabled failed";
    }
    ReleaseSRWLockExclusive(&switch_lock);
    return ok;
}

// The job threads' wait (worker_spin.hpp): applied the first time the
// switch is on, through a jump stub within 2 GB of the exe, then switched
// with set_enabled.
bool spin_patch(bool on, const char** reason) noexcept {
    AcquireSRWLockExclusive(&switch_lock);
    bool ok = true;
    if (!patches) {
        ok = false;
        *reason = "no al.patch";
    } else if (!spin_target) {
        ok = false;
        *reason = "job threads' wait not found";
    } else if (!spin_handle) {
        if (on) {
            const auto stub = spin_stub;
            std::array<std::uint8_t, cpuopt::worker_spin::patch_length> expected{}, replacement{};
            std::memcpy(expected.data(), reinterpret_cast<const void*>(spin_target), expected.size());
            if (!stub || !cpuopt::worker_spin::build_replacement(spin_target, stub, replacement)) {
                ok = false;
                *reason = stub ? "jump stub out of reach" : "no room for a jump stub within 2 GB at start-up";
            } else {
                static const std::string pattern(cpuopt::worker_spin::signature);
                al_patch_t p{};
                p.size = sizeof(p);
                p.kind = AL_PATCH_CODE;
                p.name = "job threads' wait: spin before sleeping (cpuopt spin_workers)";
                p.pattern = pattern.c_str();
                p.offset = static_cast<std::int32_t>(cpuopt::worker_spin::patch_offset);
                p.length = cpuopt::worker_spin::patch_length;
                p.address = reinterpret_cast<void*>(spin_target);
                p.expected = expected.data();
                p.replacement = replacement.data();
                spin_handle = patches->apply(self, &p);
                ok = spin_handle != 0;
                *reason = ok ? "change applied" : "al.patch refused the change (see the plugin log)";
            }
        }
    } else {
        ok = patches->set_enabled(self, spin_handle, on ? 1 : 0) == AL_OK;
        *reason = ok ? (on ? "change enabled" : "change disabled") : "set_enabled failed";
    }
    ReleaseSRWLockExclusive(&switch_lock);
    return ok;
}

// The IsSteamRunning calls (steam_running.hpp): both sites or neither,
// through the stub made at PRE_MAIN.
bool running_patch(bool on, const char** reason) noexcept {
    AcquireSRWLockExclusive(&switch_lock);
    bool ok = true;
    if (!patches) {
        ok = false;
        *reason = "no al.patch";
    } else if (!running.found) {
        ok = false;
        *reason = "IsSteamRunning calls not found";
    } else if (!running_handles[0] && !running_handles[1]) {
        if (on) {
            static const std::string poll(cpuopt::steam_running::poll_signature), lobby(cpuopt::steam_running::lobby_signature);
            const std::string* patterns[2] = {&poll, &lobby};
            const unsigned offsets[2] = {cpuopt::steam_running::poll_call, cpuopt::steam_running::lobby_call};
            static std::array<std::uint8_t, cpuopt::steam_running::call_length> expected[2], replacement[2];
            ok = running.stub != 0;
            for (unsigned i = 0; i < 2 && ok; ++i) {
                std::memcpy(expected[i].data(), reinterpret_cast<const void*>(running.sites[i]), expected[i].size());
                ok = cpuopt::steam_running::build_replacement(running.sites[i], running.stub, replacement[i]);
                if (!ok) break;
                al_patch_t p{};
                p.size = sizeof(p);
                p.kind = AL_PATCH_CODE;
                p.name = i ? "IsSteamRunning in sub_142215C00 (cpuopt cache_steam_running)"
                           : "IsSteamRunning in sub_142209BF0 (cpuopt cache_steam_running)";
                p.pattern = patterns[i]->c_str();
                p.offset = static_cast<std::int32_t>(offsets[i]);
                p.length = cpuopt::steam_running::call_length;
                p.address = reinterpret_cast<void*>(running.sites[i]);
                p.expected = expected[i].data();
                p.replacement = replacement[i].data();
                running_handles[i] = patches->apply(self, &p);
                ok = running_handles[i] != 0;
            }
            if (!ok && running_handles[0]) patches->set_enabled(self, running_handles[0], 0);  // both or neither
            *reason = ok ? "changes applied" : running.stub ? "al.patch refused a change (see the plugin log)" : "no jump stub";
        }
    } else if (!running_handles[0] || !running_handles[1]) {
        ok = false;
        *reason = "a change was refused earlier";
    } else {
        ok = patches->set_enabled(self, running_handles[0], on ? 1 : 0) == AL_OK &&
             patches->set_enabled(self, running_handles[1], on ? 1 : 0) == AL_OK;
        *reason = ok ? (on ? "changes enabled" : "changes disabled") : "set_enabled failed";
    }
    ReleaseSRWLockExclusive(&switch_lock);
    return ok;
}

// The server_time detours (server_time.hpp): both or neither.
bool server_hooks(bool on, const char** reason) noexcept {
    AcquireSRWLockExclusive(&switch_lock);
    bool ok = true;
    if (!hooks) {
        ok = false;
        *reason = "no al.hooks";
    } else if (!query_target || !date_target) {
        ok = false;
        *reason = "server time calls not found";
    } else if (!query_hook && !date_hook) {
        if (on) {
            static const std::string query_pattern(cpuopt::server_time::query_signature), date_pattern(cpuopt::server_time::date_signature);
            al_hook_t h{};
            h.size = sizeof(h);
            h.name = "server time query (cpuopt server_time)";
            h.pattern = query_pattern.c_str();
            h.address = reinterpret_cast<void*>(query_target);
            h.detour = reinterpret_cast<void*>(&cpuopt::server_time::query_detour);
            h.next = reinterpret_cast<void**>(cpuopt::server_time::query_next());
            query_hook = hooks->hook(self, &h);
            h.name = "server time date (cpuopt server_time)";
            h.pattern = date_pattern.c_str();
            h.address = reinterpret_cast<void*>(date_target);
            h.detour = reinterpret_cast<void*>(&cpuopt::server_time::date_detour);
            h.next = reinterpret_cast<void**>(cpuopt::server_time::date_next());
            date_hook = query_hook ? hooks->hook(self, &h) : 0;
            ok = query_hook && date_hook;
            if (!ok && query_hook) hooks->set_enabled(self, query_hook, 0);  // both or neither
            *reason = ok ? "hooks registered" : "al.hooks refused a hook (see the plugin log)";
        }
    } else if (!query_hook || !date_hook) {
        ok = false;
        *reason = "a hook was refused earlier";
    } else {
        ok = hooks->set_enabled(self, query_hook, on ? 1 : 0) == AL_OK && hooks->set_enabled(self, date_hook, on ? 1 : 0) == AL_OK;
        *reason = ok ? (on ? "hooks enabled" : "hooks disabled") : "set_enabled failed";
    }
    ReleaseSRWLockExclusive(&switch_lock);
    return ok;
}

void AL_CALL on_frame(al_plugin_t*, al_frame_event_t event, const al_frame_info_t* info, void*) {
    if (!info) return;
    if (event == AL_FRAME_BEGIN) {
        cpuopt::frame_events::on_begin(info->qpc, GetCurrentThreadId());
        cpuopt::job_cap::on_begin();  // before the update: no job round runs now
        std::uint64_t counter{};
        if (cpuopt::frame_clock::read(validated_renderer, counter)) cpuopt::frame_events::on_render_counter(counter);
    } else if (event == AL_FRAME_END) {
        cpuopt::frame_events::on_end(info->qpc);
    }
}

void AL_CALL on_d3d12(al_plugin_t*, al_d3d12_event_t event, const void* data, void*) {
    const auto* present = static_cast<const al_d3d12_present_t*>(data);
    if (!present || present->size < offsetof(al_d3d12_present_t, previous_qpc) + sizeof(present->previous_qpc)) return;
    if (event == AL_D3D12_PRE_PRESENT) {
        cpuopt::frame_events::on_present(present->present_qpc, present->previous_qpc, GetCurrentThreadId());
    } else if (event == AL_D3D12_POST_PRESENT) {
        cpuopt::frame_events::on_present_returned(present->present_qpc, cpuopt::qpc_now());
    }
}

void AL_CALL on_phase(al_plugin_t*, al_phase_t phase, const void* data) {
    if (phase == AL_PHASE_GAME_READY) {
        const auto* ready = static_cast<const al_phase_game_ready_t*>(data);
        cpuopt::runtime::note("game_ready", "frame", ready && ready->size >= sizeof(*ready) ? static_cast<long long>(ready->frame) : -1);
    } else if (phase == AL_PHASE_EXITING) {
        cpuopt::job_cap::restore();  // the game's own job thread count for its teardown
        cpuopt::job_timing::set_measuring(false);  // rounds from now on run untouched
        cpuopt::job_timing::set_order(0, 0);
        cpuopt::job_timing::set_inline(false, 1);
        cpuopt::server_time::set_active(false, 0);
        cpuopt::worker_spin::set_active(false, 0);  // job threads block again for the game's teardown
        cpuopt::unit_lines::restore();
        cpuopt::steam_running::set_active(false, 0);
        cpuopt::runtime::stop("exiting");
        say(AL_LOG_INFO, "stopped at EXITING");
    } else if (phase == AL_PHASE_SHUTDOWN) {
        const auto* shutdown = static_cast<const al_phase_shutdown_t*>(data);
        cpuopt::job_cap::restore();
        cpuopt::unit_lines::restore();
        cpuopt::runtime::stop("shutdown");  // when EXITING did not come
        cpuopt::runtime::note("shutdown", "exit_code", shutdown && shutdown->size >= sizeof(*shutdown) ? shutdown->exit_code : 0);
    }
}

const al_plugin_info_t info = {
    sizeof(al_plugin_info_t),
    AL_API_VERSION,
    "cpuopt",
    "cpuopt (MHW CPU cost research)",
    CPUOPT_VERSION,
    AL_PHASE_BIT(AL_PHASE_GAME_READY) | AL_PHASE_BIT(AL_PHASE_EXITING) | AL_PHASE_BIT(AL_PHASE_SHUTDOWN),
    0,        // order
    nullptr,  // load_after
    nullptr,  // load_before
    nullptr,  // exe_sha256: any; the signature decides (fail closed)
    on_phase,
    nullptr,  // dependencies
};

template <typename T>
const T* service(const char* name, std::uint32_t version) noexcept {
    if (api->size < offsetof(al_api_t, get_interface) + sizeof(api->get_interface) || !api->get_interface) return nullptr;
    return static_cast<const T*>(api->get_interface(self, name, version));
}
}  // namespace

extern "C" AL_PLUGIN_EXPORT const al_plugin_info_t* AL_CALL al_plugin_info(void) { return &info; }

extern "C" AL_PLUGIN_EXPORT al_result_t AL_CALL al_plugin_init(const al_api_t* host_api, const al_init_t* init) {
    try {
        api = host_api;
        self = init->self;
        cpuopt::runtime::Host host;
        host.api_version = api->version;
        host.folder = init->folder_real ? init->folder_real : (init->folder ? init->folder : L"");
        if (const auto* game = service<al_game_t>(AL_GAME_INTERFACE, AL_GAME_VERSION)) {
            if (game->exe_sha256) host.exe_sha256 = game->exe_sha256;
            if (game->game_version) host.game_version = game->game_version;
        }

        // Signatures, searched now: the code is unpacked at PRE_MAIN. Astera
        // Loader's scanner caches its results per exe; our own scan otherwise.
        const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        const auto* scanner = service<al_scanner_t>(AL_SCANNER_INTERFACE, AL_SCANNER_VERSION);
        host.scanner = scanner ? "al.scanner" : "own";
        const auto find = [&](std::string_view text, std::uintptr_t* matches, unsigned capacity) -> unsigned {
            if (!scanner) return cpuopt::image_scan::find(base, text, matches, capacity);
            void* found[cpuopt::frame_clock::match_capacity]{};
            const std::string pattern(text);
            auto count = scanner->find(self, pattern.c_str(), AL_SCAN_CODE, found, cpuopt::frame_clock::match_capacity);
            if (count == AL_SCAN_INVALID) count = 0;
            for (unsigned i = 0; i < count && i < capacity && i < cpuopt::frame_clock::match_capacity; ++i)
                matches[i] = reinterpret_cast<std::uintptr_t>(found[i]);
            return count;
        };
        std::uintptr_t matches[cpuopt::frame_clock::match_capacity]{};
        const auto count = find(cpuopt::frame_clock::signature, matches, cpuopt::frame_clock::match_capacity);
        host.discovery = cpuopt::frame_clock::discover(base, matches, count);
        cpuopt::frame_clock::set_target(base, host.discovery);

        std::uintptr_t instance_site[1]{}, spin_site[1]{}, unused[1]{};
        const unsigned job_counts[4] = {find(cpuopt::job_cap::instance_signature, instance_site, 1),
                                        find(cpuopt::job_cap::release_signature, unused, 1),
                                        find(cpuopt::job_cap::spin_signature, spin_site, 1),
                                        find(cpuopt::job_cap::copy_signature, unused, 1)};
        host.job = cpuopt::job_cap::discover(base, job_counts, instance_site[0]);
        // The camera draw round's call site: found once, and its call must go
        // to executeJob (found once too).
        std::uintptr_t execute_site[1]{}, camera_site[1]{};
        const auto execute_count = find(cpuopt::job_timing::execute_job_signature, execute_site, 1);
        const auto camera_count = find(cpuopt::job_timing::camera_signature, camera_site, 1);
        host.order_site = "ok";
        if (execute_count != 1 || camera_count != 1) {
            host.order_site = execute_count != 1 ? "executeJob not found once" : "camera call site not found once";
        } else {
            std::int32_t rel{};
            std::memcpy(&rel, reinterpret_cast<const void*>(camera_site[0] + cpuopt::job_timing::camera_call + 1), 4);
            const auto target = camera_site[0] + cpuopt::job_timing::camera_return + static_cast<std::intptr_t>(rel);
            if (target != execute_site[0]) host.order_site = "camera call does not go to executeJob";
            else cpuopt::job_timing::set_camera_return(camera_site[0] + cpuopt::job_timing::camera_return);
        }
        cpuopt::job_cap::set_target(base, host.job);
        // The sampler reads the job slots when it catches the game thread in
        // the spin (the spin signature's 20 bytes hold the whole loop).
        if (host.job.found) cpuopt::sampler::set_spin_probe(spin_site[0], spin_site[0] + 20, base + host.job.global_rva);
        // sPrimitive's record loop and the server time calls: searched now,
        // changed only when their switches are on.
        std::uintptr_t prim_site[2]{}, query_site[2]{}, date_site[2]{};
        host.prim = cpuopt::prim_stores::discover(base, prim_site, find(cpuopt::prim_stores::signature, prim_site, 2));
        if (host.prim.found) prim_target = prim_site[0] + cpuopt::prim_stores::patch_offset;
        const auto query_count = find(cpuopt::server_time::query_signature, query_site, 2);
        const auto date_count = find(cpuopt::server_time::date_signature, date_site, 2);
        host.server = cpuopt::server_time::discover(base, query_site, query_count, date_site, date_count);
        cpuopt::server_time::set_target(base, host.server);
        if (host.server.found) {
            query_target = query_site[0];
            date_target = date_site[0];
        }
        std::uintptr_t wait_site[2]{};
        host.spin = cpuopt::worker_spin::discover(base, wait_site, find(cpuopt::worker_spin::signature, wait_site, 2));
        if (host.spin.found) {
            spin_target = wait_site[0] + cpuopt::worker_spin::patch_offset;
            // The jump stub now, while there is still room near the exe.
            host.spin.stub = cpuopt::worker_spin::make_stub(spin_target, reinterpret_cast<const void*>(&cpuopt::worker_spin::wait));
            spin_stub = host.spin.stub;
        }
        // sMhUnit's line threshold: four signatures, and sMain's global from job_cap.
        {
            std::uintptr_t unused2[2]{};
            const unsigned counts[4] = {find(cpuopt::unit_lines::move_signature, unused2, 2), find(cpuopt::unit_lines::step_signature, unused2, 2),
                                        find(cpuopt::unit_lines::ctor_signature, unused2, 2), find(cpuopt::unit_lines::owner_signature, unused2, 2)};
            host.units = cpuopt::unit_lines::discover(counts);
            if (host.units.found && host.job.found) cpuopt::unit_lines::set_target(base + host.job.global_rva);
        }
        // IsSteamRunning: its two per-frame calls, and their stub now.
        {
            std::uintptr_t poll[2]{}, lobby[2]{};
            const auto poll_count = find(cpuopt::steam_running::poll_signature, poll, 2);
            const auto lobby_count = find(cpuopt::steam_running::lobby_signature, lobby, 2);
            const auto steam = GetModuleHandleW(L"steam_api64.dll");
            const auto function = steam ? reinterpret_cast<std::uintptr_t>(GetProcAddress(steam, "SteamAPI_IsSteamRunning")) : 0;
            running = cpuopt::steam_running::discover(poll, poll_count, lobby, lobby_count, function);
            if (running.found) {
                running.stub = cpuopt::worker_spin::make_stub(running.sites[0], reinterpret_cast<const void*>(&cpuopt::steam_running::is_running));
                cpuopt::steam_running::set_real(running.real);
            }
            host.running = running;
        }

        std::string reason;
        frame = service<al_frame_t>(AL_FRAME_INTERFACE, AL_FRAME_VERSION);
        host.frame_state = frame_state;
        hooks = service<al_hooks_t>(AL_HOOKS_INTERFACE, AL_HOOKS_VERSION);
        host.job_hook = job_hook;
        patches = service<al_patches_t>(AL_PATCH_INTERFACE, AL_PATCH_VERSION);
        host.prim_patch = prim_patch;
        host.server_hooks = server_hooks;
        host.spin_patch = spin_patch;
        host.running_patch = running_patch;
        if (!cpuopt::runtime::start(reinterpret_cast<HMODULE>(&__ImageBase), host, reason)) {
            // Not measuring: no subscriptions, so AL hooks nothing for us.
            frame = nullptr;
            say(AL_LOG_INFO, "idle: " + reason);
            api->set_status(self, AL_STATUS_DEGRADED, reason.c_str());
            return AL_OK;
        }
        say(AL_LOG_INFO, std::string("cpuopt ") + CPUOPT_VERSION + " logging to " + reason);

        // Per-frame timing: the game thread's frames and the game's Presents.
        if (frame) {
            host.frame_events = frame->subscribe(self, AL_FRAME_BEGIN, on_frame, nullptr) != 0 &&
                                frame->subscribe(self, AL_FRAME_END, on_frame, nullptr) != 0;
        }
        if (const auto* d3d12 = service<al_d3d12_t>(AL_D3D12_INTERFACE, AL_D3D12_VERSION)) {
            host.present_events = d3d12->subscribe(self, AL_D3D12_EVENT_BIT(AL_D3D12_PRE_PRESENT) | AL_D3D12_EVENT_BIT(AL_D3D12_POST_PRESENT),
                                                   0, on_d3d12, nullptr) == AL_OK;
        }
        cpuopt::runtime::note("al_frame_subscribed", "ok", host.frame_events ? 1 : 0);
        cpuopt::runtime::note("al_d3d12_subscribed", "ok", host.present_events ? 1 : 0);
        char line[384];
        std::snprintf(line, sizeof(line), "frame counter: %s (%u match(es), %s); job_cap: %s; al.frame %s; al.d3d12 Present events %s",
                      host.discovery.reason, host.discovery.matches, host.scanner.c_str(), host.job.reason, host.frame_events ? "on" : "off",
                      host.present_events ? "on" : "off");
        say(AL_LOG_INFO, line);
        if (!host.discovery.found || !host.frame_events) {
            api->set_status(self, AL_STATUS_DEGRADED, !host.discovery.found ? "frame counter not found" : "no al.frame");
        } else {
            api->set_status(self, AL_STATUS_ACTIVE, nullptr);
        }
        return AL_OK;
    } catch (...) {
        if (api && self) api->set_status(self, AL_STATUS_DISABLED, "start failed");
        return AL_ERROR;
    }
}
