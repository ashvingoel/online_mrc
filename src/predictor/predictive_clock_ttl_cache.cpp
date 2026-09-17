#include "lib/predictive_clock_ttl_cache.hpp"
#include "accurate/lru_ttl_cache.hpp"
#include "cpp_lib/cache_access.hpp"
#include "cpp_lib/cache_predictive_metadata.hpp"
#include "cpp_lib/cache_statistics.hpp"
#include "cpp_lib/duration.hpp"
#include "cpp_lib/format_measurement.hpp"
#include "cpp_lib/util.hpp"
#include "cpp_struct/hash_list.hpp"
#include "lib/eviction_cause.hpp"
#include "lib/lifetime_thresholds.hpp"
#include "lib/prediction_tracker.hpp"
#include "logger/logger.h"
#include "unused/mark_unused.h"

#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <glib.h>
#include <iostream>
#include <map>
#include <ostream>
#include <stdlib.h>
#include <string>
#include <sys/types.h>
#include <unordered_map>
#include <utility>
#include <vector>

using size_t = std::size_t;
using uint64_t = std::uint64_t;

/// @brief  Return whether an object is expired at `current_time`.
///
///         An object is still valid exactly at its expiration time. It
///         becomes expired only after that time.
static inline bool
object_is_expired(double const expiration_time, uint64_t const current_time)
{
    return current_time > expiration_time;
}

/// @brief  Read an unsigned integer from the environment.
///
///         This is used for experiment knobs. If the variable is not set,
///         the caller-provided default is used.
static uint64_t
get_env_u64(char const *const name, uint64_t const default_value)
{
    char const *const value = std::getenv(name);
    return value == nullptr ? default_value : std::strtoull(value, nullptr, 10);
}

/// @brief  Read a floating-point value from the environment.
///
///         This is used for experiment knobs. If the variable is not set,
///         the caller-provided default is used.
static double
get_env_double(char const *const name, double const default_value)
{
    char const *const value = std::getenv(name);
    return value == nullptr ? default_value : std::atof(value);
}

/// @brief  Check the cache internal bookkeeping.
///
///         This cache has one master map plus two policy queues: Clock
///         and TTL. This method checks that queue entries point to real
///         cached objects and that byte counters still make sense.
bool
PredictiveClockCache::ok(bool const fatal) const
{
    bool ok = true;
    if (size_ > capacity_) {
        LOGGER_ERROR("size exceeds capacity");
        ok = false;
    }
    if (map_.size() < clock_cache_.size()) {
        LOGGER_ERROR("mismatching map vs Clock size");
        ok = false;
    }
    if (map_.size() < ttl_cache_.size()) {
        LOGGER_ERROR("mismatching map vs TTL size");
        ok = false;
    }
    if (map_.size() != 0 && size_ == 0) {
        LOGGER_WARN("all zero-sized objects in cache");
        ok = false;
    }
    if (map_.size() == 0 && size_ != 0) {
        LOGGER_ERROR("zero objects but non-zero cache size");
        ok = false;
    }
    if (clock_size_ > size_ || ttl_size_ > size_) {
        LOGGER_ERROR(
            "Clock (%zu) or TTL (%zu) size larger than overall size (%zu)",
            clock_size_,
            ttl_size_,
            size_);
        ok = false;
    }
    if (DEBUG) {
        for (auto const n : clock_cache_) {
            if (!map_.contains(n->key) || !map_.at(n->key).uses_lru()) {
                LOGGER_ERROR("Clock queue contains stale key %zu", n->key);
                ok = false;
            }
        }
        for (auto const &[tm, key] : ttl_cache_) {
            UNUSED(tm);
            if (!map_.contains(key) || !map_.at(key).uses_ttl()) {
                LOGGER_ERROR("TTL queue contains stale key %zu", key);
                ok = false;
            }
        }
    }

    if (fatal && !ok) {
        assert(ok && "FATAL: not OK!");
        std::exit(1);
    }
    return ok;
}

