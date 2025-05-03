#include "Shards.h"
#include "ShardsFixedSize.h"

namespace facebook {
namespace cachelib {

Shards* Shards::fixedSize(uint64_t T0, uint64_t P, uint64_t SMax, uint64_t bucketSize) {
    return new ShardsFixedSize(T0, P, SMax, bucketSize);
}

Shards* Shards::fixedSize(double R0, uint64_t SMax, uint64_t bucketSize) {
    return new ShardsFixedSize(R0, SMax, bucketSize);
}

} // namespace cachelib
} // namespace facebook