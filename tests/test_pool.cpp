#include <hpc/memory/pool.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <set>
#include <stdexcept>
#include <string>

using hpc::memory::fixed_pool;
using hpc::memory::object_pool;

TEST(FixedPool, ExhaustsAndRecycles)
{
    fixed_pool pool(sizeof(int), 4);
    std::set<void*> blocks;
    for (int i = 0; i < 4; ++i) {
        void* p = pool.allocate();
        ASSERT_NE(p, nullptr);
        EXPECT_TRUE(pool.owns(p));
        blocks.insert(p);
    }
    EXPECT_EQ(blocks.size(), 4u);
    EXPECT_EQ(pool.allocate(), nullptr);

    void* freed = *blocks.begin();
    pool.deallocate(freed);
    EXPECT_EQ(pool.allocate(), freed);
}

// Regression: an odd block size used to place free-list pointers at
// misaligned addresses.
TEST(FixedPool, BlocksAreAligned)
{
    fixed_pool pool(12, 16);
    EXPECT_EQ(pool.block_size() % alignof(std::max_align_t), 0u);
    for (int i = 0; i < 16; ++i) {
        auto addr = reinterpret_cast<std::uintptr_t>(pool.allocate());
        EXPECT_EQ(addr % alignof(std::max_align_t), 0u);
    }

    fixed_pool wide(8, 4, 128);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(wide.allocate()) % 128, 0u);
}

TEST(FixedPool, FirstAllocationsAreSequential)
{
    fixed_pool pool(64, 8);
    auto* a = static_cast<std::byte*>(pool.allocate());
    auto* b = static_cast<std::byte*>(pool.allocate());
    EXPECT_EQ(b - a, static_cast<std::ptrdiff_t>(pool.block_size()));
}

TEST(ObjectPool, CreateAndDestroy)
{
    object_pool<std::string> pool(2);
    std::string* a = pool.create(3u, 'a');
    std::string* b = pool.create("b");
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(*a, "aaa");
    EXPECT_EQ(pool.create("c"), nullptr);
    pool.destroy(a);
    EXPECT_NE(pool.create("c"), nullptr);
}

TEST(ObjectPool, ConstructorExceptionReturnsBlock)
{
    struct throws {
        explicit throws(bool t) { if (t) throw std::runtime_error("boom"); }
    };
    object_pool<throws> pool(1);
    EXPECT_THROW((void)pool.create(true), std::runtime_error);
    EXPECT_NE(pool.create(false), nullptr);
}
