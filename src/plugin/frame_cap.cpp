#include "frame_cap.hpp"
#include "../core/json.hpp"
#include <windows.h>
#include <atomic>

namespace cpuopt::frame_cap {
namespace {
enum State : int { no_target, waiting, native, raised, layout_mismatch, drift, restored, kept };
constexpr const char* state_names[] = {"no target", "waiting", "native", "raised", "layout mismatch", "drift", "restored",
                                       "the game's own limit kept"};
std::atomic<std::uintptr_t> global{};
std::atomic<int> state{no_target};
std::atomic<unsigned> requested{};
std::atomic<bool> only_from_unlimited{};
std::atomic<float> game_value{}, applied{};
float written{};  // game thread only: what we last wrote (0: nothing)

bool read_float(std::uintptr_t at, float& value) noexcept {
    __try {
        value = *reinterpret_cast<volatile const float*>(at);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool write_float(std::uintptr_t at, float value) noexcept {
    __try {
        *reinterpret_cast<volatile float*>(at) = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool read_ptr(std::uintptr_t at, std::uintptr_t& value) noexcept {
    __try {
        value = *reinterpret_cast<volatile const std::uintptr_t*>(at);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool final_state(int s) noexcept { return s == no_target || s == layout_mismatch || s == drift || s == restored; }

void apply(unsigned want) noexcept {
    const int s = state.load();
    if (final_state(s)) return;
    std::uintptr_t smain{};
    float current{};
    if (!read_ptr(global.load(), smain) || !smain || !read_float(smain + max_fps_offset, current)) {
        state.store(waiting);
        return;
    }
    if (written == 0.0f) {  // nothing written yet: the field is the game's
        if (!(current >= 10.0f && current <= 1000.0f)) {
            state.store(layout_mismatch);
            return;
        }
        game_value.store(current);
        if (!want) {
            state.store(native);
            applied.store(current);
            return;
        }
        if (only_from_unlimited.load() && current != unlimited_fps) {  // a limit the player picked
            state.store(kept);
            applied.store(current);
            return;
        }
    } else if (current != written) {  // someone else (the options) changed it
        if (only_from_unlimited.load()) {  // theirs now; looked at again next frame
            written = 0.0f;
            game_value.store(current);
            applied.store(current);
            state.store(kept);
            return;
        }
        state.store(drift);
        return;
    }
    const float target = want ? static_cast<float>(want) : game_value.load();
    if (target != current && !write_float(smain + max_fps_offset, target)) {
        state.store(drift);
        return;
    }
    written = want ? target : 0.0f;
    applied.store(target);
    state.store(want ? raised : native);
}
}  // namespace

void set_target(std::uintptr_t smain_global) noexcept {
    written = 0.0f;
    applied.store(0.0f);
    global.store(smain_global);
    state.store(smain_global ? waiting : no_target);
}
void request(unsigned max_fps, bool only_unlimited) noexcept {
    only_from_unlimited.store(only_unlimited);
    requested.store(max_fps);
}
void on_begin() noexcept {
    const auto want = requested.load(std::memory_order_relaxed);
    if (!want && written == 0.0f && state.load(std::memory_order_relaxed) != waiting) return;  // nothing to do
    if (state.load(std::memory_order_relaxed) == kept) state.store(waiting);  // look at the options' value again
    apply(want);
}
void restore() noexcept {
    if (written != 0.0f) apply(0);
    if (!final_state(state.load())) state.store(restored);
}

float limit() noexcept { return applied.load(std::memory_order_relaxed); }

void append(std::string& out) {
    json::key(out, "cap");
    out += '{';
    json::key(out, "state");
    json::string(out, state_names[state.load()]);
    json::key(out, "game");
    json::fixed(out, game_value.load(), 1);
    json::key(out, "requested");
    json::number(out, std::uint64_t(requested.load()));
    json::key(out, "applied");
    json::fixed(out, applied.load(), 1);
    out += '}';
}
}  // namespace cpuopt::frame_cap
