#include <hpc/memory/numa_arena.hpp>

#include <new>

#include <numa.h>
#include <sched.h>

namespace hpc::memory {

numa_arena::numa_arena(std::size_t capacity, int node)
{
    if (::numa_available() != -1) {
        if (node < 0) {
            const int cpu = ::sched_getcpu();
            node = cpu >= 0 ? ::numa_node_of_cpu(cpu) : 0;
        }
        // numa_alloc_onnode mmaps page-aligned memory with an MPOL_BIND policy.
        memory_ = ::numa_alloc_onnode(capacity, node);
        if (memory_ == nullptr) throw std::bad_alloc();
        node_ = node;
    } else {
        memory_ = ::operator new(capacity);
    }
    arena_ = arena(memory_, capacity);
}

numa_arena::~numa_arena()
{
    if (node_ >= 0)
        ::numa_free(memory_, arena_.capacity());
    else
        ::operator delete(memory_);
}

} // namespace hpc::memory
