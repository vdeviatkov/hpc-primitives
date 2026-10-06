// Publishes fixed-size messages into a shared-memory SPSC queue at ~1 kHz.
// Consume them from another process with examples/shm_subscriber.py.

#include <hpc/ipc/shm_spsc_queue.hpp>

#include <chrono>
#include <csignal>
#include <cstdint>
#include <exception>
#include <iostream>
#include <thread>

namespace {

// Mirrored in shm_subscriber.py as struct.Struct("<QQ48s").
struct message {
    std::uint64_t seq;
    std::uint64_t timestamp_ns;
    std::uint8_t  payload[48];
};
static_assert(sizeof(message) == 64);

volatile std::sig_atomic_t g_stop = 0;

} // namespace

int main()
{
    std::signal(SIGINT, [](int) { g_stop = 1; });
    std::signal(SIGTERM, [](int) { g_stop = 1; });

    try {
        auto queue = hpc::ipc::shm_spsc_queue<message>::create("/hpc_demo_queue", 1024);
        std::cout << "publishing to /hpc_demo_queue (Ctrl-C to stop)\n";

        std::uint64_t dropped = 0;
        for (std::uint64_t seq = 0; !g_stop; ++seq) {
            message msg{};
            msg.seq = seq;
            msg.timestamp_ns = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());
            msg.payload[0] = static_cast<std::uint8_t>(seq);

            // Drop-newest backpressure: the producer never touches the
            // consumer's index, so the queue stays strictly SPSC.
            if (!queue.try_push(msg)) ++dropped;

            if (seq % 1000 == 0) std::cout << "seq=" << seq << " dropped=" << dropped << '\n';
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    } catch (const std::exception& e) {
        std::cerr << "shm_publisher: " << e.what() << '\n';
        return 1;
    }
    return 0;
}
