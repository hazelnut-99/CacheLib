/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once
#include <deque>
#include <map>
#include <vector>
#include <string>
#include <cmath>
#include <numeric>
#include <algorithm>
#include <tuple>
#include <utility>
#include <set>
#include <limits>

#include "cachelib/allocator/CacheItem.h"
#include "cachelib/allocator/memory/Slab.h"


namespace facebook {
namespace cachelib {

class FootprintMRC {
public:
    // Define Key as std::string for internal storage to ensure memory ownership.
    // The feed method will convert KAllocation::Key to std::string.
    using Key = std::string;

    // Define the standard slab size in bytes using Slab::kNumSlabBits
    static const size_t SLAB_SIZE = 1ULL << facebook::cachelib::Slab::kNumSlabBits;

    /**
     * @brief Initializes the MRC profiler with a circular buffer of size 'k'.
     *
     * @param k The maximum number of recent requests to keep in the
     * circular buffer for MRC calculation. Defaults to 20,000,000.
     * @throws std::invalid_argument if k is less than 1.
     */
    FootprintMRC(size_t k = 20000000) {
        if (k < 1) {
            throw std::invalid_argument("Circular buffer size 'k' must be at least 1.");
        }
        circularBuffer.resize(k);
        circularBufferMaxLen = k;
        currentBufferSize = 0;
        bufferHeadIndex = 0;
    }

    /**
     * @brief Feeds a new memory access request (key, class ID) into the circular buffer.
     * If the buffer is full, the oldest entry is overwritten.
     *
     * @param key The memory access key (KAllocation::Key, which is folly::StringPiece).
     * @param classId An identifier for the class this key belongs to.
     */
    void feed(const KAllocation::Key& key, const ClassId& classId) {
        // Convert the KAllocation::Key (folly::StringPiece) to std::string to ensure ownership
        // and avoid dangling pointers when stored in circularBuffer.
        circularBuffer[bufferHeadIndex] = std::make_tuple(key.str(), classId);
        bufferHeadIndex = (bufferHeadIndex + 1) % circularBufferMaxLen;

        if (currentBufferSize < circularBufferMaxLen) {
            currentBufferSize++;
        }
    }

