#include <hpc/ipc/shm_spsc_queue.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>

#include <unistd.h>

using hpc::ipc::shm_spsc_queue;

namespace {

struct message {
    std::uint64_t seq;
    double        value;
};

std::string unique_name(const char* tag)
{
    return "/hpc_test_" + std::string(tag) + "_" + std::to_string(::getpid());
}

} // namespace

TEST(ShmSpscQueue, CreateOpenPushPop)
{
    const auto name = unique_name("basic");
    auto producer = shm_spsc_queue<message>::create(name, 5);
    auto consumer = shm_spsc_queue<message>::open(name);
    EXPECT_EQ(producer.capacity(), 8u);
    EXPECT_EQ(consumer.capacity(), 8u);

    for (std::uint64_t i = 0; i < 8; ++i) EXPECT_TRUE(producer.try_push({i, 0.5 * double(i)}));
    EXPECT_FALSE(producer.try_push({99, 0}));

    message m{};
    for (std::uint64_t i = 0; i < 8; ++i) {
        ASSERT_TRUE(consumer.try_pop(m));
        EXPECT_EQ(m.seq, i);
        EXPECT_EQ(m.value, 0.5 * double(i));
    }
    EXPECT_FALSE(consumer.try_pop(m));
}

TEST(ShmSpscQueue, OpenRejectsMismatchedType)
{
    const auto name = unique_name("mismatch");
    auto producer = shm_spsc_queue<message>::create(name, 4);
    EXPECT_THROW(shm_spsc_queue<std::uint32_t>::open(name), std::runtime_error);
}

TEST(ShmSpscQueue, OpenMissingThrows)
{
    EXPECT_THROW(shm_spsc_queue<message>::open(unique_name("missing")), std::system_error);
}

TEST(ShmSpscQueue, ConcurrentAcrossMappings)
{
    constexpr std::uint64_t kItems = 200'000;
    const auto name = unique_name("concurrent");
    auto producer = shm_spsc_queue<message>::create(name, 64);
    auto consumer = shm_spsc_queue<message>::open(name);

    std::thread t([&] {
        for (std::uint64_t i = 0; i < kItems; ++i)
            while (!producer.try_push({i, 0})) std::this_thread::yield();
    });
    message m{};
    for (std::uint64_t i = 0; i < kItems;) {
        if (consumer.try_pop(m)) {
            ASSERT_EQ(m.seq, i);
            ++i;
        }
    }
    t.join();
}
