#pragma once
#include <pthread.h>
#include <sched.h>
#include <stdexcept>
#include <iostream>
#include <vector>
#include <folly/system/ThreadId.h>
#include <folly/system/ThreadName.h>

namespace facebook {
namespace cachelib {

inline void pinThreadToCore(int coreId) {
    auto tid = std::this_thread::get_id();
    auto name = folly::getThreadName(tid);
    std::cout << "[PinCpuCore] Thread " << folly::getCurrentThreadID()
              << " (name: " << (name ? *name : "unnamed")
              << ") is pinning to core " << coreId << std::endl;


    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(coreId, &cpuset);

    pthread_t current_thread = pthread_self();
    int rc = pthread_setaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset);
    if (rc != 0) {
        throw std::runtime_error("Error calling pthread_setaffinity_np");
    }
}

inline void pinThreadToCores(const std::vector<int>& coreIds) {
    auto tid = std::this_thread::get_id();
    auto name = folly::getThreadName(tid);
    std::cout << "[PinCpuCore] Thread " << folly::getCurrentThreadID()
              << " (name: " << (name ? *name : "unnamed")
              << ") is pinning to cores: ";
    for (int coreId : coreIds) {
        std::cout << coreId << " ";
    }
    std::cout << std::endl;

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    for (int coreId : coreIds) {
        CPU_SET(coreId, &cpuset);
    }

    pthread_t current_thread = pthread_self();
    int rc = pthread_setaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset);
    if (rc != 0) {
        throw std::runtime_error("Error calling pthread_setaffinity_np");
    }
}

} // namespace cachelib
} // namespace facebook