/// @brief  Decide whether an object belongs in Clock, TTL, or both.
///
///         This is the Psyche part of the cache. It compares the object
///         remaining TTL with learned eviction-time thresholds. Long TTLs
///         are useful to track in Clock; short TTLs are useful to track
///         in the TTL queue.
void
PredictiveClockCache::repredict(CacheAccess const &access,
                                uint64_t const key,
                                CachePredictiveMetadata &metadata,
                                bool const move_clock_to_back)
{
    double const ttl_ms = metadata.ttl_ms(access.timestamp_ms);
    auto [lo_t, hi_t, updated] =
        lifetime_thresholds_.get_updated_thresholds(access.timestamp_ms);
    UNUSED(updated);

    // The learned thresholds classify the object by remaining TTL.
    // Long-lived objects should usually be tracked by the main eviction
    // policy, while short-lived objects should usually be tracked by TTL.
    bool want_clock = !std::isinf(lo_t) && ttl_ms >= lo_t;
    bool want_ttl = hi_t != 0.0 && ttl_ms <= hi_t;
    if (!want_clock && !want_ttl) {
        // Do not leave the object untracked. If prediction is uncertain,
        // default to Clock so capacity eviction can still find it.
        want_clock = true;
    }

    if (want_clock) {
        pred_tracker.record_store_lru();
        if (!metadata.uses_lru()) {
            clock_size_ += metadata.size_;
            metadata.set_lru();
            metadata.insertion_time_ms_ = access.timestamp_ms;
            clock_cache_.access(key);
        } else if (move_clock_to_back) {
            metadata.insertion_time_ms_ = access.timestamp_ms;
            clock_cache_.access(key);
        }
    } else if (metadata.uses_lru()) {
        clock_cache_.remove(key);
        clock_size_ -= metadata.size_;
        metadata.unset_lru();
    }

    if (want_ttl) {
        pred_tracker.record_store_ttl();
        if (!metadata.uses_ttl()) {
            ttl_cache_.emplace(metadata.expiration_time_ms_, key);
            ttl_size_ += metadata.size_;
            metadata.set_ttl();
        }
    } else if (metadata.uses_ttl()) {
        remove_multimap_kv(ttl_cache_, metadata.expiration_time_ms_, key);
        ttl_size_ -= metadata.size_;
        metadata.unset_ttl();
    }
}

/// @brief  Insert a newly missed object into the cache.
///
///         The object first goes into `map_`, which owns the metadata.
///         Then `repredict()` chooses whether Clock, TTL, or both should
///         track it.
void
PredictiveClockCache::insert(CacheAccess const &access)
{
    statistics_.insert(access.size_bytes());
    map_.emplace(access.key, CachePredictiveMetadata{access});
    auto &metadata = map_.at(access.key);
    metadata.size_ = access.size_bytes();
    repredict(access, access.key, metadata, true);
    size_ += access.size_bytes();
}

/// @brief  Update metadata for a cache hit.
///
///         A hit marks a Clock-tracked object as `visited`, which is the
///         Second-Chance bit. The object is not moved in the Clock queue
///         until eviction reaches it.
void
PredictiveClockCache::update(CacheAccess const &access,
                             CachePredictiveMetadata &metadata)
{
    size_t const old_size = metadata.size_;
    size_t const new_size = access.size_bytes();
    bool const was_clock = metadata.uses_lru();
    bool const was_ttl = metadata.uses_ttl();
    if (new_size >= old_size) {
        size_ += new_size - old_size;
    } else {
        size_ -= old_size - new_size;
    }
    if (was_clock) {
        clock_size_ -= old_size;
    }
    if (was_ttl) {
        ttl_size_ -= old_size;
    }
    statistics_.update(old_size, new_size);
    metadata.visit_without_ttl_refresh(access);
    metadata.size_ = new_size;
    if (was_clock) {
        clock_size_ += new_size;
    }
    if (was_ttl) {
        ttl_size_ += new_size;
    }

    if (metadata.uses_lru()) {
        metadata.visited = true;
    }

    repredict(access, access.key, metadata, false);
}

/// @brief  Remove an object from the Clock queue.
///
///         For capacity evictions, this also records how long the object
///         lived in Clock. Psyche uses those samples to learn future
///         eviction-time thresholds.
void
PredictiveClockCache::remove_clock(uint64_t const victim_key,
                                   CachePredictiveMetadata const &m,
                                   CacheAccess const *const current_access,
                                   EvictionCause const cause)
{
    clock_cache_.remove(victim_key);
    clock_size_ -= m.size_;
    if (cause == EvictionCause::MainCapacity) {
        lifetime_thresholds_.register_cache_eviction(
            current_access->timestamp_ms - m.insertion_time_ms_,
            m.size_,
            current_access->timestamp_ms);
    }
}

