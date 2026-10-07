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
// Data layout
//   - One heap array of `capacity` slots (rounded up to a power of two, at
//     least 2). Each slot is { atomic<size_t> seq; raw storage for one T }.
//   - tail_ (the producer's next position) and head_ (the consumer's next
//     position), each on its own 128-byte line. Positions only grow; a
//     position's slot is pos & (capacity - 1).
//
// How it works
//   - Slot i starts with seq = i. For position pos, the slot's seq gives its
//     state: seq == pos means free, so the producer at pos may write;
//     seq == pos + 1 means it holds the element for the consumer at pos.
//   - try_push: read tail_ (only the producer writes it) and check that
//     slot.seq == pos (otherwise the queue is full). Placement-new T into the
//     slot, release-store seq = pos + 1, which publishes the element, then set
//     tail_ = pos + 1.
//   - front() / pop(): read head_ and acquire-load slot.seq; it must equal
//     pos + 1 (otherwise empty). front() hands out the element in place
//     (zero-copy). pop() destroys it, release-stores seq = pos + capacity,
//     which frees the slot for the producer's next lap, then sets
//     head_ = pos + 1.
//   - Neither side reads the other's position counter; the atomics exist only
//     for size_approx(). The only shared data is the slot being handed over,
//     so a side polling an empty or full queue spins on that one slot. With a
//     shared head/tail index, the poller kept stealing the other side's cache
//     line, which measured >10x slower at large capacities on Apple M4.
//   - Wait-free: every operation is a fixed number of steps, with no CAS and
//     no retry loop. The cost is 8 bytes of sequence number per slot.
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
