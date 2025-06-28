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

#include "cachelib/cachebench/runner/Runner.h"

#include "cachelib/cachebench/runner/Stressor.h"
#include <folly/Random.h>
#include <folly/dynamic.h>
#include <folly/json.h>
#include <fstream>

namespace facebook {
namespace cachelib {
namespace cachebench {
Runner::Runner(const CacheBenchConfig& config)
    : stressor_{Stressor::makeStressor(config.getCacheConfig(),
                                       config.getStressorConfig())} {}

bool Runner::run(std::chrono::seconds progressInterval,
                 const std::string& progressStatsFile,
                 const std::string& dumpResultJsonFile,
                 bool disableProgressTracker,
         const std::string& dumpTxFile) {
  ProgressTracker tracker{*stressor_, progressStatsFile};

  stressor_->start();

  if (!disableProgressTracker) {
    if (!tracker.start(progressInterval)) {
      throw std::runtime_error("Cannot start ProgressTracker.");
    }
  } else {
    std::cout << "Progress tracker is disabled. " << std::endl;
  }

  stressor_->finish();

  uint64_t durationNs = stressor_->getTestDurationNs();
  auto cacheStats = stressor_->getCacheStats();
  auto opsStats = stressor_->aggregateThroughputStats();
  if (!disableProgressTracker) {
    tracker.stop();
  }

  std::cout << "== Test Results ==\n== Allocator Stats ==" << std::endl;
  cacheStats.render(std::cout);

  std::cout << "\n== Throughput for  ==\n";
  std::cout << "\n== Duration: " << (static_cast<double>(durationNs) / 1e9) << " s) ==\n";
  std::cout << "== Ops: " << opsStats.ops << " ==\n";
  std::cout << "== Throughput: "
          << opsStats.ops / (static_cast<double>(durationNs) / 1e9)  << " ==\n";
  
    if (!dumpTxFile.empty()) {
      auto uuid = folly::sformat("{:016x}-{:016x}",
                                folly::Random::secureRand64(),
                                folly::Random::secureRand64());
      std::string outFile = dumpTxFile + "." + uuid + ".json";

    folly::dynamic j = folly::dynamic::object;
    j["duration_ns"] = durationNs;
    j["ops"] = opsStats.ops;
    j["throughput"] = static_cast<double>(opsStats.ops) / (static_cast<double>(durationNs) / 1e9);

    std::ofstream ofs(outFile);
    if (ofs) {
      ofs << folly::toPrettyJson(j) << std::endl;
      std::cout << "== Throughput JSON written to " << outFile << std::endl;
    } else {
      std::cerr << "Failed to write throughput JSON to " << outFile << std::endl;
    }
  }

  opsStats.render(durationNs, std::cout);

  if (!dumpResultJsonFile.empty()) {
    std::cout << "== Writing result to json file" <<  dumpResultJsonFile  << std::endl;
    cacheStats.renderToJson(dumpResultJsonFile);
  }

  stressor_->renderWorkloadGeneratorStats(durationNs, std::cout);
  std::cout << std::endl;

  stressor_.reset();

  bool passed = cacheStats.renderIsTestPassed(std::cout);
  if (aborted_) {
    std::cerr << "Test aborted.\n";
    passed = false;
  }
  return passed;
}

bool Runner::run(folly::UserCounters& counters) {
  stressor_->start();
  stressor_->finish();

  BENCHMARK_SUSPEND {
    uint64_t durationNs = stressor_->getTestDurationNs();
    auto cacheStats = stressor_->getCacheStats();
    auto opsStats = stressor_->aggregateThroughputStats();

    // Allocator Stats
    cacheStats.render(counters);

    // Throughput
    opsStats.render(durationNs, counters);

    stressor_->renderWorkloadGeneratorStats(durationNs, counters);

    counters["nvm_disable"] = cacheStats.isNvmCacheDisabled ? 100 : 0;
    counters["inconsistency_count"] = cacheStats.inconsistencyCount * 100;

    stressor_.reset();
  }

  if (aborted_) {
    std::cerr << "Test aborted.\n";
    return false;
  }
  return true;
}

} // namespace cachebench
} // namespace cachelib
} // namespace facebook
