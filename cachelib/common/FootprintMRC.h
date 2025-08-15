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
#include <unordered_map>
#include <vector>
#include <string>
#include <cmath>
#include <numeric>
#include <algorithm>
#include <tuple>
#include <utility>
#include <set>
#include <limits>
#include <chrono>
#include <iostream>
#include <mutex>

#include "cachelib/allocator/CacheItem.h"
#include "cachelib/allocator/memory/Slab.h"


namespace facebook {
namespace cachelib {

class FootprintMRC {
public:
    // Use uint64_t for keys since they're large integers in string form
    using KeyInt = uint64_t;
    using Key = std::string; // Keep for compatibility, but we'll store KeyInt internally

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
        
        // Pre-allocate snapshot buffer with same capacity as circular buffer
        bufferSnapshot.reserve(k);
    }

    // Copy constructor
    FootprintMRC(const FootprintMRC& other) {
        std::lock_guard<std::mutex> lock(other.bufferMutex);
        circularBuffer = other.circularBuffer;
        circularBufferMaxLen = other.circularBufferMaxLen;
        currentBufferSize = other.currentBufferSize;
        bufferHeadIndex = other.bufferHeadIndex;
        
        // Pre-allocate snapshot buffer with same capacity
        bufferSnapshot.reserve(circularBufferMaxLen);
    }

    // Move constructor
    FootprintMRC(FootprintMRC&& other) noexcept {
        std::lock_guard<std::mutex> lock(other.bufferMutex);
        circularBuffer = std::move(other.circularBuffer);
        circularBufferMaxLen = other.circularBufferMaxLen;
        currentBufferSize = other.currentBufferSize;
        bufferHeadIndex = other.bufferHeadIndex;
        
        // Move snapshot buffer as well
        bufferSnapshot = std::move(other.bufferSnapshot);
        
        // Reset the moved-from object
        other.circularBufferMaxLen = 0;
        other.currentBufferSize = 0;
        other.bufferHeadIndex = 0;
    }

    // Copy assignment operator
    FootprintMRC& operator=(const FootprintMRC& other) {
        if (this != &other) {
            std::lock(bufferMutex, other.bufferMutex);
            std::lock_guard<std::mutex> lock1(bufferMutex, std::adopt_lock);
            std::lock_guard<std::mutex> lock2(other.bufferMutex, std::adopt_lock);
            
            circularBuffer = other.circularBuffer;
            circularBufferMaxLen = other.circularBufferMaxLen;
            currentBufferSize = other.currentBufferSize;
            bufferHeadIndex = other.bufferHeadIndex;
            
            // Re-allocate snapshot buffer with new capacity
            bufferSnapshot.clear();
            bufferSnapshot.reserve(circularBufferMaxLen);
        }
        return *this;
    }

    // Move assignment operator
    FootprintMRC& operator=(FootprintMRC&& other) noexcept {
        if (this != &other) {
            std::lock(bufferMutex, other.bufferMutex);
            std::lock_guard<std::mutex> lock1(bufferMutex, std::adopt_lock);
            std::lock_guard<std::mutex> lock2(other.bufferMutex, std::adopt_lock);
            
            circularBuffer = std::move(other.circularBuffer);
            circularBufferMaxLen = other.circularBufferMaxLen;
            currentBufferSize = other.currentBufferSize;
            bufferHeadIndex = other.bufferHeadIndex;
            
            // Move snapshot buffer as well
            bufferSnapshot = std::move(other.bufferSnapshot);
            
            // Reset the moved-from object
            other.circularBufferMaxLen = 0;
            other.currentBufferSize = 0;
            other.bufferHeadIndex = 0;
        }
        return *this;
    }

