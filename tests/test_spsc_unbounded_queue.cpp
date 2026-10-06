#include <hpc/concurrency/spsc_unbounded_queue.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <thread>

using hpc::concurrency::spsc_unbounded_queue;

TEST(SpscUnboundedQueue, EmptyPopFails)
{
    spsc_unbounded_queue<int> q;
    int v = 0;
    EXPECT_FALSE(q.try_pop(v));
    EXPECT_EQ(q.front(), nullptr);
}

TEST(SpscUnboundedQueue, FifoAcrossSegments)
{
    spsc_unbounded_queue<int, 4> q;
    for (int i = 0; i < 100; ++i) q.push(i);
    int v = -1;
    for (int i = 0; i < 100; ++i) {
        ASSERT_TRUE(q.try_pop(v));
        EXPECT_EQ(v, i);
    }
    EXPECT_FALSE(q.try_pop(v));
}

TEST(SpscUnboundedQueue, InterleavedAtSegmentBoundary)
{
    spsc_unbounded_queue<int, 2> q;
    int v = 0;
    for (int i = 0; i < 50; ++i) {
        q.push(i);
        ASSERT_TRUE(q.try_pop(v));
        EXPECT_EQ(v, i);
        EXPECT_FALSE(q.try_pop(v));
    }
}

TEST(SpscUnboundedQueue, EmplaceNonTrivial)
{
    spsc_unbounded_queue<std::string, 2> q;
    q.emplace(4u, 'z');
    q.push("tail");
    std::string s;
    ASSERT_TRUE(q.try_pop(s));
    EXPECT_EQ(s, "zzzz");
    ASSERT_TRUE(q.try_pop(s));
    EXPECT_EQ(s, "tail");
}

TEST(SpscUnboundedQueue, DestroysRemainingElementsAcrossSegments)
{
    auto tracker = std::make_shared<int>(0);
    {
        spsc_unbounded_queue<std::shared_ptr<int>, 4> q;
        for (int i = 0; i < 10; ++i) q.push(tracker);
        std::shared_ptr<int> out;
        ASSERT_TRUE(q.try_pop(out));
    }
    EXPECT_EQ(tracker.use_count(), 1);
}

TEST(SpscUnboundedQueue, ConcurrentFifo)
{
    constexpr std::uint64_t kItems = 1'000'000;
    spsc_unbounded_queue<std::uint64_t, 256> q;

    std::thread producer([&] {
        for (std::uint64_t i = 0; i < kItems; ++i) q.push(i);
    });

    std::uint64_t expected = 0, v = 0;
    while (expected < kItems) {
        if (q.try_pop(v)) {
            ASSERT_EQ(v, expected);
            ++expected;
        }
    }
    producer.join();
}
