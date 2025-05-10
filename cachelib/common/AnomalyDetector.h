#pragma once
#include <folly/logging/xlog.h>

#include <map>
#include <memory>

#include "cachelib/allocator/memory/Slab.h"

namespace facebook {
namespace cachelib {

class AnomalyDetector {
 public:
  virtual ~AnomalyDetector() = default;

  // Process a new value and return true if anomalous
  virtual bool update(double value) = 0;

  // Reset detector state
  virtual void reset() = 0;

  // Get current threshold (in standard deviations)
  virtual double getThreshold() const = 0;

  virtual unsigned int getWindowSize() const = 0; 

  virtual double getMean() const = 0;

  virtual double getVariability() const = 0;
};

} // namespace cachelib
} // namespace facebook