/// @brief  Remove an object from every structure that currently tracks it.
///
///         This is the central deletion path. It updates statistics,
///         removes from Clock and/or TTL, then erases from the master map.
void
PredictiveClockCache::remove(uint64_t const victim_key,
                             EvictionCause const cause,
                             CacheAccess const *const current_access)
{
    ok(true);
    CachePredictiveMetadata &m = map_.at(victim_key);
    uint64_t const sz_bytes = m.size_;
    double const exp_tm = m.expiration_time_ms_;

    switch (cause) {
    case EvictionCause::MainCapacity:
        assert(current_access != NULL);
        statistics_.lru_evict(m.size_, m.ttl_ms(current_access->timestamp_ms));
        if (statistics_.current_time_ms_ <= exp_tm) {
            pred_tracker.update_correctly_evicted(sz_bytes);
        } else {
            pred_tracker.update_wrongly_evicted(sz_bytes);
        }
        break;
    case EvictionCause::ProactiveTTL:
        statistics_.ttl_expire(m.size_);
        if (oracle_.get(victim_key)) {
            pred_tracker.update_correctly_expired(sz_bytes);
        } else {
            pred_tracker.update_wrongly_expired(sz_bytes);
        }
        break;
    case EvictionCause::VolatileTTL:
        assert(current_access != NULL);
        statistics_.ttl_evict(m.size_, m.ttl_ms(current_access->timestamp_ms));
        pred_tracker.update_wrongly_expired(sz_bytes);
        break;
    case EvictionCause::AccessExpired:
        assert(current_access != NULL);
        statistics_.lazy_expire(m.size_,
                                m.ttl_ms(current_access->timestamp_ms));
        pred_tracker.update_wrongly_evicted(sz_bytes);
        break;
    case EvictionCause::NoRoom:
        assert(current_access != NULL);
        statistics_.no_room_evict(m.size_,
                                  m.ttl_ms(current_access->timestamp_ms));
        pred_tracker.update_correctly_evicted(sz_bytes);
        break;
    case EvictionCause::Sampling:
        statistics_.sampling_remove(m.size_);
        break;
    default:
        assert(0 && "impossible");
    }

    size_ -= sz_bytes;
    if (m.uses_lru()) {
        remove_clock(victim_key, m, current_access, cause);
    }
    if (m.uses_ttl()) {
        if (cause == EvictionCause::VolatileTTL) {
            lifetime_thresholds_.register_cache_eviction(
                current_access->timestamp_ms - m.insertion_time_ms_,
                m.size_,
                current_access->timestamp_ms);
        }
        remove_multimap_kv(ttl_cache_, exp_tm, victim_key);
        ttl_size_ -= m.size_;
    }
    map_.erase(victim_key);
}

/// @brief  Proactively remove expired objects from the TTL queue.
///
///         The TTL queue is sorted by expiration time, so the scan can
///         stop as soon as it sees the first unexpired object.
void
PredictiveClockCache::evict_expired_objects(uint64_t const current_time_ms)
{
    std::vector<uint64_t> victims;
    for (auto [exp_tm, key] : ttl_cache_) {
        if (!object_is_expired(exp_tm, current_time_ms)) {
            break;
        }
        victims.push_back(key);
    }
    for (auto victim : victims) {
        remove(victim, EvictionCause::ProactiveTTL, nullptr);
    }
}

/// @brief  Evict enough bytes using Clock / Second-Chance.
///
///         This is where the actual Second-Chance behavior lives. The
///         oldest Clock object is considered first. If it was visited,
///         it gets one more chance; otherwise it is evicted.
uint64_t
PredictiveClockCache::evict_from_clock(uint64_t const target_bytes,
                                       CacheAccess const &access)
{
    ok(true);
    uint64_t const ignored_key = access.key;
    uint64_t evicted_bytes = 0;

    while (evicted_bytes < target_bytes && clock_cache_.size() > 0) {
        auto victim = clock_cache_.front();
        if (!victim.has_value()) {
            break;
        }
        uint64_t const victim_key = victim.value();
        if (victim_key == ignored_key) {
            if (clock_cache_.size() == 1) {
                break;
            }
            clock_cache_.access(victim_key);
            continue;
        }

        auto &metadata = map_.at(victim_key);
        if (metadata.visited) {
            // This is the second chance: the object was accessed since
            // its last pass through the queue, so clear the bit and put
            // it back at the newest end if Psyche still wants it in Clock.
            metadata.unvisit();
            repredict(access, victim_key, metadata, true);
            continue;
        }

        // No visited bit means no second chance; evict this object.
        evicted_bytes += metadata.size_;
        remove(victim_key, EvictionCause::MainCapacity, &access);
    }

    return evicted_bytes;
}

