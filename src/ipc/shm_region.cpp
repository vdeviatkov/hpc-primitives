#include <hpc/ipc/shm_region.hpp>

#include <cerrno>
#include <cstddef>
#include <system_error>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace hpc::ipc {

namespace {

[[noreturn]] void throw_errno(const char* what)
{
    throw std::system_error(errno, std::generic_category(), what);
}

std::byte* map_fd(int fd, std::size_t size)
{
    void* p = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) throw_errno("mmap");
    return static_cast<std::byte*>(p);
}

// Closes the descriptor on scope exit; the mapping stays valid without it.
struct fd_guard {
    int fd;
    ~fd_guard() { ::close(fd); }
};

} // namespace

shm_region shm_region::create(const std::string& name, std::size_t size)
{
    int fd = ::shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd == -1 && errno == EEXIST) {
        ::shm_unlink(name.c_str()); // stale object from a previous run
        fd = ::shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
    }
    if (fd == -1) throw_errno("shm_open");
    fd_guard guard{fd};

    if (::ftruncate(fd, static_cast<off_t>(size)) != 0) {
        const int err = errno;
        ::shm_unlink(name.c_str());
        throw std::system_error(err, std::generic_category(), "ftruncate");
    }
    try {
        return shm_region(name, map_fd(fd, size), size, true);
    } catch (...) {
        ::shm_unlink(name.c_str());
        throw;
    }
}

shm_region shm_region::open(const std::string& name)
{
    const int fd = ::shm_open(name.c_str(), O_RDWR, 0);
    if (fd == -1) throw_errno("shm_open");
    fd_guard guard{fd};

    struct stat st {};
    if (::fstat(fd, &st) != 0) throw_errno("fstat");
    const auto size = static_cast<std::size_t>(st.st_size);
    return shm_region(name, map_fd(fd, size), size, false);
}

shm_region::shm_region(shm_region&& other) noexcept
    : name_(std::move(other.name_))
    , data_(std::exchange(other.data_, nullptr))
    , size_(std::exchange(other.size_, 0))
    , owner_(std::exchange(other.owner_, false))
{}

shm_region& shm_region::operator=(shm_region&& other) noexcept
{
    shm_region tmp(std::move(other));
    std::swap(name_, tmp.name_);
    std::swap(data_, tmp.data_);
    std::swap(size_, tmp.size_);
    std::swap(owner_, tmp.owner_);
    return *this;
}

shm_region::~shm_region()
{
    if (data_ != nullptr) ::munmap(data_, size_);
    if (owner_) ::shm_unlink(name_.c_str());
}

} // namespace hpc::ipc
