#pragma once

#include <cstddef>
#include <initializer_list>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace hpc::containers {

// Fixed-capacity FIFO queue on an inline circular buffer: no heap allocation.
//
//  - Storage is exactly Capacity slots; wrap-around is a compare-and-subtract,
//    so Capacity need not be a power of two.
//  - try_push / try_emplace / try_pop report full/empty by return value;
//    push / emplace / pop throw std::overflow_error / std::underflow_error.
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
        for (size_type i = 0; i < rhs.size_; ++i) unchecked_emplace(rhs[i]);
    }

    fixed_queue(fixed_queue&& rhs) noexcept(std::is_nothrow_move_constructible_v<T>)
    {
        for (size_type i = 0; i < rhs.size_; ++i) unchecked_emplace(std::move(rhs[i]));
        rhs.clear();
    }

    ~fixed_queue() { clear(); }

    fixed_queue& operator=(const fixed_queue& rhs) noexcept(std::is_nothrow_copy_constructible_v<T>)
    {
        if (this != &rhs) {
            clear();
            for (size_type i = 0; i < rhs.size_; ++i) unchecked_emplace(rhs[i]);
        }
        return *this;
    }

    fixed_queue& operator=(fixed_queue&& rhs) noexcept(std::is_nothrow_move_constructible_v<T>)
    {
        if (this != &rhs) {
            clear();
            for (size_type i = 0; i < rhs.size_; ++i) unchecked_emplace(std::move(rhs[i]));
            rhs.clear();
        }
        return *this;
    }

    // -- Capacity ------------------------------------------------------------

    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] bool full() const noexcept { return size_ == Capacity; }
    [[nodiscard]] size_type size() const noexcept { return size_; }
    static constexpr size_type capacity() noexcept { return Capacity; }

    // -- Element access (precondition: !empty()) -----------------------------

    reference       front() noexcept { return *slot(head_); }
    const_reference front() const noexcept { return *slot(head_); }
    reference       back() noexcept { return (*this)[size_ - 1]; }
    const_reference back() const noexcept { return (*this)[size_ - 1]; }

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
        while (size_ != 0) unchecked_pop();
        head_ = 0;
    }

    void swap(fixed_queue& other) noexcept(std::is_nothrow_move_constructible_v<T>)
    {
        fixed_queue tmp(std::move(other));
        other = std::move(*this);
        *this = std::move(tmp);
    }

    friend bool operator==(const fixed_queue& a, const fixed_queue& b)
    {
        if (a.size_ != b.size_) return false;
        for (size_type i = 0; i < a.size_; ++i)
            if (!(a[i] == b[i])) return false;
        return true;
    }

private:
    static size_type wrap(size_type i) noexcept { return i >= Capacity ? i - Capacity : i; }

    T* slot(size_type i) noexcept { return std::launder(reinterpret_cast<T*>(storage_ + i * sizeof(T))); }
    const T* slot(size_type i) const noexcept
    {
        return std::launder(reinterpret_cast<const T*>(storage_ + i * sizeof(T)));
    }

    // i-th element from the front.
    T&       operator[](size_type i) noexcept { return *slot(wrap(head_ + i)); }
    const T& operator[](size_type i) const noexcept { return *slot(wrap(head_ + i)); }

    template <class... Args>
    T& unchecked_emplace(Args&&... args)
    {
        T* p = ::new (static_cast<void*>(slot(wrap(head_ + size_)))) T(std::forward<Args>(args)...);
        ++size_;
        return *p;
    }

    void unchecked_pop() noexcept
    {
        slot(head_)->~T();
        head_ = wrap(head_ + 1);
        --size_;
    }

    alignas(T) std::byte storage_[sizeof(T) * Capacity];
    size_type head_{0};
    size_type size_{0};
};

} // namespace hpc::containers
