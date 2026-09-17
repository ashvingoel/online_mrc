#pragma once

#include "cpp_lib/cache_access.hpp"
#include "cpp_lib/cache_metadata.hpp"
#include "cpp_lib/cache_statistics.hpp"
#include "cpp_lib/format_measurement.hpp"
#include "cpp_lib/util.hpp"
#include "cpp_struct/hash_list.hpp"
#include "lib/eviction_cause.hpp"
#include "lib/lifetime_thresholds.hpp"
#include "logger/logger.h"
#include "shards/fixed_rate_shards_sampler.h"
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <sstream>
#include <stdlib.h>
#include <sys/types.h>
#include <unordered_map>
#include <vector>

using size_t = std::size_t;
using uint64_t = std::uint64_t;

class LRU_TTL_Cache {
private:
    bool
    ok(bool const fatal) const
    {
        bool ok = true;
        if (size_bytes_ > capacity_bytes_) {
            LOGGER_ERROR("size exceeds capacity");
            ok = false;
        }
        if (map_.size() != lfu_cache_.size()) {
            // NOTE Because of the prediction, we can have fewer items in
            //      the LRU queue than in the cache.
            LOGGER_ERROR("mismatching map (%zu) vs LRU (%zu) size",
                         map_.size(),
                         lfu_cache_.size());
            ok = false;
        }
        if (map_.size() != ttl_queue_.size()) {
            // NOTE Because of the prediction, we can have fewer items in
            //      the TTL queue than in the cache.
            LOGGER_ERROR("mismatching map (%zu) vs TTL (%zu) size",
                         map_.size(),
                         ttl_queue_.size());
            ok = false;
        }
        if (map_.size() != 0 && size_bytes_ == 0) {
            // NOTE It's possible (but unlikely) that the cache is filled
            //      with zero-byte objects, so the number of objects is
            //      non-zero but the size of the cache is zero. That's why I
            //      don't error on this case explicitly.
            LOGGER_WARN("all zero-sized objects in cache");
            // TODO - make this not an error.
            ok = false;
        }
        if (map_.size() == 0 && size_bytes_ != 0) {
            LOGGER_ERROR("zero objects but non-zero cache size");
            ok = false;
        }

        if (fatal && !ok) {
            // The assertion outputs a convenient error message.
            assert(ok && "FATAL: not OK!");
            std::exit(1);
        }
        return ok;
    }

    void
    insert(CacheAccess const &access)
    {
        statistics_.insert(access.value_size_b);
        map_.emplace(access.key, CacheMetadata{access});
        lfu_cache_.access(access.key);
        ttl_queue_.emplace(access.expiration_time_ms(), access.key);
        size_bytes_ += access.value_size_b;
    }

    void
    update(CacheAccess const &access, CacheMetadata &metadata)
    {
        size_bytes_ += access.value_size_b - metadata.size_;
        statistics_.update(metadata.size_, access.value_size_b);
        metadata.visit_without_ttl_refresh(access);
        lfu_cache_.access(access.key);
    }

    /// @brief  Evict an object in the cache (either due to the eviction
    ///         policy or TTL expiration).
    void
    remove(uint64_t const victim_key,
           EvictionCause const cause,
           CacheAccess const *const current_access)
    {
        CacheMetadata &m = map_.at(victim_key);
        uint64_t sz_bytes = m.size_;
        double const exp_tm = m.expiration_time_ms_;

        // Update metadata tracking
        switch (cause) {
        case EvictionCause::MainCapacity:
            assert(current_access != NULL);
            statistics_.lru_evict(m.size_, 0);
            break;
        case EvictionCause::ProactiveTTL:
            assert(current_access == NULL);
            statistics_.ttl_expire(m.size_);
            break;
        case EvictionCause::NoRoom:
            assert(current_access != NULL);
            statistics_.no_room_evict(m.size_, 0);
            break;
        default:
            assert(0 && "impossible");
        }

        size_bytes_ -= sz_bytes;
        lfu_cache_.remove(victim_key);
        if (cause == EvictionCause::MainCapacity) {
            lifetime_thresholds_.register_cache_eviction(
                current_access->timestamp_ms - m.last_access_time_ms_,
                m.size_,
                current_access->timestamp_ms);
        }
        remove_multimap_kv(ttl_queue_, exp_tm, victim_key);
        map_.erase(victim_key);
    }

    void
    evict_expired_objects(uint64_t const current_time_ms)
    {
        std::vector<uint64_t> victims;
        for (auto [exp_tm, key] : ttl_queue_) {
            if (exp_tm >= current_time_ms) {
                break;
            }
            victims.push_back(key);
        }
        // One cannot erase elements from a multimap while also iterating!
        for (auto victim : victims) {
            remove(victim, EvictionCause::ProactiveTTL, nullptr);
        }
    }

    /// @return number of bytes evicted.
    uint64_t
    evict_from_lru(uint64_t const target_bytes, CacheAccess const &access)
    {
        uint64_t const ignored_key = access.key;
        uint64_t evicted_bytes = 0;
        std::vector<uint64_t> victims;
        for (auto n : lfu_cache_) {
            assert(n);
            if (evicted_bytes >= target_bytes) {
                break;
            }
            if (n->key == ignored_key) {
                continue;
            }
            auto &m = map_.at(n->key);
            evicted_bytes += m.size_;
            victims.push_back(n->key);
        }
        // One cannot evict elements from the map one is iterating over.
        for (auto v : victims) {
            remove(v, EvictionCause::MainCapacity, &access);
        }
        return evicted_bytes;
    }

