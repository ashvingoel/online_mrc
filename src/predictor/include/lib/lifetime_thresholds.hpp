#pragma once

/** @brief  [TODO]
 *
 *  @todo   Change thresholds to double precision floats.
 **/

#include "cpp_lib/duration.hpp"
#include "cpp_lib/histogram.hpp"
#include "cpp_lib/temporal_data.hpp"
#include "cpp_lib/temporal_sampler.hpp"
#include "cpp_lib/util.hpp"
#include "math/doubles_are_equal.h"
#include "unused/mark_unused.h"
#include <cassert>
#include <cmath>
#include <cstdint>
#include <string>
#include <tuple>
#include <utility>

using uint64_t = std::uint64_t;

// HACK Use the mean if the ratios are 0.42 (get it, 42?).
//      This is my answer to desperation.
static constexpr bool USE_MEAN = true;

class LifeTimeThresholds {
    /// @note   This is a relatively expensive function that I removed
    ///         the optimization for because it is only called a small
    ///         number of times.
    std::pair<double, double>
    recalculate_thresholds() const
    {
        double lower = NAN, upper = NAN;
        if (USE_MEAN && lower_ratio_ == 0.42 && upper_ratio_ == 0.42) {
            double r = histogram_.mean();
            if (std::isnan(r)) {
                r = INFINITY;
            }
            return {r, r};
        }

        // We handle the case of 1.0 explicitly, we would simply set the
        // upper_threshold_ to the largest lifespan we have seen yet,
        // rather than the maximum possible.
        if (lower_ratio_ == 0.0) {
            lower = 0;
        } else if (lower_ratio_ == 1.0) {
            lower = INFINITY;
        } else {
            lower = histogram_.lower_bound_percentile(lower_ratio_);
        }
        if (upper_ratio_ == 0.0) {
            upper = 0;
        } else if (upper_ratio_ == 1.0) {
            upper = INFINITY;
        } else {
            upper = histogram_.percentile(upper_ratio_);
        }

        if (std::isnan(lower) || std::isnan(upper)) {
            if (lower_ratio_ == upper_ratio_) {
                // TODO Maybe change this to the current age of the cache.
                return {INFINITY, INFINITY};
            } else {
                // This would extend the 'training' period until we see
                // an eviction.
                return {0, INFINITY};
            }
        }
        // If the ratios are the same, then we simply return the mean.
        if (lower_ratio_ == upper_ratio_) {
            // The first bucket has no preceding bucket. Its lower-bound
            // sentinel is not a lifetime that can be averaged.
            if (std::isinf(lower) && std::signbit(lower)) {
                return {upper, upper};
            }
            double mean = (lower + upper) / 2;
            return {mean, mean};
        }
        return {lower, upper};
    }

    /// @brief  Return if the thresholds should be refreshed.
    /// @note   I allow them to be some \delta out of alignment.
    ///         By default, \delta = 1% (arbitrarily).
    bool
    should_refresh(uint64_t const current_time_ms)
    {
        double const error = refresh_error_threshold_ * coarse_counter_;

        // NOTE An update may occur any time after a hour past the last
        //      sample once the coarse histogram is off by the error
        //      margin. This is as opposed to checking once per hour
        //      whether both are true.
        if (!refresher_.should_sample(current_time_ms)) {
            return false;
        }
        if (training_period_) {
            training_period_ = false;
            return true;
        }

        double const low_cnt = lower_ratio_ * coarse_counter_;
        double const mid_cnt = (upper_ratio_ - lower_ratio_) * coarse_counter_;
        double const up_cnt = (1.0 - upper_ratio_) * coarse_counter_;

        return !(
            doubles_are_close(low_cnt, std::get<0>(coarse_histogram_), error) &&
            doubles_are_close(mid_cnt, std::get<1>(coarse_histogram_), error) &&
            doubles_are_close(up_cnt, std::get<2>(coarse_histogram_), error));
    }

    void
    measure_statistics(uint64_t const current_time_ms)
    {
        temporal_times_ms_.update(current_time_ms);
        // Global histogram statistics.
        temporal_histogram_size_.update(histogram_.size());
        temporal_mean_eviction_time_ms_.update(histogram_.mean());
        temporal_median_eviction_time_ms_.update(histogram_.percentile(0.5));
        temporal_75p_eviction_time_ms_.update(histogram_.percentile(0.75));
        temporal_25p_eviction_time_ms_.update(histogram_.percentile(0.25));
        temporal_min_eviction_time_ms_.update(histogram_.min());
        temporal_max_eviction_time_ms_.update(histogram_.max());
        // Current histogram statistics.
        temporal_ch_histogram_size_.update(histogram_.size());
        temporal_ch_mean_eviction_time_ms_.update(histogram_.mean());
        temporal_ch_median_eviction_time_ms_.update(histogram_.percentile(0.5));
        temporal_ch_75p_eviction_time_ms_.update(histogram_.percentile(0.75));
        temporal_ch_25p_eviction_time_ms_.update(histogram_.percentile(0.25));
        temporal_ch_min_eviction_time_ms_.update(histogram_.min());
        temporal_ch_max_eviction_time_ms_.update(histogram_.max());
    }