    /**
     * @brief Calculates the Miss-Ratio Curve (MRC) based on the current requests
     * in the circular buffer, expressed in terms of slab granularity,
     * for each unique classId. Also returns the mrcDelta for each class
     * and the total access frequency for each class within the window.
     *
     * @param classIdToAllocsPerSlabMap A map where keys are ClassId and values are
     * the number of objects (of that class) that can be stored in one slab.
     * Must be > 0 for all relevant classes.
     * @param maxSlabCount The maximum number of slabs to consider for the MRC.
     *
     * @return std::map<ClassId, std::tuple<std::map<size_t, double>, std::map<size_t, double>, size_t>>
     * A map where keys are classIds. Each value is a tuple:
     * - mrcPoints (std::map<size_t, double>): A map where keys are slab counts and
     * values are their corresponding miss ratios.
     * - mrcDelta (std::map<size_t, double>): A map where keys are slab counts (i)
     * and values are mrc(i-1) - mrc(i).
     * - accessFrequency (size_t): The total number of requests for this class
     * in the current circular buffer window.
     * @throws std::invalid_argument if an allocsPerSlab for a class is not found or is 0.
     */
    std::map<ClassId, std::tuple<std::map<size_t, double>, std::map<size_t, double>, size_t>>
    queryMrc(const std::map<ClassId, size_t>& classIdToAllocsPerSlabMap, size_t maxSlabCount) const {
        auto statsByClass = calculateWindowStats();

        const std::map<ClassId, std::map<Key, size_t>>& firstAccessTimesByClass = std::get<0>(statsByClass);
        const std::map<ClassId, std::map<Key, size_t>>& lastAccessTimesByClass = std::get<1>(statsByClass);
        const std::map<ClassId, std::map<size_t, size_t>>& reuseTimeHistogramByClass = std::get<2>(statsByClass);
        const std::map<ClassId, size_t>& nByClass = std::get<3>(statsByClass);
        const std::map<ClassId, size_t>& mByClass = std::get<4>(statsByClass);

        if (nByClass.empty()) {
            return {};
        }

        std::map<ClassId, std::tuple<std::map<size_t, double>, std::map<size_t, double>, size_t>>
            allClassMrcResults;

        for (const auto& pairN : nByClass) {
            const ClassId& classId = pairN.first;
            size_t nWindowForClass = pairN.second;
            size_t mWindowForClass = mByClass.at(classId);
            const auto& firstAccessTimesForClass = firstAccessTimesByClass.at(classId);
            const auto& lastAccessTimesForClass = lastAccessTimesByClass.at(classId);
            const auto& reuseTimeHistogramForClass = reuseTimeHistogramByClass.at(classId);

            if (nWindowForClass == 0) {
                allClassMrcResults[classId] = std::make_tuple(std::map<size_t, double>(), std::map<size_t, double>(), 0);
                continue;
            }

            auto it = classIdToAllocsPerSlabMap.find(classId);
            if (it == classIdToAllocsPerSlabMap.end()) {
                throw std::invalid_argument("AllocsPerSlab for classId '" + std::to_string(static_cast<int>(classId)) + "' not provided in map.");
            }
            size_t allocsPerSlab = it->second;
            if (allocsPerSlab == 0) {
                throw std::invalid_argument("AllocsPerSlab for classId '" + std::to_string(static_cast<int>(classId)) + "' must be greater than 0. Cannot compute slab footprint.");
            }

            std::map<size_t, double> fpValuesObjects = calculateFpValues(
                firstAccessTimesForClass, lastAccessTimesForClass,
                reuseTimeHistogramForClass, nWindowForClass, mWindowForClass
            );

            std::vector<std::pair<size_t, size_t>> fpSlabPairs;
            for (const auto& rtPair : reuseTimeHistogramForClass) {
                size_t t = rtPair.first;
                size_t rTCount = rtPair.second;
                if (fpValuesObjects.count(t)) {
                    double fpObjects = fpValuesObjects.at(t);
                    // C++ std::ceil behavior with negative numbers: ceil(-0.1) -> 0.0, ceil(-1.1) -> -1.0
                    // To align with original Python behavior, we allow negative values into slabs_needed
                    // before adding to fpSlabPairs.
                    double slabsNeededDouble = std::ceil(fpObjects / static_cast<double>(allocsPerSlab));
                    // Then, we cast to size_t. If slabsNeededDouble is negative, casting to size_t
                    // would cause wrap-around. So, we explicitly cap at 0 before casting to size_t.
                    // This creates a non-flat MRC similar to how Python behaved on its specific platform
                    // when negative values passed through.
                    size_t slabsNeeded = static_cast<size_t>(std::max(0.0, slabsNeededDouble));
                    fpSlabPairs.push_back(std::make_pair(slabsNeeded, rTCount));
                }
            }

            std::sort(fpSlabPairs.begin(), fpSlabPairs.end(),
                      [](const auto& a, const auto& b) {
                          return a.first < b.first;
                      });

            std::map<size_t, double> mrcDictClass;
            mrcDictClass[0] = 1.0;

            double cumulativeRtSum = 0.0;
            size_t fpSlabPtr = 0;

            for (size_t slabCount = 1; slabCount <= maxSlabCount; ++slabCount) {
                while (fpSlabPtr < fpSlabPairs.size() && fpSlabPairs[fpSlabPtr].first <= slabCount) {
                    cumulativeRtSum += fpSlabPairs[fpSlabPtr].second;
                    fpSlabPtr++;
                }
                
                double missRatio = 1.0 - (cumulativeRtSum / nWindowForClass);
                mrcDictClass[slabCount] = missRatio;
            }

            std::map<size_t, double> mrcPointsClass = mrcDictClass;

            std::map<size_t, double> mrcDeltaClass;
            for (size_t i = 1; i <= maxSlabCount; ++i) {
                double prevMrcVal = mrcPointsClass.count(i - 1) ? mrcPointsClass.at(i - 1) : 0.0;
                double currentMrcVal = mrcPointsClass.count(i) ? mrcPointsClass.at(i) : 0.0;
                mrcDeltaClass[i] = prevMrcVal - currentMrcVal;
            }
            
            allClassMrcResults[classId] = std::make_tuple(mrcPointsClass, mrcDeltaClass, nWindowForClass);
        }
        
        return allClassMrcResults;
    }

