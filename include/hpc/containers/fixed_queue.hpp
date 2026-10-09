#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace hpc::containers {

// Fixed-capacity FIFO queue on an inline circular buffer: no heap allocation.
// Capacity is a compile-time constant and the queue never grows. The buffer
// lives inside the object, on the stack or embedded in another struct.
//
// Data layout: storage_ (raw bytes for Capacity Ts, aligned for T) and two
// free-running 64-bit counters, head_ (position of the front element) and
// tail_ (one past the back). A position's slot is pos % Capacity, and
// size() is tail_ - head_.
//
// How it works
//   - push: construct at slot tail_ % Capacity, then ++tail_.
//   - pop: destroy slot head_ % Capacity, then ++head_.
//   - Push writes only tail_ and pop writes only head_, so the two do not
//     wait on each other's stores (see hpc::containers::queue for the cost of
//     a shared size count).
//   - Capacity is a compile-time constant, so % compiles to a mask when it is
//     a power of two and to a multiply otherwise; any Capacity works. The
//     counters are 64-bit, so they do not wrap in practice; with a
//     non-power-of-two Capacity, a wrap would break the slot sequence.
//   - Unused slots are raw memory, so empty capacity runs no constructors.
//   - try_push / try_emplace / try_pop report full or empty by their return
//     value (noexcept when T's operation is). push / emplace / pop throw
//     std::overflow_error / std::underflow_error instead.
//   - Copy and move go element by element, so both are O(n). With the buffer
//     inside the object, a move has no pointer to steal.
template <class T, std::size_t Capacity>
class fixed_queue {
    static_assert(Capacity > 0, "Capacity must be greater than zero");
    static_assert(std::is_nothrow_destructible_v<T>);

public:
    using value_type      = T;
    using size_type       = std::size_t;
    using reference       = T&;
    using const_reference = const T&;

    fixed_queue() noexcept = default;

    fixed_queue(std::initializer_list<T> il)
    {
        if (il.size() > Capacity) throw std::overflow_error("hpc::containers::fixed_queue: capacity exceeded");
        for (const T& v : il) unchecked_emplace(v);
    }

    fixed_queue(const fixed_queue& rhs) noexcept(std::is_nothrow_copy_constructible_v<T>)
    {
        for (size_type i = 0; i < rhs.size(); ++i) unchecked_emplace(rhs[i]);
    }

    fixed_queue(fixed_queue&& rhs) noexcept(std::is_nothrow_move_constructible_v<T>)
    {
        for (size_type i = 0; i < rhs.size(); ++i) unchecked_emplace(std::move(rhs[i]));
        rhs.clear();
    }

    ~fixed_queue() { clear(); }

    fixed_queue& operator=(const fixed_queue& rhs) noexcept(std::is_nothrow_copy_constructible_v<T>)
    {
        if (this != &rhs) {
            clear();
            for (size_type i = 0; i < rhs.size(); ++i) unchecked_emplace(rhs[i]);
        }
        return *this;
    }

    fixed_queue& operator=(fixed_queue&& rhs) noexcept(std::is_nothrow_move_constructible_v<T>)
    {
        if (this != &rhs) {
            clear();
            for (size_type i = 0; i < rhs.size(); ++i) unchecked_emplace(std::move(rhs[i]));
            rhs.clear();
        }
        return *this;
    }

    // -- Capacity ------------------------------------------------------------

    [[nodiscard]] bool empty() const noexcept { return head_ == tail_; }
    [[nodiscard]] bool full() const noexcept { return tail_ - head_ == Capacity; }
    [[nodiscard]] size_type size() const noexcept { return static_cast<size_type>(tail_ - head_); }
    static constexpr size_type capacity() noexcept { return Capacity; }

    // -- Element access (precondition: !empty()) -----------------------------

    reference       front() noexcept { return *slot(index(head_)); }
    const_reference front() const noexcept { return *slot(index(head_)); }
    reference       back() noexcept { return *slot(index(tail_ - 1)); }
    const_reference back() const noexcept { return *slot(index(tail_ - 1)); }

    // -- Modifiers -----------------------------------------------------------

    template <class... Args>
    [[nodiscard]] bool try_emplace(Args&&... args) noexcept(std::is_nothrow_constructible_v<T, Args...>)
    {
        if (full()) return false;
        unchecked_emplace(std::forward<Args>(args)...);
        return true;
    }

    [[nodiscard]] bool try_push(const T& v) noexcept(std::is_nothrow_copy_constructible_v<T>)
    {
        return try_emplace(v);
    }

    [[nodiscard]] bool try_push(T&& v) noexcept(std::is_nothrow_move_constructible_v<T>)
    {
        return try_emplace(std::move(v));
    }

    template <class... Args>
    reference emplace(Args&&... args)
    {
        if (full()) throw std::overflow_error("hpc::containers::fixed_queue: queue is full");
        return unchecked_emplace(std::forward<Args>(args)...);
    }

    void push(const T& v) { emplace(v); }
    void push(T&& v) { emplace(std::move(v)); }

    [[nodiscard]] bool try_pop() noexcept
    {
        if (empty()) return false;
        unchecked_pop();
        return true;
    }

    [[nodiscard]] bool try_pop(T& out) noexcept(std::is_nothrow_move_assignable_v<T>)
    {
        if (empty()) return false;
        out = std::move(front());
        unchecked_pop();
        return true;
    }

    void pop()
    {
        if (empty()) throw std::underflow_error("hpc::containers::fixed_queue: queue is empty");
        unchecked_pop();
    }

    void clear() noexcept
    {
        while (head_ != tail_) unchecked_pop();
        head_ = tail_ = 0;
    }

    void swap(fixed_queue& other) noexcept(std::is_nothrow_move_constructible_v<T>)
    {
        fixed_queue tmp(std::move(other));
        other = std::move(*this);
        *this = std::move(tmp);
    }

    friend bool operator==(const fixed_queue& a, const fixed_queue& b)
    {
        if (a.size() != b.size()) return false;
        for (size_type i = 0, n = a.size(); i < n; ++i)
            if (!(a[i] == b[i])) return false;
        return true;
    }

private:
    // Slot of a free-running position.
    static size_type index(std::uint64_t pos) noexcept { return static_cast<size_type>(pos % Capacity); }

    T* slot(size_type i) noexcept { return std::launder(reinterpret_cast<T*>(storage_ + i * sizeof(T))); }
    const T* slot(size_type i) const noexcept
    {
        return std::launder(reinterpret_cast<const T*>(storage_ + i * sizeof(T)));
    }

    // i-th element from the front.
    T&       operator[](size_type i) noexcept { return *slot(index(head_ + i)); }
    const T& operator[](size_type i) const noexcept { return *slot(index(head_ + i)); }

    template <class... Args>
    T& unchecked_emplace(Args&&... args)
    {
        T* p = ::new (static_cast<void*>(slot(index(tail_)))) T(std::forward<Args>(args)...);
        ++tail_;
        return *p;
    }

    void unchecked_pop() noexcept
    {
        slot(index(head_))->~T();
        ++head_;
    }

    alignas(T) std::byte storage_[sizeof(T) * Capacity];
    std::uint64_t head_{0}; // free-running; written only by pop
    std::uint64_t tail_{0}; // free-running; written only by push
};

} // namespace hpc::containers
