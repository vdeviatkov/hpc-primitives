#pragma once

#include <cstddef>
#include <initializer_list>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace hpc::containers {

// Fixed-capacity LIFO stack with inline storage: no heap allocation, ever.
//
//  - try_push / try_emplace / try_pop report full/empty by return value and
//    are noexcept when T's corresponding operation is; push / emplace / pop
//    throw std::overflow_error / std::underflow_error instead.
//  - Slots are raw storage, so unused capacity costs no constructor calls.

template <class T, std::size_t Capacity>
class fixed_stack {
    static_assert(Capacity > 0, "Capacity must be greater than zero");
    static_assert(std::is_nothrow_destructible_v<T>,
                  "T must be nothrow destructible");

    static constexpr bool is_trivial = std::is_trivially_destructible_v<T>;

public:
    using value_type = T;
    using size_type  = std::size_t;
    using reference       = T&;
    using const_reference = const T&;

    // -- Constructors / destructor ----------------------------------------

    fixed_stack() noexcept = default;

    fixed_stack(std::initializer_list<T> il)
    {
        for (auto& v : il) {
            if (size_ == Capacity)
                throw std::overflow_error("hpc::containers::fixed_stack: capacity exceeded");
            construct(ptr(size_++), v);
        }
    }

    fixed_stack(const fixed_stack& rhs)
        noexcept(std::is_nothrow_copy_constructible_v<T>)
    {
        for (size_type i = 0; i < rhs.size_; ++i)
            construct(ptr(i), *rhs.ptr(i));
        size_ = rhs.size_;
    }

    fixed_stack(fixed_stack&& rhs)
        noexcept(std::is_nothrow_move_constructible_v<T>)
    {
        for (size_type i = 0; i < rhs.size_; ++i)
            construct(ptr(i), std::move(*rhs.ptr(i)));
        size_ = rhs.size_;
        rhs.clear();
    }

    ~fixed_stack() { clear(); }

    fixed_stack& operator=(const fixed_stack& rhs)
        noexcept(std::is_nothrow_copy_constructible_v<T>)
    {
        if (this != &rhs) {
            clear();
            for (size_type i = 0; i < rhs.size_; ++i)
                construct(ptr(i), *rhs.ptr(i));
            size_ = rhs.size_;
        }
        return *this;
    }

    fixed_stack& operator=(fixed_stack&& rhs)
        noexcept(std::is_nothrow_move_constructible_v<T>)
    {
        if (this != &rhs) {
            clear();
            for (size_type i = 0; i < rhs.size_; ++i)
                construct(ptr(i), std::move(*rhs.ptr(i)));
            size_ = rhs.size_;
            rhs.clear();
        }
        return *this;
    }

    // -- Capacity ---------------------------------------------------------

    [[nodiscard]] bool      empty()    const noexcept { return size_ == 0; }
    [[nodiscard]] bool      full()     const noexcept { return size_ == Capacity; }
    [[nodiscard]] size_type size()     const noexcept { return size_; }
    static constexpr size_type capacity() noexcept    { return Capacity; }

    // -- Element access ---------------------------------------------------

    reference top() noexcept
    {
        return *ptr(size_ - 1);
    }

    const_reference top() const noexcept
    {
        return *ptr(size_ - 1);
    }

    // -- Modifiers --------------------------------------------------------

    // Push a copy of value. Returns false if the stack is full.
    [[nodiscard]] bool try_push(const T& value)
        noexcept(std::is_nothrow_copy_constructible_v<T>)
    {
        if (size_ == Capacity) return false;
        construct(ptr(size_++), value);
        return true;
    }

    // Push by move. Returns false if the stack is full.
    [[nodiscard]] bool try_push(T&& value)
        noexcept(std::is_nothrow_move_constructible_v<T>)
    {
        if (size_ == Capacity) return false;
        construct(ptr(size_++), std::move(value));
        return true;
    }

    // Push a copy; throws std::overflow_error if full.
    void push(const T& value)
    {
        if (!try_push(value))
            throw std::overflow_error("hpc::containers::fixed_stack::push: stack is full");
    }