    /**
     * @brief Feeds a new memory access request (key, class ID) into the circular buffer.
     * If the buffer is full, the oldest entry is overwritten.
     * This method is thread-safe using mutex protection as mentioned in the LAMA paper.
     *
     * @param key The memory access key (KAllocation::Key, which is folly::StringPiece).
     * @param classId An identifier for the class this key belongs to.
     */
    void feed(const KAllocation::Key& key, const ClassId& classId) {
        // Defensive: ensure we have a valid buffer
        if (circularBuffer.empty() || circularBufferMaxLen == 0) {
            return;
        }
        
        // Convert the string key to uint64_t for much faster operations
        KeyInt keyInt = 0;
        try {
            keyInt = std::stoull(key.str());
        } catch (const std::exception&) {
            // Fallback: use hash of the string if conversion fails
            keyInt = std::hash<std::string>{}(key.str());
        }
        
        // Critical section: minimal lock duration for atomic buffer access
        {
            std::lock_guard<std::mutex> lock(bufferMutex);
            
            // Capture current head index
            size_t currentHead = bufferHeadIndex;
            
            // Bounds check - defensive programming
            if (currentHead >= circularBufferMaxLen) {
                currentHead = 0;
                bufferHeadIndex = 0;
            }
            
            // Store the integer key directly
            circularBuffer[currentHead] = std::make_tuple(keyInt, classId);
            
            // Update indices atomically within the lock
            bufferHeadIndex = (currentHead + 1) % circularBufferMaxLen;

            if (currentBufferSize < circularBufferMaxLen) {
                currentBufferSize++;
            }
        }
    }

    /**
     * @brief Calculates footprint values for given cache sizes using footprint theory.
     * This is the primary method for querying miss-ratio curves efficiently.
     *
     * @param cacheSizes Array of cache sizes (in objects) to evaluate.
     * @return std::vector<double> Footprint values corresponding to each cache size.
     */
    // Optimize the queryMrc method to reduce intermediate object copies
    std::vector<double> queryMrc(const folly::Range<const uint32_t*>& cacheSizes) const {
        if (currentBufferSize == 0) {
            return std::vector<double>(cacheSizes.size(), 0.0);
        }

        // Calculate window stats once and reuse
        auto [firstAccessTimesByClass, lastAccessTimesByClass, reuseTimeHistogramByClass, nByClass, mByClass] = 
            calculateWindowStats();

        // Pre-allocate result vector for better performance
        std::vector<double> result;
        result.reserve(cacheSizes.size());

        // Process each class once and cache results
        std::unordered_map<ClassId, std::vector<double>> cachedFpValues;
        for (const auto& [classId, _] : firstAccessTimesByClass) {
            cachedFpValues[classId] = calculateFpValues(
                firstAccessTimesByClass.at(classId),
                lastAccessTimesByClass.at(classId),
                reuseTimeHistogramByClass.at(classId),
                nByClass.at(classId),
                mByClass.at(classId)
            );
        }

        for (uint32_t cacheSize : cacheSizes) {
            double totalFpValue = 0.0;
            
            for (const auto& [classId, fpValues] : cachedFpValues) {
                size_t w = std::min(static_cast<size_t>(cacheSize), static_cast<size_t>(nByClass.at(classId)));
                
                // Direct array access instead of upper_bound
                if (w < fpValues.size()) {
                    totalFpValue += fpValues[w];
                }
            }
            result.push_back(totalFpValue);
        }
        
        return result;
    }

