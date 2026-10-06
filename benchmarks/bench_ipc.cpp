// IPC transports, measured between two pinned threads. Each side opens its own
// mapping or socket end, exactly as a separate process would; the kernel and
// cache-coherency paths are the same as across processes.
//
//  - Throughput: 64-byte messages, producer -> consumer, items/s.
//  - PingPong:   one 64-byte message there and back; `rtt` is seconds per
//                round trip (printed as e.g. 180n = 180 ns).

#include <hpc/ipc/file_journal.hpp>
#include <hpc/ipc/shm_mpmc_queue.hpp>
#include <hpc/ipc/shm_spsc_queue.hpp>
#include <hpc/ipc/socket.hpp>
#include <hpc/support/platform.hpp>

#include <benchmark/benchmark.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

namespace {

constexpr std::uint64_t kMessages   = 1 << 18;
constexpr std::uint64_t kRoundTrips = 1 << 15;
constexpr std::size_t   kCapacity   = 4096;
constexpr std::uint64_t kStop       = ~std::uint64_t{0};

struct message {
    std::uint64_t seq;
    std::uint8_t  payload[56];
};
static_assert(sizeof(message) == 64);

std::string shm_name(const char* tag)
{
    return "/hpc_bench_" + std::string(tag) + "_" + std::to_string(::getpid());
}

// Runs consume() on CPU 0 and produce() on CPU 1 and waits for both.
template <class Produce, class Consume>
void run_pair(Produce&& produce, Consume&& consume)
{
    std::thread consumer([&] {
        hpc::support::pin_current_thread(0);
        consume();
    });
    std::thread producer([&] {
        hpc::support::pin_current_thread(1);
        produce();
    });
    producer.join();
    consumer.join();
}

template <class Queue>
void push_spin(Queue& q, const message& m)
{
    while (!q.try_push(m)) hpc::support::cpu_relax();
}

template <class Queue>
void pop_spin(Queue& q, message& m)
{
    while (!q.try_pop(m)) hpc::support::cpu_relax();
}

void set_throughput(benchmark::State& state, std::uint64_t per_iteration)
{
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(per_iteration));
    state.SetBytesProcessed(state.iterations()
                            * static_cast<std::int64_t>(per_iteration * sizeof(message)));
}

void set_round_trips(benchmark::State& state)
{
    state.counters["rtt"] = benchmark::Counter(
        static_cast<double>(state.iterations() * static_cast<std::int64_t>(kRoundTrips)),
        benchmark::Counter::kIsRate | benchmark::Counter::kInvert);
}

// ---- Throughput ------------------------------------------------------------

template <class Queue>
void BM_Ipc_Throughput_ShmQueue(benchmark::State& state, const char* tag)
{
    const auto name = shm_name(tag);
    auto tx = Queue::create(name, kCapacity);
    auto rx = Queue::open(name);
    for (auto _ : state) {
        run_pair(
            [&] {
                message m{};
                for (std::uint64_t i = 0; i < kMessages; ++i) {
                    m.seq = i;
                    push_spin(tx, m);
                }
            },
            [&] {
                message m{};
                for (std::uint64_t i = 0; i < kMessages; ++i) {
                    pop_spin(rx, m);
                    benchmark::DoNotOptimize(m);
                }
            });
    }
    set_throughput(state, kMessages);
}

void BM_Ipc_Throughput_ShmSpsc(benchmark::State& state)
{
    BM_Ipc_Throughput_ShmQueue<hpc::ipc::shm_spsc_queue<message>>(state, "tspsc");
}

// Arg = producers = consumers, each with its own mapping.
void BM_Ipc_Throughput_ShmMpmc(benchmark::State& state)
{
    using queue = hpc::ipc::shm_mpmc_queue<message>;
    const auto n = static_cast<unsigned>(state.range(0));
    const std::uint64_t per_producer = kMessages / n;
    const auto name = shm_name("tmpmc");
    auto owner = queue::create(name, kCapacity);

    for (auto _ : state) {
        std::vector<std::thread> threads;
        for (unsigned c = 0; c < n; ++c) {
            threads.emplace_back([&, c] {
                hpc::support::pin_current_thread(c);
                auto q = queue::open(name);
                message m{};
                for (;;) {
                    pop_spin(q, m);
                    if (m.seq == kStop) break;
                    benchmark::DoNotOptimize(m);
                }
            });
        }
        std::vector<std::thread> producers;
        for (unsigned p = 0; p < n; ++p) {
            producers.emplace_back([&, p] {
                hpc::support::pin_current_thread(n + p);
                auto q = queue::open(name);
                message m{};
                for (std::uint64_t i = 0; i < per_producer; ++i) {
                    m.seq = i;
                    push_spin(q, m);
                }
            });
        }
        for (auto& t : producers) t.join();
        message stop{};
        stop.seq = kStop;
        for (unsigned c = 0; c < n; ++c) push_spin(owner, stop);
        for (auto& t : threads) t.join();
    }
    set_throughput(state, per_producer * n);
}

// Append-only, so each iteration writes a fresh file; first-touch page faults
// on the new pages are part of the cost.
void BM_Ipc_Throughput_FileJournal(benchmark::State& state)
{
    using journal = hpc::ipc::file_journal<message>;
    const auto path = (std::filesystem::temp_directory_path()
                       / ("hpc_bench_journal_" + std::to_string(::getpid())))
                          .string();
    for (auto _ : state) {
        auto writer = journal::create(path, kMessages);
        auto reader = journal::open(path);
        run_pair(
            [&] {
                message m{};
                for (std::uint64_t i = 0; i < kMessages; ++i) {
                    m.seq = i;
                    (void)writer.try_append(m);
                }
            },
            [&] {
                message m{};
                for (std::uint64_t i = 0; i < kMessages; ++i) {
                    while (!reader.try_read(i, m)) hpc::support::cpu_relax();
                    benchmark::DoNotOptimize(m);
                }
            });
    }
    std::filesystem::remove(path);
    set_throughput(state, kMessages);
}

