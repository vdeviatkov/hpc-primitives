#pragma once

#include <algorithm>
#include <cstddef>
#include <new>
#include <utility>

namespace hpc::memory {

// Fixed-size block pool: O(1) allocate and deallocate, with no per-block
// header.
//
// Data layout: one contiguous slab of block_count blocks. Each block is
// `stride` bytes, which is block_size rounded up to the alignment and at least
// one pointer. free_ is the head of a singly linked free list.
//
// How it works
//   - The free list is intrusive: each free block's first bytes hold the
//     pointer to the next free block, so tracking free blocks costs no extra
//     memory.
//   - The constructor links every block in reverse order, so the first
//     allocations walk the slab forwards, which is good for locality.
//   - allocate() pops the list head (nullptr when the pool is empty).
//     deallocate(p) pushes p back. Each is a couple of loads and stores.
//   - Blocks are rounded up to the requested alignment, so the next-pointer
//     stored in a free block is always aligned.
//   - owns(p) is a range check against the slab.
//   - The slab never grows. Not thread-safe.
class fixed_pool {
public:
    // `alignment` must be a power of two. Throws std::bad_alloc.
    fixed_pool(std::size_t block_size, std::size_t block_count,
               std::size_t alignment = alignof(std::max_align_t))
        : alignment_(std::max(alignment, alignof(node)))
        , stride_(round_up(std::max(block_size, sizeof(node)), alignment_))
        , count_(block_count)
        , slab_(static_cast<std::byte*>(::operator new(stride_ * count_, std::align_val_t{alignment_})))
    {
        // Link in reverse so the first allocations walk the slab forwards.
        for (std::size_t i = count_; i-- > 0;)
            free_ = ::new (slab_ + i * stride_) node{free_};
    }

    ~fixed_pool() { ::operator delete(slab_, std::align_val_t{alignment_}); }

    fixed_pool(const fixed_pool&)            = delete;
    fixed_pool& operator=(const fixed_pool&) = delete;

    // Returns nullptr when exhausted.
    [[nodiscard]] void* allocate() noexcept
    {
        node* n = free_;
        if (n != nullptr) free_ = n->next;
        return n;
    }

    // `p` must come from this pool's allocate(). nullptr is ignored.
    void deallocate(void* p) noexcept
    {
        if (p != nullptr) free_ = ::new (p) node{free_};
    }

    [[nodiscard]] bool owns(const void* p) const noexcept
    {
        const auto* b = static_cast<const std::byte*>(p);
        return b >= slab_ && b < slab_ + stride_ * count_;
    }

    [[nodiscard]] std::size_t block_size() const noexcept { return stride_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return count_; }

private:
    struct node {
        node* next;
    };

    static constexpr std::size_t round_up(std::size_t n, std::size_t a) noexcept
    {
        return (n + a - 1) & ~(a - 1);
    }

    std::size_t alignment_;
    std::size_t stride_;
    std::size_t count_;
    std::byte*  slab_;
    node*       free_{nullptr};
};

// Typed object pool: a fixed_pool sized and aligned for T. create() takes a
// block and placement-news a T into it; destroy() runs ~T and returns the
// block to the pool.
template <class T>
class object_pool {
public:
    explicit object_pool(std::size_t capacity) : pool_(sizeof(T), capacity, alignof(T)) {}

    // Returns nullptr when exhausted. Propagates exceptions from T's constructor.
    template <class... Args>
    [[nodiscard]] T* create(Args&&... args)
    {
        void* p = pool_.allocate();
        if (p == nullptr) return nullptr;
        try {
            return ::new (p) T(std::forward<Args>(args)...);
        } catch (...) {
            pool_.deallocate(p);
            throw;
        }
    }

    void destroy(T* p) noexcept
    {
        if (p == nullptr) return;
        p->~T();
        pool_.deallocate(p);
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return pool_.capacity(); }

private:
    fixed_pool pool_;
};

} // namespace hpc::memory