    /**
     * @brief Calculates detailed MRC data for each class, including MRC points, MRC deltas, and access frequencies.
     * This version is used by solveSlabReallocation for detailed analysis.
     *
     * @param classIdToAllocsPerSlabMap A map where keys are ClassId and values are
     * the number of objects (of that class) that can be stored in one slab.
     * @param maxSlabCount The maximum number of slabs to consider for the MRC.
     * @return std::map<ClassId, std::tuple<std::map<size_t, double>, std::map<size_t, double>, size_t>>
     * A map where keys are classIds. Each value is a tuple:
     * - mrcPoints (std::map<size_t, double>): Miss ratios for different slab counts
     * - mrcDelta (std::map<size_t, double>): MRC deltas (difference between consecutive points)
     * - accessFrequency (size_t): Total access count for this class
     */
    std::map<ClassId, std::tuple<std::map<size_t, double>, std::map<size_t, double>, size_t>>
    queryMrc(const std::map<ClassId, size_t>& classIdToAllocsPerSlabMap, size_t maxSlabCount) const {
        if (currentBufferSize == 0) {
            return {};
        }

        // Calculate window stats once and reuse
        auto [firstAccessTimesByClass, lastAccessTimesByClass, reuseTimeHistogramByClass, nByClass, mByClass] = 
            calculateWindowStats();

        std::map<ClassId, std::tuple<std::map<size_t, double>, std::map<size_t, double>, size_t>> result;

        // Process each class that has allocation info
        for (const auto& [classId, allocsPerSlab] : classIdToAllocsPerSlabMap) {
            // Defensive: skip invalid entries instead of throwing
            if (allocsPerSlab == 0) {
                continue; // Skip this class instead of throwing
            }

            // Skip classes not present in current window
            if (firstAccessTimesByClass.find(classId) == firstAccessTimesByClass.end()) {
                continue;
            }

            // Calculate footprint values for this class
            auto fpValues = calculateFpValues(
                firstAccessTimesByClass.at(classId),
                lastAccessTimesByClass.at(classId),
                reuseTimeHistogramByClass.at(classId),
                nByClass.at(classId),
                mByClass.at(classId)
            );

            // Convert footprint values to miss ratios for different slab counts
            // Use efficient approach: pre-compute footprint->reuse mapping, then use prefix sums
            
            std::map<size_t, double> mrcPoints;
            std::map<size_t, double> mrcDelta;
            
            // Pre-compute footprint values for all reuse times - no sorting needed!
            std::vector<size_t> prefixSums; // Declare outside conditional block
            size_t totalAccesses = nByClass.at(classId);
            
            if (totalAccesses > 0) {
                const auto& reuseTimeHistogram = reuseTimeHistogramByClass.at(classId);
                
                // Pre-compute prefix sums directly from the vector (no sorting needed)
                prefixSums.reserve(reuseTimeHistogram.size() + 1);
                prefixSums.push_back(0);
                
                for (size_t reuseTime = 0; reuseTime < reuseTimeHistogram.size(); ++reuseTime) {
                    size_t reuseCount = reuseTimeHistogram[reuseTime];
                    prefixSums.push_back(prefixSums.back() + reuseCount);
                }
            }

            double prevMissRatio = 1.0; // Miss ratio for 0 slabs
            
            for (size_t slabCount = 0; slabCount <= maxSlabCount; ++slabCount) {
                size_t cacheSize = slabCount * allocsPerSlab; // Convert slabs to object count
                
                double missRatio = 1.0;
                if (totalAccesses > 0 && cacheSize > 0) {
                    // Count hits: accesses with reuse time t where fp(t) < cacheSize
                    size_t hitCount = 0;
                    const auto& reuseTimeHistogram = reuseTimeHistogramByClass.at(classId);
                    
                    for (size_t reuseTime = 0; reuseTime < reuseTimeHistogram.size(); ++reuseTime) {
                        if (reuseTimeHistogram[reuseTime] > 0) {
                            // Get footprint for this reuse time
                            double fpAtReuseTime = 0.0;
                            if (reuseTime > 0 && reuseTime < fpValues.size()) {
                                fpAtReuseTime = fpValues[reuseTime];
                            }
                            
                            // If footprint < cache size, these accesses are hits
                            if (fpAtReuseTime < static_cast<double>(cacheSize)) {
                                hitCount += reuseTimeHistogram[reuseTime];
                            }
                        }
                    }
                    
                    double hitRatio = static_cast<double>(hitCount) / totalAccesses;
                    missRatio = 1.0 - hitRatio;
                    missRatio = std::max(0.0, std::min(1.0, missRatio)); // Clamp to [0,1]
                }

                mrcPoints[slabCount] = missRatio;
                
                if (slabCount > 0) {
                    mrcDelta[slabCount] = prevMissRatio - missRatio;
                }
                prevMissRatio = missRatio;
            }

            result[classId] = std::make_tuple(
                std::move(mrcPoints),
                std::move(mrcDelta),
                nByClass.at(classId)
            );
        }

        return result;
    }

