#include <hpc/ipc/shm_mpmc_queue.hpp>
#include <hpc/ipc/shm_spsc_queue.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

using hpc::ipc::shm_mpmc_queue;

namespace {

struct message {
    std::uint32_t producer;
    std::uint32_t pad;
    std::uint64_t seq;
};

std::string unique_name(const char* tag)
{
    return "/hpc_mpmc_" + std::string(tag) + "_" + std::to_string(::getpid());
}

} // namespace

TEST(ShmMpmcQueue, CreateOpenPushPop)
{
    const auto name = unique_name("basic");
    auto a = shm_mpmc_queue<message>::create(name, 5);
    auto b = shm_mpmc_queue<message>::open(name);
    EXPECT_EQ(a.capacity(), 8u);
    EXPECT_EQ(b.capacity(), 8u);

    message m{};
    EXPECT_FALSE(b.try_pop(m));
    for (std::uint64_t i = 0; i < 8; ++i) EXPECT_TRUE(a.try_push({0, 0, i}));
    EXPECT_FALSE(b.try_push({0, 0, 99})); // full, seen through either mapping

    for (std::uint64_t i = 0; i < 8; ++i) {
        ASSERT_TRUE(b.try_pop(m));
        EXPECT_EQ(m.seq, i);
    }
    EXPECT_FALSE(a.try_pop(m));

    // Second lap reuses the re-armed slots.
    for (std::uint64_t i = 0; i < 20; ++i) {
        ASSERT_TRUE(a.try_push({0, 0, i}));
        ASSERT_TRUE(b.try_pop(m));
        EXPECT_EQ(m.seq, i);
    }
}

TEST(ShmMpmcQueue, MinimumCapacityIsTwo)
{
    auto q = shm_mpmc_queue<std::uint64_t>::create(unique_name("min"), 1);
    EXPECT_EQ(q.capacity(), 2u);
}

TEST(ShmMpmcQueue, OpenRejectsMismatchedType)
{
    const auto name = unique_name("mismatch");
    auto q = shm_mpmc_queue<message>::create(name, 4);
    EXPECT_THROW(shm_mpmc_queue<std::uint32_t>::open(name), std::runtime_error);
}

TEST(ShmMpmcQueue, OpenRejectsSpscQueue)
{
    const auto name = unique_name("spsc");
    auto q = hpc::ipc::shm_spsc_queue<message>::create(name, 4);
    EXPECT_THROW(shm_mpmc_queue<message>::open(name), std::runtime_error);
}

TEST(ShmMpmcQueue, OpenMissingThrows)
{
    EXPECT_THROW(shm_mpmc_queue<message>::open(unique_name("missing")), std::system_error);
}

// Every producer and consumer has its own mapping, as separate processes would.
TEST(ShmMpmcQueue, ConcurrentAcrossMappings)
{
    constexpr std::uint32_t kProducers = 4, kConsumers = 4;
    constexpr std::uint64_t kPerProducer = 50'000;
    const auto name = unique_name("conc");
    auto owner = shm_mpmc_queue<message>::create(name, 64);

    std::vector<std::thread> threads;
    std::vector<std::uint64_t> sums(kConsumers, 0), counts(kConsumers, 0);
    std::vector<int> order_ok(kConsumers, 1);
    for (std::uint32_t c = 0; c < kConsumers; ++c) {
        threads.emplace_back([&, c] {
            auto q = shm_mpmc_queue<message>::open(name);
            std::vector<std::uint64_t> next(kProducers, 0);
            message m{};
            for (;;) {
                if (!q.try_pop(m)) {
                    std::this_thread::yield();
                    continue;
                }
                if (m.producer == kProducers) break; // stop marker
                // Positions are claimed in order on both sides, so each
                // consumer sees each producer's items in increasing order.
                if (m.seq < next[m.producer]) order_ok[c] = 0;
                next[m.producer] = m.seq + 1;
                sums[c] += m.seq;
                ++counts[c];
            }
        });
    }
    std::vector<std::thread> producers;
    for (std::uint32_t p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p] {
            auto q = shm_mpmc_queue<message>::open(name);
            for (std::uint64_t i = 0; i < kPerProducer; ++i)
                while (!q.try_push({p, 0, i})) std::this_thread::yield();
        });
    }
    for (auto& t : producers) t.join();
    for (std::uint32_t c = 0; c < kConsumers; ++c)
        while (!owner.try_push({kProducers, 0, 0})) std::this_thread::yield();
    for (auto& t : threads) t.join();

    std::uint64_t sum = 0, count = 0;
    for (std::uint32_t c = 0; c < kConsumers; ++c) {
        sum += sums[c];
        count += counts[c];
        EXPECT_TRUE(order_ok[c]) << "consumer " << c;
    }
    EXPECT_EQ(count, kProducers * kPerProducer);
    EXPECT_EQ(sum, kProducers * (kPerProducer * (kPerProducer - 1) / 2));
}

TEST(ShmMpmcQueue, CrossProcessProducers)
{
    constexpr int kChildren = 3;
    constexpr std::uint64_t kPerChild = 20'000;
    const auto name = unique_name("fork");
    auto q = shm_mpmc_queue<message>::create(name, 256);

    std::vector<pid_t> children;
    for (int c = 0; c < kChildren; ++c) {
        const pid_t pid = ::fork();
        ASSERT_NE(pid, -1);
        if (pid == 0) {
            int rc = 0;
            try {
                auto child = shm_mpmc_queue<message>::open(name);
                for (std::uint64_t i = 0; i < kPerChild; ++i)
                    while (!child.try_push({static_cast<std::uint32_t>(c), 0, i})) ::sched_yield();
            } catch (...) {
                rc = 1;
            }
            ::_exit(rc);
        }
        children.push_back(pid);
    }

    std::uint64_t sum = 0;
    std::vector<std::uint64_t> next(kChildren, 0);
    message m{};
    for (std::uint64_t got = 0; got < kChildren * kPerChild;) {
        if (!q.try_pop(m)) continue;
        ASSERT_LT(m.producer, std::uint32_t{kChildren});
        ASSERT_EQ(m.seq, next[m.producer]); // single consumer: exact per-producer FIFO
        ++next[m.producer];
        sum += m.seq;
        ++got;
    }
    for (pid_t pid : children) {
        int status = 0;
        ASSERT_EQ(::waitpid(pid, &status, 0), pid);
        EXPECT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    EXPECT_EQ(sum, kChildren * (kPerChild * (kPerChild - 1) / 2));
}
