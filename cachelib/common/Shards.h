#pragma once

#include "Hash.h"
#include <folly/Range.h>

#include <cstdint>
#include <map>
#include <string>

namespace facebook {
namespace cachelib {

class Shards {
 public:
  virtual void feed(HashedKey hk) = 0;
  virtual std::map<uint64_t, double> mrc() = 0;
  virtual void clear() = 0;

  static Shards* fixedSize(uint64_t T0,
                           uint64_t P,
                           uint64_t SMax,
                           uint64_t bucketSize = 1);
  static Shards* fixedSize(double R0, uint64_t SMax, uint64_t bucketSize = 1);
};

} // namespace cachelib
} // namespace facebook