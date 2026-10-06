#include <hpc/memory/huge_pages.hpp>

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

std::size_t round_up(std::size_t n, std::size_t page) noexcept
{
    return (n + page - 1) & ~(page - 1);
}

#if defined(__linux__)
// Default hugetlbfs page size from /proc/meminfo ("Hugepagesize: 2048 kB"),
// or 0 if unavailable.
std::size_t linux_huge_page_size() noexcept
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
#endif

} // namespace

page_region map_pages(std::size_t size) noexcept
{
    if (size == 0) return {};

#if defined(_WIN32)
    if (const SIZE_T large = ::GetLargePageMinimum(); large != 0) {
        const std::size_t bytes = round_up(size, large);
        if (void* p = ::VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT | MEM_LARGE_PAGES,
                                     PAGE_READWRITE)) {
            return {p, bytes, large, true};
        }
    }
    SYSTEM_INFO info{};
    ::GetSystemInfo(&info);
    const std::size_t page  = info.dwPageSize;
    const std::size_t bytes = round_up(size, page);
    if (void* p = ::VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) {
        return {p, bytes, page, false};
    }
    return {};
#else
#if defined(__linux__)
    if (const std::size_t huge = linux_huge_page_size(); huge != 0) {
        const std::size_t bytes = round_up(size, huge);
        void* p = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
        if (p != MAP_FAILED) return {p, bytes, huge, true};
    }
#endif
    const long sys_page = ::sysconf(_SC_PAGESIZE);
    const std::size_t page  = sys_page > 0 ? static_cast<std::size_t>(sys_page) : 4096;
    const std::size_t bytes = round_up(size, page);
    void* p = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return {};
#if defined(__linux__) && defined(MADV_HUGEPAGE)
    ::madvise(p, bytes, MADV_HUGEPAGE);
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