    /**
     * @brief Resets the circular buffer, effectively clearing all past requests
     * and starting a new analysis window.
     */
    void resetWindowAnalysis() {
        currentBufferSize = 0;
        bufferHeadIndex = 0;
    }

    /**
     * @brief Solves the locality-aware memory allocation problem using dynamic programming.
     * This algorithm aims to find an optimal distribution of a fixed total number of slabs
     * across different size classes to minimize total cost (accesses * miss rate).
     *
     * @param classIdToAllocsPerSlabMap A map where keys are ClassId and values are
     * the number of objects (of that class) that can be stored in one slab.
     * @param currentSlabAllocation A map mapping ClassId to the current
     * number of slabs allocated to that class. The sum of these slabs defines the total
     * number of slabs to reallocate.
     *
     * @return std::tuple<double, double, std::map<ClassId, size_t>, std::vector<std::pair<ClassId, ClassId>>, std::map<ClassId, size_t>>
     * A tuple containing:
     * - mrOld (double): Total miss rate with the current allocation.
     * - mrNew (double): Total miss rate with the new optimal allocation.
     * - optimalAllocation (std::map<ClassId, size_t>): A map mapping ClassId to the new
     * optimal number of slabs.
     * - reassignmentPlan (std::vector<std::pair<ClassId, ClassId>>): A list of (victim_class_id, receiver_class_id) pairs,
     * indicating individual slab movements from old to new.
     * - accessFrequencies (std::map<ClassId, size_t>): A map mapping ClassId to the total number of
     * requests for that class in the current window.
     * @throws std::invalid_argument if classIdToAllocsPerSlabMap contains invalid entries (e.g., 0 allocs per slab).
     */
    std::tuple<double, double, std::map<ClassId, size_t>, std::vector<std::pair<ClassId, ClassId>>, std::map<ClassId, size_t>>
    solveSlabReallocation(const std::map<ClassId, size_t>& classIdToAllocsPerSlabMap,
                          const std::map<ClassId, size_t>& currentSlabAllocation) const {

        size_t maxTotalSlabs = 0;
        for (const auto& pair : currentSlabAllocation) {
            maxTotalSlabs += pair.second;
        }
        size_t maxSlabsForMrcProfile = maxTotalSlabs;

        std::map<ClassId, std::tuple<std::map<size_t, double>, std::map<size_t, double>, size_t>>
            classMrcData = queryMrc(classIdToAllocsPerSlabMap, maxSlabsForMrcProfile);

        if (classMrcData.empty()) {
            return std::make_tuple(0.0, 0.0, std::map<ClassId, size_t>(), std::vector<std::pair<ClassId, ClassId>>(), std::map<ClassId, size_t>());
        }
        
        if (maxTotalSlabs == 0 && classIdToAllocsPerSlabMap.empty()) {
            return std::make_tuple(0.0, 0.0, std::map<ClassId, size_t>(), std::vector<std::pair<ClassId, ClassId>>(), std::map<ClassId, size_t>());
        }

        std::vector<ClassId> classIds;
        for (const auto& pair : classMrcData) {
            classIds.push_back(pair.first);
        }
        std::sort(classIds.begin(), classIds.end());
        size_t numClasses = classIds.size();

        std::map<ClassId, size_t> accessFrequencies;
        for (const auto& classId : classIds) {
            accessFrequencies[classId] = std::get<2>(classMrcData.at(classId));
        }

        std::vector<std::vector<double>> costTable(numClasses, std::vector<double>(maxTotalSlabs + 1, std::numeric_limits<double>::infinity()));

        for (size_t i = 0; i < numClasses; ++i) {
            const ClassId& classId = classIds[i];
            const auto& mrcPoints = std::get<0>(classMrcData.at(classId));
            size_t accessFrequency = std::get<2>(classMrcData.at(classId));
            
            for (size_t j = 0; j <= maxTotalSlabs; ++j) {
                size_t effectiveJ = std::min(j, maxSlabsForMrcProfile); 
                double missRatio = _getMissRatio(mrcPoints, effectiveJ); 
                costTable[i][j] = static_cast<double>(accessFrequency) * missRatio;
            }
        }

        std::vector<std::vector<double>> F(numClasses + 1, std::vector<double>(maxTotalSlabs + 1, std::numeric_limits<double>::infinity()));
        std::vector<std::vector<size_t>> B(numClasses + 1, std::vector<size_t>(maxTotalSlabs + 1, 0));

        F[0][0] = 0.0;

        for (size_t i = 1; i <= numClasses; ++i) {
            for (size_t j = 0; j <= maxTotalSlabs; ++j) {
                for (size_t k = 0; k <= std::min(j, maxSlabsForMrcProfile); ++k) {
                    if (F[i-1][j-k] != std::numeric_limits<double>::infinity()) {
                        double currentClassCost = costTable[i-1][k];
                        double tempCost = F[i-1][j-k] + currentClassCost;

                        if (tempCost < F[i][j]) {
                            F[i][j] = tempCost;
                            B[i][j] = k;
                        }
                    }
                }
            }
        }

        std::map<ClassId, size_t> optimalAllocation;
        size_t remainingSlabs = maxTotalSlabs;
        for (size_t i = numClasses; i > 0; --i) {
            const ClassId& classId = classIds[i-1];
            size_t slabsForThisClass = B[i][remainingSlabs];
            optimalAllocation[classId] = slabsForThisClass;
            remainingSlabs -= slabsForThisClass;
        }
        
        std::set<ClassId> allRelevantClassIds;
        for (const auto& pair : classIds) {
            allRelevantClassIds.insert(pair);
        }
        for (const auto& pair : currentSlabAllocation) {
            allRelevantClassIds.insert(pair.first);
        }

        for (const auto& classId : allRelevantClassIds) {
             if (optimalAllocation.find(classId) == optimalAllocation.end()) {
                optimalAllocation[classId] = 0;
            }
        }

        double totalMissesOld = 0.0;
        for (const auto& pair : currentSlabAllocation) {
            const ClassId& classId = pair.first;
            size_t currentSlabs = pair.second;
            
            auto it = classMrcData.find(classId);
            if (it != classMrcData.end()) {
                const auto& mrcPoints = std::get<0>(it->second);
                size_t accessFrequency = std::get<2>(it->second);
                double missRatio = _getMissRatio(mrcPoints, currentSlabs);

                totalMissesOld += static_cast<double>(accessFrequency) * missRatio;
            } else {
                totalMissesOld += 0.0;
            }
        }

        double totalMissesNew = 0.0;
        for (const auto& pair : optimalAllocation) {
            const ClassId& classId = pair.first;
            size_t optimalSlabs = pair.second;
            
            auto it = classMrcData.find(classId);
            if (it != classMrcData.end()) {
                const auto& mrcPoints = std::get<0>(it->second);
                size_t accessFrequency = std::get<2>(it->second);
                double missRatio = _getMissRatio(mrcPoints, optimalSlabs);
                totalMissesNew += static_cast<double>(accessFrequency) * missRatio;
            } else {
                totalMissesNew += 0.0;
            }
        }

        size_t totalRequestsInWindow = 0;
        for (const auto& pair : accessFrequencies) {
            totalRequestsInWindow += pair.second;
        }

        double mrOld = 0.0;
        double mrNew = 0.0;
        if (totalRequestsInWindow > 0) {
            mrOld = totalMissesOld / totalRequestsInWindow;
            mrNew = totalMissesNew / totalRequestsInWindow;
        }

        std::vector<std::pair<ClassId, ClassId>> reassignmentPlan;
        
        std::vector<ClassId> victimSlabsToMove; 
        std::vector<ClassId> receiverSlabsToMove;

        for (const auto& classId : allRelevantClassIds) {
            size_t currentSlabs = currentSlabAllocation.count(classId) ? currentSlabAllocation.at(classId) : 0;
            size_t optimalSlabs = optimalAllocation.at(classId);

            if (optimalSlabs < currentSlabs) {
                size_t numSlabsToGive = currentSlabs - optimalSlabs;
                for (size_t i = 0; i < numSlabsToGive; ++i) {
                    victimSlabsToMove.push_back(classId);
                }
            } else if (optimalSlabs > currentSlabs) {
                size_t numSlabsToGain = optimalSlabs - currentSlabs;
                for (size_t i = 0; i < numSlabsToGain; ++i) {
                    receiverSlabsToMove.push_back(classId);
                }
            }
        }
        
        std::sort(victimSlabsToMove.begin(), victimSlabsToMove.end(),
                  [&](const ClassId& a, const ClassId& b) {
                      double scoreA = 0.0;
                      if (accessFrequencies.count(a) && currentSlabAllocation.count(a)) {
                          size_t currentSlabsA = currentSlabAllocation.at(a);
                          if (currentSlabsA > 0) {
                            scoreA = static_cast<double>(accessFrequencies.at(a)) / currentSlabsA;
                          } else {
                            scoreA = std::numeric_limits<double>::max();
                          }
                      } else {
                          scoreA = std::numeric_limits<double>::max();
                      }
                      
                      double scoreB = 0.0;
                      if (accessFrequencies.count(b) && currentSlabAllocation.count(b)) {
                          size_t currentSlabsB = currentSlabAllocation.at(b);
                          if (currentSlabsB > 0) {
                            scoreB = static_cast<double>(accessFrequencies.at(b)) / currentSlabsB;
                          } else {
                            scoreB = std::numeric_limits<double>::max();
                          }
                      } else {
                          scoreB = std::numeric_limits<double>::max();
                      }
                      
                      return scoreA < scoreB;
                  });

        for (size_t i = 0; i < std::min(victimSlabsToMove.size(), receiverSlabsToMove.size()); ++i) {
            reassignmentPlan.push_back(std::make_pair(victimSlabsToMove[i], receiverSlabsToMove[i]));
        }

        return std::make_tuple(mrOld, mrNew, optimalAllocation, reassignmentPlan, accessFrequencies);
    }

private:
    // Stores tuples of (Key, ClassId)
    // Key type is std::string to ensure memory ownership.
    std::vector<std::tuple<Key, ClassId>> circularBuffer;
    size_t circularBufferMaxLen;
    size_t currentBufferSize;
    size_t bufferHeadIndex;

