#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace hpc::ipc {

// Connected, blocking stream socket (Unix-domain or TCP). Owns the
// descriptor. Errors throw std::system_error; writing to a peer that has
// closed throws EPIPE instead of raising SIGPIPE.
class stream_socket {
public:
    stream_socket() noexcept = default;
    explicit stream_socket(int fd) noexcept : fd_(fd) {}
    stream_socket(stream_socket&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    stream_socket& operator=(stream_socket&& other) noexcept;
    stream_socket(const stream_socket&)            = delete;
    stream_socket& operator=(const stream_socket&) = delete;
    ~stream_socket();

    // Writes all `size` bytes.
    void send_all(const void* data, std::size_t size);

    // Reads exactly `size` bytes. Returns false if the peer closed the
    // connection cleanly before the first byte; throws if it closed midway.
    [[nodiscard]] bool recv_all(void* data, std::size_t size);

    // Message framing: a little-endian u32 length followed by the payload.
    void send_frame(const void* data, std::size_t size);
    // Replaces `out` with the next frame. Returns false on clean end of stream.
    [[nodiscard]] bool recv_frame(std::vector<std::byte>& out);

    // Fixed-size values, sent as raw bytes. Both ends must share T's layout.
    template <class T>
    void send_value(const T& value)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        send_all(&value, sizeof(T));
    }

    template <class T>
    [[nodiscard]] bool recv_value(T& out)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        return recv_all(&out, sizeof(T));
    }

    // TCP only: disables Nagle's algorithm so small writes go out at once.
    void set_nodelay(bool on);

    // Half-closes the write side; the peer's next read sees end of stream.
    void shutdown_write();

    [[nodiscard]] int fd() const noexcept { return fd_; }
    [[nodiscard]] bool valid() const noexcept { return fd_ != -1; }

private:
    int fd_{-1};
};

// Listening socket. A Unix-domain listener removes its path on destruction.
class stream_listener {
public:
    // Binds `path`, replacing a stale socket file left by a previous run.
    static stream_listener listen_unix(const std::string& path, int backlog = 16);

    // Binds an IPv4 address ("127.0.0.1", "0.0.0.0"). Port 0 picks a free
    // port; read it back with port().
    static stream_listener listen_tcp(const std::string& host, std::uint16_t port, int backlog = 16);

    stream_listener(stream_listener&& other) noexcept;
    stream_listener& operator=(stream_listener&& other) noexcept;
    stream_listener(const stream_listener&)            = delete;
    stream_listener& operator=(const stream_listener&) = delete;
    ~stream_listener();

    // Blocks until a client connects.
    [[nodiscard]] stream_socket accept();

    // Bound TCP port (0 for Unix-domain listeners).
    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

private:
    stream_listener(int fd, std::string path, std::uint16_t port) noexcept
        : fd_(fd), path_(std::move(path)), port_(port)
    {}

    int           fd_{-1};
    std::string   path_;
    std::uint16_t port_{0};
};

[[nodiscard]] stream_socket connect_unix(const std::string& path);
[[nodiscard]] stream_socket connect_tcp(const std::string& host, std::uint16_t port);

// Two connected Unix-domain sockets (socketpair), e.g. across fork().
[[nodiscard]] std::pair<stream_socket, stream_socket> socket_pair();

} // namespace hpc::ipc
