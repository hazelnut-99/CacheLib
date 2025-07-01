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

#include "cachelib/cachebench/util/CacheConfig.h"

#include "cachelib/allocator/FreeMemStrategy.h"
#include "cachelib/allocator/HitsPerSlabStrategy.h"
#include "cachelib/allocator/HitsPerTailSlabStrategy.h"
#include "cachelib/allocator/LAMAStrategy.h"
#include "cachelib/allocator/MarginalHitsStrategyNew.h"
#include "cachelib/allocator/MarginalHitsStrategyOld.h"
#include "cachelib/allocator/LruTailAgeStrategy.h"
#include "cachelib/allocator/MarginalHitsStrategy.h"
#include "cachelib/allocator/RandomStrategy.h"
#include "cachelib/allocator/RandomStrategyNew.h"

namespace facebook {
namespace cachelib {
namespace cachebench {
CacheConfig::CacheConfig(const folly::dynamic& configJson) {
  JSONSetVal(configJson, allocator);
  JSONSetVal(configJson, cacheDir);
  JSONSetVal(configJson, cacheSizeMB);
  JSONSetVal(configJson, poolRebalanceIntervalSec);
  JSONSetVal(configJson, poolRebalancerFreeAllocThreshold);
  JSONSetVal(configJson, poolRebalancerDisableForcedWakeUp);
  JSONSetVal(configJson, wakeUpRebalancerEveryXReqs);
  JSONSetVal(configJson, anomalyDetectionFrequency);
  JSONSetVal(configJson, useAdaptiveRebalanceInterval);
  JSONSetVal(configJson, useAdaptiveRebalanceIntervalV2);
  JSONSetVal(configJson, syncRebalance);
  JSONSetVal(configJson, useAnomalyDetection);
  JSONSetVal(configJson, increaseIntervalFactor);
  JSONSetVal(configJson, moveOnSlabRelease);
  JSONSetVal(configJson, rebalanceStrategy);
  JSONSetVal(configJson, rebalanceMinSlabs);
  JSONSetVal(configJson, rebalanceDiffRatio);
  JSONSetVal(configJson, intervalAdjustmentStrategy);
  JSONSetVal(configJson, ewmaR);
  JSONSetVal(configJson, ewmaL);

  JSONSetVal(configJson, ltaMinTailAgeDifference);
  JSONSetVal(configJson, ltaNumSlabsFreeMem);
  JSONSetVal(configJson, ltaSlabProjectionLength);

  JSONSetVal(configJson, hpsMinDiff);
  JSONSetVal(configJson, hpsNumSlabsFreeMem);
  JSONSetVal(configJson, hpsMinLruTailAge);
  JSONSetVal(configJson, hpsMaxLruTailAge);

  JSONSetVal(configJson, fmNumFreeSlabs);
  JSONSetVal(configJson, fmMaxUnAllocatedSlabs);

  JSONSetVal(configJson, mhOnlyUpdateHitIfRebalance);
  JSONSetVal(configJson, mhMovingAverageParam);
  JSONSetVal(configJson, mhEnableOnlineLearning);
  JSONSetVal(configJson, mhOnlineLearningModel);
  JSONSetVal(configJson, resetIntervalTimings);
  JSONSetVal(configJson, mhMaxFreeMemSlabs);
  JSONSetVal(configJson, mhEnableHoldOff);
  JSONSetVal(configJson, mhMinDiff);
  JSONSetVal(configJson, mhMinDiffRatio);
  JSONSetVal(configJson, countColdTailHitsOnly);
  JSONSetVal(configJson, tailSlabCnt);
  JSONSetVal(configJson, enableShardsMrc);
  JSONSetVal(configJson, mhFilterReceiverByEvictionRate);
  JSONSetVal(configJson, mhDecayWithHits);
  JSONSetVal(configJson, mhAutoDecThreshold);
  JSONSetVal(configJson, mhAutoIncThreshold);
  JSONSetVal(configJson, mhUseProjectedScoreForVictim);
  JSONSetVal(configJson, mhMinModelSampleSize);
  JSONSetVal(configJson, mhBufferSize);
  JSONSetVal(configJson, lamaMinThreshold);
  JSONSetVal(configJson, thresholdAI);
  JSONSetVal(configJson, thresholdMI);
  JSONSetVal(configJson, thresholdAD);
  JSONSetVal(configJson, thresholdMD);
  JSONSetVal(configJson, emrLow);
  JSONSetVal(configJson, emrHigh);
  JSONSetVal(configJson, thresholdAIADStep);
  JSONSetVal(configJson, thresholdMIMDFactor);



  JSONSetVal(configJson, htBucketPower);
  JSONSetVal(configJson, htLockPower);

  JSONSetVal(configJson, lruRefreshSec);
  JSONSetVal(configJson, lruRefreshRatio);
  JSONSetVal(configJson, rebalanceOnRecordAccess);
  JSONSetVal(configJson, mmReconfigureIntervalSecs);
  JSONSetVal(configJson, lruUpdateOnWrite);
  JSONSetVal(configJson, lruUpdateOnRead);
  JSONSetVal(configJson, tryLockUpdate);
  JSONSetVal(configJson, lruIpSpec);
  JSONSetVal(configJson, useCombinedLockForIterators);

  JSONSetVal(configJson, lru2qHotPct);
  JSONSetVal(configJson, lru2qColdPct);

  JSONSetVal(configJson, allocFactor);
  JSONSetVal(configJson, maxAllocSize);
  JSONSetVal(configJson, minAllocSize);
  JSONSetVal(configJson, allocSizes);

  JSONSetVal(configJson, numPools);
  JSONSetVal(configJson, poolSizes);

  JSONSetVal(configJson, nvmCacheSizeMB);
  JSONSetVal(configJson, nvmCacheMetadataSizeMB);
  JSONSetVal(configJson, nvmCachePaths);
  JSONSetVal(configJson, writeAmpDeviceList);

  JSONSetVal(configJson, navyBlockSize);
  JSONSetVal(configJson, navyRegionSizeMB);
  JSONSetVal(configJson, navySegmentedFifoSegmentRatio);
  JSONSetVal(configJson, navyReqOrderShardsPower);
  JSONSetVal(configJson, navyBigHashSizePct);
  JSONSetVal(configJson, navyBigHashBucketSize);
  JSONSetVal(configJson, navyBloomFilterPerBucketSize);
  JSONSetVal(configJson, navySmallItemMaxSize);
  JSONSetVal(configJson, navyParcelMemoryMB);
  JSONSetVal(configJson, navyHitsReinsertionThreshold);
  JSONSetVal(configJson, navyProbabilityReinsertionThreshold);
  JSONSetVal(configJson, navyReaderThreads);
  JSONSetVal(configJson, navyWriterThreads);
  JSONSetVal(configJson, navyMaxNumReads);
  JSONSetVal(configJson, navyMaxNumWrites);
  JSONSetVal(configJson, navyStackSizeKB);
  JSONSetVal(configJson, navyQDepth);
  JSONSetVal(configJson, navyEnableIoUring);
  JSONSetVal(configJson, navyCleanRegions);
  JSONSetVal(configJson, navyCleanRegionThreads);
  JSONSetVal(configJson, navyAdmissionWriteRateMB);
  JSONSetVal(configJson, navyMaxConcurrentInserts);
  JSONSetVal(configJson, navyDataChecksum);
  JSONSetVal(configJson, navyNumInmemBuffers);
  JSONSetVal(configJson, truncateItemToOriginalAllocSizeInNvm);
  JSONSetVal(configJson, navyEncryption);
  JSONSetVal(configJson, deviceMaxWriteSize);
  JSONSetVal(configJson, deviceEnableFDP);

  JSONSetVal(configJson, memoryOnlyTTL);

  JSONSetVal(configJson, usePosixShm);
  JSONSetVal(configJson, lockMemory);
  if (configJson.count("memoryTiers")) {
    for (auto& it : configJson["memoryTiers"]) {
      memoryTierConfigs.push_back(
          MemoryTierConfig(it).getMemoryTierCacheConfig());
    }
  }

  JSONSetVal(configJson, useTraceTimeStamp);
  JSONSetVal(configJson, printNvmCounters);
  JSONSetVal(configJson, tickerSynchingSeconds);
  JSONSetVal(configJson, enableItemDestructorCheck);
  JSONSetVal(configJson, enableItemDestructor);
  JSONSetVal(configJson, nvmAdmissionRetentionTimeThreshold);

  JSONSetVal(configJson, customConfigJson);

  // todo add new parameters for configuring slab rebalance

  // if you added new fields to the configuration, update the JSONSetVal
  // to make them available for the json configs and increment the size
  // below
  checkCorrectSize<CacheConfig, 1056>();

  if (numPools != poolSizes.size()) {
    throw std::invalid_argument(folly::sformat(
        "number of pools must be the same as the pool size distribution. "
        "numPools: {}, poolSizes.size(): {}",
        numPools, poolSizes.size()));
  }
}

std::shared_ptr<RebalanceStrategy> CacheConfig::getRebalanceStrategy() const {
  if (poolRebalanceIntervalSec == 0) {
    return nullptr;
  }
  // todo: support free_mem and marginal hits
  if (rebalanceStrategy == "tail-age") {
    LruTailAgeStrategy::Config ltaConfig;
    ltaConfig.tailAgeDifferenceRatio = rebalanceDiffRatio;
    ltaConfig.minTailAgeDifference = ltaMinTailAgeDifference;
    ltaConfig.minSlabs = rebalanceMinSlabs;
    ltaConfig.numSlabsFreeMem = ltaNumSlabsFreeMem;
    ltaConfig.slabProjectionLength = ltaSlabProjectionLength;
    return std::make_shared<LruTailAgeStrategy>(ltaConfig);
  } else if (rebalanceStrategy == "hits") {
    HitsPerSlabStrategy::Config hpsConfig;
    hpsConfig.minDiff = hpsMinDiff;
    hpsConfig.diffRatio = rebalanceDiffRatio;
    hpsConfig.minSlabs = rebalanceMinSlabs;
    hpsConfig.numSlabsFreeMem = hpsNumSlabsFreeMem;
    hpsConfig.minLruTailAge = hpsMinLruTailAge;
    hpsConfig.maxLruTailAge = hpsMaxLruTailAge;
    return std::make_shared<HitsPerSlabStrategy>(hpsConfig);
  } else if (rebalanceStrategy == "hits-per-tail-slab") {
    HitsPerTailSlabStrategy::Config hptsConfig;
    hptsConfig.minDiff = hpsMinDiff;
    hptsConfig.diffRatio = rebalanceDiffRatio;
    hptsConfig.minSlabs = rebalanceMinSlabs;
    hptsConfig.numSlabsFreeMem = hpsNumSlabsFreeMem;
    hptsConfig.minLruTailAge = hpsMinLruTailAge;
    hptsConfig.maxLruTailAge = hpsMaxLruTailAge;
    hptsConfig.tailSlabCnt = tailSlabCnt;
    return std::make_shared<HitsPerTailSlabStrategy>(hptsConfig);
  }

  else if (rebalanceStrategy == "marginal-hits") {
    MarginalHitsStrategy::Config mhConfig;
    mhConfig.minSlabs = rebalanceMinSlabs;
    mhConfig.movingAverageParam = mhMovingAverageParam;
    mhConfig.onlyUpdateHitIfRebalance = mhOnlyUpdateHitIfRebalance;
    mhConfig.onlineLearningModel = mhOnlineLearningModel;
    mhConfig.maxFreeMemSlabs = mhMaxFreeMemSlabs;
    mhConfig.enableHoldOff = mhEnableHoldOff;
    mhConfig.minDiff = mhMinDiff;
    mhConfig.minDiffRatio = mhMinDiffRatio;
    mhConfig.tailSlabCnt = tailSlabCnt;
    mhConfig.decayWithHits = mhDecayWithHits;
    mhConfig.enableOnlineLearning = mhEnableOnlineLearning;
    mhConfig.filterReceiverByEvictionRate = mhFilterReceiverByEvictionRate;
    mhConfig.autoIncThreshold = mhAutoIncThreshold;
    mhConfig.autoDecThreshold = mhAutoDecThreshold;
    mhConfig.useProjectedScoreForVictim = mhUseProjectedScoreForVictim;
    mhConfig.minModelSampleSize = mhMinModelSampleSize;
    mhConfig.bufferSize = mhBufferSize;
    return std::make_shared<MarginalHitsStrategy>(mhConfig);
  } else if (rebalanceStrategy == "free-mem") {
    FreeMemStrategy::Config fmConfig;
    fmConfig.minSlabs = rebalanceMinSlabs;
    fmConfig.numFreeSlabs = fmNumFreeSlabs;
    fmConfig.maxUnAllocatedSlabs = fmMaxUnAllocatedSlabs;
    return std::make_shared<FreeMemStrategy>(fmConfig);
  } else if (rebalanceStrategy == "default") {
    // the default strategy, only rebalance when allocation failures happen.
    return std::make_shared<RebalanceStrategy>();
  } else if (rebalanceStrategy == "lama") {
    LAMAStrategy::Config lamaConfig;
    lamaConfig.missRatioImprovementThreshold = lamaMinThreshold;
    return std::make_shared<LAMAStrategy>(lamaConfig);
  } else if (rebalanceStrategy == "marginal-hits-new") {
    MarginalHitsStrategyNew::Config mhNewConfig;
    mhNewConfig.minSlabs = rebalanceMinSlabs;
    mhNewConfig.movingAverageParam = mhMovingAverageParam;
    mhNewConfig.onlyUpdateHitIfRebalance = mhOnlyUpdateHitIfRebalance;
    mhNewConfig.maxFreeMemSlabs = mhMaxFreeMemSlabs;
    mhNewConfig.minDiff = mhMinDiff;
    mhNewConfig.minDiffRatio = mhMinDiffRatio;
    mhNewConfig.thresholdAI = thresholdAI;
    mhNewConfig.thresholdAD = thresholdAD;
    mhNewConfig.thresholdMI = thresholdMI;
    mhNewConfig.thresholdMD = thresholdMD;
    mhNewConfig.minRequestsObserved = minRequestsObserved;

    return std::make_shared<MarginalHitsStrategyNew>(mhNewConfig);
  } else if (rebalanceStrategy == "marginal-hits-old") {
    MarginalHitsStrategyOld::Config mhOldConfig;
    mhOldConfig.minSlabs = rebalanceMinSlabs;
    mhOldConfig.movingAverageParam = mhMovingAverageParam;
    mhOldConfig.maxFreeMemSlabs = mhMaxFreeMemSlabs;
    return std::make_shared<MarginalHitsStrategyOld>(mhOldConfig);
  } else {
    // use random strategy (custom impl)
    return std::make_shared<RandomStrategy>(RandomStrategy::Config{
        static_cast<unsigned int>(rebalanceMinSlabs)});
  }
}

MemoryTierConfig::MemoryTierConfig(const folly::dynamic& configJson) {
  JSONSetVal(configJson, ratio);
  JSONSetVal(configJson, memBindNodes);

  checkCorrectSize<MemoryTierConfig, 40>();
}
} // namespace cachebench
} // namespace cachelib
} // namespace facebook
