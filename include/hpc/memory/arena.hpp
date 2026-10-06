#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <utility>

namespace hpc::memory {

// Bump-pointer (monotonic) arena: O(1) allocation, no per-object free, O(1)
// reset of everything at once. Not thread-safe; use one arena per thread.
class arena {
public:
    arena() noexcept = default;

    // Owns `capacity` bytes from the global heap. Throws std::bad_alloc.
    explicit arena(std::size_t capacity)
        : begin_(static_cast<std::byte*>(::operator new(capacity)))
        , capacity_(capacity)
        , owning_(true)
    {}

    // Uses caller-provided memory, which must outlive the arena.
    arena(void* buffer, std::size_t capacity) noexcept
        : begin_(static_cast<std::byte*>(buffer))
        , capacity_(capacity)
    {}

    arena(arena&& other) noexcept
        : begin_(std::exchange(other.begin_, nullptr))
        , capacity_(std::exchange(other.capacity_, 0))
        , offset_(std::exchange(other.offset_, 0))
        , owning_(std::exchange(other.owning_, false))
    {}

    arena& operator=(arena&& other) noexcept
    {
        arena tmp(std::move(other));
        swap(tmp);
        return *this;
    }

    arena(const arena&)            = delete;
    arena& operator=(const arena&) = delete;

    ~arena()
    {
        if (owning_) ::operator delete(begin_);
    }

    // Returns nullptr when the arena is exhausted. `alignment` must be a
    // power of two.
    [[nodiscard]] void* allocate(std::size_t bytes,
                                 std::size_t alignment = alignof(std::max_align_t)) noexcept
    {
        const auto base  = reinterpret_cast<std::uintptr_t>(begin_);
        const auto start = ((base + offset_ + alignment - 1) & ~(alignment - 1)) - base;
        if (start > capacity_ || bytes > capacity_ - start) return nullptr;
        offset_ = start + bytes;
        return begin_ + start;
    }

    // Invalidates every allocation made so far.
    void reset() noexcept { offset_ = 0; }

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] std::size_t used() const noexcept { return offset_; }

    void swap(arena& other) noexcept
    {
        std::swap(begin_, other.begin_);
        std::swap(capacity_, other.capacity_);
        std::swap(offset_, other.offset_);
        std::swap(owning_, other.owning_);
    }

private:
    std::byte*  begin_{nullptr};
    std::size_t capacity_{0};
    std::size_t offset_{0};
    bool        owning_{false};
};

// Standard allocator over an arena, e.g. hpc::memory::arena_allocator<int>
// for std::vector. deallocate() is a no-op; memory is reclaimed by reset().
template <class T>
class arena_allocator {
public:
    using value_type = T;

    explicit arena_allocator(arena& a) noexcept : arena_(&a) {}

    template <class U>
    arena_allocator(const arena_allocator<U>& other) noexcept : arena_(other.arena_) {}

    [[nodiscard]] T* allocate(std::size_t n)
    {
        if (n > std::numeric_limits<std::size_t>::max() / sizeof(T)) throw std::bad_array_new_length();
        void* p = arena_->allocate(n * sizeof(T), alignof(T));
        if (p == nullptr) throw std::bad_alloc();
        return static_cast<T*>(p);
    }

    void deallocate(T*, std::size_t) noexcept {}

    template <class U>
    bool operator==(const arena_allocator<U>& rhs) const noexcept { return arena_ == rhs.arena_; }

private:
    template <class U>
    friend class arena_allocator;

    arena* arena_;
};

} // namespace hpc::memory
