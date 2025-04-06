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

#include "cachelib/allocator/MarginalHitsStrategy.h"

#include <folly/logging/xlog.h>

#include <algorithm>
#include <functional>
#include <folly/json.h>
#include <folly/dynamic.h>

#include "cachelib/allocator/Util.h"

namespace facebook::cachelib {

MarginalHitsStrategy::MarginalHitsStrategy(Config config)
    : RebalanceStrategy(MarginalHits), config_(std::move(config)) {}

RebalanceContext MarginalHitsStrategy::pickVictimAndReceiverImpl(
    const CacheBase& cache, PoolId pid, const PoolStats& poolStats) {
  const auto config = getConfigCopy();
  if (!cache.getPool(pid).allSlabsAllocated()) {
    XLOGF(DBG,
          "Pool Id: {} does not have all its slabs allocated"
          " and does not need rebalancing.",
          static_cast<int>(pid));
    return kNoOpContext;
  }
  auto scores = computeClassMarginalHits(pid, poolStats, config.tailSlabCnt);
  auto classesSet = poolStats.getClassIds();
  std::vector<ClassId> classes(classesSet.begin(), classesSet.end());
  std::unordered_map<ClassId, bool> validVictim;
  std::unordered_map<ClassId, bool> validReceiver;
  
  auto& poolState = getPoolState(pid);

  const auto poolEvictionAgeStats =
      cache.getPoolEvictionAgeStats(pid, 0);

  for (auto it : classes) {
    auto acStats = poolStats.mpStats.acStats;
    // a class can be a victim only if it has more than config.minSlabs slabs
    validVictim[it] = acStats.at(it).totalSlabs() > config.minSlabs;
    // a class can be a receiver only if its free memory (free allocs, free
    // slabs, etc) is small
    validReceiver[it] = (acStats.at(it).getTotalFreeMemory() <
                        config.maxFreeMemSlabs * Slab::kSize) && 
                        (!config.filterReceiverByEvictionRate || poolState.at(it).getDeltaEvictions(poolStats) > 0);

    // for debugging purposes
    folly::dynamic logData = folly::dynamic::object
    ("pool_id", static_cast<int>(pid))
    ("class_id", static_cast<int>(it))
    ("class_total_slabs", acStats.at(it).totalSlabs())
    ("class_marginal_hits", scores.at(it))
    ("class_free_slabs", acStats.at(it).freeSlabs)
    ("class_free_allocs", acStats.at(it).freeAllocs)
    ("class_delta_evictions", poolState.at(it).getDeltaEvictions(poolStats))
    ("class_alloc_failures", poolState.at(it).deltaAllocFailures(poolStats))
    ("delta_cold_hits", poolState.at(it).getColdHits(poolStats))
    ("delta_warm_hits", poolState.at(it).getWarmHits(poolStats))
    ("delta_hot_hits", poolState.at(it).getHotHits(poolStats))
    ("delta_total_hits", poolState.at(it).getColdHits(poolStats) + poolState.at(it).getWarmHits(poolStats) + poolState.at(it).getHotHits(poolStats))
    ("class_oldest_element_age", poolEvictionAgeStats.getOldestElementAge(it))
    ("pool_all_slabs_allocated", cache.getPool(pid).allSlabsAllocated());

    std::string jsonString = folly::toJson(logData);
    XLOGF(DBG, "Rebalance_states_logging: {}", jsonString);
  }
  if (classStates_[pid].entities.empty()) {
    // initialization
    classStates_[pid].entities = classes;
    for (auto cid : classes) {
      classStates_[pid].smoothedRanks[cid] = 0;
    }
  }
  classStates_[pid].updateRankings(scores, config.movingAverageParam);
  RebalanceContext ctx = pickVictimAndReceiverFromRankings(pid, validVictim, validReceiver);
  if (ctx.victimClassId != Slab::kInvalidClassId 
      && ctx.receiverClassId != Slab::kInvalidClassId
      && ctx.victimClassId != ctx.receiverClassId) {
    if((scores.at(ctx.receiverClassId) - scores.at(ctx.victimClassId)) < config.minDiff){
      XLOG(DBG, " Not enough to trigger slab rebalancing");
    }
  }

  for (const auto i : poolStats.getClassIds()) {
    poolState[i].updateTailHits(poolStats);
  }

  return ctx;
}

ClassId MarginalHitsStrategy::pickVictimImpl(const CacheBase& cache,
                                             PoolId pid,
                                             const PoolStats& stats) {
  return pickVictimAndReceiverImpl(cache, pid, stats).victimClassId;
}

std::unordered_map<ClassId, double>
MarginalHitsStrategy::computeClassMarginalHits(PoolId pid,
                                               const PoolStats& poolStats,
                                               unsigned int tailSlabCnt) {
  const auto& poolState = getPoolState(pid);
  std::unordered_map<ClassId, double> scores;
  for (auto info : poolState) {
    if (info.id != Slab::kInvalidClassId) {
      scores[info.id] = info.getMarginalHits(poolStats, tailSlabCnt);
    }
  }
  return scores;
}

RebalanceContext MarginalHitsStrategy::pickVictimAndReceiverFromRankings(
    PoolId pid,
    const std::unordered_map<ClassId, bool>& validVictim,
    const std::unordered_map<ClassId, bool>& validReceiver) {
  auto victimAndReceiver = classStates_[pid].pickVictimAndReceiverFromRankings(
      validVictim, validReceiver, Slab::kInvalidClassId);
  RebalanceContext ctx{victimAndReceiver.first, victimAndReceiver.second};
  if (ctx.victimClassId == Slab::kInvalidClassId ||
      ctx.receiverClassId == Slab::kInvalidClassId ||
      ctx.victimClassId == ctx.receiverClassId) {
    return kNoOpContext;
  }

  XLOGF(DBG,
        "Rebalancing: receiver = {}, smoothed rank = {}, victim = {}, smoothed "
        "rank = {}",
        static_cast<int>(ctx.receiverClassId),
        classStates_[pid].smoothedRanks[ctx.receiverClassId],
        static_cast<int>(ctx.victimClassId),
        classStates_[pid].smoothedRanks[ctx.victimClassId]);
  return ctx;
}
} // namespace facebook::cachelib
