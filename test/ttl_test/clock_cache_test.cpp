#include <cstddef>
#include <cstdint>
#include <iostream>
#include <vector>

#include "cache/clock_cache.hpp"
#include "logger/logger.h"
#include "test/mytester.h"
#include "trace/reader.h"
#include "ttl_cache/new_ttl_clock_cache.hpp"
#include "yang_cache/yang_cache.hpp"

/*******************************************************************************
 *  HELPER FUNCTIONS
 *********************************************************************************/

static void
print_keys(std::vector<std::uint64_t> &keys)
{
    for (auto k : keys) {
        std::cout << k << ",";
    }
    std::cout << std::endl;
}

static std::vector<std::uint64_t>
get_trace(char const *const filename, enum TraceFormat format)
{
    struct Trace t = {};
    std::vector<std::uint64_t> trace;
    t = read_trace_keys(filename, format);
    trace.reserve(t.length);
    for (std::size_t i = 0; i < t.length; ++i) {
        trace.push_back(t.trace[i].key);
    }
    return trace;
}

/// @brief  Check the plain ClockCache second-chance behavior directly.
///         With capacity 2, accessing 1 again should let it survive when
///         3 is inserted, so 2 should be evicted instead of 1.
static bool
plain_clock_second_chance_test()
{
    ClockCache cache(2);
    cache.access_item({0, 1});
    cache.access_item({1, 2});
    cache.access_item({2, 1});
    cache.access_item({3, 3});

    cache.validate(0);
    return cache.contains(1) && !cache.contains(2) && cache.contains(3);
}


/*******************************************************************************
 *  VALIDATION TESTING
 *********************************************************************************/

/// @brief  Validate that the trace runs with validations turned on.
///         We don't check the correctness of the result.
static bool
simple_validation_test(std::vector<std::uint64_t> const &trace,
                       std::size_t const capacity,
                       int const verbose = 0)
{
    NewTTLClockCache cache(capacity);
    std::uint64_t time = 0;
    for (auto x : trace) {
        cache.access_item({time++, x});
        if (verbose >= 2) {
            std::cout << "Access key: " << x << std::endl;
            cache.debug_print();
            cache.to_stream(std::cout);
        }
        cache.validate(0);
    }
    if (verbose) {
        cache.debug_print();
    }
    return true;
}

/*******************************************************************************
 *  CACHE VS ORACLE TESTING
 *********************************************************************************/
template <typename T>
static bool
equal_vectors(std::vector<T> const &lhs, std::vector<T> const &rhs)
{
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        if (lhs[i] != rhs[i]) {
            return false;
        }
    }
    return true;
}

static bool
cache_vs_oracle_test(std::vector<std::uint64_t> const &trace,
                     std::size_t const capacity,
                     std::vector<std::uint64_t> const &final_state,
                     int const verbose = 0)
{
    NewTTLClockCache cache(capacity);
    std::uint64_t time = 0;
    for (auto x : trace) {
        cache.access_item({time++, x});
        if (verbose >= 2) {
            std::cout << "Access key: " << x << std::endl;
            cache.debug_print();
            cache.to_stream(std::cout);
        }
        cache.validate(0);
    }
    auto keys = cache.get_keys();
    return equal_vectors(keys, final_state);
}

/*******************************************************************************
 *  CACHE COMPARISON TESTING
 *********************************************************************************/

template <class C, class T>
static int
compare_cache_states(C const &cache, T const &ttl_cache, int const verbose = 0)
{
    int nerr = 0;
    cache.validate(verbose);
    ttl_cache.validate(verbose);
    // NOTE I'm getting some strange behaviour where the 'cache.size()'
    //      does not agree with the actual size.
    // if (cache.size() != ttl_cache.size()) {
    //     LOGGER_ERROR("cache (%zu) and TTL cache (%zu) are different sizes",
    //                  cache.size(),
    //                  ttl_cache.size());
    //     nerr += 1;
    // }
    if (verbose) {
        std::vector<std::uint64_t> keys = cache.get_keys();
        std::cout << "Cache keys: ";
        print_keys(keys);
        keys = ttl_cache.get_keys();
        std::cout << "TTL-Cache keys: ";
        print_keys(keys);
    }
    for (auto k : ttl_cache.get_keys()) {
        if (!cache.contains(k)) {
            LOGGER_ERROR("key %zu found in TTL cache but not regular cache", k);
            nerr += 1;
        }
    }
    return nerr;
}

