#pragma once

#include <cstddef>
#include <string>
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

} // namespace hpc::ipc
