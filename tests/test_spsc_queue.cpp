#include <hpc/concurrency/spsc_queue.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <thread>

using hpc::concurrency::spsc_queue;

TEST(SpscQueue, CapacityRoundsUpToPowerOfTwoMinimumTwo)
{
    EXPECT_EQ(spsc_queue<int>(0).capacity(), 2u);
    EXPECT_EQ(spsc_queue<int>(5).capacity(), 8u);
    EXPECT_EQ(spsc_queue<int>(8).capacity(), 8u);
}

TEST(SpscQueue, FullCapacityIsUsable)
{
    spsc_queue<int> q(4);
    EXPECT_TRUE(q.empty());
    for (int i = 0; i < 4; ++i) EXPECT_TRUE(q.try_push(i));
    EXPECT_FALSE(q.try_push(99));
    EXPECT_EQ(q.size_approx(), 4u);

    int v = -1;
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(q.try_pop(v));
        EXPECT_EQ(v, i);
    }
    EXPECT_FALSE(q.try_pop(v));
    EXPECT_TRUE(q.empty());
}

// Regression: the previous ring buffer computed `tail - head` on masked
// indices and lost every element once the indices wrapped.
TEST(SpscQueue, WrapAroundPreservesFifo)
{
    spsc_queue<int> q(4);
    int next_in = 0, next_out = 0, v = 0;
    for (int round = 0; round < 100; ++round) {
        while (q.try_push(next_in)) ++next_in;
        for (int i = 0; i < 3; ++i) {
            ASSERT_TRUE(q.try_pop(v));
            EXPECT_EQ(v, next_out++);
        }
    }
    while (q.try_pop(v)) EXPECT_EQ(v, next_out++);
    EXPECT_EQ(next_in, next_out);
}

TEST(SpscQueue, FrontAndPopAreZeroCopy)
{
    spsc_queue<std::string> q(2);
    EXPECT_EQ(q.front(), nullptr);
    ASSERT_TRUE(q.try_emplace(5u, 'x'));
    ASSERT_NE(q.front(), nullptr);
    EXPECT_EQ(*q.front(), "xxxxx");
    q.pop();
    EXPECT_EQ(q.front(), nullptr);
}

TEST(SpscQueue, DestroysRemainingElements)
{
    auto tracker = std::make_shared<int>(0);
    {
        spsc_queue<std::shared_ptr<int>> q(8);
        for (int i = 0; i < 5; ++i) ASSERT_TRUE(q.try_push(tracker));
        EXPECT_EQ(tracker.use_count(), 6);
    }
    EXPECT_EQ(tracker.use_count(), 1);
}

TEST(SpscQueue, MoveOnlyType)
{
    spsc_queue<std::unique_ptr<int>> q(2);
    ASSERT_TRUE(q.try_push(std::make_unique<int>(7)));
    std::unique_ptr<int> out;
    ASSERT_TRUE(q.try_pop(out));
    EXPECT_EQ(*out, 7);
}

TEST(SpscQueue, ConcurrentFifo)
{
    constexpr std::uint64_t kItems = 1'000'000;
    spsc_queue<std::uint64_t> q(1024);

    std::thread producer([&] {
        for (std::uint64_t i = 0; i < kItems; ++i)
            while (!q.try_push(i)) std::this_thread::yield();
    });

    std::uint64_t expected = 0, v = 0;
    while (expected < kItems) {
        if (q.try_pop(v)) {
            ASSERT_EQ(v, expected);
            ++expected;
        }
    }
    producer.join();
    EXPECT_TRUE(q.empty());
}