/// @brief  Fallback eviction from the TTL queue.
///
///         Psyche may place many objects only in TTL. If Clock cannot
///         free enough space, this evicts soonest-expiring objects even
///         if they have not expired yet.
uint64_t
PredictiveClockCache::evict_smallest_ttl(uint64_t const target_bytes,
                                         CacheAccess const &access)
{
    uint64_t const ignored_key = access.key;
    uint64_t evicted_bytes = 0;
    std::vector<uint64_t> victims;
    for (auto [tm, key] : ttl_cache_) {
        UNUSED(tm);
        if (evicted_bytes >= target_bytes) {
            break;
        }
        if (key == ignored_key) {
            continue;
        }
        auto &m = map_.at(key);
        evicted_bytes += m.size_;
        victims.push_back(key);
    }
    for (auto v : victims) {
        remove(v, EvictionCause::VolatileTTL, &access);
    }
    return evicted_bytes;
}

/// @brief  Make sure there is enough capacity for an insert or update.
///
///         This tries Clock eviction first. If that is not enough, it
///         falls back to TTL eviction. Returning false means the object
///         is too large or the cache could not free enough space.
bool
PredictiveClockCache::ensure_enough_room(size_t const old_nbytes,
                                         CacheAccess const &access)
{
    size_t const new_nbytes = access.size_bytes();
    assert(size_ <= capacity_);
    if (old_nbytes >= new_nbytes) {
        return true;
    }
    size_t const nbytes = new_nbytes - old_nbytes;
    if (new_nbytes > capacity_) {
        if (DEBUG) {
            LOGGER_WARN("not enough capacity (%zu) for object (%zu)",
                        capacity_,
                        nbytes);
        }
        return false;
    }
    if (nbytes <= capacity_ - size_) {
        return true;
    }
    uint64_t const reqd_b = nbytes - (capacity_ - size_);
    uint64_t const clock_evicted_b = evict_from_clock(reqd_b, access);
    if (clock_evicted_b >= reqd_b) {
        return true;
    }
    uint64_t const ttl_evicted_b =
        evict_smallest_ttl(reqd_b - clock_evicted_b, access);
    if (clock_evicted_b + ttl_evicted_b >= reqd_b) {
        return true;
    }
    LOGGER_ERROR("could not evict enough from cache");
    return false;
}

/// @brief  Remove an object that is accessed after its TTL has expired.
void
PredictiveClockCache::evict_expired_accessed_object(CacheAccess const &access)
{
    remove(access.key, EvictionCause::AccessExpired, &access);
}

/// @brief  Remove an existing object when its updated size cannot fit.
void
PredictiveClockCache::evict_too_big_accessed_object(CacheAccess const &access)
{
    remove(access.key, EvictionCause::NoRoom, &access);
}

/// @brief  Check whether cached metadata is expired at this access time.
bool
PredictiveClockCache::is_expired(CacheAccess const &access,
                                 CachePredictiveMetadata const &metadata)
{
    return object_is_expired(metadata.expiration_time_ms_, access.timestamp_ms);
}

/// @brief  Handle a cache hit.
///
///         Before updating metadata, this ensures the new object size can
///         still fit. Updates can change object sizes in this trace format.
void
PredictiveClockCache::hit(CacheAccess const &access)
{
    auto &metadata = map_.at(access.key);
    if (!ensure_enough_room(metadata.size_, access)) {
        statistics_.skip(access.size_bytes());
        evict_too_big_accessed_object(access);
        if (DEBUG) {
            LOGGER_WARN("too big updated object");
        }
        return;
    }
    update(access, metadata);
}

/// @brief  Handle a cache miss.
///
///         The cache first makes room. If that succeeds, the object is
///         inserted and `repredict()` decides its queue placement.
bool
PredictiveClockCache::miss(CacheAccess const &access)
{
    if (!ensure_enough_room(0, access)) {
        if (DEBUG) {
            LOGGER_WARN("not enough room to insert!");
        }
        statistics_.skip(access.size_bytes());
        return false;
    }
    insert(access);
    return true;
}

/// @brief  Construct a predictive Clock cache.
///
///         `capacity` is measured in bytes. The lower and upper ratios
///         configure the learned eviction-time thresholds used by Psyche.
PredictiveClockCache::PredictiveClockCache(
    size_t const capacity,
    double const lower_ratio,
    double const upper_ratio,
    double const shards_sampling_ratio,
    std::map<std::string, std::string> kwargs)
    : capacity_(capacity),
      lifetime_thresholds_(
          lower_ratio,
          upper_ratio,
          get_env_u64("PSYCHE_REFRESH_PERIOD_MS", 60 * Duration::MINUTE),
          get_env_double("PSYCHE_THRESHOLD_DECAY", 0.0)),
      oracle_(capacity, shards_sampling_ratio),
      kwargs_(kwargs)
{
}