    void
    measure_threshold_statistics(uint64_t const current_time_ms)
    {
        temporal_refresh_times_ms_.update(current_time_ms);
        temporal_refresh_low_threshold_ms_.update(lower_threshold_);
        temporal_refresh_high_threshold_ms_.update(upper_threshold_);
    }

public:
    /// @todo   Allow setting the starting time. This is more for
    ///         redundancy to make sure everything is safe.
    ///         This isn't strictly necessary because we refresh the
    ///         thresholds before we have any data, we just set it to
    ///         the defaults (0, INFINITY).
    LifeTimeThresholds(double const lower_ratio,
                       double const upper_ratio,
                       uint64_t const refresh_period_ms = 60 * Duration::MINUTE,
                       double const decay = 0.0,
                       double const refresh_error_threshold = 0.01)
        : lower_ratio_(lower_ratio),
          upper_ratio_(upper_ratio),
          refresher_{refresh_period_ms, false},
          decay_{decay},
          refresh_error_threshold_{refresh_error_threshold}
    {
        assert(lower_ratio >= 0.0 && lower_ratio <= 1.0);
        assert(upper_ratio >= 0.0 && upper_ratio <= 1.0);
        assert(lower_ratio <= upper_ratio);

        if (lower_ratio == 0 && upper_ratio == 0) {
            lower_threshold_ = 0;
            upper_threshold_ = 0;
        } else if (lower_ratio == 1.0 && upper_ratio == 1.0) {
            lower_threshold_ = INFINITY;
            upper_threshold_ = INFINITY;
        } else if (lower_ratio == upper_ratio) {
            lower_threshold_ = INFINITY;
            upper_threshold_ = INFINITY;
        }
    }

    void
    register_cache_eviction(uint64_t const lifetime,
                            uint64_t const size,
                            uint64_t const current_time_ms)
    {
        // Before we do anything, we want to measure some statistics!
        if (temporal_sampler_.should_sample(current_time_ms)) {
            measure_statistics(current_time_ms);
            current_histogram_.reset();
        }
        current_histogram_.update(lifetime);

        // The current_time_ms was formerly used to check if we should
        // update the thresholds; however, this is redundant with the
        // check that supplies the thresholds.
        UNUSED(current_time_ms);
        // We are counting objects for now.
        // TODO Count bytes in the future, maybe?
        (void)size;
        histogram_.update(lifetime);

        if (lower_threshold_ > lifetime) {
            ++std::get<0>(coarse_histogram_);
            ++coarse_counter_;
        } else if (upper_threshold_ > lifetime) {
            ++std::get<1>(coarse_histogram_);
            ++coarse_counter_;
        } else {
            ++std::get<2>(coarse_histogram_);
            ++coarse_counter_;
        }
    }

    void
    refresh_thresholds()
    {
        auto x = recalculate_thresholds();
        histogram_.decay_histogram(/*alpha=*/1 - decay_);
        lower_threshold_ = x.first;
        upper_threshold_ = x.second;
        coarse_histogram_ = {0, 0, 0};
        coarse_counter_ = 0;
    }

    /// @note   Get the current thresholds without refreshing.
    std::pair<double, double>
    thresholds() const
    {
        return {lower_threshold_, upper_threshold_};
    }

    std::pair<double, double>
    ratios() const
    {
        return {lower_ratio_, upper_ratio_};
    }

    /// @note   Automatically refresh the thresholds when there's a mismatch.
    std::tuple<double, double, bool>
    get_updated_thresholds(uint64_t const current_time_ms)
    {
        bool r = false;
        if (should_refresh(current_time_ms)) {
            refresh_thresholds();
            measure_threshold_statistics(current_time_ms);
            r = true;
        }
        return {lower_threshold_, upper_threshold_, r};
    }

    uint64_t
    refreshes() const
    {
        return refresher_.nr_samples();
    }

    uint64_t
    evictions() const
    {
        return histogram_.total();
    }

    uint64_t
    since_refresh() const
    {
        return coarse_counter_;
    }

    bool
    has_real_data() const
    {
        return has_real_data_;
    }

    void
    set_real_data()
    {
        has_real_data_ = true;
    }