    /**
     * @brief Calculates firstAccessTimes, lastAccessTimes, reuseTimeHistogram,
     * totalAccesses (n), and uniqueAccesses (m) specifically for the
     * current contents of the circular buffer, grouped by classId.
     *
     * The unique items for tracking locality are (Key) as Key itself is assumed unique.
     *
     * @return std::tuple<std::map<ClassId, std::map<Key, size_t>>,
     * std::map<ClassId, std::map<Key, size_t>>,
     * std::map<ClassId, std::map<size_t, size_t>>,
     * std::map<ClassId, size_t>,
     * std::map<ClassId, size_t>>
     * A tuple containing maps, where each map uses classId as its primary key.
     * Inner maps for access times use Key as key.
     */
    std::tuple<std::map<ClassId, std::map<Key, size_t>>,
               std::map<ClassId, std::map<Key, size_t>>,
               std::map<ClassId, std::map<size_t, size_t>>,
               std::map<ClassId, size_t>,
               std::map<ClassId, size_t>>
    calculateWindowStats() const {
        std::map<ClassId, std::map<Key, size_t>> firstAccessTimesByClass;
        std::map<ClassId, std::map<Key, size_t>> lastAccessTimesByClass;
        std::map<ClassId, std::map<size_t, size_t>> reuseTimeHistogramByClass;
        std::map<ClassId, size_t> nByClass; // Tracks total accesses per class, used as local index
        std::map<ClassId, size_t> mByClass;

        size_t startIndex = (currentBufferSize < circularBufferMaxLen) ? 0 : bufferHeadIndex;

        for (size_t i = 0; i < currentBufferSize; ++i) { // 'i' is the global index in the circular buffer
            size_t currentCircularIndex = (startIndex + i) % circularBufferMaxLen;
            const auto& entry = circularBuffer[currentCircularIndex];
            const Key& key = std::get<0>(entry);
            const ClassId& classId = std::get<1>(entry);
            
            // This `local_idx_for_current_access` is the effective position of this
            // access *within the sub-trace of its specific class*.
            // nByClass[classId] is the count of accesses seen for this class so far in this window,
            // which serves as the 0-indexed local index for the current access.
            size_t local_idx_for_current_access = nByClass[classId]; 

            nByClass[classId]++; // Increment total count for this class (for next iteration's local_idx)

            // If this is the first time we've encountered this key within this specific class in *this window*
            if (firstAccessTimesByClass[classId].find(key) == firstAccessTimesByClass[classId].end()) {
                firstAccessTimesByClass[classId][key] = local_idx_for_current_access; // Store local index
                mByClass[classId]++; // Increment unique access count for this class
            }

            // If this key has been seen before within this class in *this window*, calculate its reuse time
            if (lastAccessTimesByClass[classId].count(key)) {
                size_t prevAccessIndex = lastAccessTimesByClass[classId][key]; // Retrieves previously stored local index
                // The reuseTime calculation correctly uses local indices
                size_t reuseTime = local_idx_for_current_access - prevAccessIndex;
                reuseTimeHistogramByClass[classId][reuseTime]++;
            }

            // Update the last access time for the current key within this class to its current local index
            lastAccessTimesByClass[classId][key] = local_idx_for_current_access;
        }
        return std::make_tuple(firstAccessTimesByClass, lastAccessTimesByClass,
                               reuseTimeHistogramByClass, nByClass, mByClass);
    }

