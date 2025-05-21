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

#include <folly/dynamic.h>
#include <folly/json.h>
#include <folly/logging/xlog.h>

#include <algorithm>
#include <cmath>
#include <functional>

#include "cachelib/allocator/Util.h"

namespace facebook::cachelib {

MarginalHitsStrategy::MarginalHitsStrategy(Config config)
    : RebalanceStrategy(MarginalHits),
      config_(std::move(config)),
      minDiffInUse_(config.minDiff) {
  if (config_.enableOnlineLearning) {
    XLOG(INFO) << "Online learning enabled. Initializing ModelApiClients for "
                  "each pool.";
  } else {
    XLOG(INFO) << "Online learning disabled. ModelApiClients not initialized.";
  }
}

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

  if (config.autoIncThreshold || config.autoDecThreshold) {
    bool thrashingDetected = checkForThrashing(pid);
    auto eventQueueSize = getRebalanceEventQueueSize(pid);
    if (thrashingDetected && config.autoIncThreshold) {
      XLOGF(INFO, "min diff value in the queue: {}",
            getMinDiffValueFromRebalanceEvents(pid));
      increaseRebalanceThreshold(getMinDiffValueFromRebalanceEvents(pid));
      clearPoolRebalanceEvent(pid);
    } else if (!thrashingDetected && config.autoDecThreshold 
      && (eventQueueSize == 0 || queryEffectiveMoveRate(pid) >= 0.8)) {
      decreaseRebalanceThreshold();
    }
  }

  XLOGF(DBG, "rebalance_threshold: {}", minDiffInUse_);

  auto scores = computeClassMarginalHits(pid, poolStats, config.tailSlabCnt);
  auto classesSet = poolStats.getClassIds();
  std::vector<ClassId> classes(classesSet.begin(), classesSet.end());
  std::unordered_map<ClassId, bool> validVictim;
  std::unordered_map<ClassId, bool> validReceiver;

  auto& poolState = getPoolState(pid);

  const auto poolEvictionAgeStats = cache.getPoolEvictionAgeStats(pid, 0);

  for (auto it : classes) {
    auto acStats = poolStats.mpStats.acStats;
    // a class can be a victim only if it has more than config.minSlabs slabs
    validVictim[it] = acStats.at(it).totalSlabs() > config.minSlabs;
    // a class can be a receiver only if its free memory (free allocs, free
    // slabs, etc) is small
    validReceiver[it] = (acStats.at(it).getTotalFreeMemory() <
                         config.maxFreeMemSlabs * Slab::kSize) &&
                        (!config.filterReceiverByEvictionRate ||
                         poolState.at(it).getDeltaEvictions(poolStats) > 0);
  }

  if (classStates_[pid].entities.empty()) {
    // initialization
    classStates_[pid].entities = classes;
    for (auto cid : classes) {
      classStates_[pid].smoothedRanks[cid] = 0;
    }
  }
  classStates_[pid].updateRankings(scores, config.movingAverageParam,
                                   config.decayWithHits);
  RebalanceContext ctx =
      pickVictimAndReceiverFromRankings(pid, validVictim, validReceiver);

  // classStates_[pid].smoothedRanks[ctx.victimClassId]
  folly::dynamic logArray = folly::dynamic::array;
  for (auto it : classes) {
    auto acStats = poolStats.mpStats.acStats;

    uint64_t totalSlabs = acStats.at(it).totalSlabs();

    folly::dynamic logData = folly::dynamic::object(
        "pool_id", static_cast<int>(pid))("class_id", static_cast<int>(it))(
        "class_total_slabs", acStats.at(it).totalSlabs())(
        "class_marginal_hits", scores.at(it))("class_free_slabs",
                                              acStats.at(it).freeSlabs)(
        "class_free_allocs", acStats.at(it).freeAllocs)(
        "class_delta_evictions", poolState.at(it).getDeltaEvictions(poolStats))(
        "class_alloc_failures", poolState.at(it).deltaAllocFailures(poolStats))(
        "delta_cold_hits", poolState.at(it).getColdHits(poolStats))(
        "delta_total_hits_corrected", poolState.at(it).deltaHits(poolStats))(
        "delta_warm_hits", poolState.at(it).getWarmHits(poolStats))(
        "delta_hot_hits", poolState.at(it).getHotHits(poolStats))(
        "delta_total_hits", poolState.at(it).getColdHits(poolStats) +
                                poolState.at(it).getWarmHits(poolStats) +
                                poolState.at(it).getHotHits(poolStats))(
        "class_oldest_element_age",
        poolEvictionAgeStats.getOldestElementAge(it))(
        "hot_queue_oldest_element_age",
        poolEvictionAgeStats.getHotEvictionStat(it).oldestElementAge)(
        "pool_all_slabs_allocated", cache.getPool(pid).allSlabsAllocated())(
        "smoothed_rank", classStates_[pid].smoothedRanks[it]);

    std::string jsonString = folly::toJson(logData);
    XLOGF(DBG, "Rebalance_states_logging: {}", jsonString);

    logArray.push_back(logData);
  }
  std::string jsonString = folly::toJson(logArray);
  XLOGF(DBG, "Rebalance_class_snapshot: {}", jsonString);

