#include <hpc/ipc/socket.hpp>

#include <array>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <system_error>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace hpc::ipc {

namespace {

[[noreturn]] void throw_errno(const char* what)
{
    throw std::system_error(errno, std::generic_category(), what);
}

// Linux suppresses SIGPIPE per call; macOS only per socket.
#if defined(MSG_NOSIGNAL)
constexpr int send_flags = MSG_NOSIGNAL;
void suppress_sigpipe(int) noexcept {}
#else
constexpr int send_flags = 0;
void suppress_sigpipe(int fd) noexcept
{
    int on = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
}
#endif

int make_socket(int domain)
{
    const int fd = ::socket(domain, SOCK_STREAM, 0);
    if (fd == -1) throw_errno("socket");
    suppress_sigpipe(fd);
    return fd;
}

sockaddr_un unix_address(const std::string& path)
{
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path))
        throw std::invalid_argument("unix socket path too long: " + path);
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    return addr;
}

sockaddr_in tcp_address(const std::string& host, std::uint16_t port)
{
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1)
        throw std::invalid_argument("not an IPv4 address: " + host);
    return addr;
}

template <class Addr>
void bind_and_listen(int fd, const Addr& addr, int backlog)
{
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        const int err = errno;
        ::close(fd);
        throw std::system_error(err, std::generic_category(), "bind");
    }
    if (::listen(fd, backlog) != 0) {
        const int err = errno;
        ::close(fd);
        throw std::system_error(err, std::generic_category(), "listen");
    }
}

template <class Addr>
stream_socket connect_to(int domain, const Addr& addr)
{
    stream_socket s(make_socket(domain));
    int rc = 0;
    do {
        rc = ::connect(s.fd(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    } while (rc != 0 && errno == EINTR);
    if (rc != 0) throw_errno("connect");
    return s;
}

} // namespace

stream_socket& stream_socket::operator=(stream_socket&& other) noexcept
{
    stream_socket tmp(std::move(other));
    std::swap(fd_, tmp.fd_);
    return *this;
}

stream_socket::~stream_socket()
{
    if (fd_ != -1) ::close(fd_);
}

void stream_socket::send_all(const void* data, std::size_t size)
{
    const auto* p = static_cast<const std::byte*>(data);
    while (size > 0) {
        const ssize_t n = ::send(fd_, p, size, send_flags);
        if (n < 0) {
            if (errno == EINTR) continue;
            throw_errno("send");
        }
        p += n;
        size -= static_cast<std::size_t>(n);
    }
}

bool stream_socket::recv_all(void* data, std::size_t size)
{
    auto* p = static_cast<std::byte*>(data);
    std::size_t got = 0;
    while (got < size) {
        const ssize_t n = ::recv(fd_, p + got, size - got, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            throw_errno("recv");
        }
        if (n == 0) {
            if (got == 0) return false;
            throw std::runtime_error("stream_socket: connection closed mid-message");
        }
        got += static_cast<std::size_t>(n);
    }
    return true;
}

void stream_socket::send_frame(const void* data, std::size_t size)
{
    if (size > UINT32_MAX) throw std::length_error("stream_socket: frame larger than 4 GiB");
    const auto len = static_cast<std::uint32_t>(size);
    const std::array<std::uint8_t, 4> prefix{
        static_cast<std::uint8_t>(len), static_cast<std::uint8_t>(len >> 8),
        static_cast<std::uint8_t>(len >> 16), static_cast<std::uint8_t>(len >> 24)};
    send_all(prefix.data(), prefix.size());
    send_all(data, size);
}

bool stream_socket::recv_frame(std::vector<std::byte>& out)
{
    std::array<std::uint8_t, 4> prefix{};
    if (!recv_all(prefix.data(), prefix.size())) return false;
    const std::uint32_t len = std::uint32_t{prefix[0]} | std::uint32_t{prefix[1]} << 8
                            | std::uint32_t{prefix[2]} << 16 | std::uint32_t{prefix[3]} << 24;
    out.resize(len);
    if (len > 0 && !recv_all(out.data(), len))
        throw std::runtime_error("stream_socket: connection closed mid-message");
    return true;
}

void stream_socket::set_nodelay(bool on)
{
    int v = on ? 1 : 0;
    if (::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &v, sizeof(v)) != 0) throw_errno("setsockopt");
}

void stream_socket::shutdown_write()
{
    if (::shutdown(fd_, SHUT_WR) != 0) throw_errno("shutdown");
}

stream_listener stream_listener::listen_unix(const std::string& path, int backlog)
{
    const sockaddr_un addr = unix_address(path);
    const int fd = make_socket(AF_UNIX);
    ::unlink(path.c_str()); // stale socket file from a previous run
    bind_and_listen(fd, addr, backlog);
    return stream_listener(fd, path, 0);
}

stream_listener stream_listener::listen_tcp(const std::string& host, std::uint16_t port, int backlog)
{
    const sockaddr_in addr = tcp_address(host, port);
    const int fd = make_socket(AF_INET);
    int on = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    bind_and_listen(fd, addr, backlog);

    sockaddr_in bound{};
    socklen_t len = sizeof(bound);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &len) != 0) {
        const int err = errno;
        ::close(fd);
        throw std::system_error(err, std::generic_category(), "getsockname");
    }
    return stream_listener(fd, {}, ntohs(bound.sin_port));
}

stream_listener::stream_listener(stream_listener&& other) noexcept
    : fd_(std::exchange(other.fd_, -1))
    , path_(std::move(other.path_))
    , port_(other.port_)
{
    other.path_.clear();
}

stream_listener& stream_listener::operator=(stream_listener&& other) noexcept
{
    stream_listener tmp(std::move(other));
    std::swap(fd_, tmp.fd_);
    std::swap(path_, tmp.path_);
    std::swap(port_, tmp.port_);
    return *this;
}

stream_listener::~stream_listener()
{
    if (fd_ != -1) ::close(fd_);
    if (!path_.empty()) ::unlink(path_.c_str());
}

stream_socket stream_listener::accept()
{
    for (;;) {
        const int fd = ::accept(fd_, nullptr, nullptr);
        if (fd != -1) {
            suppress_sigpipe(fd);
            return stream_socket(fd);
        }
        if (errno != EINTR && errno != ECONNABORTED) throw_errno("accept");
    }
}

stream_socket connect_unix(const std::string& path)
{
    return connect_to(AF_UNIX, unix_address(path));
}

stream_socket connect_tcp(const std::string& host, std::uint16_t port)
{
    return connect_to(AF_INET, tcp_address(host, port));
}

std::pair<stream_socket, stream_socket> socket_pair()
{
    int fds[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) throw_errno("socketpair");
    suppress_sigpipe(fds[0]);
    suppress_sigpipe(fds[1]);
    return {stream_socket(fds[0]), stream_socket(fds[1])};
}

} // namespace hpc::ipc
