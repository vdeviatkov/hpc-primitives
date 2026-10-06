#include <hpc/ipc/socket.hpp>

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

using namespace hpc::ipc;

namespace {

std::vector<std::byte> bytes(const std::string& s)
{
    std::vector<std::byte> v(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) v[i] = static_cast<std::byte>(s[i]);
    return v;
}

// /tmp keeps the path well under the ~104-byte sun_path limit on every OS.
std::string socket_path(const char* tag)
{
    return "/tmp/hpc_sock_" + std::string(tag) + "_" + std::to_string(::getpid());
}

} // namespace

TEST(Socket, PairFramesAndEndOfStream)
{
    auto [a, b] = socket_pair();
    const auto hello = bytes("hello");
    a.send_frame(hello.data(), hello.size());
    a.send_frame(nullptr, 0);
    a.send_value(std::uint64_t{42});
    a.shutdown_write();

    std::vector<std::byte> frame;
    ASSERT_TRUE(b.recv_frame(frame));
    EXPECT_EQ(frame, hello);
    ASSERT_TRUE(b.recv_frame(frame));
    EXPECT_TRUE(frame.empty());
    std::uint64_t v = 0;
    ASSERT_TRUE(b.recv_value(v));
    EXPECT_EQ(v, 42u);
    EXPECT_FALSE(b.recv_frame(frame)); // clean end of stream
}

TEST(Socket, TruncatedMessageThrows)
{
    auto [a, b] = socket_pair();
    const char partial[3] = {1, 2, 3};
    a.send_all(partial, sizeof(partial));
    a.shutdown_write();
    std::uint64_t v = 0;
    EXPECT_THROW((void)b.recv_value(v), std::runtime_error);
}

TEST(Socket, SendToClosedPeerThrowsInsteadOfSigpipe)
{
    auto [a, b] = socket_pair();
    b = stream_socket{};
    const std::vector<std::byte> chunk(4096);
    EXPECT_THROW(
        {
            for (int i = 0; i < 1000; ++i) a.send_all(chunk.data(), chunk.size());
        },
        std::system_error);
}

TEST(Socket, UnixListenerEcho)
{
    const auto path = socket_path("echo");
    auto listener = stream_listener::listen_unix(path);
    EXPECT_EQ(listener.port(), 0);

    std::thread server([&] {
        auto conn = listener.accept();
        std::vector<std::byte> frame;
        while (conn.recv_frame(frame)) conn.send_frame(frame.data(), frame.size());
    });

    auto client = connect_unix(path);
    std::vector<std::byte> reply;
    for (int i = 0; i < 100; ++i) {
        const auto msg = bytes("message " + std::to_string(i));
        client.send_frame(msg.data(), msg.size());
        ASSERT_TRUE(client.recv_frame(reply));
        EXPECT_EQ(reply, msg);
    }
    client.shutdown_write();
    EXPECT_FALSE(client.recv_frame(reply));
    server.join();
}

TEST(Socket, UnixListenerRemovesPath)
{
    const auto path = socket_path("cleanup");
    {
        auto listener = stream_listener::listen_unix(path);
        EXPECT_EQ(::access(path.c_str(), F_OK), 0);
    }
    EXPECT_NE(::access(path.c_str(), F_OK), 0);
}

TEST(Socket, TcpLoopbackEphemeralPort)
{
    auto listener = stream_listener::listen_tcp("127.0.0.1", 0);
    ASSERT_NE(listener.port(), 0);

    std::thread server([&] {
        auto conn = listener.accept();
        conn.set_nodelay(true);
        std::uint64_t v = 0;
        while (conn.recv_value(v)) conn.send_value(v * 2);
    });

    auto client = connect_tcp("127.0.0.1", listener.port());
    client.set_nodelay(true);
    for (std::uint64_t i = 0; i < 1000; ++i) {
        client.send_value(i);
        std::uint64_t r = 0;
        ASSERT_TRUE(client.recv_value(r));
        EXPECT_EQ(r, 2 * i);
    }
    client.shutdown_write();
    server.join();
}

TEST(Socket, ConnectFailuresThrow)
{
    EXPECT_THROW((void)connect_unix(socket_path("nobody")), std::system_error);
    EXPECT_THROW((void)connect_tcp("not-an-ip", 1), std::invalid_argument);
    EXPECT_THROW((void)stream_listener::listen_unix("/tmp/" + std::string(200, 'x')),
                 std::invalid_argument);
}

TEST(Socket, CrossProcessEcho)
{
    auto [parent, child] = socket_pair();
    const pid_t pid = ::fork();
    ASSERT_NE(pid, -1);
    if (pid == 0) {
        int rc = 0;
        try {
            parent = stream_socket{}; // close the parent's end in the child
            std::vector<std::byte> frame;
            while (child.recv_frame(frame)) child.send_frame(frame.data(), frame.size());
        } catch (...) {
            rc = 1;
        }
        ::_exit(rc);
    }
    child = stream_socket{};

    std::vector<std::byte> reply;
    for (int i = 0; i < 1000; ++i) {
        const auto msg = bytes(std::string(static_cast<std::size_t>(i % 300), 'a'));
        parent.send_frame(msg.data(), msg.size());
        ASSERT_TRUE(parent.recv_frame(reply));
        ASSERT_EQ(reply, msg);
    }
    parent.shutdown_write();
    int status = 0;
    ASSERT_EQ(::waitpid(pid, &status, 0), pid);
    EXPECT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}