  if (!ctx.isEffective()) {
    ctx = kNoOpContext;
  } else {
    auto improvement =
        scores.at(ctx.receiverClassId) - scores.at(ctx.victimClassId);
    
    if ((minDiffInUse_ > 0 && improvement < minDiffInUse_) || 
          (config.minDiffRatio > 0 
            && improvement < config.minDiffRatio * scores.at(ctx.victimClassId))) {
      XLOGF(DBG,
            "Not enough to trigger rebalancing, receiver score: {}, victim "
            "score: {}, threshold1: {}, threshold2: {}",
            scores.at(ctx.receiverClassId),
            scores.at(ctx.victimClassId),
            minDiffInUse_, config.minDiffRatio);
      ctx = kNoOpContext;
    }
  }

  if (config.enableHoldOff) {
    for (const auto& cid : classes) {
      if (poolState[cid].decrementVictimHoldOff() && cid == ctx.victimClassId) {
        XLOGF(DBG, "Victim class {} is on hold-off, setting context to no-op.",
              static_cast<int>(cid));
        ctx = kNoOpContext;
      }

      if (poolState[cid].decrementReceiverHoldOff() &&
          cid == ctx.receiverClassId) {
        XLOGF(DBG,
              "Receiver class {} is on hold-off, setting context to no-op.",
              static_cast<int>(cid));
        ctx = kNoOpContext;
      }
    }
  }

  if (ctx.isEffective()) {
    ctx.diffValue =
        scores.at(ctx.receiverClassId) - scores.at(ctx.victimClassId);
    ctx.deltaDiffValue = lastDiffs_[pid] - ctx.diffValue;
    lastDiffs_[pid] = ctx.diffValue;
    ctx.normalizedRange = computeNormalizedMarginalHitsRange(scores);
    recordRebalanceEvent(pid, ctx);
    if (config.enableHoldOff) {
      poolState[ctx.receiverClassId].startVictimHoldOff();
      poolState[ctx.victimClassId].startReceiverHoldOff();
    }
    folly::dynamic logData = folly::dynamic::object(
        "receiver", folly::dynamic::object("id", ctx.receiverClassId)(
                        "score", scores.at(ctx.receiverClassId))(
                        "smoothed_rank",
                        classStates_[pid].smoothedRanks[ctx.receiverClassId]))(
        "victim", folly::dynamic::object("id", ctx.victimClassId)(
                      "score", scores.at(ctx.victimClassId))(
                      "smoothed_rank",
                      classStates_[pid].smoothedRanks[ctx.victimClassId]));

    std::string jsonString = folly::toJson(logData);
    XLOGF(DBG, "Marginal-hits-decision: {}", jsonString);
  }

  if (ctx.isEffective() && config.enableOnlineLearning) {


    if (modelTrainingNegativeSamples_[pid] >= config.minModelSampleSize && modelTrainingPositiveSamples_[pid] >= config.minModelSampleSize) {
      int prediction = predictForPool(pid, ctx.diffValue, ctx.deltaDiffValue);
      if (prediction == 1) {
        XLOGF(DBG,
              "Model prediction: 1 will be cancelled in the future, setting "
              "context to no-op.");
        ctx = kNoOpContext;
      }
    }

    if(ctx.isEffective()) {
      auto output = processBuffer(pid, ctx);
      for (const auto& [pastEvent, isCancelled] : output) {
        int y = isCancelled ? 1 : 0;
        fitModelForPool(pid, pastEvent.diffValue, pastEvent.deltaDiffValue, y);
      }
    }

  }
  for (const auto i : poolStats.getClassIds()) {
    poolState[i].updateHits(poolStats);
  }