    std::string
    json() const
    {
        std::string coarse_hist_str{
            "[" + val2str(std::get<0>(coarse_histogram_)) + ", " +
            val2str(std::get<1>(coarse_histogram_)) + ", " +
            val2str(std::get<2>(coarse_histogram_)) + "]"};
        return map2str(std::vector<std::pair<std::string, std::string>>{
            {"Histogram", histogram_.json()},
            {"Coarse Histogram", coarse_hist_str},
            {"Threshold Refreshes [#]", format_engineering(refreshes())},
            {"Samples Since Threshold Refresh [#]",
             format_engineering(since_refresh())},
            {"LRU Lifetime Evictions [#]", format_engineering(evictions())},
            // Temporal Threshold Statistics.
            {"Temporal Refresh Times [ms]", temporal_refresh_times_ms_.str()},
            {"Temporal Refresh Low Threshold [ms]",
             temporal_refresh_low_threshold_ms_.str()},
            {"Temporal Refresh High Threshold [ms]",
             temporal_refresh_high_threshold_ms_.str()},
            // Other temporal statistics.
            {"Temporal Times [ms]", temporal_times_ms_.str()},
            {"Temporal Histogram Sizes [#]", temporal_histogram_size_.str()},
            {"Temporal Mean Eviction Times [ms]",
             temporal_mean_eviction_time_ms_.str()},
            {"Temporal Median Eviction Times [ms]",
             temporal_median_eviction_time_ms_.str()},
            {"Temporal 75th-percentile Eviction Times [ms]",
             temporal_75p_eviction_time_ms_.str()},
            {"Temporal 25th-percentile Eviction Times [ms]",
             temporal_25p_eviction_time_ms_.str()},
            {"Temporal Min Eviction Times [ms]",
             temporal_min_eviction_time_ms_.str()},
            {"Temporal Max Eviction Times [ms]",
             temporal_max_eviction_time_ms_.str()},
            // Current histogram (C.H.) statistics.
            {"Temporal Current Histogram Histogram Size [#]",
             temporal_ch_histogram_size_.str()},
            {"Temporal Current Histogram Mean Eviction Times [ms]",
             temporal_ch_mean_eviction_time_ms_.str()},
            {"Temporal Current Histogram Median Eviction Times [ms]",
             temporal_ch_median_eviction_time_ms_.str()},
            {"Temporal Current Histogram 75th-percentile Eviction Times [ms]",
             temporal_ch_75p_eviction_time_ms_.str()},
            {"Temporal Current Histogram 25th-percentile Eviction Times [ms]",
             temporal_ch_25p_eviction_time_ms_.str()},
            {"Temporal Current Histogram Min Eviction Times [ms]",
             temporal_ch_min_eviction_time_ms_.str()},
            {"Temporal Current Histogram Max Eviction Times [ms]",
             temporal_ch_max_eviction_time_ms_.str()},
        });
    }

private:
    double const lower_ratio_;
    double const upper_ratio_;
    double lower_threshold_ = 0.0;
    double upper_threshold_ = INFINITY;

    bool training_period_ = true;
    bool has_real_data_ = false;

    Histogram histogram_;
    // This tells us how far off our current estimate of the thresholds
    // may possibly be.
    std::tuple<uint64_t, uint64_t, uint64_t> coarse_histogram_{0, 0, 0};
    uint64_t coarse_counter_ = 0;

    // Data structures for refreshing the threshold estimates.
    TemporalSampler refresher_;
    double const decay_;
    double const refresh_error_threshold_;

    // Statistics on thresholds.
    TemporalData temporal_refresh_times_ms_;
    TemporalData temporal_refresh_low_threshold_ms_;
    TemporalData temporal_refresh_high_threshold_ms_;

    // Temporal sampler.
    TemporalSampler temporal_sampler_{Duration::HOUR, false, false};

    // Global histogram statistics.
    TemporalData temporal_times_ms_;
    TemporalData temporal_histogram_size_;
    TemporalData temporal_mean_eviction_time_ms_;
    TemporalData temporal_median_eviction_time_ms_;
    TemporalData temporal_75p_eviction_time_ms_;
    TemporalData temporal_25p_eviction_time_ms_;
    TemporalData temporal_min_eviction_time_ms_;
    TemporalData temporal_max_eviction_time_ms_;

    // Current histogram for most recent values within temporal sample.
    // Reset after every sample.
    Histogram current_histogram_;
    // Current histogram (C.H.) statistics.
    TemporalData temporal_ch_histogram_size_;
    TemporalData temporal_ch_mean_eviction_time_ms_;
    TemporalData temporal_ch_median_eviction_time_ms_;
    TemporalData temporal_ch_75p_eviction_time_ms_;
    TemporalData temporal_ch_25p_eviction_time_ms_;
    TemporalData temporal_ch_min_eviction_time_ms_;
    TemporalData temporal_ch_max_eviction_time_ms_;
};
