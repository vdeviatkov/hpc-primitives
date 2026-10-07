#pragma once

#include <cstddef>

namespace hpc::memory {

// Page-granular memory straight from the OS (mmap / VirtualAlloc), bypassing
// malloc and preferring explicit huge pages.
//
// How it works
//   - Linux: read the huge page size from /proc/meminfo, round the request up
//     to it, and mmap with MAP_HUGETLB. That succeeds only if huge pages were
//     reserved (vm.nr_hugepages). Otherwise map regular pages and
//     madvise(MADV_HUGEPAGE), so transparent huge pages may back them later.
//   - Windows: VirtualAlloc(MEM_LARGE_PAGES), which needs
//     SeLockMemoryPrivilege; otherwise regular pages. Elsewhere: a plain mmap.
//   - The returned page_region says what you got: `huge`, the rounded size and
//     the page size. unmap_pages() releases it.
//   - The benefit is fewer TLB misses: one 2 MiB page covers what would
//     otherwise take 512 entries for 4 KiB pages. Mapping is a syscall, so do
//     it at startup and hand the memory to an arena or a pool.
struct page_region {
    void*       ptr       = nullptr;
    std::size_t size      = 0;     // bytes mapped (rounded up to page_size)
    std::size_t page_size = 0;
    bool        huge      = false; // true if backed by explicit huge pages
};

// Maps at least `size` bytes of zeroed, page-aligned anonymous memory,
// preferring explicit huge pages:
//   Linux:   mmap(MAP_HUGETLB) from the hugetlbfs pool; otherwise regular
//            pages with madvise(MADV_HUGEPAGE) as a hint for transparent
//            huge pages.
//   Windows: VirtualAlloc(MEM_LARGE_PAGES), which needs SeLockMemoryPrivilege;
//            otherwise regular pages.
//   Other:   regular pages.
// Returns an empty region on failure. Fewer TLB misses is the benefit;
// allocation itself is a syscall and belongs off the hot path.
[[nodiscard]] page_region map_pages(std::size_t size) noexcept;

// Unmaps a region returned by map_pages(). An empty region is a no-op.
void unmap_pages(const page_region& region) noexcept;

} // namespace hpc::memory
