#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <type_traits>
#include <utility>

namespace hpc::containers {

// Growable FIFO queue on one contiguous circular buffer, with the std::queue
// interface. std::queue sits on std::deque, which allocates and frees chunks
// as the queue moves forward; this queue never allocates in steady state.
//
// Data layout: buf_ (a heap ring whose capacity cap_ is a power of two) and
// two free-running counters, head_ (position of the front element) and
// tail_ (one past the back). A position's slot is pos & (cap_ - 1), and
// size() is tail_ - head_.
//
// How it works
//   - push: construct at buf_[tail_ & (cap_ - 1)], then ++tail_.
//   - pop: destroy buf_[head_ & (cap_ - 1)], then ++head_.
//   - Push writes only tail_ and pop writes only head_. An earlier version
//     kept head_ and a size count, so push and pop both read-modify-wrote the
//     count, and whenever the queue lived in memory each operation waited for
//     a store-to-load forward of the previous one's count. That made steady
//     push + pop about 3x slower than libstdc++'s std::queue on Zen 5.
//   - The counters may wrap around at the top of their range: cap_ is a power
//     of two, so masking and tail_ - head_ stay correct across the wrap.
//   - When the ring is full, allocate 2x the capacity (8 to start). Construct
//     the new element at its final slot first, so q.push(q.front()) is safe.
//     Then relocate the ring unrolled, front element first, so the front
//     lands at slot 0; head_ restarts at 0 and tail_ at size(). Relocation
//     moves elements, or copies them if T's move can throw (strong
//     guarantee).
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
        reserve(rhs.size());
        for (size_type i = 0, n = rhs.size(); i < n; ++i) push(rhs[i]);
    }

    queue(queue&& rhs) noexcept
        : buf_(std::exchange(rhs.buf_, nullptr))
        , cap_(std::exchange(rhs.cap_, 0))
        , head_(std::exchange(rhs.head_, 0))
        , tail_(std::exchange(rhs.tail_, 0))
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

    [[nodiscard]] bool empty() const noexcept { return head_ == tail_; }
    [[nodiscard]] size_type size() const noexcept { return tail_ - head_; }
    [[nodiscard]] size_type capacity() const noexcept { return cap_; }

    void reserve(size_type n)
    {
        if (n > cap_) reallocate(std::bit_ceil(n));
    }

    void shrink_to_fit()
    {
        const size_type n = size();
        const size_type target = n ? std::bit_ceil(n) : 0;
        if (target < cap_) reallocate(target);
    }

    // -- Element access (precondition: !empty()) -----------------------------

    reference       front() noexcept { return buf_[head_ & (cap_ - 1)]; }
    const_reference front() const noexcept { return buf_[head_ & (cap_ - 1)]; }
    reference       back() noexcept { return buf_[(tail_ - 1) & (cap_ - 1)]; }
    const_reference back() const noexcept { return buf_[(tail_ - 1) & (cap_ - 1)]; }

    // -- Modifiers -----------------------------------------------------------

    template <class... Args>
    reference emplace(Args&&... args)
    {
        if (tail_ - head_ == cap_) return emplace_grow(std::forward<Args>(args)...);
        T* p = std::construct_at(buf_ + (tail_ & (cap_ - 1)), std::forward<Args>(args)...);
        ++tail_;
        return *p;
    }

    void push(const T& v) { emplace(v); }
    void push(T&& v) { emplace(std::move(v)); }

    // Precondition: !empty().
    void pop() noexcept
    {
        std::destroy_at(buf_ + (head_ & (cap_ - 1)));
        ++head_;
    }

    // Moves the front element into `out`, then pops. Precondition: !empty().
    void pop(T& out) noexcept(std::is_nothrow_move_assignable_v<T>)
    {
        out = std::move(front());
        pop();
    }

    void clear() noexcept
    {
        while (head_ != tail_) pop();
        head_ = tail_ = 0;
    }

    void swap(queue& o) noexcept
    {
        std::swap(buf_, o.buf_);
        std::swap(cap_, o.cap_);
        std::swap(head_, o.head_);
        std::swap(tail_, o.tail_);
    }

    friend bool operator==(const queue& a, const queue& b)
    {
        if (a.size() != b.size()) return false;
        for (size_type i = 0, n = a.size(); i < n; ++i)
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
        const size_type n     = size();
        const size_type h     = head_ & (cap_ - 1);
        const size_type first = std::min(n, cap_ - h); // slots [h, cap_)
        if constexpr (std::is_nothrow_move_constructible_v<T> || !std::is_copy_constructible_v<T>) {
            std::uninitialized_move_n(buf_ + h, first, dst);
            std::uninitialized_move_n(buf_, n - first, dst + first);
        } else {
            std::uninitialized_copy_n(buf_ + h, first, dst);
            try {
                std::uninitialized_copy_n(buf_, n - first, dst + first);
            } catch (...) {
                std::destroy_n(dst, first);
                throw;
            }
        }
    }

    void adopt(T* buf, size_type cap) noexcept
    {
        const size_type n = size();
        for (size_type i = 0; i < n; ++i) std::destroy_at(&(*this)[i]);
        deallocate(buf_, cap_);
        buf_  = buf;
        cap_  = cap;
        head_ = 0;
        tail_ = n;
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
        T* p   = buf + size();
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
        ++tail_;
        return *p;
    }

    T*        buf_{nullptr};
    size_type cap_{0};
    size_type head_{0}; // free-running; written only by pop
    size_type tail_{0}; // free-running; written only by push
};

} // namespace hpc::containers
