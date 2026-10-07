#pragma once

#include <cstddef>

#include <hpc/memory/arena.hpp>

namespace hpc::memory {

// Arena whose backing memory is placed on a specific NUMA node via libnuma.
// Only available when built with HPC_HAS_NUMA=1 (Linux + libnuma).
//
// How it works
//   - The constructor takes one block from numa_alloc_onnode(capacity, node).
//     That memory is page-aligned and bound to the node with MPOL_BIND, so its
//     pages are allocated on that node when first touched.
//   - node < 0 picks the node of the CPU the calling thread runs on
//     (sched_getcpu + numa_node_of_cpu).
//   - A plain `arena` over that block hands out memory, so allocate() costs
//     the same as arena's bump pointer.
//   - The destructor returns the block with numa_free.
//   - If the kernel reports NUMA as unavailable (e.g. in a restricted
//     container), it falls back to ::operator new memory and node() returns
//     -1.
//   - Binding through libnuma is the reliable route: mbind() on operator new
//     memory fails when the address is not page-aligned.
class numa_arena {
public:
    // node < 0 selects the node of the calling thread. Throws std::bad_alloc.
    numa_arena(std::size_t capacity, int node);
    ~numa_arena();

    numa_arena(const numa_arena&)            = delete;
    numa_arena& operator=(const numa_arena&) = delete;

    [[nodiscard]] void* allocate(std::size_t bytes,
                                 std::size_t alignment = alignof(std::max_align_t)) noexcept
    {
        return arena_.allocate(bytes, alignment);
    }

    void reset() noexcept { arena_.reset(); }

    [[nodiscard]] std::size_t capacity() const noexcept { return arena_.capacity(); }
    [[nodiscard]] std::size_t used() const noexcept { return arena_.used(); }

    // NUMA node the memory is bound to, or -1 if NUMA was unavailable.
    [[nodiscard]] int node() const noexcept { return node_; }

private:
    void* memory_{nullptr};
    int   node_{-1};
    arena arena_;
};

} // namespace hpc::memory
