#pragma once

#include <atomic>
#include <cstdint>
#include <thread>

#include <hpc/support/platform.hpp>

namespace hpc::concurrency {

// Test-and-test-and-set spinlock with bounded exponential backoff. Satisfies
// Lockable, so it works with std::scoped_lock and std::unique_lock.
//
// State: a single std::atomic_flag locked_ (set = held). atomic_flag is the
// one atomic type the standard guarantees to be lock-free; its C++20 test()
// supplies the plain read that TTAS spins on. There is no waiter queue and no
// fairness.
//
// How it works
//   - lock(): test_and_set(acquire). If the flag was clear, the caller owns
//     the lock, so the uncontended path is one atomic instruction (xchg on
//     x86-64, swpab on arm64).
//   - Otherwise, spin on a relaxed test(), a plain load (the "test" before
//     the next test-and-set). The line stays shared in the waiter's cache
//     until the owner's unlock invalidates it. Spinning on test_and_set
//     instead would issue a read-for-ownership on every attempt and bounce
//     the line between waiters.
//   - Between loads, back off 1, 2, 4 ... 64 pause instructions. Past that,
//     call std::this_thread::yield(), so an oversubscribed system still makes
//     progress. When test() sees the flag clear, retry test_and_set.
//   - try_lock(): a relaxed test() first, then test_and_set only if the lock
//     looks free.
//   - unlock(): clear(release), a plain release store.
//
// For very short critical sections only. It is unfair, and once many threads
// contend it loses to std::mutex, which puts waiters to sleep. Place it on its
// own cache line if neighbouring data is written by other threads.
class ttas_spinlock {
public:
    void lock() noexcept
    {
        while (locked_.test_and_set(std::memory_order_acquire)) {
            std::uint32_t backoff = 1;
            while (locked_.test(std::memory_order_relaxed)) {
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
        return !locked_.test(std::memory_order_relaxed)
            && !locked_.test_and_set(std::memory_order_acquire);
    }

    void unlock() noexcept { locked_.clear(std::memory_order_release); }

private:
    static constexpr std::uint32_t max_backoff = 64;

    std::atomic_flag locked_; // C++20: default-constructed clear
};

} // namespace hpc::concurrency
