#include <hpc/memory/huge_pages.hpp>

#include <limits>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#if defined(__linux__)
#include <cstdio>
#endif
#endif

namespace hpc::memory {

namespace {

// Rounds n up to a multiple of the power-of-two `page`; 0 on overflow.
std::size_t round_up(std::size_t n, std::size_t page) noexcept
{
    if (n > std::numeric_limits<std::size_t>::max() - (page - 1)) return 0;
    return (n + page - 1) & ~(page - 1);
}

#if defined(__linux__)
// Encodes log2(page size) into mmap flags; values from <linux/mman.h>, which
// older glibc headers do not expose.
#ifndef MAP_HUGE_SHIFT
#define MAP_HUGE_SHIFT 26
#endif
constexpr std::size_t k2MiB = std::size_t{1} << 21;
constexpr std::size_t k1GiB = std::size_t{1} << 30;
constexpr int map_huge_2mb = 21 << MAP_HUGE_SHIFT;
constexpr int map_huge_1gb = 30 << MAP_HUGE_SHIFT;

// Default hugetlbfs page size from /proc/meminfo ("Hugepagesize: 2048 kB"),
// or 0 if unavailable.
std::size_t linux_default_huge_page_size() noexcept
{
    std::size_t kb = 0;
    if (std::FILE* f = std::fopen("/proc/meminfo", "r")) {
        char line[256];
        while (std::fgets(line, sizeof(line), f) != nullptr) {
            if (std::sscanf(line, "Hugepagesize: %zu kB", &kb) == 1) break;
        }
        std::fclose(f);
    }
    return kb * 1024;
}

// One explicit hugetlbfs attempt; size_flag 0 means the default huge size.
page_region try_hugetlb(std::size_t size, std::size_t page, int size_flag) noexcept
{
    const std::size_t bytes = round_up(size, page);
    if (bytes == 0) return {};
    void* p = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | size_flag, -1, 0);
    if (p == MAP_FAILED) return {};
    return {p, bytes, page, true};
}
#endif

} // namespace

page_region map_pages(std::size_t size, page_kind preferred) noexcept
{
    if (size == 0) return {};

#if defined(_WIN32)
    // Windows has one large-page size (2 MiB on x64), used for every huge kind.
    if (preferred != page_kind::regular) {
        if (const SIZE_T large = ::GetLargePageMinimum(); large != 0) {
            const std::size_t bytes = round_up(size, large);
            if (bytes != 0) {
                if (void* p = ::VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT | MEM_LARGE_PAGES,
                                             PAGE_READWRITE)) {
                    return {p, bytes, large, true};
                }
            }
        }
    }
    SYSTEM_INFO info{};
    ::GetSystemInfo(&info);
    const std::size_t page  = info.dwPageSize;
    const std::size_t bytes = round_up(size, page);
    if (bytes == 0) return {};
    if (void* p = ::VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) {
        return {p, bytes, page, false};
    }
    return {};
#else
#if defined(__linux__)
    switch (preferred) {
    case page_kind::huge_1g:
        if (auto r = try_hugetlb(size, k1GiB, map_huge_1gb); r.ptr != nullptr) return r;
        [[fallthrough]];
    case page_kind::huge_2m:
        if (auto r = try_hugetlb(size, k2MiB, map_huge_2mb); r.ptr != nullptr) return r;
        break;
    case page_kind::huge:
        if (const std::size_t page = linux_default_huge_page_size(); page != 0) {
            if (auto r = try_hugetlb(size, page, 0); r.ptr != nullptr) return r;
        }
        break;
    case page_kind::regular:
        break;
    }
#else
    (void)preferred; // no huge pages here: always regular pages
#endif
    const long sys_page = ::sysconf(_SC_PAGESIZE);
    const std::size_t page  = sys_page > 0 ? static_cast<std::size_t>(sys_page) : 4096;
    const std::size_t bytes = round_up(size, page);
    if (bytes == 0) return {};
    void* p = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return {};
#if defined(__linux__) && defined(MADV_HUGEPAGE) && defined(MADV_NOHUGEPAGE)
    // Without explicit huge pages, ask for transparent ones, unless the caller
    // asked for regular pages, in which case keep THP away so page_size is true.
    ::madvise(p, bytes, preferred == page_kind::regular ? MADV_NOHUGEPAGE : MADV_HUGEPAGE);
#endif
    return {p, bytes, page, false};
#endif
}

void unmap_pages(const page_region& region) noexcept
{
    if (region.ptr == nullptr) return;
#if defined(_WIN32)
    ::VirtualFree(region.ptr, 0, MEM_RELEASE);
#else
    ::munmap(region.ptr, region.size);
#endif
}

} // namespace hpc::memory
