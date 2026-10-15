#pragma once
#include <array>
#include <atomic>
#include <cstdint>

namespace cpuopt {
// Microsecond values in fixed buckets plus an overflow bucket. One thread
// adds, another takes a window; relaxed counters, so a value at a window edge
// lands in either window.
template <std::uint64_t BucketUs, unsigned Buckets>
class Histogram {
public:
    static constexpr std::uint64_t bucket_us = BucketUs;
    static constexpr unsigned bucket_count = Buckets;
    struct Summary {
        std::uint64_t count{}, total_us{}, max_us{};
        std::uint64_t p50_us{}, p90_us{}, p95_us{}, p99_us{}, p999_us{};
        std::uint64_t over_25ms{}, over_50ms{};  // exact counts, not from buckets
    };

    void add(std::uint64_t us) noexcept {
        const auto bucket = us / bucket_us;
        buckets_[bucket < bucket_count ? bucket : bucket_count].fetch_add(1, std::memory_order_relaxed);
        total_us_.fetch_add(us, std::memory_order_relaxed);
        if (us > 25000) over_25ms_.fetch_add(1, std::memory_order_relaxed);
        if (us > 50000) over_50ms_.fetch_add(1, std::memory_order_relaxed);
        auto seen = max_us_.load(std::memory_order_relaxed);
        while (us > seen && !max_us_.compare_exchange_weak(seen, us, std::memory_order_relaxed)) {}
    }

    // Percentiles are the upper edge of the bucket holding the rank (at most
    // one bucket high); a rank in the overflow bucket reports the maximum.
    Summary take() noexcept {
        Summary s;
        std::array<std::uint64_t, bucket_count + 1> counts{};
        for (unsigned i = 0; i <= bucket_count; ++i) s.count += counts[i] = buckets_[i].exchange(0, std::memory_order_relaxed);
        s.total_us = total_us_.exchange(0, std::memory_order_relaxed);
        s.max_us = max_us_.exchange(0, std::memory_order_relaxed);
        s.over_25ms = over_25ms_.exchange(0, std::memory_order_relaxed);
        s.over_50ms = over_50ms_.exchange(0, std::memory_order_relaxed);
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
        return s;
    }

private:
    std::array<std::atomic<std::uint64_t>, bucket_count + 1> buckets_{};
    std::atomic<std::uint64_t> total_us_{}, max_us_{}, over_25ms_{}, over_50ms_{};
};

// Frame intervals: 50 us buckets up to 100 ms.
using IntervalHistogram = Histogram<50, 2000>;
// Work inside a frame (game update, a Present call): 10 us buckets up to 20 ms.
using DurationHistogram = Histogram<10, 2000>;
}  // namespace cpuopt
