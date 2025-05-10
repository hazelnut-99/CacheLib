#pragma once
#include "AnomalyDetector.h"
#include <deque>
#include <algorithm>
#include <vector>
#include <cmath>

namespace facebook {
namespace cachelib {

class MadDetector : public AnomalyDetector {
    std::deque<double> window_;
    size_t windowSize_;
    double threshold_;
    double median_ = 0;
    double mad_ = 0;

    template <typename Container>
    static double computeMedian(Container& values) {
        auto copy = values;
        auto n = copy.size() / 2;
        std::nth_element(copy.begin(), copy.begin() + n, copy.end());
        return copy[n];
    }

    static double computeMad(std::deque<double>& values, double median) {
        std::vector<double> absDeviations;
        absDeviations.reserve(values.size());
        for (double v : values) {
            absDeviations.push_back(std::abs(v - median));
        }
        return computeMedian(absDeviations);
    }

public:
    MadDetector(size_t windowSize, double threshold = 3.0)
        : windowSize_(windowSize), threshold_(threshold) {}

    bool update(double value) override {
        window_.push_back(value);
        if (window_.size() > windowSize_) {
            window_.pop_front();
        }
        if (window_.empty()) return false;

        median_ = computeMedian(window_);
        mad_ = computeMad(window_, median_);
        const double scaledMad = 1.4826 * mad_;

        return scaledMad > 0 && (std::abs(value - median_) > threshold_ * scaledMad);
    }

    void reset() override {
        window_.clear();
        median_ = 0;
        mad_ = 0;
    }

    unsigned int getWindowSize() const override { return window_.size(); }

    double getThreshold() const override { return threshold_; }
    double getMean() const override { return median_; }
    double getVariability() const override { return 1.4826 * mad_; }
};

} // namespace cachelib
} // namespace facebook