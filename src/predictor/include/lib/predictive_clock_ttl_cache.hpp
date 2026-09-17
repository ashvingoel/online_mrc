#pragma once

#include "accurate/lru_ttl_cache.hpp"
#include "cpp_lib/cache_access.hpp"
#include "cpp_lib/cache_predictive_metadata.hpp"
#include "cpp_lib/cache_statistics.hpp"
#include "cpp_lib/remaining_lifetime.hpp"
#include "cpp_struct/hash_list.hpp"
#include "lib/eviction_cause.hpp"
#include "lib/lifetime_thresholds.hpp"
#include "lib/prediction_tracker.hpp"
#include "lib/removal_policy_statistics.hpp"
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <ostream>
#include <stdlib.h>
#include <string>
#include <sys/types.h>
#include <unordered_map>

using size_t = std::size_t;
using uint64_t = std::uint64_t;

class PredictiveClockCache {
private:
    bool
    ok(bool const fatal = false) const;

    void
    insert(CacheAccess const &access);

    void
    update(CacheAccess const &access, CachePredictiveMetadata &metadata);

    void
    repredict(CacheAccess const &access,
              uint64_t const key,
              CachePredictiveMetadata &metadata,
              bool const move_clock_to_back);

    void
    remove_clock(uint64_t const victim_key,
                 CachePredictiveMetadata const &m,
                 CacheAccess const *const current_access,
                 EvictionCause const cause);

    void
    remove(uint64_t const victim_key,
           EvictionCause const cause,
           CacheAccess const *const current_access);

    void
    evict_expired_objects(uint64_t const current_time_ms);

    uint64_t
    evict_from_clock(uint64_t const target_bytes, CacheAccess const &access);

    uint64_t
    evict_smallest_ttl(uint64_t const target_bytes, CacheAccess const &access);

    bool
    ensure_enough_room(size_t const old_nbytes, CacheAccess const &access);

    void
    evict_expired_accessed_object(CacheAccess const &access);

    void
    evict_too_big_accessed_object(CacheAccess const &access);

    bool
    is_expired(CacheAccess const &access,
               CachePredictiveMetadata const &metadata);

    void
    hit(CacheAccess const &access);

    bool
    miss(CacheAccess const &access);

public:
    PredictiveClockCache(size_t const capacity,
                         double const lower_ratio,
                         double const upper_ratio,
                         double const shards_sampling_ratio,
                         std::map<std::string, std::string> kwargs = {});

    void
    start_simulation();

    void
    end_simulation();

    int
    access(CacheAccess const &access);

    size_t
    size() const;

    size_t
    capacity() const;

    CachePredictiveMetadata const *
    get(uint64_t const key);

    std::string
    record_remaining_lifetime(CacheAccess const &access) const
    {
        RemainingLifetime rl{clock_cache_, map_, access.timestamp_ms, 1000};
        return rl.json();
    }

    void
    print();

    PredictionTracker const &
    predictor() const;

    CacheStatistics const &
    statistics() const;

    std::string
    json(std::map<std::string, std::string> extras) const;

    void
    print_json(std::ostream &ostrm = std::cout,
               std::map<std::string, std::string> extras = {}) const;

private:
    static constexpr bool DEBUG = false;

    size_t const capacity_;

    size_t size_ = 0;
    size_t clock_size_ = 0;
    size_t ttl_size_ = 0;

    PredictionTracker pred_tracker;
    CacheStatistics statistics_;
    RemovalPolicyStatistics rm_policy_statistics_;

    std::unordered_map<uint64_t, CachePredictiveMetadata> map_;
    HashList clock_cache_;
    std::multimap<double, uint64_t> ttl_cache_;

    LifeTimeThresholds lifetime_thresholds_;
    LRU_TTL_Cache oracle_;

    std::map<std::string, std::string> const kwargs_;
};
