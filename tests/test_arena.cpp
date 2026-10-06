#include <hpc/memory/arena.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <vector>

using hpc::memory::arena;
using hpc::memory::arena_allocator;

namespace {
bool is_aligned(const void* p, std::size_t a)
{
    return reinterpret_cast<std::uintptr_t>(p) % a == 0;
}
} // namespace

TEST(Arena, BumpsAndResets)
{
    arena a(1024);
    void* p1 = a.allocate(16, 8);
    void* p2 = a.allocate(16, 8);
    ASSERT_NE(p1, nullptr);
    ASSERT_NE(p2, nullptr);
    EXPECT_EQ(static_cast<std::byte*>(p2) - static_cast<std::byte*>(p1), 16);
    EXPECT_EQ(a.used(), 32u);

    a.reset();
    EXPECT_EQ(a.used(), 0u);
    EXPECT_EQ(a.allocate(1024, 1), p1);
}

TEST(Arena, HonoursAlignment)
{
    arena a(4096);
    ASSERT_NE(a.allocate(1, 1), nullptr);
    for (std::size_t align : {2u, 8u, 64u, 256u}) {
        void* p = a.allocate(3, align);
        ASSERT_NE(p, nullptr);
        EXPECT_TRUE(is_aligned(p, align)) << align;
    }
}

TEST(Arena, ExhaustionReturnsNullWithoutOverflow)
{
    arena a(64);
    EXPECT_NE(a.allocate(64, 1), nullptr);
    EXPECT_EQ(a.allocate(1, 1), nullptr);
    a.reset();
    EXPECT_EQ(a.allocate(std::numeric_limits<std::size_t>::max(), 1), nullptr);
    EXPECT_EQ(a.used(), 0u);
}

TEST(Arena, ExternalBufferAndMove)
{
    alignas(16) std::byte buffer[128];
    arena a(buffer, sizeof(buffer));
    EXPECT_EQ(a.allocate(16), buffer);

    arena b(std::move(a));
    EXPECT_EQ(b.used(), 16u);
    EXPECT_EQ(a.capacity(), 0u);
    EXPECT_EQ(a.allocate(1), nullptr);
}

TEST(ArenaAllocator, BacksStdVector)
{
    arena a(1 << 16);
    std::vector<int, arena_allocator<int>> v{arena_allocator<int>(a)};
    for (int i = 0; i < 1000; ++i) v.push_back(i);
    EXPECT_EQ(v[999], 999);
    EXPECT_GT(a.used(), 1000 * sizeof(int));
}
