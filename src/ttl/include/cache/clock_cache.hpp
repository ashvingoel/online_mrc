#pragma once

#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <optional>
#include <unordered_map>

#include "cache/base_cache.hpp"
#include "cpp_lib/cache_metadata.hpp"
#include "cpp_lib/cache_statistics.hpp"
#include "ttl/ttl.hpp"
#include "unused/mark_unused.h"

/// @brief  A simple Second-Chance cache.
///
///         The cache is mostly FIFO: new objects enter at the front of
///         `evictor_`, and old objects are considered for eviction from
///         the back. The "second chance" part is the `visited` bit. If
///         an old object was visited, it is moved back to the front
///         instead of being evicted immediately.

/// @note   I implemented this as FIFO with reinsertion, which is slower
///         but easier to get right.
class ClockCache : public BaseCache {
private:
    /// @brief  Handle a cache hit.
    ///
    ///         This updates the object metadata and marks it as visited.
    ///         It does not move the object in `evictor_`; in
    ///         Second-Chance, the queue only changes later when eviction
    ///         reaches the object.
    void
    hit(std::uint64_t const access_time_ms,
        std::uint64_t const key,
        std::uint64_t const expiration_time_ms)
    {
        auto &metadata = map_.at(key);
        metadata.visit(access_time_ms, expiration_time_ms);
        metadata.visited = true;
    }

    /// @brief  Handle a cache miss.
    ///
    ///         If the cache is full, this removes objects until there is
    ///         room. It checks the oldest object first, which is stored
    ///         at the back of `evictor_`.
    void
    miss(std::uint64_t const access_time_ms,
         std::uint64_t const key,
         std::uint64_t const expiration_time_ms)
    {
        assert(evictor_.size() <= capacity_);
        while (evictor_.size() >= capacity_) {
            // The back of the deque is the oldest object, so it is the
            // next eviction candidate.
            std::uint64_t const victim_key = evictor_.back();
            evictor_.pop_back();
            if (map_.at(victim_key).visited) {
                // Second chance: keep the object, clear its visited bit,
                // and move it to the front as if it were new again.
                map_.at(victim_key).unvisit();
                evictor_.push_front(victim_key);
                continue;
            } else {
                // No second chance: the object was not visited recently,
                // so it is removed from the cache.
                map_.erase(victim_key);
                break;
            }
        }
        // Insert the missed object at the front. In this implementation,
        // the front means newest and the back means oldest.
        evictor_.push_front(key);
        map_.emplace(key, CacheMetadata(access_time_ms, expiration_time_ms));
    }

public:
    /// @brief  Create a Clock cache with room for `capacity` objects.
    ClockCache(std::size_t capacity)
        : BaseCache(capacity)
    {
    }

    /// @brief  Print the cache internal state for debugging.
    ///
    ///         `Stream` can be something like `std::cout` or a string
    ///         stream. This method is not part of the cache policy; it is
    ///         just a way to inspect what the cache contains.
    template <class Stream>
    void
    to_stream(Stream &s) const
    {
        s << name << "(capacity=" << capacity_ << ",size=" << size() << ")"
          << std::endl;
        s << "> Key-Metadata Map:" << std::endl;
        for (auto [k, metadata] : map_) {
            // This is inefficient, but looks easier on the eyes.
            std::stringstream ss;
            metadata.to_stream(ss);
            s << ">> key: " << k << ", metadata: " << ss.str() << std::endl;
        }
        s << "> Evictor" << std::endl;
        for (auto k : evictor_) {
            s << ">> key: " << k << std::endl;
        }
    }

    /// @brief  Check that the internal data structures agree.
    ///
    ///         Every cached key should appear once in `map_` and once in
    ///         `evictor_`. Assertions fail if the cache bookkeeping is
    ///         inconsistent.
    bool
    validate(int const verbose = 0) const
    {
        if (verbose) {
            std::cout << "validate(name=" << name << ",verbose=" << verbose
                      << ")" << std::endl;
        }
        assert(map_.size() == evictor_.size());
        assert(size() <= capacity_);
        if (verbose) {
            std::cout << "> size: " << size() << std::endl;
        }
        if (verbose >= 2) {
            to_stream(std::cout);
        }

        for (auto k : evictor_) {
            if (verbose >= 2) {
                std::cout << "> Validating: key=" << k << std::endl;
            }
            assert(map_.count(k));
        }
        return true;
    }

    /// @brief  Process one cache access.
    ///
    ///         This is the public entry point for the cache. It decides
    ///         whether the access is a hit or a miss, then calls `hit()`
    ///         or `miss()` to do the actual work.
    int
    access_item(CacheAccess const &access)
    {
        assert(map_.size() == evictor_.size());
        assert(map_.size() <= capacity_);
        if (capacity_ == 0) {
            statistics_.deprecated_miss();
            return 0;
        }

        // TODO Change this to enable TTLs.
        std::uint64_t expiration_time_ms =
            get_expiration_time(access.timestamp_ms, FOREVER);
        if (map_.count(access.key)) {
            hit(access.timestamp_ms, access.key, expiration_time_ms);
            statistics_.deprecated_hit();
        } else {
            miss(access.timestamp_ms, access.key, expiration_time_ms);
            statistics_.deprecated_miss();
        }
        assert(map_.size() <= capacity_);
        return 0;
    }

protected:
    /// @brief  Eviction order for keys in the cache.
    ///
    ///         Front = newest object. Back = oldest object and next
    ///         eviction candidate.
    std::deque<std::uint64_t> evictor_;

public:
    /// @brief  Human-readable cache name used by tests and runners.
    static constexpr char name[] = "ClockCache";
};