  if(ctx.isEffective() || !config.onlyUpdateHitIfRebalance) {
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
  const auto config = getConfigCopy();
  std::unordered_map<ClassId, double> scores;
  for (auto info : poolState) {
    if (info.id != Slab::kInvalidClassId) {
      scores[info.id] = info.getMarginalHits(poolStats, tailSlabCnt);
    }
  }
  return scores;
}

double MarginalHitsStrategy::computeNormalizedMarginalHitsRange(
    const std::unordered_map<ClassId, double>& scores, double epsilon) const {
  if (scores.empty()) {
    return 1.0;
  }

  // Find the minimum and maximum values in the scores map
  auto [minIt, maxIt] = std::minmax_element(
      scores.begin(), scores.end(),
      [](const auto& lhs, const auto& rhs) { return lhs.second < rhs.second; });

  double min = minIt->second;
  double max = maxIt->second;

  // Compute the normalized marginal hits range
  return (max - min) / (max + min + epsilon);
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

void MarginalHitsStrategy::increaseRebalanceThreshold(double suggestedValue) {
  auto newValue = std::max(minDiffInUse_, std::ceil(suggestedValue) + 1);
  XLOGF(INFO, "increase rebalance threshold: before: {}, after: {}",
        minDiffInUse_, newValue);
  minDiffInUse_ = newValue;
}

void MarginalHitsStrategy::decreaseRebalanceThreshold() {
  auto newValue = std::max(minDiffInUse_ - 1, 1.0);
  if (newValue == minDiffInUse_) {
    return;
  }
  XLOGF(INFO, "decrease rebalance threshold: before: {}, after: {}",
        minDiffInUse_, newValue);
  minDiffInUse_ = newValue;
}

std::string MarginalHitsStrategy::RandomString(int len) {
  std::string ret;
  ret.resize(len);
  for (int i = 0; i < len; i++) {
    ret[i] = static_cast<char>(' ' + folly::Random::secureRand64(95)); //
  }
  return ret;
}

void MarginalHitsStrategy::initializeModelForPool(PoolId poolId) {
  if (!getConfigCopy().enableOnlineLearning) {
    XLOG(INFO) << "Online learning is disabled. Skipping model initialization "
                  "for PoolId: "
               << poolId;
    return;
  }

  std::string modelName = RandomString(16);
  auto modelApiClient =
      std::make_unique<ModelApiClient>("http://127.0.0.1:5000");
  modelNames_[poolId] = modelName;
  modelTrainingNegativeSamples_[poolId] = 0;
  modelTrainingPositiveSamples_[poolId] = 0;
  modelApiClients_[poolId] = std::move(modelApiClient);
  modelApiClients_[poolId]->createModel(modelName, config_.onlineLearningModel);

  XLOG(INFO) << "Initialized ModelApiClient for PoolId: " << poolId
             << " with model name: " << modelName;
}

ModelApiClient* MarginalHitsStrategy::getModelApiClient(PoolId poolId) {
  auto it = modelApiClients_.find(poolId);
  if (it != modelApiClients_.end()) {
    return it->second.get();
  }
  initializeModelForPool(poolId);
  auto newIt = modelApiClients_.find(poolId);
  return (newIt != modelApiClients_.end()) ? newIt->second.get() : nullptr;
}

void MarginalHitsStrategy::fitModelForPool(PoolId poolId, int x1, int x2, int y) {
  // Get the ModelApiClient for the given PoolId
  ModelApiClient* client = getModelApiClient(poolId);
  if (!client) {
    XLOG(WARNING) << "Failed to retrieve ModelApiClient for PoolId: " << poolId;
    return;
  }

  // Get the model name for the PoolId
  auto it = modelNames_.find(poolId);
  if (it == modelNames_.end()) {
    XLOG(WARNING) << "No model name found for PoolId: " << poolId;
    return;
  }
  const std::string& modelName = it->second;

  client->fitModel(modelName, x1, x2, y);
  if(y == 1) {
    modelTrainingPositiveSamples_[poolId]++;
  } else {
    modelTrainingNegativeSamples_[poolId]++;
  }
  XLOGF(DBG, "Called fitModel for PoolId: {}, x1: {}, x2: {}, y: {}",
    poolId, x1, x2, y);
}

int MarginalHitsStrategy::predictForPool(PoolId poolId, int x1, int x2) {
  // Get the ModelApiClient for the given PoolId
  ModelApiClient* client = getModelApiClient(poolId);
  if (!client) {
    XLOG(WARNING) << "Failed to retrieve ModelApiClient for PoolId: " << poolId;
    return -1; // Return an error code
  }

  // Get the model name for the PoolId
  auto it = modelNames_.find(poolId);
  if (it == modelNames_.end()) {
    XLOG(WARNING) << "No model name found for PoolId: " << poolId;
    return -1; // Return an error code
  }
  const std::string& modelName = it->second;

  // Call predict on the client
  int prediction = client->predict(modelName, x1, x2);
  XLOGF(DBG, "Called predict for PoolId: {}, x1: {}, x2: {}, prediction: {}",
    poolId, x1, x2, prediction);

  return prediction;
}

std::vector<std::pair<RebalanceContext, bool>>
MarginalHitsStrategy::processBuffer(PoolId poolId,
                                    const RebalanceContext& newEvent) {
  auto& buffer = eventBuffers_[poolId];
  std::vector<std::pair<RebalanceContext, bool>> output;
  std::deque<BufferedEvent> newBuffer;

  for (auto& bufferedEvent : buffer) {
    auto& pastEvent = bufferedEvent.event;
    auto& counter = bufferedEvent.counter;
    auto& cancelled = bufferedEvent.cancelled;

    if (!cancelled) {
      if (pastEvent.victimClassId == newEvent.receiverClassId ||
          pastEvent.receiverClassId == newEvent.victimClassId) {
        output.emplace_back(pastEvent, true);
        continue;
      }
    }
    counter++;
    if (counter >= config_.bufferSize) {
      // Timed out
      output.emplace_back(pastEvent, cancelled);
    } else {
      newBuffer.push_back(bufferedEvent);
    }
  }
  buffer = std::move(newBuffer);
  buffer.push_back({newEvent, 0, false});

  return output;
}

} // namespace facebook::cachelib
