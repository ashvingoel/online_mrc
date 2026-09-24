#include "cpp_lib/cache_access.hpp"
#include "lib/predictive_clock_ttl_cache.hpp"
#include <glib.h>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <iostream>
#include <unordered_map>
#include <sys/types.h>

using size_t = std::size_t;
using uint64_t = std::uint64_t;

static bool
test_second_chance_single_visit()
{
    PredictiveClockCache p(2, 0.0, 0.0, /*shards_sampling_ratio=*/1.0);
    CacheAccess accesses[] = {CacheAccess{0, 1, 1, 100},
                              CacheAccess{1, 2, 1, 100},
                              CacheAccess{2, 1, 1, 100},
                              CacheAccess{3, 3, 1, 100}};

    for (auto const &access : accesses) {
        g_assert_true(p.access(access) == 0);
    }

    g_assert_true(p.size() == 2);
    g_assert_true(p.get(1) != nullptr);
    g_assert_true(p.get(2) == nullptr);
    g_assert_true(p.get(3) != nullptr);
    return true;
}

static bool
test_second_chance_full_sweep()
{
    PredictiveClockCache p(2, 0.0, 0.0, /*shards_sampling_ratio=*/1.0);
    CacheAccess accesses[] = {CacheAccess{0, 1, 1, 100},
                              CacheAccess{1, 2, 1, 100},
                              CacheAccess{2, 1, 1, 100},
                              CacheAccess{3, 2, 1, 100},
                              CacheAccess{4, 3, 1, 100}};

    for (auto const &access : accesses) {
        g_assert_true(p.access(access) == 0);
    }

    g_assert_true(p.size() == 2);
    g_assert_true(p.get(1) == nullptr);
    g_assert_true(p.get(2) != nullptr);
    g_assert_true(p.get(3) != nullptr);
    return true;
}

static bool
test_multiple_visited_skips_and_one_shot_behavior()
{
    PredictiveClockCache p(3, 0.0, 0.0, /*shards_sampling_ratio=*/1.0);
    CacheAccess accesses[] = {CacheAccess{0, 1, 1, 100},
                              CacheAccess{1, 2, 1, 100},
                              CacheAccess{2, 3, 1, 100},
                              CacheAccess{3, 1, 1, 100},
                              CacheAccess{4, 2, 1, 100},
                              CacheAccess{5, 4, 1, 100},
                              CacheAccess{6, 5, 1, 100}};

    for (auto const &access : accesses) {
        g_assert_true(p.access(access) == 0);
    }

    // Inserting 4 skips visited 1 and 2, then evicts 3. Their bits are
    // consumed, so inserting 5 evicts 1 on the next pass.
    g_assert_true(p.size() == 3);
    g_assert_true(p.get(1) == nullptr);
    g_assert_true(p.get(2) != nullptr);
    g_assert_true(p.get(3) == nullptr);
    g_assert_true(p.get(4) != nullptr);
    g_assert_true(p.get(5) != nullptr);
    return true;
}

static bool
test_size_updates_and_protected_updated_key()
{
    PredictiveClockCache p(4, 0.0, 0.0, /*shards_sampling_ratio=*/1.0);
    g_assert_true(p.access(CacheAccess{0, 1, 1, 100}) == 0);
    g_assert_true(p.access(CacheAccess{1, 2, 3, 100}) == 0);

    // Growing key 1 requires eviction. The eviction scan must not evict the
    // object currently being updated even though it is the oldest entry.
    g_assert_true(p.access(CacheAccess{2, 1, 2, 100}) == 0);
    g_assert_true(p.size() == 2);
    g_assert_true(p.get(1) != nullptr);
    g_assert_true(p.get(1)->size_ == 2);
    g_assert_true(p.get(2) == nullptr);

    // Shrinking must not underflow any unsigned byte counters.
    g_assert_true(p.access(CacheAccess{3, 1, 1, 100}) == 0);
    g_assert_true(p.size() == 1);
    g_assert_true(p.get(1)->size_ == 1);
    return true;
}

static bool
test_infinite_ttl()
{
    PredictiveClockCache p(2, 0.0, 0.0, /*shards_sampling_ratio=*/1.0);
    g_assert_true(p.access(CacheAccess{0, 1}) == 0);
    g_assert_true(p.access(CacheAccess{1, 1}) == 0);
    g_assert_true(p.get(1) != nullptr);
    g_assert_true(p.statistics().hit_ops_ == 1);
    return true;
}

