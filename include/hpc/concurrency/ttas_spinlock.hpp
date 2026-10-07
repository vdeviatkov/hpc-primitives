#pragma once

#include <atomic>
#include <cstdint>
#include <thread>

#include <hpc/support/platform.hpp>

namespace hpc::concurrency {

// Test-and-test-and-set spinlock with bounded exponential backoff. Satisfies
// Lockable, so it works with std::scoped_lock and std::unique_lock.
//
// State: a single atomic<bool> locked_. There is no waiter queue and no
// fairness.
//
// How it works
//   - lock(): exchange(true, acquire). If it returns false, the caller owns
//     the lock, so the uncontended path is one atomic instruction.
//   - Otherwise, spin on a relaxed *load* (the "test" before the next
//     test-and-set). The line stays shared in the waiter's cache until the
//     owner's unlock invalidates it. Spinning on the exchange instead would
//     issue a read-for-ownership on every attempt and bounce the line between
//     waiters.
//   - Between loads, back off 1, 2, 4 ... 64 pause instructions. Past that,
//     call std::this_thread::yield(), so an oversubscribed system still makes
//     progress. When the load sees false, retry the exchange.
//   - try_lock(): a relaxed load first, then the exchange only if the lock
//     looks free.
//   - unlock(): a release store of false.
//
// For very short critical sections only. It is unfair, and once many threads
// contend it loses to std::mutex, which puts waiters to sleep. Place it on its
// own cache line if neighbouring data is written by other threads.
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
