#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <utility>

namespace trade_ngin::testing {

// Test-only process containment: a blocked worker cannot leave the suite hung.
class SyncTestDeadline {
public:
    static constexpr int kTimeoutExitCode = 86;
    explicit SyncTestDeadline(std::chrono::milliseconds timeout)
        : watchdog_([this, expiry = std::chrono::steady_clock::now() + timeout] {
              std::unique_lock<std::mutex> lock(mutex_);
              if (!cv_.wait_until(lock, expiry, [this] { return cancelled_; })) {
                  std::_Exit(kTimeoutExitCode);
              }
          }) {}
    SyncTestDeadline(const SyncTestDeadline&) = delete;
    SyncTestDeadline& operator=(const SyncTestDeadline&) = delete;
    ~SyncTestDeadline() noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            cancelled_ = true;
        }
        cv_.notify_one();
        watchdog_.join();
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool cancelled_{false};
    std::thread watchdog_;
};

template <typename Function>
class SyncTestScopeExit {
public:
    explicit SyncTestScopeExit(Function function) : function_(std::move(function)) {}
    SyncTestScopeExit(const SyncTestScopeExit&) = delete;
    SyncTestScopeExit& operator=(const SyncTestScopeExit&) = delete;
    ~SyncTestScopeExit() noexcept { function_(); }

private:
    Function function_;
};

template <typename Function>
SyncTestScopeExit<Function> sync_test_scope_exit(Function function) {
    return SyncTestScopeExit<Function>(std::move(function));
}

}  // namespace trade_ngin::testing
