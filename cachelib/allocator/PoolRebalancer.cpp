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

#include "cachelib/allocator/PoolRebalancer.h"

#include <folly/logging/xlog.h>

#include <stdexcept>
#include <thread>

namespace facebook::cachelib {

PoolRebalancer::PoolRebalancer(CacheBase& cache,
                               std::shared_ptr<RebalanceStrategy> strategy,
                               unsigned int freeAllocThreshold)
    : cache_(cache),
      defaultStrategy_(std::move(strategy)),
      freeAllocThreshold_(freeAllocThreshold) {
  if (!defaultStrategy_) {
    throw std::invalid_argument("The default rebalance strategy is not set.");
  }
}

PoolRebalancer::~PoolRebalancer() { stop(std::chrono::seconds(0)); }

void PoolRebalancer::work() {
  try {
    for (const auto pid : cache_.getRegularPoolIds()) {
      auto strategy = cache_.getRebalanceStrategy(pid);
      if (!strategy) {
        strategy = defaultStrategy_;
      }
      // to do make sure each pool has its own strategy object
      tryRebalancing(pid, *strategy);
    }
  } catch (const std::exception& ex) {
    XLOGF(ERR, "Rebalancing interrupted due to exception: {}", ex.what());
  }
}

void PoolRebalancer::processAllocFailure(PoolId pid) {
  auto strategy = cache_.getRebalanceStrategy(pid);
  if (!strategy) {
    strategy = defaultStrategy_;
  }
  strategy->uponAllocFailure();
}

void PoolRebalancer::publicWork(uint64_t request_id) {
  try {
    XLOG(DBG, "synchronous rebalancing");
    for (const auto pid : cache_.getRegularPoolIds()) {
      auto strategy = cache_.getRebalanceStrategy(pid);
      if (!strategy) {
        strategy = defaultStrategy_;
      }
      tryRebalancing(pid, *strategy, request_id);
    }
  } catch (const std::exception& ex) {
    XLOGF(ERR, "Rebalancing interrupted due to exception: {}", ex.what());
  }
}

void PoolRebalancer::releaseSlab(PoolId pid,
                                 ClassId victimClassId,
                                 ClassId receiverClassId,
                                uint64_t request_id) {
  const auto now = util::getCurrentTimeMs();
  cache_.releaseSlab(pid, victimClassId, receiverClassId,
                     SlabReleaseMode::kRebalance);
  const auto elapsed_time =
      static_cast<uint64_t>(util::getCurrentTimeMs() - now);
  const PoolStats poolStats = cache_.getPoolStats(pid);
  unsigned int numSlabsInReceiver = 0;
  uint32_t receiverAllocSize = 0;
  uint64_t receiverEvictionAge = 0;
  if (receiverClassId != Slab::kInvalidClassId) {
    numSlabsInReceiver = poolStats.numSlabsForClass(receiverClassId);
    receiverAllocSize = poolStats.allocSizeForClass(receiverClassId);
    receiverEvictionAge = poolStats.evictionAgeForClass(receiverClassId);
  }
  // stats_.addSlabReleaseEvent(
  //     victimClassId, receiverClassId, elapsed_time, pid,
  //     poolStats.numSlabsForClass(victimClassId), numSlabsInReceiver,
  //     poolStats.allocSizeForClass(victimClassId), receiverAllocSize,
  //     poolStats.evictionAgeForClass(victimClassId), receiverEvictionAge,
  //     poolStats.mpStats.acStats.at(victimClassId).freeAllocs);
  // workaround to track request_id
  stats_.addSlabReleaseEvent(
        victimClassId, receiverClassId, request_id, pid,
        poolStats.numSlabsForClass(victimClassId), numSlabsInReceiver,
        poolStats.allocSizeForClass(victimClassId), receiverAllocSize,
        poolStats.evictionAgeForClass(victimClassId), receiverEvictionAge,
        poolStats.mpStats.acStats.at(victimClassId).freeAllocs);
  XLOGF(DBG,
        "Moved slab in Pool Id: {}, Victim Class Id: {}, Receiver "
        "Class Id: {}",
        static_cast<int>(pid), static_cast<int>(victimClassId),
        static_cast<int>(receiverClassId));
}

RebalanceContext PoolRebalancer::pickVictimByFreeAlloc(PoolId pid) const {
  const auto& mpStats = cache_.getPool(pid).getStats();
  uint64_t maxFreeAllocSlabs = 1;
  ClassId retId = Slab::kInvalidClassId;
  for (auto& id : mpStats.classIds) {
    uint64_t freeAllocSlabs = mpStats.acStats.at(id).freeAllocs /
                              mpStats.acStats.at(id).allocsPerSlab;

    if (freeAllocSlabs > freeAllocThreshold_ &&
        freeAllocSlabs > maxFreeAllocSlabs) {
      maxFreeAllocSlabs = freeAllocSlabs;
      retId = id;
    }
  }
  RebalanceContext ctx;
  ctx.victimClassId = retId;
  ctx.receiverClassId = Slab::kInvalidClassId;
  return ctx;
}

bool PoolRebalancer::tryRebalancing(PoolId pid, RebalanceStrategy& strategy, uint64_t request_id) {
  const auto begin = util::getCurrentTimeMs();

  if (freeAllocThreshold_ > 0) {
    auto ctx = pickVictimByFreeAlloc(pid);
    if (ctx.victimClassId != Slab::kInvalidClassId) {
      releaseSlab(pid, ctx.victimClassId, Slab::kInvalidClassId, request_id);
    }
  }

  if (!cache_.getPool(pid).allSlabsAllocated()) {
    return false;
  }

  auto currentTimeSec = util::getCurrentTimeMs();
  const auto context = strategy.pickVictimAndReceiver(cache_, pid);
  // add to the queue
  //strategy.recordRebalanceEvent(pid, context);
  auto end = util::getCurrentTimeMs();
  pickVictimStats_.recordLoopTime(end > currentTimeSec ? end - currentTimeSec
                                                       : 0);

  if (context.victimClassId == Slab::kInvalidClassId) {
    XLOGF(DBG,
          "Pool Id: {} rebalancing strategy didn't find an victim",
          static_cast<int>(pid));
    return false;
  }
  currentTimeSec = util::getCurrentTimeMs();
  releaseSlab(pid, context.victimClassId, context.receiverClassId, request_id);
  end = util::getCurrentTimeMs();
  releaseStats_.recordLoopTime(end > currentTimeSec ? end - currentTimeSec : 0);
  rebalanceStats_.recordLoopTime(end > begin ? end - begin : 0);

  XLOGF(DBG, "rebalance_event: request_id: {}, pool_id: {}, victim_class_id: {}, receiver_class_id: {}",
    request_id,
    static_cast<int>(pid),
    static_cast<int>(context.victimClassId),
    static_cast<int>(context.receiverClassId));
  addToPoolEventMap(pid, context.victimClassId,
                    context.receiverClassId);

  return true;
}

void PoolRebalancer::addToPoolEventMap(PoolId pid, ClassId victimClassId, ClassId receiverClassId) {
  auto& eventQueue = poolEventMap_[pid];
  eventQueue.emplace_back(victimClassId, receiverClassId);

  // Enforce the fixed size
  if (eventQueue.size() > kMaxQueueSize) {
    eventQueue.pop_front();
  }
}

unsigned int PoolRebalancer::getRebalanceEventQueueSize(PoolId pid) {
  auto& eventQueue = poolEventMap_[pid];
  return eventQueue.size();
}

void PoolRebalancer::clearPoolEventMap(PoolId pid) {
  poolEventMap_.erase(pid);
}

bool PoolRebalancer::checkForThrashing(PoolId pid) {
  const auto it = poolEventMap_.find(pid);
  if (it == poolEventMap_.end() || it->second.empty()) {
      return false;
  }

  const auto& events = it->second;
  std::unordered_map<ClassId, int> netChanges;

  for (const auto& [fromClass, toClass] : events) {
      netChanges[fromClass]--;
      netChanges[toClass]++;
  }

  int currentAbsNet = 0;
  for (const auto& [classId, net] : netChanges) {
      currentAbsNet += std::abs(net);
  }
  int totalEffectiveMoves = currentAbsNet / 2;

  double overallRate = static_cast<double>(totalEffectiveMoves) / events.size();
  return overallRate <= 0.5 && events.size() > 4;  
}

RebalancerStats PoolRebalancer::getStats() const noexcept {
  RebalancerStats stats;
  stats.numRuns = getRunCount();
  stats.numRebalancedSlabs = rebalanceStats_.getNumLoops();
  stats.lastRebalanceTimeMs = rebalanceStats_.getLastLoopTimeMs();
  stats.avgRebalanceTimeMs = rebalanceStats_.getAvgLoopTimeMs();

  stats.lastReleaseTimeMs = releaseStats_.getLastLoopTimeMs();
  stats.avgReleaseTimeMs = releaseStats_.getAvgLoopTimeMs();

  stats.lastPickTimeMs = pickVictimStats_.getLastLoopTimeMs();
  stats.avgPickTimeMs = pickVictimStats_.getAvgLoopTimeMs();
  stats.pickVictimRounds = pickVictimStats_.getNumLoops();
  return stats;
}

} // namespace facebook::cachelib
