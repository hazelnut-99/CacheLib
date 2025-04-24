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
#include <cmath>

#include "cachelib/allocator/Util.h"

namespace facebook::cachelib {

MarginalHitsStrategy::MarginalHitsStrategy(Config config)
    : RebalanceStrategy(MarginalHits), config_(std::move(config)), minDiffInUse_(config.minDiff) {}

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

  if(config.autoIncThreshold || config.autoDecThreshold) { 
    bool thrashingDetected = checkForThrashing(pid);
    auto eventQueueSize = getRebalanceEventQueueSize(pid);
    if (thrashingDetected && config.autoIncThreshold) {
      doubleRebalanceThreshold();
      clearPoolRebalanceEvent(pid);
    } else if (!thrashingDetected && config.autoDecThreshold && eventQueueSize > 2) {
        // not including 1 because 1 is always 100% effective
        decreaseRebalanceThreshold();
    }
  }

  XLOGF(DBG, "rebalance_threshold: {}" , minDiffInUse_);

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
  }

  if (classStates_[pid].entities.empty()) {
    // initialization
    classStates_[pid].entities = classes;
    for (auto cid : classes) {
      classStates_[pid].smoothedRanks[cid] = 0;
    }
  }
  classStates_[pid].updateRankings(scores, config.movingAverageParam, config.decayWithHits);
  RebalanceContext ctx = pickVictimAndReceiverFromRankings(pid, validVictim, validReceiver);
  
  // classStates_[pid].smoothedRanks[ctx.victimClassId]
  folly::dynamic logArray = folly::dynamic::array;
  for (auto it : classes) {
    auto acStats = poolStats.mpStats.acStats;
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
    ("hot_queue_oldest_element_age", poolEvictionAgeStats.getHotEvictionStat(it).oldestElementAge)
    ("pool_all_slabs_allocated", cache.getPool(pid).allSlabsAllocated())
    ("smoothed_rank", classStates_[pid].smoothedRanks[it]);

    std::string jsonString = folly::toJson(logData);
    XLOGF(DBG, "Rebalance_states_logging: {}", jsonString);

    logArray.push_back(logData);
  }
  std::string jsonString = folly::toJson(logArray);
  XLOGF(DBG, "Rebalance_class_snapshot: {}", jsonString);


  if(!ctx.isEffective()) {
    ctx = kNoOpContext;
  } else {
    auto improvement = classStates_[pid].smoothedRanks.at(ctx.receiverClassId) - classStates_[pid].smoothedRanks.at(ctx.victimClassId);
    if(minDiffInUse_ > 0 && improvement < minDiffInUse_) {
      XLOGF(DBG, "Not enough to trigger rebalancing, receiver score: {}, victim score: {}, threshold: {}",
        classStates_[pid].smoothedRanks.at(ctx.receiverClassId), classStates_[pid].smoothedRanks.at(ctx.victimClassId), minDiffInUse_);
      ctx = kNoOpContext;
    }
  }

  if (ctx.isEffective()) {
    XLOGF(DBG, "marginal-hits diff between receiver and victim: {}, receiver_id: {}, victim_id: {}",
          (scores.at(ctx.receiverClassId) - scores.at(ctx.victimClassId)), ctx.receiverClassId, ctx.victimClassId);
  }

  if(!config.onlyUpdateHitsIfRebalance || ctx.isEffective()) {
    for (const auto i : poolStats.getClassIds()) {
      poolState[i].updateTailHits(poolStats);
    }
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

void MarginalHitsStrategy::doubleRebalanceThreshold() {
  auto newValue = std::max(minDiffInUse_ * 2, static_cast<unsigned int>(1));
  XLOGF(INFO, "double rebalance threshold: before: {}, after: {}", minDiffInUse_, newValue);  
  minDiffInUse_ = newValue;
}

void MarginalHitsStrategy::decreaseRebalanceThreshold() {
  auto newValue = std::max(minDiffInUse_ - 1, static_cast<unsigned int>(1));
  if (newValue == minDiffInUse_) {
    return;
  }
  XLOGF(INFO, "decrease rebalance threshold: before: {}, after: {}", minDiffInUse_, newValue);  
  minDiffInUse_ = newValue;
}


} // namespace facebook::cachelib
