#pragma once

#include <cstddef>

namespace hpc::memory {

// Page-granular memory straight from the OS (mmap / VirtualAlloc), bypassing
// malloc, with a choice of page size.
//
// How it works
//   - The caller names a preferred page_kind. map_pages tries it first and
//     then walks down to smaller sizes: 1 GiB -> 2 MiB -> regular pages. It
//     never fails just because huge pages are unavailable; the returned
//     page_region says what was actually mapped.
//   - Linux: explicit huge pages come from the hugetlbfs pools via
//     mmap(MAP_HUGETLB | MAP_HUGE_2MB / MAP_HUGE_1GB). They exist only if an
//     admin reserved them (/sys/kernel/mm/hugepages/hugepages-<size>kB/
//     nr_hugepages, or hugepagesz= / hugepages= on the kernel command line).
//     When every huge size fails, the fallback is regular pages with
//     madvise(MADV_HUGEPAGE), so transparent huge pages may back them later.
//     page_kind::regular instead sets MADV_NOHUGEPAGE, so the reported page
//     size stays true.
//   - Windows: large pages via VirtualAlloc(MEM_LARGE_PAGES), which needs
//     SeLockMemoryPrivilege. Large pages are 2 MiB on x64, so huge_1g falls
//     back to them. Other systems (macOS) always map regular pages.
//   - The request is rounded up to whole pages of the size actually used, so
//     a 100 MB request backed by 1 GiB pages occupies 1 GiB.
//   - Why bother: one 2 MiB page covers what would otherwise take 512 TLB
//     entries for 4 KiB pages, and one 1 GiB page covers 512 2 MiB pages.
//     That pays off for large, randomly accessed data. Mapping is a syscall,
//     so do it at startup and hand the memory to an arena or a pool.

enum class page_kind {
    regular, // the base page size: 4 KiB on x86-64 Linux, 16 KiB on Apple Silicon
    huge,    // the system's default huge page size (usually 2 MiB), else regular
    huge_2m, // 2 MiB, else regular
    huge_1g, // 1 GiB, else 2 MiB, else regular
};

struct page_region {
    void*       ptr       = nullptr;
    std::size_t size      = 0;     // bytes mapped (rounded up to page_size)
    std::size_t page_size = 0;     // page size actually used
    bool        huge      = false; // true if backed by explicit huge pages
};

// Maps at least `size` bytes of zeroed, page-aligned anonymous memory,
// trying `preferred` first and falling back as described above. Returns an
// empty region only if even regular pages cannot be mapped. Callers that
// must not run on small pages should check `huge` / `page_size` at startup.
[[nodiscard]] page_region map_pages(std::size_t size, page_kind preferred = page_kind::huge) noexcept;

// Unmaps a region returned by map_pages(). An empty region is a no-op.
void unmap_pages(const page_region& region) noexcept;

} // namespace hpc::memory
