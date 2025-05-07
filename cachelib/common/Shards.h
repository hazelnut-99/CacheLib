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
  virtual std::unordered_map<uint64_t, uint64_t> getReuseDistanceHistogram() const {
    return {}; 
  }
  virtual void resetReuseDistanceHistogram() {}
  
  virtual void clear() = 0;

  static Shards *fixedRate(uint64_t T, uint64_t P, uint64_t bucket_size = 1);
  static Shards *fixedRate(double R, uint64_t bucket_size = 1);

  static Shards *fixedSize(uint64_t T0, uint64_t P, uint64_t SMax, uint64_t bucketSize = 1);
  static Shards *fixedSize(double R0, uint64_t SMax, uint64_t bucketSize = 1);

  struct HashedKeyHash {
    size_t operator()(const HashedKey& hk) const {
        return hk.keyHash(); // Use HashedKey's keyHash() method
    }
  };

  struct HashedKeyEqual {
      bool operator()(const HashedKey& lhs, const HashedKey& rhs) const {
          return lhs.key() == rhs.key(); // Compare keys using HashedKey's key() method
      }
  };
};

} // namespace cachelib
} // namespace facebook