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

 #include <folly/Random.h>
 
 #include "cachelib/allocator/RebalanceStrategy.h"
 
 namespace facebook {
 namespace cachelib {
 
 // similar to random eviction in https://blog.x.com/engineering/en_us/a/2012/caching-with-twemcache 
 // pick a random victim, no receiver specified.
 class RandomStrategyNew : public RebalanceStrategy {
  public:
   struct Config : public BaseConfig {
     Config() = default;
     explicit Config(unsigned int m) : minSlabs(m) {}
     unsigned int minSlabs{1};
   };
 
   RandomStrategyNew() = default;
   explicit RandomStrategyNew(Config c) : RebalanceStrategy(Random), config_{c} {}
 
   RebalanceContext pickVictimAndReceiverImpl(const CacheBase&,
                                              PoolId pid,
                                              const PoolStats& stats) final {
     auto victims = stats.getClassIds();   
     victims =
      filterByNumEvictableSlabs(stats, std::move(victims), config_.minSlabs);
     victims = filterVictimsByHoldOff(pid, stats, std::move(victims));  
                                        
     if (victims.empty()) {
        XLOG(DBG, "Rebalancing: No victims available");
        return kNoOpContext;
     }
     const auto victim = pickRandom(victims);

     RebalanceContext ctx;
     ctx.victimClassId = victim;
     if (ctx.victimClassId == Slab::kInvalidClassId) {
        return kNoOpContext;
     }

     XLOGF(DBG, "Rebalancing: victimAC = {}", static_cast<int>(ctx.victimClassId));
     return ctx;
   }
 
   void updateConfig(const BaseConfig& baseConfig) override {
     config_ = static_cast<const Config&>(baseConfig);
   }
 
  private:
   ClassId pickRandom(const std::set<ClassId>& classIds) {
     auto r = folly::Random::rand32(0, classIds.size());
     for (auto c : classIds) {
       if (r-- == 0) {
         return c;
       }
     }
     return Slab::kInvalidClassId;
   }
 
   Config config_{};
 };
 } // namespace cachelib
 } // namespace facebook
 