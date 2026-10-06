#pragma once

#include <cstddef>

namespace hpc::memory {

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
