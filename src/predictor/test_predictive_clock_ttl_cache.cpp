#include "cpp_lib/cache_access.hpp"
#include "lib/predictive_clock_ttl_cache.hpp"
#include <cassert>
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
        assert(p.access(access) == 0);
    }

    assert(p.size() == 2);
    assert(p.get(1) != nullptr);
    assert(p.get(2) == nullptr);
    assert(p.get(3) != nullptr);
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
        assert(p.access(access) == 0);
    }

    assert(p.size() == 2);
    assert(p.get(1) == nullptr);
    assert(p.get(2) != nullptr);
    assert(p.get(3) != nullptr);
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
        assert(p.access(access) == 0);
    }

    // Inserting 4 skips visited 1 and 2, then evicts 3. Their bits are
    // consumed, so inserting 5 evicts 1 on the next pass.
    assert(p.size() == 3);
    assert(p.get(1) == nullptr);
    assert(p.get(2) != nullptr);
    assert(p.get(3) == nullptr);
    assert(p.get(4) != nullptr);
    assert(p.get(5) != nullptr);
    return true;
}

static bool
test_size_updates_and_protected_updated_key()
{
    PredictiveClockCache p(4, 0.0, 0.0, /*shards_sampling_ratio=*/1.0);
    assert(p.access(CacheAccess{0, 1, 1, 100}) == 0);
    assert(p.access(CacheAccess{1, 2, 3, 100}) == 0);

    // Growing key 1 requires eviction. The eviction scan must not evict the
    // object currently being updated even though it is the oldest entry.
    assert(p.access(CacheAccess{2, 1, 2, 100}) == 0);
    assert(p.size() == 2);
    assert(p.get(1) != nullptr);
    assert(p.get(1)->size_ == 2);
    assert(p.get(2) == nullptr);

    // Shrinking must not underflow any unsigned byte counters.
    assert(p.access(CacheAccess{3, 1, 1, 100}) == 0);
    assert(p.size() == 1);
    assert(p.get(1)->size_ == 1);
    return true;
}

static bool
test_infinite_ttl()
{
    PredictiveClockCache p(2, 0.0, 0.0, /*shards_sampling_ratio=*/1.0);
    assert(p.access(CacheAccess{0, 1}) == 0);
    assert(p.access(CacheAccess{1, 1}) == 0);
    assert(p.get(1) != nullptr);
    assert(p.statistics().hit_ops_ == 1);
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

        assert(p.access(CacheAccess{tm, key}) == 0);
        assert(p.size() == visited.size());
        for (uint64_t candidate = 0; candidate < 19; ++candidate) {
            assert((p.get(candidate) != nullptr) ==
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
        assert(p.access(access) == 0);
    }

    assert(p.size() == 1);
    assert(p.get(1) == nullptr);
    assert(p.get(2) != nullptr);
    return true;
}

int
main()
{
    // Keep the function calls active in release builds; assert() itself is
    // compiled out when NDEBUG is defined.
    if (!test_second_chance_single_visit()) return 1;
    if (!test_second_chance_full_sweep()) return 1;
    if (!test_multiple_visited_skips_and_one_shot_behavior()) return 1;
    if (!test_size_updates_and_protected_updated_key()) return 1;
    if (!test_infinite_ttl()) return 1;
    if (!test_clock_against_reference_model()) return 1;
    if (!test_ttl_only_expiration()) return 1;
    std::cout << "OK!" << std::endl;
    return 0;
}
