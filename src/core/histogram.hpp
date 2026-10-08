#pragma once
#include <array>
#include <atomic>
#include <cstdint>

namespace cpuopt {
// Frame intervals in 50 us buckets up to 100 ms plus an overflow bucket. One
// thread adds, another takes a window; relaxed counters, so an interval at a
// window edge lands in either window.
class IntervalHistogram {
public:
    static constexpr std::uint64_t bucket_us = 50;
    static constexpr unsigned bucket_count = 2000;
    struct Summary {
        std::uint64_t count{}, total_us{}, max_us{};
        std::uint64_t p50_us{}, p90_us{}, p95_us{}, p99_us{}, p999_us{};
        std::uint64_t over_25ms{}, over_50ms{};
    };

    void add(std::uint64_t us) noexcept {
        const auto bucket = us / bucket_us;
        buckets_[bucket < bucket_count ? bucket : bucket_count].fetch_add(1, std::memory_order_relaxed);
        total_us_.fetch_add(us, std::memory_order_relaxed);
        auto seen = max_us_.load(std::memory_order_relaxed);
        while (us > seen && !max_us_.compare_exchange_weak(seen, us, std::memory_order_relaxed)) {}
    }

    // Percentiles are the upper edge of the bucket holding the rank (at most
    // 50 us high); a rank in the overflow bucket reports the window maximum.
    Summary take() noexcept {
        Summary s;
        std::array<std::uint64_t, bucket_count + 1> counts{};
        for (unsigned i = 0; i <= bucket_count; ++i) s.count += counts[i] = buckets_[i].exchange(0, std::memory_order_relaxed);
        s.total_us = total_us_.exchange(0, std::memory_order_relaxed);
        s.max_us = max_us_.exchange(0, std::memory_order_relaxed);
        const auto rank = [&](std::uint64_t per_10000) -> std::uint64_t {
            if (!s.count) return 0;
            const auto wanted = (s.count * per_10000 + 9999) / 10000;
            std::uint64_t seen{};
            for (unsigned i = 0; i < bucket_count; ++i)
                if ((seen += counts[i]) >= wanted) return std::uint64_t(i + 1) * bucket_us;
            return s.max_us;
        };
        s.p50_us = rank(5000);
        s.p90_us = rank(9000);
        s.p95_us = rank(9500);
        s.p99_us = rank(9900);
        s.p999_us = rank(9990);
        for (unsigned i = 25000 / bucket_us; i <= bucket_count; ++i) s.over_25ms += counts[i];
        for (unsigned i = 50000 / bucket_us; i <= bucket_count; ++i) s.over_50ms += counts[i];
        return s;
    }

private:
    std::array<std::atomic<std::uint64_t>, bucket_count + 1> buckets_{};
    std::atomic<std::uint64_t> total_us_{}, max_us_{};
};
}  // namespace cpuopt
