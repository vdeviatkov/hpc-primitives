#include <hpc/memory/huge_pages.hpp>

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>

#if !defined(_WIN32)
#include <unistd.h>
#endif

using hpc::memory::map_pages;
using hpc::memory::page_kind;
using hpc::memory::page_region;
using hpc::memory::unmap_pages;

namespace {

constexpr std::size_t k2MiB = std::size_t{1} << 21;
[[maybe_unused]] constexpr std::size_t k1GiB = std::size_t{1} << 30; // Linux only
constexpr std::size_t kSize = (std::size_t{1} << 20) + 1; // 1 MiB + 1 byte

constexpr page_kind kAllKinds[] = {page_kind::regular, page_kind::huge, page_kind::huge_2m,
                                   page_kind::huge_1g};

// Asserts the region is usable: big enough, aligned, zeroed and writable.
void check_usable(const page_region& r, std::size_t requested)
{
    ASSERT_NE(r.ptr, nullptr);
    EXPECT_GE(r.size, requested);
    ASSERT_NE(r.page_size, 0u);
    EXPECT_EQ(r.size % r.page_size, 0u);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(r.ptr) % r.page_size, 0u);

    auto* bytes = static_cast<unsigned char*>(r.ptr);
    EXPECT_EQ(bytes[0], 0);
    EXPECT_EQ(bytes[requested - 1], 0);
    std::memset(bytes, 0xAB, requested);
    EXPECT_EQ(bytes[requested - 1], 0xAB);
}

#if !defined(_WIN32)
std::size_t base_page_size()
{
    return static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
}
#endif

#if defined(__linux__)
// Free pages in the hugetlbfs pool for `page_kb`, or 0 if the pool is absent.
long free_huge_pages(std::size_t page_kb)
{
    std::ifstream f("/sys/kernel/mm/hugepages/hugepages-" + std::to_string(page_kb)
                    + "kB/free_hugepages");
    long n = 0;
    return (f >> n) ? n : 0;
}
#endif

} // namespace

TEST(HugePages, DefaultKindMapsUsableMemory)
{
    const auto r = map_pages(kSize);
    check_usable(r, kSize);
    unmap_pages(r);
}

TEST(HugePages, EveryKindMapsUsableMemory)
{
    for (page_kind kind : kAllKinds) {
        SCOPED_TRACE(static_cast<int>(kind));
        const auto r = map_pages(kSize, kind);
        check_usable(r, kSize);
        unmap_pages(r);
    }
}

TEST(HugePages, RegularIsNeverHuge)
{
    const auto r = map_pages(kSize, page_kind::regular);
    ASSERT_NE(r.ptr, nullptr);
    EXPECT_FALSE(r.huge);
#if !defined(_WIN32)
    EXPECT_EQ(r.page_size, base_page_size());
#endif
    unmap_pages(r);
}

// Whatever the machine has reserved, the reported page size must match the
// kind that was actually used.
TEST(HugePages, ReportedPageSizeMatchesWhatWasMapped)
{
    {
        const auto r = map_pages(kSize, page_kind::huge_2m);
        ASSERT_NE(r.ptr, nullptr);
        if (r.huge) {
            EXPECT_EQ(r.page_size, k2MiB);
        }
        unmap_pages(r);
    }
    {
        const auto r = map_pages(kSize, page_kind::huge_1g);
        ASSERT_NE(r.ptr, nullptr);
#if defined(__linux__)
        if (r.huge) {
            EXPECT_TRUE(r.page_size == k1GiB || r.page_size == k2MiB) << r.page_size;
        }
#endif
        unmap_pages(r);
    }
#if !defined(_WIN32)
    for (page_kind kind : kAllKinds) {
        const auto r = map_pages(kSize, kind);
        if (!r.huge) {
            EXPECT_EQ(r.page_size, base_page_size());
        }
        unmap_pages(r);
    }
#endif
}

#if defined(__linux__)
// The fallback chain is deterministic given the pool sizes, so check it
// exactly. On a machine with no reserved huge pages (the common case) every
// kind lands on regular pages; with reserved pages it must use them.
TEST(HugePages, LinuxFollowsFallbackChain)
{
    const long free_2m = free_huge_pages(2048);
    const long free_1g = free_huge_pages(1024 * 1024);

    {
        const auto r = map_pages(kSize, page_kind::huge_2m);
        ASSERT_NE(r.ptr, nullptr);
        EXPECT_EQ(r.huge, free_2m > 0);
        EXPECT_EQ(r.page_size, free_2m > 0 ? k2MiB : base_page_size());
        unmap_pages(r);
    }
    {
        const auto r = map_pages(kSize, page_kind::huge_1g);
        ASSERT_NE(r.ptr, nullptr);
        const std::size_t expected = free_1g > 0 ? k1GiB : free_2m > 0 ? k2MiB : base_page_size();
        EXPECT_EQ(r.page_size, expected);
        EXPECT_EQ(r.huge, expected != base_page_size());
        unmap_pages(r);
    }
}
#endif

TEST(HugePages, ZeroSizeIsEmpty)
{
    for (page_kind kind : kAllKinds) {
        const auto r = map_pages(0, kind);
        EXPECT_EQ(r.ptr, nullptr);
        unmap_pages(r);
    }
}

TEST(HugePages, ImpossibleSizeFailsCleanly)
{
    for (page_kind kind : kAllKinds) {
        const auto r = map_pages(std::numeric_limits<std::size_t>::max(), kind);
        EXPECT_EQ(r.ptr, nullptr);
        EXPECT_EQ(r.size, 0u);
    }
}