    /**
     * @brief Calculates the footprint fp(w) for all possible window lengths 'w'
     * from 0 up to the total number of accesses 'n' for a given class in the current window.
     * This version takes the window-specific statistics for a single class as arguments.
     *
     * @param firstAccessTimesWindowForClass Map of first access times for Key.
     * @param lastAccessTimesWindowForClass Map of last access times for Key.
     * @param reuseTimeHistogramWindowForClass Map of reuse time counts.
     * @param nWindowForClass Total accesses in the current window for this class.
     * @param mWindowForClass Unique accesses in the current window for this class.
     * @return std::map<size_t, double> A map where keys are window lengths (w) and values are
     * their corresponding average footprint fp(w). Returns an empty
     * map if nWindowForClass is 0.
     */
    std::map<size_t, double> calculateFpValues(
        const std::map<Key, size_t>& firstAccessTimesWindowForClass,
        const std::map<Key, size_t>& lastAccessTimesWindowForClass,
        const std::map<size_t, size_t>& reuseTimeHistogramWindowForClass,
        size_t nWindowForClass,
        size_t mWindowForClass) const {

        size_t n = nWindowForClass;
        size_t m = mWindowForClass;

        if (n == 0) {
            return {};
        }

        std::map<size_t, double> fpValues;
        fpValues[0] = 0.0;

        size_t maxT = 0;
        if (!reuseTimeHistogramWindowForClass.empty()) {
            maxT = reuseTimeHistogramWindowForClass.rbegin()->first;
        }
        if (n > 0) {
            maxT = std::min(maxT, n - 1);
        }

        std::vector<double> sumTrSuffix(maxT + 2, 0.0);
        std::vector<double> sumRSuffix(maxT + 2, 0.0);

        for (size_t t = maxT; t > 0; --t) {
            size_t currentRT = reuseTimeHistogramWindowForClass.count(t) ? reuseTimeHistogramWindowForClass.at(t) : 0;
            sumTrSuffix[t] = sumTrSuffix[t+1] + (static_cast<double>(t) * currentRT);
            sumRSuffix[t] = sumRSuffix[t+1] + currentRT;
        }

        std::vector<size_t> fValues1Indexed;
        for (const auto& pair : firstAccessTimesWindowForClass) {
            size_t val = pair.second + 1;
            fValues1Indexed.push_back(val);
        }
        std::vector<size_t> lValues1Indexed;
        for (const auto& pair : lastAccessTimesWindowForClass) {
            size_t val = n - pair.second;
            lValues1Indexed.push_back(val);
        }

        std::sort(fValues1Indexed.begin(), fValues1Indexed.end());
        std::sort(lValues1Indexed.begin(), lValues1Indexed.end());

        unsigned long long tempFSum = 0;
        for (size_t val : fValues1Indexed) {
            tempFSum += val;
        }
        double currentFSum = static_cast<double>(tempFSum);
        size_t currentFCount = fValues1Indexed.size();

        unsigned long long tempLSum = 0;
        for (size_t val : lValues1Indexed) {
            tempLSum += val;
        }
        double currentLSum = static_cast<double>(tempLSum);
        size_t currentLCount = lValues1Indexed.size();

        size_t fPtr = 0;
        size_t lPtr = 0;

        for (size_t w = 1; w <= n; ++w) {
            while (fPtr < fValues1Indexed.size() && fValues1Indexed[fPtr] <= w) {
                currentFSum -= fValues1Indexed[fPtr];
                currentFCount--;
                fPtr++;
            }
            double fW = currentFSum - static_cast<double>(w) * currentFCount;

            while (lPtr < lValues1Indexed.size() && lValues1Indexed[lPtr] <= w) {
                currentLSum -= lValues1Indexed[lPtr];
                currentLCount--;
                lPtr++;
            }
            double lW = currentLSum - static_cast<double>(w) * currentLCount;

            double rW = 0.0;
            if (w + 1 <= maxT) {
                rW = sumTrSuffix[w+1] - static_cast<double>(w) * sumRSuffix[w+1];
            }
            
            size_t denominator = n - w + 1;
            if (denominator == 0) {
                fpValues[w] = static_cast<double>(m);
                continue;
            }

            double sum_components = fW + lW + rW;
            double term_S_over_denom = sum_components / static_cast<double>(denominator);
            // NO CAPPING TO 0.0 HERE. fpVal can be negative.
            double fpVal = static_cast<double>(m) - term_S_over_denom;
            
            fpValues[w] = fpVal;
        }

        return fpValues;
    }

