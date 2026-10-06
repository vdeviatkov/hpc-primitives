#pragma once

#include <cstddef>
#include <string>
#include <utility>

namespace hpc::ipc {

// RAII shared mapping of a regular file (open + mmap MAP_SHARED). Unlike
// shm_region, the file outlives every mapping: data written through it stays
// in the page cache and reaches disk on writeback or flush().
class mapped_file {
public:
    // Creates or truncates `path` to `size` bytes (sparse, zero-filled).
    static mapped_file create(const std::string& path, std::size_t size);

    // Maps an existing file in full, read-write.
    static mapped_file open(const std::string& path);

    mapped_file(mapped_file&& other) noexcept;
    mapped_file& operator=(mapped_file&& other) noexcept;
    mapped_file(const mapped_file&)            = delete;
    mapped_file& operator=(const mapped_file&) = delete;
    ~mapped_file();

    // Blocks until [offset, offset + length) is written to storage (msync).
    // Needed only for durability across machine crashes, not for visibility
    // to other processes.
    void flush(std::size_t offset, std::size_t length) const;

    [[nodiscard]] std::byte* data() const noexcept { return data_; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }

private:
    mapped_file(std::byte* data, std::size_t size) noexcept : data_(data), size_(size) {}

    std::byte*  data_{nullptr};
    std::size_t size_{0};
};

} // namespace hpc::ipc
