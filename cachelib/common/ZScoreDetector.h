#pragma once
#include "AnomalyDetector.h"
#include <deque>
#include <numeric>
#include <cmath>

namespace facebook {
namespace cachelib {

class ZScoreDetector : public AnomalyDetector {
    std::deque<double> window_;
    size_t windowSize_;
    double threshold_;
    double mean_ = 0;
    double stdDev_ = 0;

public:
    ZScoreDetector(size_t windowSize, double threshold = 3.0)
        : windowSize_(windowSize), threshold_(threshold) {}

    bool update(double value) override {
        window_.push_back(value);
        if (window_.size() > windowSize_) window_.pop_front();
        
        if (window_.size() < 3) return false;

        mean_ = std::accumulate(window_.begin(), window_.end(), 0.0) / window_.size();
        double sqSum = std::inner_product(window_.begin(), window_.end(), window_.begin(), 0.0);
        stdDev_ = std::sqrt(sqSum / window_.size() - mean_ * mean_);

        return std::abs(value - mean_) > threshold_ * stdDev_;
    }

    unsigned int getWindowSize() const override { return window_.size(); }

    void reset() override { window_.clear(); mean_ = 0; stdDev_ = 0; }
    double getThreshold() const override { return threshold_; }
    double getMean() const override { return mean_; }
    double getVariability() const override { return stdDev_; }
};

} // namespace cachelib
} // namespace facebook