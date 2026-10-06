#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace hpc::ipc {

// RAII POSIX shared-memory mapping (shm_open + mmap).
class shm_region {
public:
    // Creates a new object, replacing any stale one with the same name. The
    // creator unlinks the name on destruction.
    static shm_region create(const std::string& name, std::size_t size);

    // Maps an existing object in full.
    static shm_region open(const std::string& name);

    shm_region(shm_region&& other) noexcept;
    shm_region& operator=(shm_region&& other) noexcept;
    shm_region(const shm_region&)            = delete;
    shm_region& operator=(const shm_region&) = delete;
    ~shm_region();

    [[nodiscard]] std::byte* data() const noexcept { return data_; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }

private:
    shm_region(std::string name, std::byte* data, std::size_t size, bool owner) noexcept
        : name_(std::move(name)), data_(data), size_(size), owner_(owner)
    {}

    std::string name_;
    std::byte*  data_{nullptr};
    std::size_t size_{0};
    bool        owner_{false};
};

// Shared-memory layout. Offsets are fixed so that non-C++ readers can attach
// (see examples/shm_subscriber.py); 128-byte spacing keeps head and tail on
// separate cache lines on every mainstream CPU.
//
//   offset    0  u64 magic        written last by the creator (release)
//   offset    8  u64 capacity     slot count, power of two
//   offset   16  u64 slot_size    sizeof(T), checked on open
//   offset  128  u64 tail         next write position (producer)
//   offset  256  u64 head         next read position (consumer)
//   offset  384  T slots[capacity], slot = position & (capacity - 1)
struct shm_queue_header {
    static constexpr std::uint64_t magic_value = 0x3151535053435048; // "HPCSPSQ1" read as little-endian u64

    std::atomic<std::uint64_t> magic;
    std::uint64_t              capacity;
    std::uint64_t              slot_size;
    alignas(128) std::atomic<std::uint64_t> tail;
    alignas(128) std::atomic<std::uint64_t> head;
};

static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
              "cross-process atomics must be lock-free (address-free)");
static_assert(offsetof(shm_queue_header, tail) == 128);
static_assert(offsetof(shm_queue_header, head) == 256);
static_assert(sizeof(shm_queue_header) == 384);

// Single-producer / single-consumer queue between two processes. T must be
// trivially copyable and contain no pointers; both sides must agree on its
// layout. Positions are monotonically increasing 64-bit counters.
template <class T>
class shm_spsc_queue {
    static_assert(std::is_trivially_copyable_v<T>);
    static_assert(alignof(T) <= 128);

public:
    // Capacity is rounded up to a power of two.
    static shm_spsc_queue create(const std::string& name, std::size_t capacity)
    {
        capacity = std::bit_ceil(std::max<std::size_t>(capacity, 1));
        shm_region region = shm_region::create(name, bytes_for(capacity));
        auto* h = ::new (region.data()) shm_queue_header{};
        h->capacity  = capacity;
        h->slot_size = sizeof(T);
        h->magic.store(shm_queue_header::magic_value, std::memory_order_release);
        return shm_spsc_queue(std::move(region));
    }

    static shm_spsc_queue open(const std::string& name)
    {
        shm_region region = shm_region::open(name);
        if (region.size() < sizeof(shm_queue_header))
            throw std::runtime_error("shm_spsc_queue: region too small");
        auto* h = reinterpret_cast<shm_queue_header*>(region.data());
        if (h->magic.load(std::memory_order_acquire) != shm_queue_header::magic_value)
            throw std::runtime_error("shm_spsc_queue: not initialized or incompatible");
        if (h->slot_size != sizeof(T) || region.size() < bytes_for(h->capacity))
            throw std::runtime_error("shm_spsc_queue: element type or size mismatch");
        return shm_spsc_queue(std::move(region));
    }

    // Producer only.
    [[nodiscard]] bool try_push(const T& value) noexcept
    {
        const std::uint64_t tail = header_->tail.load(std::memory_order_relaxed);
        if (tail - header_->head.load(std::memory_order_acquire) == capacity_) return false;
        std::memcpy(slot(tail), &value, sizeof(T));
        header_->tail.store(tail + 1, std::memory_order_release);
        return true;
    }

    // Consumer only.
    [[nodiscard]] bool try_pop(T& out) noexcept
    {
        const std::uint64_t head = header_->head.load(std::memory_order_relaxed);
        if (head == header_->tail.load(std::memory_order_acquire)) return false;
        std::memcpy(&out, slot(head), sizeof(T));
        header_->head.store(head + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

private:
    explicit shm_spsc_queue(shm_region region) noexcept
        : region_(std::move(region))
        , header_(reinterpret_cast<shm_queue_header*>(region_.data()))
        , slots_(region_.data() + sizeof(shm_queue_header))
        , capacity_(header_->capacity)
    {}

    static std::size_t bytes_for(std::size_t capacity) noexcept
    {
        return sizeof(shm_queue_header) + capacity * sizeof(T);
    }

    std::byte* slot(std::uint64_t pos) const noexcept
    {
        return slots_ + (pos & (capacity_ - 1)) * sizeof(T);
    }

    shm_region        region_;
    shm_queue_header* header_;
    std::byte*        slots_;
    std::size_t       capacity_;
};

} // namespace hpc::ipc
