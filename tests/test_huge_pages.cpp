#include <hpc/memory/huge_pages.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>

using hpc::memory::map_pages;
using hpc::memory::unmap_pages;

TEST(HugePages, MapsZeroedPageAlignedMemory)
{
    constexpr std::size_t kSize = (1 << 20) + 1;
    const auto region = map_pages(kSize);
    ASSERT_NE(region.ptr, nullptr);
    EXPECT_GE(region.size, kSize);
    ASSERT_NE(region.page_size, 0u);
    EXPECT_EQ(region.size % region.page_size, 0u);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(region.ptr) % region.page_size, 0u);

    auto* bytes = static_cast<unsigned char*>(region.ptr);
    EXPECT_EQ(bytes[0], 0);
    EXPECT_EQ(bytes[kSize - 1], 0);
    std::memset(bytes, 0xAB, region.size);

    unmap_pages(region);
}

TEST(HugePages, ZeroSizeIsEmpty)
{
    const auto region = map_pages(0);
    EXPECT_EQ(region.ptr, nullptr);
    unmap_pages(region);
}