static bool
test_clock_against_reference_model()
{
    static constexpr size_t capacity = 7;
    PredictiveClockCache p(capacity,
                           0.0,
                           0.0,
                           /*shards_sampling_ratio=*/1.0);
    std::deque<uint64_t> queue;
    std::unordered_map<uint64_t, bool> visited;
    uint64_t state = 0x9e3779b97f4a7c15ULL;

    for (uint64_t tm = 0; tm < 2000; ++tm) {
        state = state * 6364136223846793005ULL + 1;
        uint64_t const key = (state >> 32) % 19;

        auto found = visited.find(key);
        if (found != visited.end()) {
            found->second = true;
        } else {
            while (queue.size() >= capacity) {
                uint64_t const victim = queue.front();
                queue.pop_front();
                if (visited.at(victim)) {
                    visited.at(victim) = false;
                    queue.push_back(victim);
                } else {
                    visited.erase(victim);
                    break;
                }
            }
            queue.push_back(key);
            visited.emplace(key, false);
        }

        g_assert_true(p.access(CacheAccess{tm, key}) == 0);
        g_assert_true(p.size() == visited.size());
        for (uint64_t candidate = 0; candidate < 19; ++candidate) {
            g_assert_true((p.get(candidate) != nullptr) ==
                   visited.contains(candidate));
        }
    }
    return true;
}

static bool
test_ttl_only_expiration()
{
    PredictiveClockCache p(2, 1.0, 1.0, /*shards_sampling_ratio=*/1.0);
    CacheAccess accesses[] = {CacheAccess{0, 1, 1, 1},
                              CacheAccess{1002, 2, 1, 100}};

    for (auto const &access : accesses) {
        g_assert_true(p.access(access) == 0);
    }

    g_assert_true(p.size() == 1);
    g_assert_true(p.get(1) == nullptr);
    g_assert_true(p.get(2) != nullptr);
    return true;
}

static bool
test_second_chance_preserves_queue_residence()
{
    for (double upper_ratio : {0.0, 1.0}) {
        PredictiveClockCache p(2, 0.0, upper_ratio, 1.0);
        g_assert_true(p.access(CacheAccess{0, 1}) == 0);
        g_assert_true(p.access(CacheAccess{1, 2}) == 0);
        g_assert_true(p.access(CacheAccess{2, 1}) == 0);
        g_assert_true(p.access(CacheAccess{3, 2}) == 0);
        g_assert_true(p.access(CacheAccess{100, 3}) == 0);
        g_assert_null(p.get(1));
        g_assert_nonnull(p.get(2));
        g_assert_cmpuint(p.get(2)->insertion_time_ms_, ==, 1);
        g_assert_cmpuint(p.get(2)->last_access_time_ms_, ==, 3);
        g_assert_false(p.get(2)->visited);
    }
    return true;
}

static bool
test_learned_threshold_after_second_chance()
{
    constexpr uint64_t hour = 3600000;
    PredictiveClockCache p(2, 0.5, 0.5, 1.0);
    g_assert_true(p.access(CacheAccess{0, 1}) == 0);
    g_assert_true(p.access(CacheAccess{1, 2}) == 0);
    g_assert_true(p.access(CacheAccess{100, 3}) == 0);
    g_assert_true(p.access(CacheAccess{hour, 2}) == 0);
    g_assert_true(p.access(CacheAccess{hour + 1, 3}) == 0);
    g_assert_true(p.get(2)->uses_lru_only());
    g_assert_true(p.get(3)->uses_lru_only());
    g_assert_true(p.access(CacheAccess{hour + 100, 4}) == 0);
    g_assert_null(p.get(2));
    g_assert_nonnull(p.get(3));
    g_assert_true(p.access(CacheAccess{2 * hour, 3}) == 0);
    CacheAccess short_lived{2 * hour + 1, 5};
    short_lived.ttl_ms = 75;
    g_assert_true(p.access(short_lived) == 0);
    g_assert_nonnull(p.get(5));
    g_assert_true(p.get(5)->uses_ttl_only());
    CacheAccess long_lived{2 * hour + 2, 6};
    long_lived.ttl_ms = 125;
    g_assert_true(p.access(long_lived) == 0);
    g_assert_nonnull(p.get(6));
    g_assert_true(p.get(6)->uses_lru_only());
    g_assert_true(p.access(CacheAccess{2 * hour + 77, 6}) == 0);
    g_assert_null(p.get(5));
    g_assert_nonnull(p.get(6));
    g_assert_cmpuint(p.size(), ==, 1);
    return true;
}

int
main()
{
    // Keep the function calls active in release builds; assert() itself is
    // compiled out when NDEBUG is defined.
    g_unsetenv("PSYCHE_REFRESH_PERIOD_MS");
    g_unsetenv("PSYCHE_THRESHOLD_DECAY");
    if (!test_second_chance_preserves_queue_residence()) return 1;
    if (!test_second_chance_single_visit()) return 1;
    if (!test_second_chance_full_sweep()) return 1;
    if (!test_multiple_visited_skips_and_one_shot_behavior()) return 1;
    if (!test_size_updates_and_protected_updated_key()) return 1;
    if (!test_infinite_ttl()) return 1;
    if (!test_clock_against_reference_model()) return 1;
    if (!test_learned_threshold_after_second_chance()) return 1;
    if (!test_ttl_only_expiration()) return 1;
    std::cout << "OK!" << std::endl;
    return 0;
}