// One send() / recv() system call per message.
void socket_throughput(benchmark::State& state, hpc::ipc::stream_socket& tx,
                       hpc::ipc::stream_socket& rx)
{
    for (auto _ : state) {
        run_pair(
            [&] {
                message m{};
                for (std::uint64_t i = 0; i < kMessages; ++i) {
                    m.seq = i;
                    tx.send_value(m);
                }
            },
            [&] {
                message m{};
                for (std::uint64_t i = 0; i < kMessages; ++i) {
                    if (!rx.recv_value(m)) state.SkipWithError("unexpected end of stream");
                    benchmark::DoNotOptimize(m);
                }
            });
    }
    set_throughput(state, kMessages);
}

void BM_Ipc_Throughput_UnixSocket(benchmark::State& state)
{
    auto [tx, rx] = hpc::ipc::socket_pair();
    socket_throughput(state, tx, rx);
}

void BM_Ipc_Throughput_TcpLoopback(benchmark::State& state)
{
    auto listener = hpc::ipc::stream_listener::listen_tcp("127.0.0.1", 0);
    auto tx = hpc::ipc::connect_tcp("127.0.0.1", listener.port());
    auto rx = listener.accept();
    socket_throughput(state, tx, rx);
}

// ---- Round-trip latency ----------------------------------------------------

template <class Queue>
void BM_Ipc_PingPong_ShmQueue(benchmark::State& state, const char* tag)
{
    const auto ping_name = shm_name(tag) + "p";
    const auto pong_name = shm_name(tag) + "q";
    auto ping_tx = Queue::create(ping_name, kCapacity);
    auto pong_tx = Queue::create(pong_name, kCapacity);
    auto ping_rx = Queue::open(ping_name);
    auto pong_rx = Queue::open(pong_name);
    for (auto _ : state) {
        run_pair(
            [&] { // echo
                message m{};
                for (std::uint64_t i = 0; i < kRoundTrips; ++i) {
                    pop_spin(ping_rx, m);
                    push_spin(pong_tx, m);
                }
            },
            [&] { // initiator
                message m{};
                for (std::uint64_t i = 0; i < kRoundTrips; ++i) {
                    m.seq = i;
                    push_spin(ping_tx, m);
                    pop_spin(pong_rx, m);
                }
            });
    }
    set_round_trips(state);
}

void BM_Ipc_PingPong_ShmSpsc(benchmark::State& state)
{
    BM_Ipc_PingPong_ShmQueue<hpc::ipc::shm_spsc_queue<message>>(state, "pspsc");
}

void BM_Ipc_PingPong_ShmMpmc(benchmark::State& state)
{
    BM_Ipc_PingPong_ShmQueue<hpc::ipc::shm_mpmc_queue<message>>(state, "pmpmc");
}

void socket_ping_pong(benchmark::State& state, hpc::ipc::stream_socket& a,
                      hpc::ipc::stream_socket& b)
{
    for (auto _ : state) {
        run_pair(
            [&] { // echo
                message m{};
                for (std::uint64_t i = 0; i < kRoundTrips; ++i) {
                    if (!b.recv_value(m)) return;
                    b.send_value(m);
                }
            },
            [&] { // initiator
                message m{};
                for (std::uint64_t i = 0; i < kRoundTrips; ++i) {
                    m.seq = i;
                    a.send_value(m);
                    if (!a.recv_value(m)) state.SkipWithError("unexpected end of stream");
                }
            });
    }
    set_round_trips(state);
}

void BM_Ipc_PingPong_UnixSocket(benchmark::State& state)
{
    auto [a, b] = hpc::ipc::socket_pair();
    socket_ping_pong(state, a, b);
}

void BM_Ipc_PingPong_TcpLoopback(benchmark::State& state)
{
    auto listener = hpc::ipc::stream_listener::listen_tcp("127.0.0.1", 0);
    auto a = hpc::ipc::connect_tcp("127.0.0.1", listener.port());
    auto b = listener.accept();
    a.set_nodelay(true);
    b.set_nodelay(true);
    socket_ping_pong(state, a, b);
}

} // namespace

BENCHMARK(BM_Ipc_Throughput_ShmSpsc)->UseRealTime()->Unit(benchmark::kMillisecond);
BENCHMARK(BM_Ipc_Throughput_ShmMpmc)->Arg(1)->Arg(2)->Arg(4)->UseRealTime()->Unit(benchmark::kMillisecond);
BENCHMARK(BM_Ipc_Throughput_FileJournal)->UseRealTime()->Unit(benchmark::kMillisecond);
BENCHMARK(BM_Ipc_Throughput_UnixSocket)->UseRealTime()->Unit(benchmark::kMillisecond);
BENCHMARK(BM_Ipc_Throughput_TcpLoopback)->UseRealTime()->Unit(benchmark::kMillisecond);

BENCHMARK(BM_Ipc_PingPong_ShmSpsc)->UseRealTime()->Unit(benchmark::kMillisecond);
BENCHMARK(BM_Ipc_PingPong_ShmMpmc)->UseRealTime()->Unit(benchmark::kMillisecond);
BENCHMARK(BM_Ipc_PingPong_UnixSocket)->UseRealTime()->Unit(benchmark::kMillisecond);
BENCHMARK(BM_Ipc_PingPong_TcpLoopback)->UseRealTime()->Unit(benchmark::kMillisecond);
