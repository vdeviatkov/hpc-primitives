#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

#include <hpc/support/platform.hpp>

namespace hpc::concurrency {

// Unbounded single-producer / single-consumer queue built from a linked list
// of fixed-size segments. push() never fails (it allocates a new segment when
// the current one is full); try_pop() is wait-free.
//
//  - The producer owns the tail segment, the consumer owns the head segment.
//    Shared state per segment is `written` (element count, release-stored by
//    the producer after each construction) and `next` (release-stored once
//    when the producer moves on).
//  - The consumer frees a segment only after it has consumed all of it and
//    observed `next`; the producer never touches a segment after linking its
//    successor, so no further reclamation scheme is needed.
//  - SegmentSize defaults to ~4 KiB of elements.
template <class T, std::size_t SegmentSize = std::max<std::size_t>(4096 / sizeof(T), 8)>
class spsc_unbounded_queue {
    static_assert(SegmentSize >= 1);
    static_assert(std::is_nothrow_destructible_v<T>);

    struct segment {
        std::atomic<std::size_t> written{0};
        std::atomic<segment*>    next{nullptr};
        alignas(T) std::byte     storage[sizeof(T) * SegmentSize];

        T* at(std::size_t i) noexcept
        {
            return std::launder(reinterpret_cast<T*>(storage + i * sizeof(T)));
        }
    };

public:
    using value_type = T;

    spsc_unbounded_queue() : tail_seg_(new segment), head_seg_(tail_seg_) {}

    ~spsc_unbounded_queue()
    {
        T* p = nullptr;
        while ((p = front()) != nullptr) pop();
        delete head_seg_;
    }

    spsc_unbounded_queue(const spsc_unbounded_queue&)            = delete;
    spsc_unbounded_queue& operator=(const spsc_unbounded_queue&) = delete;

    // -- Producer ------------------------------------------------------------

    // Throws std::bad_alloc if a new segment cannot be allocated, or whatever
    // T's constructor throws; the queue is unchanged in either case.
    template <class... Args>
    void emplace(Args&&... args)
    {
        if (tail_pos_ == SegmentSize) {
            auto* next = new segment;
            tail_seg_->next.store(next, std::memory_order_release);
            tail_seg_ = next;
            tail_pos_ = 0;
        }
        ::new (static_cast<void*>(tail_seg_->at(tail_pos_))) T(std::forward<Args>(args)...);
        tail_seg_->written.store(++tail_pos_, std::memory_order_release);
    }

    void push(const T& v) { emplace(v); }
    void push(T&& v) { emplace(std::move(v)); }

    // -- Consumer ------------------------------------------------------------

    // Pointer to the oldest element, or nullptr if empty. Valid until pop().
    [[nodiscard]] T* front() noexcept
    {
        if (head_pos_ == written_cache_) {
            if (head_pos_ == SegmentSize) {
                segment* next = head_seg_->next.load(std::memory_order_acquire);
                if (next == nullptr) return nullptr;
                delete head_seg_;
                head_seg_ = next;
                head_pos_ = 0;
                written_cache_ = 0;
            }
            written_cache_ = head_seg_->written.load(std::memory_order_acquire);
            if (head_pos_ == written_cache_) return nullptr;
        }
        return head_seg_->at(head_pos_);
    }

    // Destroys the element returned by front(). Precondition: front() != nullptr.
    void pop() noexcept
    {
        head_seg_->at(head_pos_)->~T();
        ++head_pos_;
    }

    [[nodiscard]] bool try_pop(T& out) noexcept(std::is_nothrow_move_assignable_v<T>)
    {
        T* p = front();
        if (p == nullptr) return false;
        out = std::move(*p);
        pop();
        return true;
    }

    static constexpr std::size_t segment_size() noexcept { return SegmentSize; }

private:
    // Producer-owned.
    alignas(support::cache_line_size) segment* tail_seg_;
    std::size_t tail_pos_{0};

    // Consumer-owned.
    alignas(support::cache_line_size) segment* head_seg_;
    std::size_t head_pos_{0};
    std::size_t written_cache_{0};
};

} // namespace hpc::concurrency
