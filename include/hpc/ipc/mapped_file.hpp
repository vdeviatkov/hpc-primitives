#pragma once

#include <cstddef>
#include <string>
#include <utility>

namespace hpc::ipc {

// RAII shared mapping of a regular file (open + mmap MAP_SHARED).
//
// How it works
//   - create(path, size): open with O_CREAT | O_TRUNC, ftruncate to `size`
//     (the file is sparse, so untouched pages take no disk space), then mmap
//     it MAP_SHARED, read-write. The descriptor is closed right after mapping.
//   - open(path): open an existing file, read its size with fstat, and map
//     all of it. An empty file is rejected.
//   - Writes go to the page cache, so other processes that map the same file
//     see them at once. Unlike shm_region, the data outlives every mapping and
//     reaches disk on kernel writeback, or on flush().
//   - flush(offset, length) rounds the start down to a page boundary and calls
//     msync(MS_SYNC). That is needed only for durability across power loss or
//     a kernel crash, not for visibility.
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
