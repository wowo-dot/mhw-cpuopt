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
#include "log.hpp"
#include "runtime.hpp"
#include "sampler.hpp"
#include <cstddef>
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
        cpuopt::runtime::stop("exiting");
        say(AL_LOG_INFO, "stopped at EXITING");
    } else if (phase == AL_PHASE_SHUTDOWN) {
        const auto* shutdown = static_cast<const al_phase_shutdown_t*>(data);
        cpuopt::job_cap::restore();
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
        cpuopt::job_cap::set_target(base, host.job);
        // The sampler reads the job slots when it catches the game thread in
        // the spin (the spin signature's 20 bytes hold the whole loop).
        if (host.job.found) cpuopt::sampler::set_spin_probe(spin_site[0], spin_site[0] + 20, base + host.job.global_rva);

        std::string reason;
        frame = service<al_frame_t>(AL_FRAME_INTERFACE, AL_FRAME_VERSION);
        host.frame_state = frame_state;
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