    /**
     * @brief Resets the circular buffer, effectively clearing all past requests
     * and starting a new analysis window. Thread-safe.
     */
    void resetWindowAnalysis() {
        std::lock_guard<std::mutex> lock(bufferMutex);
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
     * @return std::tuple<double, double, std::unordered_map<ClassId, size_t>, std::vector<std::pair<ClassId, ClassId>>, std::unordered_map<ClassId, size_t>>
     * A tuple containing:
     * - mrOld (double): Total miss rate with the current allocation.
     * - mrNew (double): Total miss rate with the new optimal allocation.
     * - optimalAllocation (std::unordered_map<ClassId, size_t>): A map mapping ClassId to the new
     * optimal number of slabs.
     * - reassignmentPlan (std::vector<std::pair<ClassId, ClassId>>): A list of (victim_class_id, receiver_class_id) pairs,
     * indicating individual slab movements from old to new.
     * - accessFrequencies (std::unordered_map<ClassId, size_t>): A map mapping ClassId to the total number of
     * requests for that class in the current window.
     * @throws std::invalid_argument if classIdToAllocsPerSlabMap contains invalid entries (e.g., 0 allocs per slab).
     */
    std::tuple<double, double, std::unordered_map<ClassId, size_t>, std::vector<std::pair<ClassId, ClassId>>, std::unordered_map<ClassId, size_t>>
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
            return std::make_tuple(0.0, 0.0, std::unordered_map<ClassId, size_t>(), std::vector<std::pair<ClassId, ClassId>>(), std::unordered_map<ClassId, size_t>());
        }
        
        if (maxTotalSlabs == 0 && classIdToAllocsPerSlabMap.empty()) {
            return std::make_tuple(0.0, 0.0, std::unordered_map<ClassId, size_t>(), std::vector<std::pair<ClassId, ClassId>>(), std::unordered_map<ClassId, size_t>());
        }

        std::vector<ClassId> classIds;
        for (const auto& pair : classMrcData) {
            classIds.push_back(pair.first);
        }
        std::sort(classIds.begin(), classIds.end());
        size_t numClasses = classIds.size();

        std::unordered_map<ClassId, size_t> accessFrequencies;
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

        std::unordered_map<ClassId, size_t> optimalAllocation;
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
    // Stores tuples of (KeyInt, ClassId) - using integers for much faster operations
    std::vector<std::tuple<KeyInt, ClassId>> circularBuffer;
    size_t circularBufferMaxLen;
    size_t currentBufferSize;
    size_t bufferHeadIndex;
    
    // Persistent snapshot buffer to avoid repeated allocations across calculateWindowStats calls
    mutable std::vector<std::tuple<KeyInt, ClassId>> bufferSnapshot;
    
    // Mutex for thread-safe access to the circular buffer as mentioned in LAMA paper
    mutable std::mutex bufferMutex;

