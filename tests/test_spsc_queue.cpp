#include <hpc/concurrency/spsc_queue.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <deque>
#include <memory>
#include <random>
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
    EXPECT_TRUE(q.pop());
    EXPECT_EQ(q.front(), nullptr);
}

// pop() checks for an element itself, so calling it on an empty queue is a
// harmless no-op that returns false. Before, an extra pop() marked an unwritten
// slot as free and the queue then reported full to the producer and empty to
// the consumer forever.
TEST(SpscQueue, PopOnEmptyReturnsFalseAndQueueStaysUsable)
{
    spsc_queue<int> q(4);
    EXPECT_FALSE(q.pop());
    ASSERT_TRUE(q.try_push(10));
    EXPECT_TRUE(q.pop());
    EXPECT_FALSE(q.pop());
    EXPECT_FALSE(q.pop());

    for (int i = 0; i < 4; ++i) EXPECT_TRUE(q.try_push(20 + i)); // full capacity still usable
    EXPECT_FALSE(q.try_push(99));
    int v = 0;
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(q.try_pop(v));
        EXPECT_EQ(v, 20 + i);
    }
    EXPECT_FALSE(q.try_pop(v));
}

TEST(SpscQueue, PopWithoutFrontRemovesOldest)
{
    spsc_queue<int> q(4);
    ASSERT_TRUE(q.try_push(1));
    ASSERT_TRUE(q.try_push(2));
    EXPECT_TRUE(q.pop());
    ASSERT_NE(q.front(), nullptr);
    EXPECT_EQ(*q.front(), 2);
}

// Non-trivial T: an unchecked pop() would run ~string on raw memory, which
// ASan reports.
TEST(SpscQueue, ExtraPopsWithNonTrivialType)
{
    spsc_queue<std::string> q(2);
    ASSERT_TRUE(q.try_push(std::string(100, 'a')));
    EXPECT_TRUE(q.pop());
    for (int i = 0; i < 3; ++i) EXPECT_FALSE(q.pop());
    ASSERT_TRUE(q.try_push(std::string(100, 'b')));
    std::string s;
    ASSERT_TRUE(q.try_pop(s));
    EXPECT_EQ(s, std::string(100, 'b'));
}

// Random pushes, pops and extra pops, checked against std::deque, through
// many wrap-arounds of a small ring.
TEST(SpscQueue, RandomPopsMatchModel)
{
    spsc_queue<int> q(8);
    std::deque<int> model;
    std::mt19937 rng(42);
    int next = 0;
    for (int step = 0; step < 100'000; ++step) {
        switch (rng() % 3) {
        case 0:
            if (q.try_push(next)) model.push_back(next);
            else EXPECT_EQ(model.size(), q.capacity());
            ++next;
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

TEST(SpscQueue, DestroysEachElementExactlyOnce)
{
    auto tracker = std::make_shared<int>(0);
    {
        spsc_queue<std::shared_ptr<int>> q(4);
        for (int i = 0; i < 3; ++i) ASSERT_TRUE(q.try_push(tracker));
        EXPECT_TRUE(q.pop());
        EXPECT_EQ(tracker.use_count(), 3);
        EXPECT_TRUE(q.pop());
        EXPECT_TRUE(q.pop());
        EXPECT_FALSE(q.pop());
        EXPECT_EQ(tracker.use_count(), 1);
        ASSERT_TRUE(q.try_push(tracker));
    }
    EXPECT_EQ(tracker.use_count(), 1);
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
