#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

#include <hpc/ipc/mapped_file.hpp>

namespace hpc::ipc {

// File layout of file_journal. Offsets are fixed so that other languages can
// read the file (it is just a header followed by an array of records).
//
//   offset    0  u64 magic        written last by the creator (release)
//   offset    8  u64 capacity     record count the file can hold
//   offset   16  u64 record_size  sizeof(T), checked on open
//   offset  128  u64 committed    records [0, committed) are complete
//   offset  256  T records[capacity]
struct journal_header {
    static constexpr std::uint64_t magic_value = 0x314c4e524a435048; // "HPCJRNL1" read as little-endian u64

    std::atomic<std::uint64_t> magic;
    std::uint64_t              capacity;
    std::uint64_t              record_size;
    alignas(128) std::atomic<std::uint64_t> committed;
};

static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
              "cross-process atomics must be lock-free (address-free)");
static_assert(offsetof(journal_header, committed) == 128);
static_assert(sizeof(journal_header) == 256);

// Append-only log of fixed-size records in a memory-mapped file. One writer
// and any number of readers, in any number of processes, each reading at its
// own pace. T must be trivially copyable and contain no pointers.
//
// How it works
//   - The file is a 256-byte header followed by a plain array of T (layout
//     above). It is mapped with mapped_file, so writer and readers share the
//     same page-cache pages.
//   - create(): size the file for `capacity` records, write the header, and
//     release-store `magic` last. open() acquire-loads `magic` and checks
//     sizeof(T) and the file size.
//   - try_append (writer only): n = committed; if n == capacity, the file is
//     full. Otherwise memcpy the record into records[n], then release-store
//     committed = n + 1, which publishes it.
//   - size() acquire-loads `committed`. try_read(i) returns record i only if
//     i < committed. Readers keep their own index and share no state with
//     each other or with the writer, beyond reading `committed`.
//   - Records are never overwritten, so a slow reader cannot lose data, and a
//     reader started later can replay from record 0. When the file is full,
//     roll over to a new one.
//   - Crash consistency: `committed` advances only after the record is fully
//     written. A writer that dies mid-append leaves a consistent prefix, and
//     open() followed by try_append resumes after it.
//   - flush() msyncs the committed records for durability against power loss.
//     Other processes do not need it to see new records.
template <class T>
class file_journal {
    static_assert(std::is_trivially_copyable_v<T>);
    static_assert(alignof(T) <= 128);

public:
    // Creates (or truncates) the file with room for `capacity` records.
    static file_journal create(const std::string& path, std::size_t capacity)
    {
        mapped_file file = mapped_file::create(path, bytes_for(capacity));
        auto* h = ::new (file.data()) journal_header{};
        h->capacity    = capacity;
        h->record_size = sizeof(T);
        h->magic.store(journal_header::magic_value, std::memory_order_release);
        return file_journal(std::move(file));
    }

    // Opens an existing journal, to read it or to resume appending.
    static file_journal open(const std::string& path)
    {
        mapped_file file = mapped_file::open(path);
        if (file.size() < sizeof(journal_header))
            throw std::runtime_error("file_journal: file too small");
        auto* h = reinterpret_cast<journal_header*>(file.data());
        if (h->magic.load(std::memory_order_acquire) != journal_header::magic_value)
            throw std::runtime_error("file_journal: not initialized or incompatible");
        if (h->record_size != sizeof(T) || file.size() < bytes_for(h->capacity))
            throw std::runtime_error("file_journal: record type or size mismatch");
        return file_journal(std::move(file));
    }

    // Writer only. Returns false when the file is full.
    [[nodiscard]] bool try_append(const T& record) noexcept
    {
        const std::uint64_t n = header_->committed.load(std::memory_order_relaxed);
        if (n == capacity_) return false;
        std::memcpy(records_ + n * sizeof(T), &record, sizeof(T));
        header_->committed.store(n + 1, std::memory_order_release);
        return true;
    }

    // Number of complete records. Readers poll this to discover new data.
    [[nodiscard]] std::uint64_t size() const noexcept
    {
        return header_->committed.load(std::memory_order_acquire);
    }

    // Copies record `index` into `out`; false if it is not committed yet.
    [[nodiscard]] bool try_read(std::uint64_t index, T& out) const noexcept
    {
        if (index >= size()) return false;
        std::memcpy(&out, records_ + index * sizeof(T), sizeof(T));
        return true;
    }

    // Writes committed records and the header to storage (msync). Only needed
    // for durability across power loss or kernel crashes.
    void flush() const { file_.flush(0, bytes_for(size())); }

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

private:
    explicit file_journal(mapped_file file) noexcept
        : file_(std::move(file))
        , header_(reinterpret_cast<journal_header*>(file_.data()))
        , records_(file_.data() + sizeof(journal_header))
        , capacity_(header_->capacity)
    {}

    static std::size_t bytes_for(std::size_t capacity) noexcept
    {
        return sizeof(journal_header) + capacity * sizeof(T);
    }

    mapped_file     file_;
    journal_header* header_;
    std::byte*      records_;
    std::size_t     capacity_;
};

} // namespace hpc::ipc
