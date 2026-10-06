#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

#include <hpc/support/platform.hpp>

namespace hpc::concurrency {

// Bounded wait-free single-producer / single-consumer queue.
//
//  - Each slot carries a sequence number, as in mpmc_queue but without CAS:
//    slot.seq == pos means free for the producer at position pos, and
//    seq == pos + 1 means it holds an element for the consumer. The
//    producer's release store of seq publishes the element; the consumer's
//    release store of pos + capacity hands the slot back.
//  - Neither side ever reads the other side's position counter. A consumer
//    polling an empty queue (or a producer polling a full one) only touches
//    the slot it is waiting for. With a shared head/tail index, the polling
//    side keeps stealing the index cache line from the other thread, which
//    measured >10x slower at large capacities on Apple M4.
//  - Positions are 64-bit and never wrap in practice.
template <class T>
class spsc_queue {
    static_assert(std::is_nothrow_destructible_v<T>);

    struct slot {
        std::atomic<std::size_t> seq;
        alignas(T) std::byte storage[sizeof(T)];

        explicit slot(std::size_t s) noexcept : seq(s) {}
        T* get() noexcept { return std::launder(reinterpret_cast<T*>(storage)); }
    };

public:
    using value_type = T;

    // Capacity is rounded up to a power of two, minimum 2 (with one slot,
    // seq values for "full on lap n" and "free on lap n+1" coincide).
    explicit spsc_queue(std::size_t capacity)
        : capacity_(std::bit_ceil(std::max<std::size_t>(capacity, 2)))
        , slots_(std::allocator<slot>{}.allocate(capacity_))
    {
        for (std::size_t i = 0; i < capacity_; ++i) ::new (static_cast<void*>(slots_ + i)) slot(i);
    }

    ~spsc_queue()
    {
        if constexpr (!std::is_trivially_destructible_v<T>) {
            while (front() != nullptr) pop();
        }
        std::allocator<slot>{}.deallocate(slots_, capacity_);
    }

    spsc_queue(const spsc_queue&)            = delete;
    spsc_queue& operator=(const spsc_queue&) = delete;

    // -- Producer ------------------------------------------------------------

    template <class... Args>
    [[nodiscard]] bool try_emplace(Args&&... args)
        noexcept(std::is_nothrow_constructible_v<T, Args...>)
    {
        const std::size_t pos = tail_.load(std::memory_order_relaxed);
        slot& s = at(pos);
        if (s.seq.load(std::memory_order_acquire) != pos) return false; // full
        ::new (static_cast<void*>(s.storage)) T(std::forward<Args>(args)...);
        s.seq.store(pos + 1, std::memory_order_release);
        tail_.store(pos + 1, std::memory_order_relaxed);
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

    // -- Consumer ------------------------------------------------------------

    // Zero-copy access: pointer to the oldest element, or nullptr if empty.
    // The element stays valid until pop().
    [[nodiscard]] T* front() noexcept
    {
        const std::size_t pos = head_.load(std::memory_order_relaxed);
        slot& s = at(pos);
        if (s.seq.load(std::memory_order_acquire) != pos + 1) return nullptr;
        return s.get();
    }

    // Destroys the element returned by front(). Precondition: front() != nullptr.
    void pop() noexcept
    {
        const std::size_t pos = head_.load(std::memory_order_relaxed);
        slot& s = at(pos);
        s.get()->~T();
        s.seq.store(pos + capacity_, std::memory_order_release);
        head_.store(pos + 1, std::memory_order_relaxed);
    }

    [[nodiscard]] bool try_pop(T& out) noexcept(std::is_nothrow_move_assignable_v<T>)
    {
        T* p = front();
        if (p == nullptr) return false;
        out = std::move(*p);
        pop();
        return true;
    }

    // -- Observers (approximate under concurrency) ---------------------------

    [[nodiscard]] std::size_t size_approx() const noexcept
    {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        return tail > head ? tail - head : 0;
    }

    [[nodiscard]] bool empty() const noexcept { return size_approx() == 0; }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

private:
    slot& at(std::size_t pos) const noexcept { return slots_[pos & (capacity_ - 1)]; }

    const std::size_t capacity_;
    slot* const       slots_;

    // Each counter is written only by its owner; atomic only so that the
    // observers above can read it.
    alignas(support::cache_line_size) std::atomic<std::size_t> tail_{0};
    alignas(support::cache_line_size) std::atomic<std::size_t> head_{0};
};

} // namespace hpc::concurrency