    bool
    ensure_enough_room(size_t const old_nbytes, CacheAccess const &access)
    {
        size_t const new_nbytes = access.key_size_b + access.value_size_b;
        assert(size_bytes_ <= capacity_bytes_);
        // We already have enough room if we're not increasing the data.
        if (old_nbytes >= new_nbytes) {
            assert(size_bytes_ <= capacity_bytes_);
            return true;
        }
        size_t const nbytes = new_nbytes - old_nbytes;
        // We can't possibly fit the new object into the cache!
        // A side-effect is that in this case, we don't flush our cache
        // for no reason.
        if (new_nbytes > capacity_bytes_) {
            if (DEBUG) {
                LOGGER_WARN("not enough capacity (%zu) for object (%zu)",
                            capacity_bytes_,
                            nbytes);
            }
            return false;
        }
        // Check that the required bytes to free is greater than zero.
        if (nbytes <= capacity_bytes_ - size_bytes_) {
            return true;
        }
        uint64_t required_bytes = nbytes - (capacity_bytes_ - size_bytes_);
        uint64_t const evicted_bytes = evict_from_lru(required_bytes, access);
        if (evicted_bytes >= required_bytes) {
            return true;
        }
        LOGGER_WARN("could not evict enough from cache: required %zu "
                    "vs %zu -- %zu items left in cache with size %zu",
                    required_bytes,
                    evicted_bytes,
                    map_.size(),
                    size_bytes_);
        return false;
    }

    void
    evict_too_big_accessed_object(CacheAccess const &access)
    {
        remove(access.key, EvictionCause::NoRoom, &access);
    }

    void
    hit(CacheAccess const &access)
    {
        auto &metadata = map_.at(access.key);
        if (!ensure_enough_room(metadata.size_, access)) {
            statistics_.skip(access.value_size_b);
            evict_too_big_accessed_object(access);
            if (DEBUG) {
                LOGGER_WARN("too big updated object");
            }
            return;
        }
        update(access, metadata);
    }

    bool
    miss(CacheAccess const &access)
    {
        if (!ensure_enough_room(0, access)) {
            if (DEBUG) {
                LOGGER_WARN("not enough room to insert!");
            }
            statistics_.skip(access.value_size_b);
            return false;
        }
        insert(access);
        return true;
    }

public:
    /// @param  capacity: size_t - The capacity of the cache in bytes.
    LRU_TTL_Cache(size_t const capacity, double const shards_sampling_ratio)
        : capacity_bytes_(capacity),
          shards_{shards_sampling_ratio, true},
          lifetime_thresholds_(0.0, 1.0)
    {
    }

    void
    start_simulation()
    {
        statistics_.start_simulation();
    }

    void
    end_simulation()
    {
        statistics_.end_simulation();
    }

    int
    access(CacheAccess const &access)
    {
        ok(true);
        assert(size_bytes_ == statistics_.size_);
        statistics_.time(access.timestamp_ms);
        assert(size_bytes_ == statistics_.size_);
        evict_expired_objects(access.timestamp_ms);
        if (map_.count(access.key)) {
            hit(access);
        } else {
            if (!miss(access)) {
                if (DEBUG) {
                    LOGGER_WARN("cannot handle miss");
                }
                return -1;
            }
        }
        return 0;
    }

    size_t
    size() const
    {
        return size_bytes_;
    }

    size_t
    capacity() const
    {
        return capacity_bytes_;
    }

    CacheMetadata const *
    get(uint64_t const key)
    {
        if (map_.count(key)) {
            return &map_.at(key);
        }
        return nullptr;
    }

    CacheStatistics const &
    statistics() const
    {
        return statistics_;
    }

    void
    print() const
    {
        std::cout << "> LRUTTLCache(sz: " << size_bytes_
                  << ", cap: " << capacity_bytes_ << ")\n";
        std::cout << "> \tLRU: ";
        for (auto n : lfu_cache_) {
            std::cout << n->key << ", ";
        }
        std::cout << "\n";
        std::cout << "> \tTTL: ";
        for (auto [tm, key] : ttl_queue_) {
            std::cout << key << "@" << tm << ", ";
        }
        std::cout << "\n";
    }

    std::string
    json() const
    {
        std::stringstream ss;
        ss << "{\"Capacity [B]\": " << format_memory_size(capacity_bytes_)
           << ", \"Max Size [B]\": "
           << format_memory_size(statistics_.max_size_)
           << ", \"Max Resident Objects\": "
           << format_engineering(statistics_.max_resident_objs_)
           << ", \"Uptime [ms]\": " << format_time(statistics_.uptime_ms())
           << ", \"Number of Insertions\": "
           << format_engineering(statistics_.insert_ops_)
           << ", \"Number of Updates\": "
           << format_engineering(statistics_.update_ops_)
           << ", \"Miss Ratio\": " << statistics_.miss_ratio()
           << ", \"Lifetime Thresholds\": " << lifetime_thresholds_.json()
           << ", \"Statistics\": " << statistics_.json() << "}";
        return ss.str();
    }

private:
    static constexpr bool DEBUG = false;

    // Maximum number of bytes in the cache.
    size_t const capacity_bytes_;
    FixedRateShardsSampler shards_;

    // Number of bytes in the current cache.
    size_t size_bytes_ = 0;

    // Maps key to [last access time, expiration time]
    std::unordered_map<uint64_t, CacheMetadata> map_;
    // Maps last access time to keys.
    HashList lfu_cache_;
    // Maps expiration time to keys.
    std::multimap<double, uint64_t> ttl_queue_;

    // Statistics related to cache performance.
    CacheStatistics statistics_;

    LifeTimeThresholds lifetime_thresholds_;
};
