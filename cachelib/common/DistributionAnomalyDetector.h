#pragma once
#include "AnomalyDetector.h"
#include "ZScoreDetector.h"
#include "MadDetector.h"
#include <unordered_map>
#include <map>
#include <folly/logging/xlog.h>

namespace facebook {
namespace cachelib {

class DistributionAnomalyDetector {
    std::unordered_map<ClassId, std::unique_ptr<AnomalyDetector>> detectors_;
    double threshold_;
    size_t minSamples_;
    bool useMad_;

public:
    DistributionAnomalyDetector(double threshold = 3.0, size_t minSamples = 30, bool useMad = false)
        : threshold_(threshold), minSamples_(minSamples), useMad_(useMad) {}

    bool update(const std::map<ClassId, double>& distribution) {
        int anomalyCount = 0;
    
        for (const auto& [classId, prob] : distribution) {
            if (!detectors_.count(classId)) {
                detectors_[classId] = useMad_
                    ? std::unique_ptr<AnomalyDetector>(std::make_unique<MadDetector>(minSamples_, threshold_))
                    : std::unique_ptr<AnomalyDetector>(std::make_unique<ZScoreDetector>(minSamples_, threshold_));
            }
    
            if (detectors_[classId]->update(prob)) {
                XLOGF(DBG, "Anomaly in class {}: value={:.4f}", classId, prob);
                anomalyCount++;
                if (anomalyCount >= 2) {
                    return true; 
                }
            }
        }
    
        return false; 
    }

    unsigned int getWindowSize() const {
        return detectors_.empty() ? 0 : detectors_.begin()->second->getWindowSize();
    }

    void reset() {
        detectors_.clear();
    }
};

} // namespace cachelib
} // namespace facebook