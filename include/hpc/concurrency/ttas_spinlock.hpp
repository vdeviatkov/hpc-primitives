#pragma once

#include <atomic>
#include <cstdint>
#include <thread>

#include <hpc/support/platform.hpp>

namespace hpc::concurrency {

// Test-and-test-and-set spinlock with bounded exponential backoff.
// Satisfies Lockable, so it works with std::scoped_lock / std::unique_lock.
//
//  - Uncontended path is a single exchange.
//  - Under contention, waiters spin on a relaxed load, which hits their local
//    cache until the owner's release store invalidates it, instead of
//    hammering the line with read-for-ownership requests.
//  - Backoff doubles up to 64 pause instructions, then falls back to
//    std::this_thread::yield() so an oversubscribed system still progresses.
//
// For very short critical sections only. Place it on its own cache line if
// neighbouring data is written by other threads.
class ttas_spinlock {
public:
    void lock() noexcept
    {
        while (locked_.exchange(true, std::memory_order_acquire)) {
            std::uint32_t backoff = 1;
            while (locked_.load(std::memory_order_relaxed)) {
                if (backoff <= max_backoff) {
                    for (std::uint32_t i = 0; i < backoff; ++i) support::cpu_relax();
                    backoff <<= 1;
                } else {
                    std::this_thread::yield();
                }
            }
        }
    }

    [[nodiscard]] bool try_lock() noexcept
    {
        return !locked_.load(std::memory_order_relaxed)
            && !locked_.exchange(true, std::memory_order_acquire);
    }

    void unlock() noexcept { locked_.store(false, std::memory_order_release); }

private:
    static constexpr std::uint32_t max_backoff = 64;

    std::atomic<bool> locked_{false};
};

} // namespace hpc::concurrency
