#pragma once

#include <cstddef>
#include <string>
#include <utility>

namespace hpc::ipc {

// RAII POSIX shared-memory mapping.
//
// How it works
//   - create(name, size): shm_open(O_CREAT | O_EXCL) names a new memory
//     object. If the name exists from a crashed run, it is unlinked and
//     created again. ftruncate sizes the object (zero-filled), and
//     mmap(MAP_SHARED) maps it. The descriptor is closed right after mapping,
//     since the mapping stays valid without it.
//   - open(name): shm_open an existing object, read its size with fstat, and
//     map all of it.
//   - Every process that maps the same name sees the same physical pages, so
//     writes become visible to the others under the usual atomic and
//     memory-order rules.
//   - The creator owns the name and unlinks it on destruction. Processes that
//     already have it mapped keep working, but no new process can open it.
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

} // namespace hpc::ipc
