#pragma once
#include <linux/perf_event.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <string.h>
#include <iostream>
#include <stdexcept>
#include <system_error>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <folly/logging/xlog.h>

namespace facebook {
namespace cachelib {
/**
do this first: sudo sh -c 'echo 1 >/proc/sys/kernel/perf_event_paranoid'
*/
class ThreadCpuCycleCounter {
public:
    ThreadCpuCycleCounter() {
        try {
            memset(&pe_, 0, sizeof(pe_));
            pe_.type = PERF_TYPE_HARDWARE;
            pe_.size = sizeof(pe_);
            pe_.config = PERF_COUNT_HW_CPU_CYCLES;
            pe_.disabled = 1;
            pe_.exclude_kernel = 0;
            pe_.exclude_hv = 1;

            fd_ = syscall(__NR_perf_event_open, &pe_, 0, -1, -1, 0);
            if (fd_ == -1) {
                throw std::system_error(errno, std::generic_category(), "perf_event_open failed");
            }
            ioctl(fd_, PERF_EVENT_IOC_RESET, 0);
            ioctl(fd_, PERF_EVENT_IOC_ENABLE, 0);
        } catch (const std::exception& ex) {
            XLOG(ERR) << "[ThreadCpuCycleCounter] Exception in constructor: " << ex.what();
            fd_ = -1;
        }
    }

    ~ThreadCpuCycleCounter() {
        if (fd_ != -1) {
            ioctl(fd_, PERF_EVENT_IOC_DISABLE, 0);
            close(fd_);
        }
    }

    uint64_t read() const {
        try {
            uint64_t count = 0;
            XLOG(DBG1) << "ThreadCpuCycleCounter::read() fd_=" << fd_;
            if (::read(fd_, &count, sizeof(count)) != sizeof(count)) {
                throw std::runtime_error(
                    std::string("Failed to read perf counter: ") + strerror(errno));
            }
            return count;
        } catch (const std::exception& ex) {
            XLOG(ERR) << "[ThreadCpuCycleCounter] Exception in read(): " << ex.what();
            return 0;
        }
    }

    uint64_t stop() {
        try {
            if (fd_ == -1) {
                return 0;
            }
            ioctl(fd_, PERF_EVENT_IOC_DISABLE, 0);
            uint64_t count = 0;
            if (::read(fd_, &count, sizeof(count)) != sizeof(count)) {
                throw std::runtime_error(
                    std::string("Failed to read perf counter: ") + strerror(errno));
            }
            close(fd_);
            fd_ = -1;
            return count;
        } catch (const std::exception& ex) {
            XLOG(ERR) << "[ThreadCpuCycleCounter] Exception in stop(): " << ex.what();
            return 0;
        }
    }

private:
    int fd_{-1};
    struct perf_event_attr pe_;
};

} // namespace cachelib
} // namespace facebook