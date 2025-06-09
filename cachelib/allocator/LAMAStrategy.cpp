/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "cachelib/allocator/LAMAStrategy.h"

#include <folly/logging/xlog.h>

#include <algorithm>
#include <functional>

#include "cachelib/allocator/Util.h"


/**
 * 
 * LAMA, Optimized Locality-aware Memory Allocation for Key-value Cache
 * 
 * https://www.usenix.org/system/files/conference/atc15/atc15-paper-hu_update.pdf
 * 
 * The MRC analysis is performed by a separate thread. 
 * Each analysis samples recent 20 million requests which are stored using a circular buffer.
 * 
 * In our implementation, we buffer and analyze 20 million requests before each repartitioning.
 * 
 * We split the global access trace into different sub-traces according to their classes. 
 * With the sub-trace of each class, we generate the MRCs as follows. 
 * We use a hash table to record the last access time of each item. 
 * With this hash table, we can easily compute the reuse time distribution rt, 
 * which represents the number of accesses with a reuse time t. 
 * For access trace of length n, if the number of unique data is m, 
 * the average number of items accessed in a time window of size w can be calculated using Xiang’s formula
 * 
 * LAMA has two main parameters as explained in Section 3.4: 
 * the repartitioning interval M, which is the number of items accesses before repartitioning; 
 * and the reassignment upper bound N, which is the maximal number of slabs reassigned at repartitioning.
 * 
 * For fast and steady convergence, we choose M = 1, 000, 000 and N = 50 for LAMA.
 * 
 * In order to avoid the cost of reassigning too many slabs, 
 * we set N slabs as the upper bound on the total reassignment. 
 * 
 * At each repartitioning, we choose N slabs with the lowest risk. 
 * We use the risk definition of PSA, 
 * which is the ratio between reference rate and number of slabs for each class. 
 * The re-allocation is global, since multiple candidate slabs are selected from possibly many size classes. 
 * In contrast, PSA selects a single candidate from one size class. 
 * The bound N is the maximal number of slab reassignments. 
 * In the steady state, the repartitioning algorithm may decide that 
 * the current allocation is the best possible and does not reassign any slab. 
 * The number of actual reassignments can be 0 or any number not exceeding N.
 */

namespace facebook {
namespace cachelib {

LAMAStrategy::LAMAStrategy(Config config)
    : RebalanceStrategy(LAMA), config_(std::move(config)) {}


RebalanceContext LAMAStrategy::pickVictimAndReceiverImpl(
    const CacheBase& cache,
    PoolId pid,
    const PoolStats& poolStats) {
  const auto config = getConfigCopy();

  auto [mrImprovement, victimReceiverPairs] = pickVictimReceiverPairs(cache, pid, poolStats);

  if (mrImprovement < config.missRatioImprovementThreshold) {
    XLOGF(INFO, "MR improvement {} is below threshold {}. No rebalancing.",
          mrImprovement, config.missRatioImprovementThreshold);
    return kNoOpContext;
  }
  if (victimReceiverPairs.empty()) {
    XLOGF(INFO, "No victim-receiver pairs found for pool {}. No rebalancing.",
          static_cast<int>(pid));
    return kNoOpContext;
  }

  // Take up to maxSlabsToMove pairs
  size_t numToMove = std::min<size_t>(config.maxSlabsToMove, victimReceiverPairs.size());
  std::vector<std::pair<ClassId, ClassId>> selectedPairs(
      victimReceiverPairs.begin(), victimReceiverPairs.begin() + numToMove);
      
    for (const auto& [victim, receiver] : selectedPairs) {
        XLOGF(INFO, "Selected victim-receiver pair: victim={}, receiver={}",
            static_cast<int>(victim), static_cast<int>(receiver));
    }
  RebalanceContext ctx;
  ctx.victimReceiverPairs = std::move(selectedPairs);
  return ctx;
}


ClassId LAMAStrategy::pickVictimImpl(const CacheBase& cache,
                                     PoolId pid,
                                     const PoolStats& poolStats) {
    auto [mrImprovement, victimReceiverPairs] = pickVictimReceiverPairs(cache, pid, poolStats);
    if (victimReceiverPairs.empty()) {
        return Slab::kInvalidClassId;
    }
    // Return the first ClassId of the first pair (the victim)
    return victimReceiverPairs.front().first;
}


std::tuple<double, std::vector<std::pair<ClassId, ClassId>>> LAMAStrategy::pickVictimReceiverPairs(
      const CacheBase& cache,
      PoolId pid,
      const PoolStats& poolStats) {
    
    if (!cache.getPool(pid).allSlabsAllocated()) {
        XLOGF(DBG,
            "Pool Id: {} does not have all its slabs allocated"
            " and does not need rebalancing.",
            static_cast<int>(pid));
        return {};
    }

    const FootprintMRC* mrc = cache.getFootprintMrcForPool(pid);
    if (!mrc) {
        XLOG(ERR) << "No MRC available for pool " << pid;
        return {};
    }

    auto classesSet = poolStats.getClassIds();
    auto acStats = poolStats.mpStats.acStats;
    std::map<ClassId, size_t> classIdToAllocsPerSlabMap;
    std::map<ClassId, size_t> currentSlabAllocation;
    for (const auto& classId : classesSet) {
        classIdToAllocsPerSlabMap[classId] = acStats.at(classId).allocsPerSlab;
        currentSlabAllocation[classId] = acStats.at(classId).totalSlabs();
    }
    /*
     * A tuple containing:
     * - mrOld (double): Total miss rate with the current allocation.
     * - mrNew (double): Total miss rate with the new optimal allocation.
     * - optimalAllocation (std::map<ClassId, size_t>): A map mapping ClassId to the new
     * optimal number of slabs.
     * - reassignmentPlan (std::vector<std::pair<ClassId, ClassId>>): A list of (victim_class_id, receiver_class_id) pairs,
     * indicating individual slab movements from old to new.
     * - accessFrequencies (std::map<ClassId, size_t>): A map mapping ClassId to the total number of
     * requests for that class in the current window.
     */
    std::tuple<double, double, std::map<ClassId, size_t>, std::vector<std::pair<ClassId, ClassId>>, std::map<ClassId, size_t>>
        result = mrc->solveSlabReallocation(classIdToAllocsPerSlabMap, currentSlabAllocation);
    
    double mrOld, mrNew;
    std::map<ClassId, size_t> optimalAllocation;
    std::vector<std::pair<ClassId, ClassId>> reassignmentPlan;
    std::map<ClassId, size_t> accessFrequencies;

    std::tie(mrOld, mrNew, optimalAllocation, reassignmentPlan, accessFrequencies) = result;
    double mrImprovement = mrOld - mrNew;
    XLOGF(INFO, "MRC Reallocation: Old MR: {}, New MR: {}, Improvement: {}", mrOld, mrNew, mrImprovement);

    return {mrImprovement, reassignmentPlan};
}


} // namespace cachelib
} // namespace facebook
