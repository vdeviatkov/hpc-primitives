#include <hpc/concurrency/mpmc_queue.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using hpc::concurrency::mpmc_queue;

TEST(MpmcQueue, CapacityRoundsUpToPowerOfTwoMinimumTwo)
{
    EXPECT_EQ(mpmc_queue<int>(0).capacity(), 2u);
    EXPECT_EQ(mpmc_queue<int>(1).capacity(), 2u);
    EXPECT_EQ(mpmc_queue<int>(100).capacity(), 128u);
}

TEST(MpmcQueue, SingleThreadFifoAndBounds)
{
    mpmc_queue<int> q(8);
    EXPECT_TRUE(q.empty());
    for (int i = 0; i < 8; ++i) EXPECT_TRUE(q.try_push(i));
    EXPECT_FALSE(q.try_push(8));

    int v = -1;
    for (int i = 0; i < 8; ++i) {
        ASSERT_TRUE(q.try_pop(v));
        EXPECT_EQ(v, i);
    }
    EXPECT_FALSE(q.try_pop(v));
}

TEST(MpmcQueue, ManyLaps)
{
    mpmc_queue<int> q(4);
    int v = 0;
    for (int i = 0; i < 1000; ++i) {
        ASSERT_TRUE(q.try_push(i));
        ASSERT_TRUE(q.try_pop(v));
        EXPECT_EQ(v, i);
    }
}

TEST(MpmcQueue, EmplaceNonTrivial)
{
    mpmc_queue<std::string> q(4);
    ASSERT_TRUE(q.try_emplace(3u, 'a'));
    std::string s;
    ASSERT_TRUE(q.try_pop(s));
    EXPECT_EQ(s, "aaa");
}

TEST(MpmcQueue, DestroysRemainingElements)
{
    auto tracker = std::make_shared<int>(0);
    {
        mpmc_queue<std::shared_ptr<int>> q(8);
        for (int i = 0; i < 3; ++i) ASSERT_TRUE(q.try_push(tracker));
    }
    EXPECT_EQ(tracker.use_count(), 1);
}

// Every item is delivered exactly once, and each consumer sees each
// producer's items in the order they were pushed.
TEST(MpmcQueue, ConcurrentExactlyOnceAndPerProducerOrder)
{
    constexpr unsigned kProducers = 4, kConsumers = 4;
    constexpr std::uint64_t kPerProducer = 200'000;
    mpmc_queue<std::uint64_t> q(256);

    std::atomic<std::uint64_t> consumed{0};
    std::vector<std::vector<std::uint64_t>> received(kConsumers);
    std::vector<std::thread> threads;

    for (unsigned p = 0; p < kProducers; ++p) {
        threads.emplace_back([&, p] {
            for (std::uint64_t i = 0; i < kPerProducer; ++i)
                while (!q.try_push(p * kPerProducer + i)) std::this_thread::yield();
        });
    }
    for (unsigned c = 0; c < kConsumers; ++c) {
        threads.emplace_back([&, c] {
            std::uint64_t v = 0;
            while (consumed.load(std::memory_order_relaxed) < kProducers * kPerProducer) {
                if (q.try_pop(v)) {
                    received[c].push_back(v);
                    consumed.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& t : threads) t.join();

    std::vector<char> seen(kProducers * kPerProducer, 0);
    for (const auto& items : received) {
        std::vector<std::int64_t> last(kProducers, -1);
        for (std::uint64_t v : items) {
            ASSERT_EQ(seen[v], 0) << "duplicate " << v;
            seen[v] = 1;
            const auto producer = v / kPerProducer;
            const auto index    = static_cast<std::int64_t>(v % kPerProducer);
            ASSERT_GT(index, last[producer]);
            last[producer] = index;
        }
    }
    for (char s : seen) ASSERT_EQ(s, 1);
}
