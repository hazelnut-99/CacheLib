#pragma once

#include "Shards.h"
#include "SplayTree.h"
#include <unordered_map>
#include <map>
#include <folly/Range.h> // For folly::StringPiece

namespace facebook {
namespace cachelib {


class ShardsFixedRate : public Shards {
    // Replace folly::F14FastMap with std::unordered_map and use custom hash and equality
    std::unordered_map<HashedKey, uint64_t, HashedKeyHash, HashedKeyEqual> m_timePerObject;
    std::unordered_map<uint64_t, uint64_t> m_distanceHistogram;
    SplayTree<uint64_t> m_distanceTree;

    uint64_t m_objectCounter{0};

    inline void updateHistogram(uint64_t const bucket);
    inline uint64_t getDistance(HashedKey const& key);

public:
    uint64_t const kP{1 << 24};
    double const kR{0.001};
    uint64_t const kT{static_cast<uint64_t>(kR * kP)};
    uint64_t const kBucketSize{1};

    ShardsFixedRate(uint64_t T, uint64_t P, uint64_t bucketSize = 1);
    ShardsFixedRate(double R, uint64_t bucketSize = 1);
    void feed(HashedKey hk) override final;
    std::unordered_map<uint64_t, uint64_t> getReuseDistanceHistogram() const override final;
    void resetReuseDistanceHistogram() override final;
    std::map<uint64_t, double> mrc() override final;
    void clear() override final;
};

} // namespace cachelib
} // namespace facebook