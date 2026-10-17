#include "menu.hpp"
#include "../core/settings.hpp"
#include "../core/toggles.hpp"
#include "cmfm/cmfm.h"
#include "log.hpp"
#include "runtime.hpp"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

namespace cpuopt::menu {
namespace {
enum Id : std::uint32_t {
    id_enabled = 1,
    id_key,
    id_status,
    id_limited,
    id_optimizations = 10,
    id_effect_stores,
    id_job_order,
    id_server_time,
    id_unit_threshold,
    id_cache_ccd,
    id_parallel_contacts,
    id_sleep_limiter,
    id_experimental = 20,
    id_remove_fps_cap,
    id_benchmark = 30,
    id_run,
    id_result,
    id_cpu_result,
};
Availability available;
std::atomic<double> interval_ms{};  // smoothed time between frame starts
std::atomic<std::int64_t> last_begin{};

int row_of(std::uint32_t id) {
    if (id >= id_effect_stores && id <= id_sleep_limiter) return static_cast<int>(id - id_effect_stores);
    if (id == id_remove_fps_cap) return row_remove_fps_cap;
    return -1;
}
bool* flag_of(Settings& s, std::uint32_t id) {
    switch (id) {
    case id_enabled: return &s.enabled;
    case id_effect_stores: return &s.effect_stores;
    case id_job_order: return &s.job_order;
    case id_server_time: return &s.server_time;
    case id_unit_threshold: return &s.unit_threshold;
    case id_cache_ccd: return &s.cache_ccd;
    case id_parallel_contacts: return &s.parallel_contacts;
    case id_sleep_limiter: return &s.sleep_limiter;
    case id_remove_fps_cap: return &s.remove_fps_cap;
    default: return nullptr;
    }
}
constexpr unsigned toggle_of_row[row_count] = {cpuopt::plain_prim_stores, cpuopt::order_jobs, cpuopt::reuse_server_time,
                                              cpuopt::unit_threshold,   cpuopt::cache_ccd,  cpuopt::parallel_contacts,
                                              cpuopt::sleep_limiter,    cpuopt::uncap};
static_assert(toggle_of_row[row_unit_threshold] == 256U && toggle_of_row[row_parallel_contacts] == 32768U);

cmfm_result_t CMFM_CALL get(void*, std::uint32_t id, cmfm_value_t* value) {
    auto s = runtime::settings();
    if (id == id_key) {
        value->i = static_cast<std::int32_t>(s.toggle_key);
        return CMFM_OK;
    }
    if (auto* flag = flag_of(s, id)) {
        value->i = *flag ? 1 : 0;
        return CMFM_OK;
    }
    return CMFM_ERROR;
}
cmfm_result_t CMFM_CALL set(void*, std::uint32_t id, const cmfm_value_t* value) {
    auto s = runtime::settings();
    if (id == id_key) {
        s.toggle_key = static_cast<std::uint32_t>(value->i);
    } else if (auto* flag = flag_of(s, id)) {
        *flag = value->i != 0;
    } else {
        return CMFM_ERROR;
    }
    runtime::change(s);
    return CMFM_OK;
}
cmfm_result_t CMFM_CALL validate(void*, std::uint32_t id, const cmfm_value_t* value, char* why, std::uint32_t size) {
    const auto key = static_cast<std::uint32_t>(value->i);
    if (id == id_key && (key == 0x1B || key == 0x0D)) {  // plain Esc or Enter
        std::snprintf(why, size, "Esc and Enter are the game's; pick another key or add Ctrl, Shift or Alt.");
        return CMFM_ERROR;
    }
    return CMFM_OK;
}
void CMFM_CALL get_text(void*, std::uint32_t id, char* text, std::uint32_t size) {
    if (id == id_status) std::snprintf(text, size, "%s", status_text().c_str());
    if (id == id_limited) std::snprintf(text, size, "%s", runtime::limit_text().c_str());
    if (id == id_result) std::snprintf(text, size, "%s", runtime::bench_text(0).c_str());
    if (id == id_cpu_result) std::snprintf(text, size, "%s", runtime::bench_text(1).c_str());
}
cmfm_result_t CMFM_CALL press(void*, std::uint32_t id) {
    if (id != id_run) return CMFM_ERROR;
    runtime::bench_press();
    return CMFM_OK;
}
void CMFM_CALL on_event(void*, cmfm_event_t event) {
    if (event == CMFM_EVENT_OPENED) runtime::menu_shown(true);
    if (event == CMFM_EVENT_CLOSED) runtime::menu_shown(false);
}
std::uint32_t CMFM_CALL locked(void*, std::uint32_t id, char* why, std::uint32_t size) {
    if (id == id_run) {
        auto s = runtime::settings();
        s.enabled = true;
        const char* reason = runtime::bench_busy() ? "Running: the hotkey or any change here stops it."
                             : (toggles_of(s) & ~unsigned(cpuopt::uncap)) == 0 ? "Turn on at least one optimization first."
                                                                               : nullptr;
        if (reason) std::snprintf(why, size, "%s", reason);
        return reason ? 1 : 0;
    }
    const int row = row_of(id);
    if (row < 0 || !available.why[row]) return 0;
    std::snprintf(why, size, "%s", available.why[row]);
    return 1;
}

cmfm_setting_t row(std::uint32_t id, cmfm_type_t type, const char* label, const char* help, std::int32_t default_value = 0,
                   cmfm_flags_t flags = 0) {
    cmfm_setting_t s;
    std::memset(&s, 0, sizeof(s));
    s.size = sizeof(s);
    s.id = id;
    s.type = type;
    s.flags = flags;
    s.label = label;
    s.help = help;
    s.default_value.i = default_value;
    return s;
}
}  // namespace

std::string status_text() {
    const auto s = runtime::settings();
    const auto requested = toggles_of(s);
    const auto failed = runtime::failed_toggles();
    unsigned on = 0, usable = 0, broken = 0;
    for (unsigned r = 0; r < row_remove_fps_cap; ++r) {  // the optimizations, not the cap switch
        if (available.why[r]) continue;
        ++usable;
        if (!(requested & toggle_of_row[r])) continue;
        ++on;
        if (failed & toggle_of_row[r]) ++broken;
    }
    char fps[32] = "";
    const double ms = interval_ms.load(std::memory_order_relaxed);
    if (ms > 0.1) std::snprintf(fps, sizeof(fps), "%.0f fps, ", 1000.0 / ms);
    char line[128];
    if (runtime::bench_busy()) std::snprintf(line, sizeof(line), "%sbenchmark running", fps);
    else if (!s.enabled) std::snprintf(line, sizeof(line), "%soptimizations off", fps);
    else if (broken) std::snprintf(line, sizeof(line), "%s%u of %u on, %u failed (log)", fps, on - broken, usable, broken);
    else if (on == usable) std::snprintf(line, sizeof(line), usable == 1 ? "%sthe optimization on" : "%sall %u optimizations on", fps, usable);
    else std::snprintf(line, sizeof(line), "%s%u of %u optimizations on", fps, on, usable);
    return line;
}

void on_frame(std::int64_t qpc) noexcept {
    const auto previous = last_begin.exchange(qpc, std::memory_order_relaxed);
    if (!previous || qpc <= previous) return;
    const double ms = double(qpc - previous) * 1000.0 / double(qpc_frequency());
    if (ms > 1000.0) return;  // a load or a pause, not a frame
    const double old = interval_ms.load(std::memory_order_relaxed);
    interval_ms.store(old > 0 ? old + (ms - old) * 0.02 : ms, std::memory_order_relaxed);
}

bool add(const void* cmfm, const Availability& a, const char* version, std::string& why) noexcept {
    if (!cmfm) {
        why = "CMFM not installed: settings in cpuopt.ini only";
        return false;
    }
    available = a;
    const Settings d;
    // The benchmark's question names its length (benchmark_s as the game started).
    static std::string question;
    const unsigned total_s = runtime::settings().benchmark_s * 16 + 10;
    question = "Close the menu and stand still, camera too, for about " +
               (total_s >= 90 ? std::to_string((total_s + 30) / 60) + " minutes" : std::to_string(total_s) + " seconds") +
               " while cpuopt turns its optimizations off and on. Start?";
    cmfm_setting_t run = row(id_run, CMFM_TYPE_BUTTON, "Run benchmark",
                             "Measures this PC with and without the optimizations in the spot you stand: 8 pairs of turns. A sound plays "
                             "at the end; the result shows below and in cpuopt-benchmark.txt next to cpuopt.dll.");
    run.button = "Run";
    run.confirm = question.c_str();
    const cmfm_setting_t settings[] = {
        row(id_enabled, CMFM_TYPE_BOOL, "All optimizations",
            "Every optimization below at once. Off is the game as it runs without cpuopt; the hotkey flips this in game.", d.enabled),
        row(id_key, CMFM_TYPE_KEY, "On/off hotkey", "A key that flips All optimizations in game, to compare with and without them.",
            static_cast<std::int32_t>(d.toggle_key)),
        row(id_status, CMFM_TYPE_TEXT, "Status", "Frame rate, and how many optimizations run.", 0, CMFM_FLAG_LIVE),
        row(id_limited, CMFM_TYPE_TEXT, "Limited by",
            "What sets the frame rate right now. When it is the graphics card or a limit, cpuopt cannot raise fps; it still cuts the "
            "CPU time each frame needs.",
            0, CMFM_FLAG_LIVE),
        row(id_optimizations, CMFM_TYPE_SECTION, "Optimizations", nullptr),
        row(id_effect_stores, CMFM_TYPE_BOOL, "Effect drawing",
            "Effects write their vertex data without reading GPU memory back. The biggest gain, most of all in effect-heavy scenes and fights.",
            d.effect_stores),
        row(id_job_order, CMFM_TYPE_BOOL, "Draw job order", "The slowest draw jobs start first, so each round of jobs ends sooner.",
            d.job_order),
        row(id_server_time, CMFM_TYPE_BOOL, "Steam server time",
            "Steam's server time is read once a second instead of several times a frame.", d.server_time),
        row(id_unit_threshold, CMFM_TYPE_BOOL, "Update threads",
            "More of each frame's object updates run on the job threads instead of the main thread.", d.unit_threshold),
        row(id_cache_ccd, CMFM_TYPE_BOOL, "Stay on the cache die",
            "On CPUs with a 3D V-Cache die (7950X3D, 9950X3D...) the game runs on that die only. A large gain there.", d.cache_ccd),
        row(id_parallel_contacts, CMFM_TYPE_BOOL, "Parallel hit detection",
            "Hit detection runs on all job threads, as the engine was built to do. New: report it if a hit ever behaves oddly.",
            d.parallel_contacts),
        row(id_sleep_limiter, CMFM_TYPE_BOOL, "Sleeping frame limiter",
            "With a frame rate limit, the game sleeps while it waits instead of keeping a core busy. Same frame pacing.", d.sleep_limiter),
        row(id_experimental, CMFM_TYPE_SECTION, "Experimental", nullptr),
        row(id_remove_fps_cap, CMFM_TYPE_BOOL, "Remove 300 fps limit",
            "Lifts the engine's 300 fps limit while the in-game frame rate is set to no limit. A lower limit you chose stays.",
            d.remove_fps_cap),
        row(id_benchmark, CMFM_TYPE_SECTION, "Benchmark", nullptr),
        run,
        row(id_result, CMFM_TYPE_TEXT, "Result",
            "The last benchmark's frame rate, without and with the optimizations. The full report is in cpuopt-benchmark.txt next to cpuopt.dll.",
            0, CMFM_FLAG_LIVE),
        row(id_cpu_result, CMFM_TYPE_TEXT, "CPU time per frame",
            "The game thread's CPU time per frame, without and with: what the CPU needs, whatever the graphics card or a frame limit allows.",
            0, CMFM_FLAG_LIVE),
    };
    cmfm_page_t page;
    std::memset(&page, 0, sizeof(page));
    page.size = sizeof(page);
    page.plugin = "cpuopt";
    page.title = "CPU optimizations";
    page.help = "Makes Monster Hunter: World use the CPU better. Changes apply at once and are kept in cpuopt.ini.";
    page.settings = settings;
    page.setting_count = static_cast<std::uint32_t>(std::size(settings));
    page.get = get;
    page.set = set;
    page.validate = validate;
    page.get_text = get_text;
    page.locked = locked;
    page.press = press;
    page.on_event = on_event;
    page.version = version;
    page.mod = "cpuopt";
    const auto* menu = static_cast<const cmfm_menu_t*>(cmfm);
    char reason[200] = "";
    if (!menu->add_page(&page, reason, sizeof(reason))) {
        why = std::string("CMFM refused the page: ") + reason;
        return false;
    }
    why = "page added";
    return true;
}
}  // namespace cpuopt::menu
