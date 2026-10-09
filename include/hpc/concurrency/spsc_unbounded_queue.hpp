#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

#include <hpc/support/platform.hpp>

namespace hpc::concurrency {

// Unbounded single-producer / single-consumer queue built from a singly linked
// list of fixed-size segments. push() never fails; try_pop() is wait-free.
//
// Data layout
//   - segment = { atomic written; atomic next; raw storage for SegmentSize Ts }.
//     SegmentSize defaults to about 4 KiB of elements (at least 8).
//   - Producer-owned: tail_seg_ (the segment being filled) and tail_pos_ (its
//     next free slot). Consumer-owned: head_seg_, head_pos_ and written_cache_.
//     The two groups sit on separate cache lines.
//
// How it works
//   - push: if the tail segment is full, allocate a new one, release-store it
//     into tail_seg_->next and continue in it. Placement-new the element at
//     tail_pos_, then release-store written = ++tail_pos_, which publishes it.
//   - front: when head_pos_ catches up with written_cache_, refresh the cache
//     with an acquire load of `written`. The consumer touches the shared
//     counter only after it has used up every element it already knew about.
//     When the segment is exhausted (head_pos_ == SegmentSize), acquire-load
//     `next`. If there is a next segment, delete the old one and continue.
//   - pop: run the same check as front(), so on an empty queue it returns
//     false and changes nothing; otherwise destroy the element and
//     ++head_pos_, with no shared write.
//   - Reclamation is trivial. The producer never touches a segment after
//     linking its successor, and the consumer deletes a segment only after
//     consuming all of it and seeing `next`, so no hazard pointers or epochs
//     are needed.
//   - Cost: one new/delete per SegmentSize elements, and memory is unbounded
//     if the consumer falls behind.
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
        while (front() != nullptr) pop_front_unchecked();
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

    // Destroys the oldest element, the one front() returns. Returns false, and
    // changes nothing, if the queue is empty, so a pop() without a successful
    // front() cannot corrupt the queue. If front() returned nullptr and
    // pop() then returns true, an element arrived in between and was dropped
    // unread: check one of the two results.
    [[nodiscard]] bool pop() noexcept
    {
        if (front() == nullptr) return false;
        pop_front_unchecked();
        return true;
    }

    [[nodiscard]] bool try_pop(T& out) noexcept(std::is_nothrow_move_assignable_v<T>)
    {
        T* p = front();
        if (p == nullptr) return false;
        out = std::move(*p);
        pop_front_unchecked();
        return true;
    }

    static constexpr std::size_t segment_size() noexcept { return SegmentSize; }

private:
    // Precondition: front() != nullptr, which also moved head_seg_ past any
    // exhausted segment.
    void pop_front_unchecked() noexcept
    {
        head_seg_->at(head_pos_)->~T();
        ++head_pos_;
    }

    // Producer-owned.
    alignas(support::cache_line_size) segment* tail_seg_;
    std::size_t tail_pos_{0};

    // Consumer-owned.
    alignas(support::cache_line_size) segment* head_seg_;
    std::size_t head_pos_{0};
    std::size_t written_cache_{0};
};

} // namespace hpc::concurrency
