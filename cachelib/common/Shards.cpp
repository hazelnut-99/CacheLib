#include "Shards.h"
#include "ShardsFixedRate.h"
#include "ShardsFixedSize.h"

namespace facebook {
namespace cachelib {

Shards *Shards::fixedRate(uint64_t T, uint64_t P, uint64_t bucketSize)
{
    return new ShardsFixedRate(T, P, bucketSize);
}

Shards *Shards::fixedRate(double R, uint64_t bucketSize)
{
    return new ShardsFixedRate(R, bucketSize);
}

Shards *Shards::fixedSize(uint64_t T0, uint64_t P, uint64_t SMax, uint64_t bucketSize)
{
    return new ShardsFixedSize(T0, P, SMax, bucketSize);
}

Shards *Shards::fixedSize(double R0, uint64_t SMax, uint64_t bucketSize)
{
    return new ShardsFixedSize(R0, SMax, bucketSize);
}

} // namespace cachelib
} // namespace facebook