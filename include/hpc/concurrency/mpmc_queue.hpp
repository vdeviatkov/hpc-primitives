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

// Bounded lock-free multi-producer / multi-consumer queue (Dmitry Vyukov's
// sequence-slot design).
//
//  - Every slot carries a sequence number. For position `pos`, a slot with
//    seq == pos is free for the producer that claims pos; seq == pos + 1
//    holds an element for the consumer that claims pos. After consuming, the
//    slot is re-armed for the next lap with seq = pos + capacity.
//  - Producers claim a position by CAS on tail_, consumers by CAS on head_.
//    The CAS only reserves a position; the release store to slot.seq
//    publishes the data and pairs with the acquire load on the other side.
//  - Positions are 64-bit and never wrap in practice, so there is no ABA.
//  - Not linearizable as a whole: try_pop can fail while a producer that
//    claimed an earlier position is still writing its element.
template <class T>
class mpmc_queue {
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
    explicit mpmc_queue(std::size_t capacity)
        : capacity_(std::bit_ceil(std::max<std::size_t>(capacity, 2)))
        , slots_(std::allocator<slot>{}.allocate(capacity_))
    {
        for (std::size_t i = 0; i < capacity_; ++i)
            ::new (static_cast<void*>(slots_ + i)) slot(i);
    }

    ~mpmc_queue()
    {
        if constexpr (!std::is_trivially_destructible_v<T>) {
            const std::size_t tail = tail_.load(std::memory_order_relaxed);
            for (std::size_t pos = head_.load(std::memory_order_relaxed); pos != tail; ++pos)
                at(pos).get()->~T();
        }
        std::allocator<slot>{}.deallocate(slots_, capacity_);
    }

    mpmc_queue(const mpmc_queue&)            = delete;
    mpmc_queue& operator=(const mpmc_queue&) = delete;

    template <class... Args>
    [[nodiscard]] bool try_emplace(Args&&... args)
        noexcept(std::is_nothrow_constructible_v<T, Args...>)
    {
        std::size_t pos = tail_.load(std::memory_order_relaxed);
        for (;;) {
            slot& s = at(pos);
            const std::size_t seq = s.seq.load(std::memory_order_acquire);
            const auto diff = static_cast<std::ptrdiff_t>(seq - pos);
            if (diff == 0) {
                if (tail_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    ::new (static_cast<void*>(s.storage)) T(std::forward<Args>(args)...);
                    s.seq.store(pos + 1, std::memory_order_release);
                    return true;
                }
                // CAS failure reloaded pos.
            } else if (diff < 0) {
                return false; // slot still holds the previous lap's element: full
            } else {
                pos = tail_.load(std::memory_order_relaxed); // another producer got here first
            }
        }
    }

    [[nodiscard]] bool try_push(const T& v) noexcept(std::is_nothrow_copy_constructible_v<T>)
    {
        return try_emplace(v);
    }

    [[nodiscard]] bool try_push(T&& v) noexcept(std::is_nothrow_move_constructible_v<T>)
    {
        return try_emplace(std::move(v));
    }

    [[nodiscard]] bool try_pop(T& out) noexcept(std::is_nothrow_move_assignable_v<T>)
    {
        std::size_t pos = head_.load(std::memory_order_relaxed);
        for (;;) {
            slot& s = at(pos);
            const std::size_t seq = s.seq.load(std::memory_order_acquire);
            const auto diff = static_cast<std::ptrdiff_t>(seq - (pos + 1));
            if (diff == 0) {
                if (head_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    T* p = s.get();
                    out = std::move(*p);
                    p->~T();
                    s.seq.store(pos + capacity_, std::memory_order_release);
                    return true;
                }
            } else if (diff < 0) {
                return false; // element not yet published: empty
            } else {
                pos = head_.load(std::memory_order_relaxed);
            }
        }
    }

    // Approximate under concurrency.
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

    alignas(support::cache_line_size) std::atomic<std::size_t> head_{0};
    alignas(support::cache_line_size) std::atomic<std::size_t> tail_{0};
};

} // namespace hpc::concurrency