    /**
     * @brief Calculates firstAccessTimes, lastAccessTimes, reuseTimeHistogram,
     * totalAccesses (n), and uniqueAccesses (m) specifically for the
     * current contents of the circular buffer, grouped by classId.
     *
     * The unique items for tracking locality are (Key) as Key itself is assumed unique.
     *
     * @return std::tuple<std::unordered_map<ClassId, std::vector<size_t>>,
     * std::unordered_map<ClassId, std::vector<size_t>>,
     * std::unordered_map<ClassId, std::vector<size_t>>,
     * std::unordered_map<ClassId, size_t>,
     * std::unordered_map<ClassId, size_t>>
     * A tuple containing maps, where each map uses classId as its primary key.
     * First and last access times are pre-sorted vectors (no KeyInt mapping needed).
     * Reuse time histogram uses vector where index is reuse time and value is count.
     */
    std::tuple<std::unordered_map<ClassId, std::vector<size_t>>,
               std::unordered_map<ClassId, std::vector<size_t>>,
               std::unordered_map<ClassId, std::vector<size_t>>,
               std::unordered_map<ClassId, size_t>,
               std::unordered_map<ClassId, size_t>>
    calculateWindowStats() const {
        // Snapshot approach: Copy buffer state atomically, then analyze without holding lock
        size_t snapshotSize;
        size_t snapshotHeadIndex;
        size_t snapshotMaxLen;
        
        // Critical section: minimal lock duration for consistent snapshot
        {
            std::lock_guard<std::mutex> lock(bufferMutex);
            snapshotSize = currentBufferSize;
            snapshotHeadIndex = bufferHeadIndex;
            snapshotMaxLen = circularBufferMaxLen;
            
            // Clear and reuse persistent snapshot buffer (capacity already reserved)
            bufferSnapshot.clear();
            
            // Only copy if we have data
            if (snapshotSize > 0 && !circularBuffer.empty()) {
                bufferSnapshot = circularBuffer; // Copy entire buffer - no reallocation needed
            }
        }
        
        // Early exit if no data
        if (snapshotSize == 0 || bufferSnapshot.empty()) {
            return std::make_tuple(
                std::unordered_map<ClassId, std::vector<size_t>>(),
                std::unordered_map<ClassId, std::vector<size_t>>(),
                std::unordered_map<ClassId, std::vector<size_t>>(),
                std::unordered_map<ClassId, size_t>(),
                std::unordered_map<ClassId, size_t>()
            );
        }
        
        // Bounds check and correction on snapshot
        if (snapshotSize > snapshotMaxLen) {
            snapshotSize = snapshotMaxLen;
        }
        if (snapshotHeadIndex >= snapshotMaxLen) {
            snapshotHeadIndex = 0;
        }

        size_t startIndex = (snapshotSize < snapshotMaxLen) ? 0 : snapshotHeadIndex;

        // PASS 1: Count total accesses (n) and unique keys (m) per class using snapshot
        std::unordered_map<ClassId, size_t> nByClass;
        std::unordered_map<ClassId, size_t> mByClass;
        std::unordered_map<ClassId, std::unordered_set<KeyInt>> uniqueKeysPerClass;
        
        for (size_t i = 0; i < snapshotSize; ++i) {
            size_t currentCircularIndex = (startIndex + i) % snapshotMaxLen;
            
            // Defensive bounds check
            if (currentCircularIndex >= snapshotMaxLen || currentCircularIndex >= bufferSnapshot.size()) {
                break;
            }
            
            auto entry = bufferSnapshot[currentCircularIndex];
            const KeyInt& keyInt = std::get<0>(entry);
            const ClassId& classId = std::get<1>(entry);
            
            nByClass[classId]++;
            uniqueKeysPerClass[classId].insert(keyInt);
        }
        
        // Calculate m for each class
        for (const auto& [classId, uniqueKeys] : uniqueKeysPerClass) {
            mByClass[classId] = uniqueKeys.size();
        }

        // PASS 2: Initialize data structures with exact sizes and populate them
        std::unordered_map<ClassId, std::vector<size_t>> firstAccessTimesByClass;
        std::unordered_map<ClassId, std::vector<size_t>> lastAccessTimesByClass;
        std::unordered_map<ClassId, std::vector<size_t>> reuseTimeHistogramByClass;
        
        // Pre-allocate with exact sizes
        for (const auto& [classId, n] : nByClass) {
            size_t m = mByClass[classId];
            firstAccessTimesByClass[classId].reserve(m);
            lastAccessTimesByClass[classId].reserve(m);
            reuseTimeHistogramByClass[classId].resize(n, 0); // Max reuse time is n-1
        }
        
        // Reset counters for second pass
        std::unordered_map<ClassId, size_t> currentAccessIndex;
        
        // Forward pass: collect first access times, last access times, and reuse times using counting sort approach on snapshot
        std::unordered_map<ClassId, std::unordered_map<KeyInt, size_t>> keyToFirstAccess;
        std::unordered_map<ClassId, std::unordered_map<KeyInt, size_t>> keyToLastAccess;
        
        for (size_t i = 0; i < snapshotSize; ++i) {
            size_t currentCircularIndex = (startIndex + i) % snapshotMaxLen;
            
            // Defensive bounds check
            if (currentCircularIndex >= snapshotMaxLen || currentCircularIndex >= bufferSnapshot.size()) {
                break;
            }
            
            auto entry = bufferSnapshot[currentCircularIndex];
            const KeyInt& keyInt = std::get<0>(entry);
            const ClassId& classId = std::get<1>(entry);
            
            size_t local_idx_for_current_access = currentAccessIndex[classId]++;

            // Process first access and last access
            auto& firstAccessForClass = keyToFirstAccess[classId];
            auto& lastAccessForClass = keyToLastAccess[classId];
            auto& reuseHistogramForClass = reuseTimeHistogramByClass[classId];
            
            auto [firstIt, firstInserted] = firstAccessForClass.emplace(keyInt, local_idx_for_current_access);
            // First access is recorded when we first see the key

            // Process last access and reuse time calculation
            auto [lastIt, lastInserted] = lastAccessForClass.emplace(keyInt, local_idx_for_current_access);
            if (!lastInserted) {
                // Key existed, calculate reuse time
                size_t prevAccessIndex = lastIt->second;
                size_t reuseTime = local_idx_for_current_access - prevAccessIndex;
                
                // No need for bounds check since we pre-allocated with size n
                reuseHistogramForClass[reuseTime]++;
                
                // Update the last access time
                lastIt->second = local_idx_for_current_access;
            }
        }
        
        // Build sorted access time vectors using counting sort approach (O(n) for each class)
        for (const auto& [classId, n] : nByClass) {
            auto& firstAccessVectorForClass = firstAccessTimesByClass[classId];
            auto& lastAccessVectorForClass = lastAccessTimesByClass[classId];
            const auto& keyToFirstForClass = keyToFirstAccess[classId];
            const auto& keyToLastForClass = keyToLastAccess[classId];
            
            // Create counting arrays for first and last access times
            std::vector<size_t> firstAccessCount(n + 2, 0);  // +2 to handle 1-indexed safely
            std::vector<size_t> lastAccessCount(n + 2, 0);   // +2 to handle n+1-x safely
            
            // Count first access times (1-indexed for fp formula)
            for (const auto& [keyInt, firstAccessTime] : keyToFirstForClass) {
                size_t firstAccess1Indexed = firstAccessTime + 1; // +1 for fp formula
                if (firstAccess1Indexed <= n + 1) {
                    firstAccessCount[firstAccess1Indexed]++;
                }
            }
            
            // Count last access times (reverse: n + 1 - lastAccessTime)
            for (const auto& [keyInt, lastAccessTime] : keyToLastForClass) {
                size_t reverseLastAccess = n - lastAccessTime; // n - accessTime as needed by calculateFpValues
                if (reverseLastAccess <= n) {
                    lastAccessCount[reverseLastAccess]++;
                }
            }
            
            // Build sorted vectors from counting arrays
            firstAccessVectorForClass.reserve(keyToFirstForClass.size());
            lastAccessVectorForClass.reserve(keyToLastForClass.size());
            
            // Generate sorted first access times
            for (size_t accessTime = 1; accessTime <= n + 1; ++accessTime) {
                for (size_t count = 0; count < firstAccessCount[accessTime]; ++count) {
                    firstAccessVectorForClass.push_back(accessTime);
                }
            }
            
            // Generate sorted last access times (reverse order)
            for (size_t reverseTime = 0; reverseTime <= n; ++reverseTime) {
                for (size_t count = 0; count < lastAccessCount[reverseTime]; ++count) {
                    lastAccessVectorForClass.push_back(reverseTime);
                }
            }
        }
        
        return std::make_tuple(std::move(firstAccessTimesByClass), std::move(lastAccessTimesByClass),
                               std::move(reuseTimeHistogramByClass), std::move(nByClass), std::move(mByClass));
    }

