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

#pragma once

#include "cachelib/allocator/RebalanceStrategy.h"

namespace facebook {
namespace cachelib {

class LAMAStrategy : public RebalanceStrategy {
 public:
    
   struct Config : public BaseConfig {
    unsigned int maxSlabsToMove{50};  //the N parameter as in the paper.
    /*
    * Once the new allocation is determined, 
    * it is compared with the previous allocation to see if the performance improvement is above a certain threshold. 
    * If it is, slabs are reassigned to change the allocation.
    */
    double missRatioImprovementThreshold{0.005}; 
    Config() noexcept {}
   };

  void updateConfig(const BaseConfig& baseConfig) override {
    std::lock_guard<std::mutex> l(configLock_);
    config_ = static_cast<const Config&>(baseConfig);
  }

  explicit LAMAStrategy(Config config = {});

 protected:
 
  Config getConfigCopy() const {
    std::lock_guard<std::mutex> l(configLock_);
    return config_;
  }

  RebalanceContext pickVictimAndReceiverImpl(
      const CacheBase& cache,
      PoolId pid,
      const PoolStats& poolStats) override final;

  ClassId pickVictimImpl(const CacheBase& cache,
                         PoolId pid,
                         const PoolStats& poolStats) override final;


  private:
    std::tuple<double, std::vector<std::pair<ClassId, ClassId>>> pickVictimReceiverPairs(
      const CacheBase& cache,
      PoolId pid,
      const PoolStats& poolStats);

    Config config_;
    mutable std::mutex configLock_;

};


} // namespace cachelib
} // namespace facebook
