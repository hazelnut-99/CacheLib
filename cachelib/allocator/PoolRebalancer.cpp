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
#include <folly/dynamic.h>
#include <folly/json.h>

#include <stdexcept>
#include <thread>

namespace facebook::cachelib {

PoolRebalancer::PoolRebalancer(CacheBase& cache,
                               std::shared_ptr<RebalanceStrategy> strategy,
                               unsigned int freeAllocThreshold)
    : cache_(cache),
      defaultStrategy_(std::move(strategy)),
      monitorStrategy_(std::make_shared<RebalanceStrategy>()),
      freeAllocThreshold_(freeAllocThreshold) {
  if (!defaultStrategy_) {
    throw std::invalid_argument("The default rebalance strategy is not set.");
  }
}

PoolRebalancer::~PoolRebalancer() { stop(std::chrono::seconds(0)); }

std::shared_ptr<RebalanceStrategy> PoolRebalancer::findRebalanceStrategyForPool(PoolId pid) const {
  auto strategy = cache_.getRebalanceStrategy(pid);
  if (!strategy) {
      strategy = defaultStrategy_;
  }
  return strategy;
}

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
      auto strategy = findRebalanceStrategyForPool(pid);
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
  
  folly::dynamic logData = folly::dynamic::object(
          "request_id", request_id)(
          "pool_id", static_cast<int>(pid))(
          "victim", folly::dynamic::object("id", static_cast<int>(victimClassId)))(
          "receiver", folly::dynamic::object("id", static_cast<int>(receiverClassId)));
    
  std::string jsonString = folly::toJson(logData);
  XLOGF(DBG, "Slab_movement_event: {}", jsonString);
  
  
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

  lastRebalance_[pid] = strategy.isThrashing(pid, context);

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

  return true;
}

unsigned int PoolRebalancer::getRebalanceEventQueueSize(PoolId pid) const{
  auto strategy = findRebalanceStrategyForPool(pid);
  return strategy->getRebalanceEventQueueSize(pid);
}

void PoolRebalancer::clearPoolEventMap(PoolId pid) {
  auto strategy = findRebalanceStrategyForPool(pid);
  strategy->clearPoolRebalanceEvent(pid);
}

bool PoolRebalancer::checkForThrashing(PoolId pid) const{
  auto strategy = findRebalanceStrategyForPool(pid);
  return strategy->checkForThrashing(pid);
}

double PoolRebalancer::queryEffectiveMoveRate(PoolId pid) const{
  auto strategy = findRebalanceStrategyForPool(pid);
  return strategy->queryEffectiveMoveRate(pid);
}

bool PoolRebalancer::isLastRebalanceThrashing(PoolId pid) const {
  auto it = lastRebalance_.find(pid);
  if (it == lastRebalance_.end()) {
      return false; 
  }
  return it->second;
}

std::map<std::string, std::map<ClassId, double>> PoolRebalancer::getPoolDeltaStats(PoolId pid) {
  return monitorStrategy_->getPoolDeltaStats(cache_, pid);
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
