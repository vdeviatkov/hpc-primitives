#include <hpc/ipc/mapped_file.hpp>

#include <cerrno>
#include <cstdint>
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

mapped_file mapped_file::create(const std::string& path, std::size_t size)
{
    const int fd = ::open(path.c_str(), O_CREAT | O_TRUNC | O_RDWR, 0600);
    if (fd == -1) throw_errno("open");
    fd_guard guard{fd};
    if (::ftruncate(fd, static_cast<off_t>(size)) != 0) throw_errno("ftruncate");
    return mapped_file(map_fd(fd, size), size);
}

mapped_file mapped_file::open(const std::string& path)
{
    const int fd = ::open(path.c_str(), O_RDWR);
    if (fd == -1) throw_errno("open");
    fd_guard guard{fd};

    struct stat st {};
    if (::fstat(fd, &st) != 0) throw_errno("fstat");
    const auto size = static_cast<std::size_t>(st.st_size);
    if (size == 0) throw std::system_error(EINVAL, std::generic_category(), "mapped_file: empty file");
    return mapped_file(map_fd(fd, size), size);
}

mapped_file::mapped_file(mapped_file&& other) noexcept
    : data_(std::exchange(other.data_, nullptr)), size_(std::exchange(other.size_, 0))
{}

mapped_file& mapped_file::operator=(mapped_file&& other) noexcept
{
    mapped_file tmp(std::move(other));
    std::swap(data_, tmp.data_);
    std::swap(size_, tmp.size_);
    return *this;
}

mapped_file::~mapped_file()
{
    if (data_ != nullptr) ::munmap(data_, size_);
}

void mapped_file::flush(std::size_t offset, std::size_t length) const
{
    // msync wants a page-aligned start address.
    const auto page  = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
    const std::size_t begin = offset / page * page;
    if (::msync(data_ + begin, offset + length - begin, MS_SYNC) != 0) throw_errno("msync");
}

} // namespace hpc::ipc
