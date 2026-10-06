#include <hpc/concurrency/ttas_spinlock.hpp>

#include <gtest/gtest.h>

#include <mutex>
#include <thread>
#include <vector>

using hpc::concurrency::ttas_spinlock;

TEST(TtasSpinlock, TryLock)
{
    ttas_spinlock lock;
    EXPECT_TRUE(lock.try_lock());
    EXPECT_FALSE(lock.try_lock());
    lock.unlock();
    EXPECT_TRUE(lock.try_lock());
    lock.unlock();
}

// A non-atomic counter only ends up exact if the lock provides mutual
// exclusion and acquire/release ordering.
TEST(TtasSpinlock, MutualExclusion)
{
    constexpr int kThreads = 8;
    constexpr int kIters   = 100'000;

    ttas_spinlock lock;
    long counter = 0;

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < kIters; ++i) {
                std::scoped_lock guard(lock);
                ++counter;
            }
        });
    }
    for (auto& th : threads) th.join();

    EXPECT_EQ(counter, long{kThreads} * kIters);
}