/// @brief  Mark the beginning of a simulation run.
void
PredictiveClockCache::start_simulation()
{
    statistics_.start_simulation();
    oracle_.start_simulation();
}

/// @brief  Mark the end of a simulation run.
void
PredictiveClockCache::end_simulation()
{
    statistics_.end_simulation();
    oracle_.end_simulation();
}

/// @brief  Process one access from the trace.
///
///         This is the public entry point. It removes expired TTL objects,
///         checks hit vs miss, handles lazy expiration on access, and then
///         delegates to `hit()` or `miss()`.
int
PredictiveClockCache::access(CacheAccess const &access)
{
    ok(true);
    g_assert_cmpuint(size_, ==, statistics_.size_);
    statistics_.time(access.timestamp_ms);
    evict_expired_objects(access.timestamp_ms);
    oracle_.access(access);
    rm_policy_statistics_.access(access,
                                 clock_cache_.size(),
                                 clock_size_,
                                 ttl_cache_.size(),
                                 ttl_size_);
    if (map_.count(access.key)) {
        auto &metadata = map_.at(access.key);
        if (!is_expired(access, metadata)) {
            hit(access);
            return 0;
        }
        evict_expired_accessed_object(access);
    }

    bool ok = miss(access);
    if (!ok) {
        if (DEBUG) {
            LOGGER_WARN("cannot handle miss");
        }
        return -1;
    }
    return 0;
}

/// @brief  Return the current cache size in bytes.
size_t
PredictiveClockCache::size() const
{
    return size_;
}

/// @brief  Return the configured byte capacity.
size_t
PredictiveClockCache::capacity() const
{
    return capacity_;
}

/// @brief  Return metadata for a cached key, or `nullptr` if absent.
CachePredictiveMetadata const *
PredictiveClockCache::get(uint64_t const key)
{
    if (map_.count(key)) {
        return &map_.at(key);
    }
    return nullptr;
}

/// @brief  Print the current Clock and TTL queues for debugging.
void
PredictiveClockCache::print()
{
    std::cout << "> PredictiveClockCache(sz: " << size_
              << ", cap: " << capacity_ << ")\n";
    std::cout << "> \tClock: ";
    for (auto n : clock_cache_) {
        std::cout << n->key << ", ";
    }
    std::cout << "\n";
    std::cout << "> \tTTL: ";
    for (auto [tm, key] : ttl_cache_) {
        std::cout << key << "@" << tm << ", ";
    }
    std::cout << "\n";
}

/// @brief  Return prediction-quality counters.
PredictionTracker const &
PredictiveClockCache::predictor() const
{
    return pred_tracker;
}

/// @brief  Return cache hit/miss/eviction statistics.
CacheStatistics const &
PredictiveClockCache::statistics() const
{
    return statistics_;
}

/// @brief  Serialize this cache measurements as a JSON-like string.
std::string
PredictiveClockCache::json(std::map<std::string, std::string> extras) const
{
    auto [lo_t, hi_t] = lifetime_thresholds_.thresholds();
    auto [lo_r, hi_r] = lifetime_thresholds_.ratios();
    return map2str(std::vector<std::pair<std::string, std::string>>{
        {"Capacity [B]", format_memory_size(capacity_)},
        {"Lower Ratio", val2str(lo_r)},
        {"Upper Ratio", val2str(hi_r)},
        {"Statistics", statistics_.json()},
        {"Removal Policy Statistics", rm_policy_statistics_.json()},
        {"PredictionTracker", pred_tracker.json()},
        {"Oracle", oracle_.json()},
        {"Lifetime Thresholds", lifetime_thresholds_.json()},
        {"Threshold Refreshes [#]",
         format_engineering(lifetime_thresholds_.refreshes())},
        {"Samples Since Threshold Refresh [#]",
         format_engineering(lifetime_thresholds_.since_refresh())},
        {"Clock Lifetime Evictions [#]",
         format_engineering(lifetime_thresholds_.evictions())},
        {"Lower Threshold [ms]", val2str(format_time(lo_t))},
        {"Upper Threshold [ms]", val2str(format_time(hi_t))},
        {"Kwargs", map2str(kwargs_, true)},
        {"Extras", map2str(extras, false)},
    });
}

/// @brief  Print `json()` to a stream.
void
PredictiveClockCache::print_json(std::ostream &ostrm,
                                 std::map<std::string, std::string> extras) const
{
    ostrm << "> " << json(extras) << std::endl;
}
