#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <type_traits>
#include <utility>

namespace hpc::containers {

// Growable FIFO queue on a contiguous circular buffer. Same interface as
// std::queue, but a single allocation instead of std::deque's chunk map, so
// steady-state push/pop never allocates and iteration is cache-friendly.
//
//  - Capacity is a power of two; wrap-around is a mask.
//  - 2x growth. Relocation unrolls the ring so that head is at slot 0.
//  - push/emplace are safe when the argument aliases an element of the queue.
template <class T>
class queue {
public:
    using value_type      = T;
    using size_type       = std::size_t;
    using reference       = T&;
    using const_reference = const T&;

    queue() noexcept = default;

    explicit queue(size_type initial_capacity) { reserve(initial_capacity); }

    queue(std::initializer_list<T> il)
    {
        reserve(il.size());
        for (const T& v : il) push(v);
    }

    queue(const queue& rhs)
    {
        reserve(rhs.size_);
        for (size_type i = 0; i < rhs.size_; ++i) push(rhs[i]);
    }

    queue(queue&& rhs) noexcept
        : buf_(std::exchange(rhs.buf_, nullptr))
        , cap_(std::exchange(rhs.cap_, 0))
        , head_(std::exchange(rhs.head_, 0))
        , size_(std::exchange(rhs.size_, 0))
    {}

    ~queue()
    {
        clear();
        deallocate(buf_, cap_);
    }

    queue& operator=(const queue& rhs)
    {
        if (this != &rhs) {
            queue tmp(rhs);
            swap(tmp);
        }
        return *this;
    }

    queue& operator=(queue&& rhs) noexcept
    {
        queue tmp(std::move(rhs));
        swap(tmp);
        return *this;
    }

    queue& operator=(std::initializer_list<T> il)
    {
        queue tmp(il);
        swap(tmp);
        return *this;
    }

    // -- Capacity ------------------------------------------------------------

    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] size_type size() const noexcept { return size_; }
    [[nodiscard]] size_type capacity() const noexcept { return cap_; }

    void reserve(size_type n)
    {
        if (n > cap_) reallocate(std::bit_ceil(n));
    }

    void shrink_to_fit()
    {
        const size_type target = size_ ? std::bit_ceil(size_) : 0;
        if (target < cap_) reallocate(target);
    }

    // -- Element access (precondition: !empty()) -----------------------------

    reference       front() noexcept { return buf_[head_]; }
    const_reference front() const noexcept { return buf_[head_]; }
    reference       back() noexcept { return (*this)[size_ - 1]; }
    const_reference back() const noexcept { return (*this)[size_ - 1]; }

    // -- Modifiers -----------------------------------------------------------

    template <class... Args>
    reference emplace(Args&&... args)
    {
        if (size_ == cap_) return emplace_grow(std::forward<Args>(args)...);
        T* p = std::construct_at(buf_ + index(size_), std::forward<Args>(args)...);
        ++size_;
        return *p;
    }

    void push(const T& v) { emplace(v); }
    void push(T&& v) { emplace(std::move(v)); }

    // Precondition: !empty().
    void pop() noexcept
    {
        std::destroy_at(buf_ + head_);
        head_ = (head_ + 1) & (cap_ - 1);
        --size_;
    }

    // Moves the front element into `out`, then pops. Precondition: !empty().
    void pop(T& out) noexcept(std::is_nothrow_move_assignable_v<T>)
    {
        out = std::move(front());
        pop();
    }

    void clear() noexcept
    {
        while (size_ != 0) pop();
        head_ = 0;
    }

    void swap(queue& o) noexcept
    {
        std::swap(buf_, o.buf_);
        std::swap(cap_, o.cap_);
        std::swap(head_, o.head_);
        std::swap(size_, o.size_);
    }

    friend bool operator==(const queue& a, const queue& b)
    {
        if (a.size_ != b.size_) return false;
        for (size_type i = 0; i < a.size_; ++i)
            if (!(a[i] == b[i])) return false;
        return true;
    }

private:
    static T* allocate(size_type n) { return n ? std::allocator<T>{}.allocate(n) : nullptr; }

    static void deallocate(T* p, size_type n) noexcept
    {
        if (p != nullptr) std::allocator<T>{}.deallocate(p, n);
    }

    size_type index(size_type i) const noexcept { return (head_ + i) & (cap_ - 1); }

    T&       operator[](size_type i) noexcept { return buf_[index(i)]; }
    const T& operator[](size_type i) const noexcept { return buf_[index(i)]; }

    // Moves (or copies, if moving could throw) the ring, unrolled, into `dst`.
    void relocate_to(T* dst)
    {
        const size_type first = std::min(size_, cap_ - head_); // [head_, cap_)
        if constexpr (std::is_nothrow_move_constructible_v<T> || !std::is_copy_constructible_v<T>) {
            std::uninitialized_move_n(buf_ + head_, first, dst);
            std::uninitialized_move_n(buf_, size_ - first, dst + first);
        } else {
            std::uninitialized_copy_n(buf_ + head_, first, dst);
            try {
                std::uninitialized_copy_n(buf_, size_ - first, dst + first);
            } catch (...) {
                std::destroy_n(dst, first);
                throw;
            }
        }
    }

    void adopt(T* buf, size_type cap) noexcept
    {
        for (size_type i = 0; i < size_; ++i) std::destroy_at(&(*this)[i]);
        deallocate(buf_, cap_);
        buf_  = buf;
        cap_  = cap;
        head_ = 0;
    }

    void reallocate(size_type new_cap)
    {
        T* buf = allocate(new_cap);
        try {
            relocate_to(buf);
        } catch (...) {
            deallocate(buf, new_cap);
            throw;
        }
        adopt(buf, new_cap);
    }

    // Constructs the new element before relocating, so `args` may alias.
    template <class... Args>
    reference emplace_grow(Args&&... args)
    {
        const size_type new_cap = cap_ ? cap_ * 2 : 8;
        T* buf = allocate(new_cap);
        T* p   = buf + size_;
        try {
            std::construct_at(p, std::forward<Args>(args)...);
        } catch (...) {
            deallocate(buf, new_cap);
            throw;
        }
        try {
            relocate_to(buf);
        } catch (...) {
            std::destroy_at(p);
            deallocate(buf, new_cap);
            throw;
        }
        adopt(buf, new_cap);
        ++size_;
        return *p;
    }

    T*        buf_{nullptr};
    size_type cap_{0};
    size_type head_{0};
    size_type size_{0};
};

} // namespace hpc::containers
