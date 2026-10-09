#include <hpc/concurrency/spsc_unbounded_queue.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <deque>
#include <memory>
#include <random>
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

// pop() checks for an element itself, so calling it on an empty queue is a
// harmless no-op that returns false. Before, an extra pop() moved the read
// position past the published elements: later front() calls returned slots
// the producer had not written, and elements were skipped or destroyed raw.
TEST(SpscUnboundedQueue, PopOnEmptyReturnsFalseAndQueueStaysUsable)
{
    spsc_unbounded_queue<int> q;
    EXPECT_FALSE(q.pop());
    q.push(10);
    EXPECT_TRUE(q.pop());
    EXPECT_FALSE(q.pop());
    EXPECT_FALSE(q.pop());
    q.push(20);
    q.push(30);
    int v = 0;
    ASSERT_TRUE(q.try_pop(v));
    EXPECT_EQ(v, 20); // the extra pops did not skip it
    ASSERT_TRUE(q.try_pop(v));
    EXPECT_EQ(v, 30);
    EXPECT_FALSE(q.try_pop(v));
}

TEST(SpscUnboundedQueue, PopWithoutFrontRemovesOldest)
{
    spsc_unbounded_queue<int> q;
    q.push(1);
    q.push(2);
    EXPECT_TRUE(q.pop());
    ASSERT_NE(q.front(), nullptr);
    EXPECT_EQ(*q.front(), 2);
}

// The segment is used up and the producer has not linked the next one yet:
// pop() must report empty instead of reading past the segment.
TEST(SpscUnboundedQueue, ExtraPopAtExhaustedSegment)
{
    spsc_unbounded_queue<std::string, 2> q;
    q.push("a");
    q.push("b"); // fills the first segment exactly
    EXPECT_TRUE(q.pop());
    EXPECT_TRUE(q.pop());
    for (int i = 0; i < 3; ++i) EXPECT_FALSE(q.pop());
    q.push("c"); // goes into a new segment
    std::string s;
    ASSERT_TRUE(q.try_pop(s));
    EXPECT_EQ(s, "c");
    EXPECT_FALSE(q.pop());
}

// Random pushes, pops and extra pops, checked against std::deque, across
// many segment boundaries.
TEST(SpscUnboundedQueue, RandomPopsMatchModel)
{
    spsc_unbounded_queue<int, 4> q;
    std::deque<int> model;
    std::mt19937 rng(7);
    int next = 0;
    for (int step = 0; step < 100'000; ++step) {
        switch (rng() % 3) {
        case 0:
            q.push(next);
            model.push_back(next++);
            break;
        case 1: {
            const bool popped = q.pop();
            ASSERT_EQ(popped, !model.empty());
            if (popped) model.pop_front();
            break;
        }
        default:
            if (model.empty()) {
                ASSERT_EQ(q.front(), nullptr);
            } else {
                ASSERT_NE(q.front(), nullptr);
                ASSERT_EQ(*q.front(), model.front());
            }
        }
    }
}

TEST(SpscUnboundedQueue, DestroysEachElementExactlyOnce)
{
    auto tracker = std::make_shared<int>(0);
    {
        spsc_unbounded_queue<std::shared_ptr<int>, 2> q;
        for (int i = 0; i < 5; ++i) q.push(tracker);
        EXPECT_TRUE(q.pop());
        EXPECT_TRUE(q.pop());
        EXPECT_EQ(tracker.use_count(), 4);
        for (int i = 0; i < 3; ++i) EXPECT_TRUE(q.pop());
        EXPECT_FALSE(q.pop());
        EXPECT_EQ(tracker.use_count(), 1);
        q.push(tracker);
    }
    EXPECT_EQ(tracker.use_count(), 1);
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
