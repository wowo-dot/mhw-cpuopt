#pragma once
#include <optional>
#include <string>
#include <string_view>

namespace cpuopt {
// The run folder's control.ini, written by tools/run.py while the game runs:
//   toggles=<decimal or 0x hex>   runtime switches (toggles.hpp)
//   marker=<label>                scene label, [A-Za-z0-9_.-], at most 64 characters
//   window_ms=<1000..60000>       statistics window length
//   job_threads=<1..64>           threads per sMain job round while toggle job_cap
//                                 is on, the game thread included
//   job_order=<1|2|3>             toggle order_jobs: 1 reversed, 2 learned, 3 sorted
//   order_us=<1..10000>           learned order: job types at least this slow go first
//   inline_us=<1..1000>           toggle inline_rounds: call sites whose rounds take less
//                                 work than this run on the game thread alone
//   time_ms=<10..60000>           toggle server_time: how long a server time read is reused
//   unit_us=<1..100000>           toggle unit_threshold: sMhUnit's line threshold
//   cpu_count=<2..64>             toggle smaller_cpu: logical processors the game keeps
//   sample_jobs=<0..32>           toggle sample_profile: job threads sampled per tick (0 off)
//   max_fps=<60..1000>            toggle uncap: the frame cap to set
//   margin_us=<50..2000>          toggle sleep_limiter: the spin left before the limiter's deadline
//   contacts=<1..3>               toggle parallel_contacts: 1 contact pass, 2 setup pass, 3 both
//   follow=<vtable RVA>           toggles time_jobs + sample_profile: sample the thread running a job
//                                 of this class while it runs (0 off; hex with 0x)
//   follow_us=<5..1000>           follow mode: the gap between its samples
//   wc_scan=<0|1>                 toggle sample_profile: flag samples whose registers point into
//                                 write-combined memory (sampler.hpp)
// Blank lines, [sections] and lines starting with ';' or '#' are ignored. A
// malformed line rejects the whole file, so a half-written file changes nothing.
struct Control {
    std::optional<unsigned> toggles;
    std::optional<std::string> marker;
    std::optional<unsigned> window_ms;
    std::optional<unsigned> job_threads;
    std::optional<unsigned> job_order, order_us;
    std::optional<unsigned> inline_us, time_ms, unit_us, cpu_count, sample_jobs, max_fps, margin_us, follow, follow_us, contacts, wc_scan;
    bool operator==(const Control&) const = default;
};
std::optional<Control> parse_control(std::string_view text);
}  // namespace cpuopt