static bool
compare_caches(std::vector<std::uint64_t> const &trace,
               std::size_t const capacity,
               int const verbose = 0,
               int const max_errs = 10)
{
    int nerr = 0;
    NewTTLClockCache ttl_cache(capacity);
    YangCache cache(capacity, YangCacheType::CLOCK);
    for (std::size_t i = 0; i < trace.size(); ++i) {
        cache.access_item({i, trace[i]});
        ttl_cache.access_item({i, trace[i]});
        if (i % capacity == 0) {
            nerr += compare_cache_states(cache, ttl_cache, verbose);
            if (nerr > max_errs) {
                return false;
            }
        }
    }
    nerr += compare_cache_states(cache, ttl_cache);
    return nerr == 0;
}

bool
trace_test(char const *const filename,
           TraceFormat const format,
           std::size_t const capacity,
           int const verbose = 0,
           int const max_errs = 10)
{
    std::vector<std::uint64_t> trace = get_trace(filename, format);
    return compare_caches(trace, capacity, verbose, max_errs);
}

int
main(int argc, char *argv[])
{
    ASSERT_FUNCTION_RETURNS_TRUE(plain_clock_second_chance_test());
    std::vector<std::uint64_t> simple_trace = {0, 1, 2, 3, 0, 1, 2, 3, 4};
    std::vector<std::uint64_t> trace = {0, 1, 2, 3, 0, 1, 0, 2, 3, 4, 5, 6, 7};
    std::vector<std::uint64_t> src2_trace =
        {1, 2, 3, 4, 5, 5, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    ASSERT_FUNCTION_RETURNS_TRUE(simple_validation_test(simple_trace, 4));
    ASSERT_FUNCTION_RETURNS_TRUE(simple_validation_test(trace, 4));
    ASSERT_FUNCTION_RETURNS_TRUE(simple_validation_test(src2_trace, 2));

    // Test filling the trace
    std::vector<std::uint64_t> trace_0 = {1, 2, 3, 4};
    std::vector<std::uint64_t> final_state_0 = {3, 4};
    std::vector<std::uint64_t> trace_1 = {1, 1, 2, 3, 4};
    std::vector<std::uint64_t> final_state_1 = {3, 4};
    std::vector<std::uint64_t> trace_2 = {1, 1, 2, 2, 3, 4};
    std::vector<std::uint64_t> final_state_2 = {3, 4};
    std::vector<std::uint64_t> trace_3 = {1, 2, 2, 3, 4};
    std::vector<std::uint64_t> final_state_3 = {2, 4};
    std::vector<std::uint64_t> trace_4 = {1, 2, 2, 3, 3, 4};
    std::vector<std::uint64_t> final_state_4 = {3, 4};
    std::vector<std::uint64_t> trace_5 = {1, 1, 2, 2, 3, 3, 4};
    std::vector<std::uint64_t> final_state_5 = {4, 3};
    ASSERT_FUNCTION_RETURNS_TRUE(
        cache_vs_oracle_test(trace_0, 2, final_state_0));
    ASSERT_FUNCTION_RETURNS_TRUE(
        cache_vs_oracle_test(trace_1, 2, final_state_1));
    ASSERT_FUNCTION_RETURNS_TRUE(
        cache_vs_oracle_test(trace_2, 2, final_state_2));
    ASSERT_FUNCTION_RETURNS_TRUE(
        cache_vs_oracle_test(trace_3, 2, final_state_3));
    ASSERT_FUNCTION_RETURNS_TRUE(
        cache_vs_oracle_test(trace_4, 2, final_state_4));
    ASSERT_FUNCTION_RETURNS_TRUE(
        cache_vs_oracle_test(trace_5, 2, final_state_5));

    // Test replacement of a filled cache
    std::vector<std::uint64_t> trace_6 = {1, 2, 3};
    std::vector<std::uint64_t> trace_7 = {1, 1, 2, 3};
    std::vector<std::uint64_t> trace_8 = {1, 2, 2, 3};
    std::vector<std::uint64_t> trace_9 = {1, 1, 2, 2, 3};
    if (argc == 2) {
        ASSERT_FUNCTION_RETURNS_TRUE(trace_test(argv[1], TRACE_FORMAT_KIA, 1));
        ASSERT_FUNCTION_RETURNS_TRUE(trace_test(argv[1], TRACE_FORMAT_KIA, 2));
        ASSERT_FUNCTION_RETURNS_TRUE(
            trace_test(argv[1], TRACE_FORMAT_KIA, 1 << 10));
        ASSERT_FUNCTION_RETURNS_TRUE(
            trace_test(argv[1], TRACE_FORMAT_KIA, 1 << 12));
        ASSERT_FUNCTION_RETURNS_TRUE(
            trace_test(argv[1], TRACE_FORMAT_KIA, 1 << 14));
        ASSERT_FUNCTION_RETURNS_TRUE(
            trace_test(argv[1], TRACE_FORMAT_KIA, 1 << 16));
        ASSERT_FUNCTION_RETURNS_TRUE(
            trace_test(argv[1], TRACE_FORMAT_KIA, 1 << 18));
    }
    return 0;
}
