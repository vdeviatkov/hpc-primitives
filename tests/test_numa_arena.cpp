#include <hpc/memory/numa_arena.hpp>

#include <gtest/gtest.h>

#include <cstring>

using hpc::memory::numa_arena;

TEST(NumaArena, AllocateResetOnLocalNode)
{
    numa_arena a(1 << 16, -1);
    EXPECT_EQ(a.capacity(), 1u << 16);

    void* p = a.allocate(4096, 64);
    ASSERT_NE(p, nullptr);
    std::memset(p, 0xCD, 4096);
    EXPECT_EQ(a.used(), 4096u);

    a.reset();
    EXPECT_EQ(a.allocate(1 << 16, 1), p);
}

TEST(NumaArena, ExplicitNodeZero)
{
    numa_arena a(4096, 0);
    EXPECT_TRUE(a.node() == 0 || a.node() == -1);
    EXPECT_NE(a.allocate(64), nullptr);
}