    // Push by move; throws std::overflow_error if full.
    void push(T&& value)
    {
        if (!try_push(std::move(value)))
            throw std::overflow_error("hpc::containers::fixed_stack::push: stack is full");
    }

    // Construct an element in-place. Returns false if the stack is full.
    template <class... Args>
    [[nodiscard]] bool try_emplace(Args&&... args)
        noexcept(std::is_nothrow_constructible_v<T, Args...>)
    {
        if (size_ == Capacity) return false;
        construct(ptr(size_++), std::forward<Args>(args)...);
        return true;
    }

    // Construct an element in-place; throws std::overflow_error if full.
    template <class... Args>
    reference emplace(Args&&... args)
    {
        if (size_ == Capacity)
            throw std::overflow_error("hpc::containers::fixed_stack::emplace: stack is full");
        T* slot = ptr(size_++);
        construct(slot, std::forward<Args>(args)...);
        return *slot;
    }

    // Pop the top element. Returns false if the stack is empty.
    [[nodiscard]] bool try_pop() noexcept
    {
        if (size_ == 0) return false;
        --size_;
        destroy(ptr(size_));
        return true;
    }

    // Pop the top element; throws std::underflow_error if empty.
    void pop()
    {
        if (size_ == 0)
            throw std::underflow_error("hpc::containers::fixed_stack::pop: stack is empty");
        --size_;
        destroy(ptr(size_));
    }

    // Pop the top element into out. Returns false if empty.
    [[nodiscard]] bool try_pop(T& out)
        noexcept(std::is_nothrow_move_assignable_v<T>)
    {
        if (size_ == 0) return false;
        --size_;
        out = std::move(*ptr(size_));
        destroy(ptr(size_));
        return true;
    }

    // Clear all elements.
    void clear() noexcept
    {
        if constexpr (!is_trivial) {
            while (size_ > 0) {
                --size_;
                ptr(size_)->~T();
            }
        }
        size_ = 0;
    }

    // Swap contents with another stack.
    void swap(fixed_stack& other)
        noexcept(std::is_nothrow_move_constructible_v<T> &&
                 std::is_nothrow_move_assignable_v<T>)
    {
        // Swap overlapping region, then move the tail from the larger stack.
        size_type common = (size_ < other.size_) ? size_ : other.size_;
        for (size_type i = 0; i < common; ++i) {
            using std::swap;
            swap(*ptr(i), *other.ptr(i));
        }
        if (size_ < other.size_) {
            for (size_type i = size_; i < other.size_; ++i) {
                construct(ptr(i), std::move(*other.ptr(i)));
                other.destroy(other.ptr(i));
            }
        } else {
            for (size_type i = other.size_; i < size_; ++i) {
                other.construct(other.ptr(i), std::move(*ptr(i)));
                destroy(ptr(i));
            }
        }
        size_type tmp = size_;
        size_ = other.size_;
        other.size_ = tmp;
    }

    // -- Comparison -------------------------------------------------------

    bool operator==(const fixed_stack& rhs) const
    {
        if (size_ != rhs.size_) return false;
        for (size_type i = 0; i < size_; ++i)
            if (!(*ptr(i) == *rhs.ptr(i))) return false;
        return true;
    }

private:
    alignas(T) std::byte storage_[sizeof(T) * Capacity];
    size_type size_ = 0;

    // -- Helpers ----------------------------------------------------------

    T* ptr(size_type i) noexcept
    {
        return std::launder(reinterpret_cast<T*>(storage_ + i * sizeof(T)));
    }

    const T* ptr(size_type i) const noexcept
    {
        return std::launder(reinterpret_cast<const T*>(storage_ + i * sizeof(T)));
    }

    template <class... Args>
    static void construct(T* p, Args&&... args)
    {
        ::new (static_cast<void*>(p)) T(std::forward<Args>(args)...);
    }

    static void destroy(T* p) noexcept
    {
        if constexpr (!is_trivial)
            p->~T();
    }
};

} // namespace hpc::containers