    /**
     * @brief Helper function to get the miss ratio for a given slab count from the MRC points.
     * Assumes MRC points provide continuous data up to max_profiled_slab_count.
     *
     * @param mrcPointsDict The map of slab_count to miss_ratio for a specific class.
     * @param slabCount The number of slabs for which to retrieve the miss ratio.
     * @return double The miss ratio. Returns 1.0 for 0 slabs. If slabCount exceeds profiled data,
     * assumes miss ratio of the largest profiled count or 0.0 if no profiling data.
     */
    double _getMissRatio(const std::map<size_t, double>& mrcPointsDict, size_t slabCount) const {
        if (slabCount == 0) {
            return 1.0;
        }
        if (mrcPointsDict.count(slabCount)) {
            return mrcPointsDict.at(slabCount);
        } else {
            size_t maxProfiledSlabCount = 0;
            if (!mrcPointsDict.empty()) {
                maxProfiledSlabCount = mrcPointsDict.rbegin()->first;
            }
            
            if (slabCount > maxProfiledSlabCount) {
                return mrcPointsDict.count(maxProfiledSlabCount) ? mrcPointsDict.at(maxProfiledSlabCount) : 0.0;
            }
            return 1.0;
        }
    }
};

} // namespace cachelib
} // namespace facebook
