/// TODO
/// 1. Test with real trace (how to get TTLs?)
/// 2. How to count miscounts?
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <future>
#include <iostream>
#include <sstream>
#include <stdlib.h>
#include <string>
#include <sys/types.h>
#include <thread>
#include <utility>
#include <vector>

#include "cpp_lib/cache_access.hpp"
#include "cpp_lib/cache_trace_format.hpp"
#include "cpp_lib/format_measurement.hpp"
#include "cpp_lib/progress_bar.hpp"
#include "cpp_lib/util.hpp"
#include "lib/predictive_lfu_ttl_cache.hpp"
#include "lib/predictive_clock_ttl_cache.hpp"
#include "logger/logger.h"

#include "cpp_lib/cache_trace.hpp"
#include "lib/predictive_lru_ttl_cache.hpp"
#include "shards/fixed_rate_shards_sampler.h"

using size_t = std::size_t;
using uint64_t = std::uint64_t;

template <typename P>
static bool
run_single_cache(std::promise<std::string> ret,
                 int const id,
                 CacheAccessTrace &trace,
                 size_t const capacity_bytes,
                 double const lower_ratio,
                 double const upper_ratio,
                 double const shards_ratio,
                 bool const show_progress)
{
    P p(capacity_bytes * shards_ratio,
        lower_ratio,
        upper_ratio,
        shards_ratio,
        {{"shards_ratio", std::to_string(shards_ratio)}});
    FixedRateShardsSampler sampler{shards_ratio, true};
    std::stringstream ss;
    LOGGER_TIMING("starting test_trace(trace: %s, nominal cap: %zu, sampled "
                  "cap: %zu, lt: %f, ut: %f, shards: %f)",
                  trace.path().c_str(),
                  capacity_bytes,
                  (size_t)(capacity_bytes * shards_ratio),
                  lower_ratio,
                  upper_ratio,
                  shards_ratio);
    ProgressBar pbar{trace.size(), show_progress && id == 0};
    p.start_simulation();
    for (size_t i = 0; i < trace.size(); ++i) {
        pbar.tick();
        auto const &access = trace.get_wait(i);
        if (!sampler.sample(access.key)) {
            continue;
        }
        if (access.is_read()) {
            p.access(access);
        }
    }
    p.end_simulation();
    LOGGER_TIMING("finished test_trace(trace: %s, cap: %zu, lt: %f, ut: "
                  "%f, shards: %f)",
                  trace.path().c_str(),
                  capacity_bytes,
                  lower_ratio,
                  upper_ratio,
                  shards_ratio);
    p.print_json(
        ss,
        {
            {"SHARDS", sampler.json(false)},
            {
                "remaining_lifetime",
                p.record_remaining_lifetime(trace.back()),
            },
            {"Nominal Capacity [B]", format_memory_size(capacity_bytes)},
        });
    ret.set_value(ss.str());
    FixedRateShardsSampler__destroy(&sampler);

    return true;
}

template <typename P>
static void
run_caches(std::string const &path,
           CacheTraceFormat format,
           std::vector<uint64_t> const &capacity_bytes,
           double const lower_ratio,
           double const upper_ratio,
           double const shards_ratio,
           bool const show_progress)
{
    CacheAccessTrace trace{path, format, capacity_bytes.size()};
    std::vector<std::thread> workers;
    std::vector<std::future<std::string>> futs;

    int id = 0;
    for (auto c : capacity_bytes) {
        std::promise<std::string> promise;
        futs.push_back(promise.get_future());
        workers.emplace_back(run_single_cache<P>,
                             std::move(promise),
                             id++,
                             std::ref(trace),
                             c,
                             lower_ratio,
                             upper_ratio,
                             shards_ratio,
                             show_progress);
    }
    for (auto &w : workers) {
        w.join();
    }
    int i = 0;
    for (auto &r : futs) {
        std::cout << "Run: " << path << " " << CacheTraceFormat__string(format)
                  << " " << lower_ratio << " " << upper_ratio << " "
                  << capacity_bytes[i++] << " " << std::endl;
        std::cout << r.get();
    }
}

int
main(int argc, char *argv[])
{
    if (argc != 8) {
        std::cout
            << "Usage: predictor <trace> <format> <lower_ratio [0.0, 1.0]> "
               "<upper_ratio [0.0, 1.0]> <cache-capacities>+ "
               "<shards-ratio [0.0, 1.0]> <policy lru|lfu|clock> "
            << std::endl;
        exit(1);
    }

    std::string const path{argv[1]};
    CacheTraceFormat const format{CacheTraceFormat__parse(argv[2])};
    double const lower_ratio{atof(argv[3])};
    double const upper_ratio{atof(argv[4])};
    // This will panic if it was unsuccessful.
    std::vector<uint64_t> capacity_bytes{parse_capacities(argv[5])};
    double const shards_ratio{atof(argv[6])};
    std::string const policy{argv[7]};
    bool const show_progress{false};
    LOGGER_INFO("Running: %s %s with %s",
                path.c_str(),
                CacheTraceFormat__string(format).c_str(),
                policy.c_str());
    if (policy == "lru") {
        run_caches<PredictiveCache>(path,
                                    format,
                                    capacity_bytes,
                                    lower_ratio,
                                    upper_ratio,
                                    shards_ratio,
                                    show_progress);
    } else if (policy == "lfu") {
        run_caches<PredictiveLFUCache>(path,
                                       format,
                                       capacity_bytes,
                                       lower_ratio,
                                       upper_ratio,
                                       shards_ratio,
                                       show_progress);
    } else if (policy == "clock") {
        run_caches<PredictiveClockCache>(path,
                                         format,
                                         capacity_bytes,
                                         lower_ratio,
                                         upper_ratio,
                                         shards_ratio,
                                         show_progress);
    } else {
        LOGGER_ERROR("Unrecognized policy: '%s'", policy.c_str());
        return EXIT_FAILURE;
    }
    std::cout << "OK!" << std::endl;
    return 0;
}