    /**
     * @brief Calculates the footprint fp(w) for all possible window lengths 'w'
     * from 0 up to the total number of accesses 'n' for a given class in the current window.
     * This version takes pre-sorted vectors for better performance.
     *
     * @param sortedFirstAccessTimesForClass Pre-sorted vector of first access times (1-indexed for fp formula).
     * @param sortedLastAccessTimesForClass Pre-sorted vector of last access times (transformed: n - accessTime).
     * @param reuseTimeHistogramWindowForClass Vector of reuse time counts where index is reuse time.
     * @param nWindowForClass Total accesses in the current window for this class.
     * @param mWindowForClass Unique accesses in the current window for this class.
     * @return std::vector<double> A vector where index w represents the footprint fp(w).
     * Returns an empty vector if nWindowForClass is 0.
     */
    std::vector<double> calculateFpValues(
        const std::vector<size_t>& sortedFirstAccessTimesForClass,
        const std::vector<size_t>& sortedLastAccessTimesForClass,
        const std::vector<size_t>& reuseTimeHistogramWindowForClass,
        size_t nWindowForClass,
        size_t mWindowForClass) const {

        size_t n = nWindowForClass;
        size_t m = mWindowForClass;

        if (n == 0) {
            return {};
        }

        // Pre-calculate static values
        const double staticM = static_cast<double>(m);
        size_t maxT = (n > 0) ? n - 1 : 0;

        // More efficient suffix sum calculation with better memory usage
        std::vector<double> sumTrSuffix(maxT + 2, 0.0);
        std::vector<double> sumRSuffix(maxT + 2, 0.0);

        // Process reuse times more efficiently - direct vector iteration
        for (size_t reuseTime = 0; reuseTime < reuseTimeHistogramWindowForClass.size(); ++reuseTime) {
            size_t count = reuseTimeHistogramWindowForClass[reuseTime];
            if (count > 0 && reuseTime > 0 && reuseTime <= maxT && reuseTime < sumTrSuffix.size()) {
                double dCount = static_cast<double>(count);
                sumTrSuffix[reuseTime] = static_cast<double>(reuseTime) * dCount;
                sumRSuffix[reuseTime] = dCount;
            }
        }
        
        // Build suffix sums in reverse order - single pass with bounds check
        for (size_t t = maxT; t > 0 && t < sumTrSuffix.size() && t + 1 < sumTrSuffix.size(); --t) {
            sumTrSuffix[t] += sumTrSuffix[t+1];
            sumRSuffix[t] += sumRSuffix[t+1];
        }

        // Pre-allocate access time vectors - they're already sorted!
        const size_t fSize = sortedFirstAccessTimesForClass.size();
        const size_t lSize = sortedLastAccessTimesForClass.size();
        
        // No need to build or sort vectors - they're already provided sorted!
        const std::vector<size_t>& fValues1Indexed = sortedFirstAccessTimesForClass;
        const std::vector<size_t>& lValues1Indexed = sortedLastAccessTimesForClass;

        // Pre-compute total sums
        double currentFSum = std::accumulate(fValues1Indexed.begin(), fValues1Indexed.end(), 0.0);
        double currentLSum = std::accumulate(lValues1Indexed.begin(), lValues1Indexed.end(), 0.0);
        
        size_t currentFCount = fSize;
        size_t currentLCount = lSize;
        size_t fPtr = 0;
        size_t lPtr = 0;

        // Pre-allocate result vector with size n+1 (index 0 to n)
        std::vector<double> fpValues(n + 1, 0.0);
        
        // Main computation loop - optimized for cache efficiency
        for (size_t w = 1; w <= n; ++w) {
            // Process f component updates
            while (fPtr < fSize && fValues1Indexed[fPtr] <= w) {
                currentFSum -= static_cast<double>(fValues1Indexed[fPtr]);
                currentFCount--;
                fPtr++;
            }
            double fW = currentFSum - static_cast<double>(w) * currentFCount;

            // Process l component updates
            while (lPtr < lSize && lValues1Indexed[lPtr] <= w) {
                currentLSum -= static_cast<double>(lValues1Indexed[lPtr]);
                currentLCount--;
                lPtr++;
            }
            double lW = currentLSum - static_cast<double>(w) * currentLCount;

            // Compute reuse component using pre-computed suffix sums with bounds check
            double rW = 0.0;
            if (w + 1 <= maxT && w + 1 < sumTrSuffix.size()) {
                rW = sumTrSuffix[w+1] - static_cast<double>(w) * sumRSuffix[w+1];
            }
            
            // Final computation
            size_t denominator = n - w + 1;
            if (denominator == 0) {
                fpValues[w] = staticM;
            } else {
                double sum_components = fW + lW + rW;
                fpValues[w] = staticM - (sum_components / static_cast<double>(denominator));
            }
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
