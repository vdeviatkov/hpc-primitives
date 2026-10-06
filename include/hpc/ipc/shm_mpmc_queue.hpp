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

#include <hpc/ipc/shm_region.hpp>

namespace hpc::ipc {

// Shared-memory layout of shm_mpmc_queue. Same header shape as the SPSC queue,
// but every slot carries a sequence number (Vyukov's bounded MPMC design, as
// in hpc::concurrency::mpmc_queue).
//
//   offset    0  u64 magic        written last by the creator (release)
//   offset    8  u64 capacity     slot count, power of two, >= 2
//   offset   16  u64 slot_size    sizeof(T), checked on open
//   offset   24  u64 slot_stride  bytes per slot (sequence + T, padded)
//   offset  128  u64 tail         next position to claim (producers, CAS)
//   offset  256  u64 head         next position to claim (consumers, CAS)
//   offset  384  slots[capacity]  { u64 seq; T value; }, slot = pos & (capacity - 1)
struct shm_mpmc_header {
    static constexpr std::uint64_t magic_value = 0x31514d504d435048; // "HPCMPMQ1" read as little-endian u64

    std::atomic<std::uint64_t> magic;
    std::uint64_t              capacity;
    std::uint64_t              slot_size;
    std::uint64_t              slot_stride;
    alignas(128) std::atomic<std::uint64_t> tail;
    alignas(128) std::atomic<std::uint64_t> head;
};

static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
              "cross-process atomics must be lock-free (address-free)");
static_assert(offsetof(shm_mpmc_header, tail) == 128);
static_assert(offsetof(shm_mpmc_header, head) == 256);
static_assert(sizeof(shm_mpmc_header) == 384);

// Bounded lock-free multi-producer / multi-consumer queue between processes.
// T must be trivially copyable and contain no pointers; all sides must agree
// on its layout.
//
// Lock-free, not crash-safe: a process that dies between claiming a position
// and publishing its slot leaves that slot permanently busy, and the queue
// stalls once the other side reaches it. Recreate the queue in that case.
template <class T>
class shm_mpmc_queue {
    static_assert(std::is_trivially_copyable_v<T>);
    static_assert(alignof(T) <= 128);

    struct slot {
        std::atomic<std::uint64_t> seq;
        T                          value;
    };

public:
    // Capacity is rounded up to a power of two, minimum 2.
    static shm_mpmc_queue create(const std::string& name, std::size_t capacity)
    {
        capacity = std::bit_ceil(std::max<std::size_t>(capacity, 2));
        shm_region region = shm_region::create(name, bytes_for(capacity));
        auto* h = ::new (region.data()) shm_mpmc_header{};
        h->capacity    = capacity;
        h->slot_size   = sizeof(T);
        h->slot_stride = sizeof(slot);
        auto* slots = reinterpret_cast<slot*>(region.data() + sizeof(shm_mpmc_header));
        for (std::size_t i = 0; i < capacity; ++i)
            ::new (static_cast<void*>(&slots[i].seq)) std::atomic<std::uint64_t>(i);
        h->magic.store(shm_mpmc_header::magic_value, std::memory_order_release);
        return shm_mpmc_queue(std::move(region));
    }

    static shm_mpmc_queue open(const std::string& name)
    {
        shm_region region = shm_region::open(name);
        if (region.size() < sizeof(shm_mpmc_header))
            throw std::runtime_error("shm_mpmc_queue: region too small");
        auto* h = reinterpret_cast<shm_mpmc_header*>(region.data());
        if (h->magic.load(std::memory_order_acquire) != shm_mpmc_header::magic_value)
            throw std::runtime_error("shm_mpmc_queue: not initialized or incompatible");
        if (h->slot_size != sizeof(T) || h->slot_stride != sizeof(slot)
            || region.size() < bytes_for(h->capacity))
            throw std::runtime_error("shm_mpmc_queue: element type or size mismatch");
        return shm_mpmc_queue(std::move(region));
    }

    // Any number of producers, in any number of processes.
    [[nodiscard]] bool try_push(const T& value) noexcept
    {
        std::uint64_t pos = header_->tail.load(std::memory_order_relaxed);
        for (;;) {
            slot& s = at(pos);
            const std::uint64_t seq = s.seq.load(std::memory_order_acquire);
            const auto diff = static_cast<std::int64_t>(seq - pos);
            if (diff == 0) {
                if (header_->tail.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    std::memcpy(&s.value, &value, sizeof(T));
                    s.seq.store(pos + 1, std::memory_order_release);
                    return true;
                }
            } else if (diff < 0) {
                return false; // full
            } else {
                pos = header_->tail.load(std::memory_order_relaxed);
            }
        }
    }

    // Any number of consumers, in any number of processes.
    [[nodiscard]] bool try_pop(T& out) noexcept
    {
        std::uint64_t pos = header_->head.load(std::memory_order_relaxed);
        for (;;) {
            slot& s = at(pos);
            const std::uint64_t seq = s.seq.load(std::memory_order_acquire);
            const auto diff = static_cast<std::int64_t>(seq - (pos + 1));
            if (diff == 0) {
                if (header_->head.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    std::memcpy(&out, &s.value, sizeof(T));
                    s.seq.store(pos + capacity_, std::memory_order_release);
                    return true;
                }
            } else if (diff < 0) {
                return false; // empty
            } else {
                pos = header_->head.load(std::memory_order_relaxed);
            }
        }
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

private:
    explicit shm_mpmc_queue(shm_region region) noexcept
        : region_(std::move(region))
        , header_(reinterpret_cast<shm_mpmc_header*>(region_.data()))
        , slots_(reinterpret_cast<slot*>(region_.data() + sizeof(shm_mpmc_header)))
        , capacity_(header_->capacity)
    {}

    static std::size_t bytes_for(std::size_t capacity) noexcept
    {
        return sizeof(shm_mpmc_header) + capacity * sizeof(slot);
    }

    slot& at(std::uint64_t pos) const noexcept { return slots_[pos & (capacity_ - 1)]; }

    shm_region       region_;
    shm_mpmc_header* header_;
    slot*            slots_;
    std::size_t      capacity_;
};

} // namespace hpc::ipc
