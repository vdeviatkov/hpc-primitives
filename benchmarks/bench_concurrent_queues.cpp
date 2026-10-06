// Cross-thread queue throughput: producers push kItems in total, consumers pop
// them all. Reported as items/s of wall-clock time.

#include <hpc/concurrency/mpmc_queue.hpp>
#include <hpc/concurrency/spsc_queue.hpp>
#include <hpc/concurrency/spsc_unbounded_queue.hpp>
#include <hpc/support/platform.hpp>

#include <benchmark/benchmark.h>

#include <cstdint>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace {

constexpr std::uint64_t kItems    = 1 << 20;
constexpr std::size_t   kCapacity = 4096;

// Baseline: std::queue behind a std::mutex.
template <class T>
class locked_queue {
public:
    explicit locked_queue(std::size_t) {}
    bool try_push(const T& v)
    {
        std::lock_guard lock(m_);
        q_.push(v);
        return true;
    }
    bool try_pop(T& out)
    {
        std::lock_guard lock(m_);
        if (q_.empty()) return false;
        out = q_.front();
        q_.pop();
        return true;
    }

private:
    std::mutex   m_;
    std::queue<T> q_;
};

// Adapts the unbounded queue's infallible push to the try_push interface.
template <class T>
struct unbounded_adapter : hpc::concurrency::spsc_unbounded_queue<T> {
    explicit unbounded_adapter(std::size_t) {}
    bool try_push(const T& v)
    {
        this->push(v);
        return true;
    }
};

// Producers push [0, kItems / producers); once all have finished, the main
// thread pushes one end-of-stream sentinel per consumer. (The join orders the
// sentinel pushes after every producer push, so SPSC queues stay SPSC.)
template <class Queue>
void run(benchmark::State& state, unsigned producers, unsigned consumers)
{
    constexpr std::uint64_t kStop   = ~std::uint64_t{0};
    const std::uint64_t per_producer = kItems / producers;

    for (auto _ : state) {
        Queue q(kCapacity);
        std::vector<std::thread> consumer_threads, producer_threads;

        for (unsigned c = 0; c < consumers; ++c) {
            consumer_threads.emplace_back([&q, c] {
                hpc::support::pin_current_thread(c);
                std::uint64_t v = 0;
                for (;;) {
                    if (!q.try_pop(v)) {
                        hpc::support::cpu_relax();
                        continue;
                    }
                    if (v == kStop) break;
                    benchmark::DoNotOptimize(v);
                }
            });
        }
        for (unsigned p = 0; p < producers; ++p) {
            producer_threads.emplace_back([&q, p, consumers, per_producer] {
                hpc::support::pin_current_thread(consumers + p);
                for (std::uint64_t i = 0; i < per_producer; ++i)
                    while (!q.try_push(i)) hpc::support::cpu_relax();
            });
        }
        for (auto& t : producer_threads) t.join();
        for (unsigned c = 0; c < consumers; ++c)
            while (!q.try_push(kStop)) hpc::support::cpu_relax();
        for (auto& t : consumer_threads) t.join();
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(per_producer * producers));
}

using u64 = std::uint64_t;

void BM_Spsc_LockedStdQueue(benchmark::State& s) { run<locked_queue<u64>>(s, 1, 1); }
void BM_Spsc_SpscQueue(benchmark::State& s) { run<hpc::concurrency::spsc_queue<u64>>(s, 1, 1); }
void BM_Spsc_SpscUnboundedQueue(benchmark::State& s) { run<unbounded_adapter<u64>>(s, 1, 1); }
void BM_Spsc_MpmcQueue(benchmark::State& s) { run<hpc::concurrency::mpmc_queue<u64>>(s, 1, 1); }

void BM_Mpmc_LockedStdQueue(benchmark::State& s)
{
    const auto n = static_cast<unsigned>(s.range(0));
    run<locked_queue<u64>>(s, n, n);
}

void BM_Mpmc_MpmcQueue(benchmark::State& s)
{
    const auto n = static_cast<unsigned>(s.range(0));
    run<hpc::concurrency::mpmc_queue<u64>>(s, n, n);
}

} // namespace

BENCHMARK(BM_Spsc_LockedStdQueue)->UseRealTime()->Unit(benchmark::kMillisecond);
BENCHMARK(BM_Spsc_SpscQueue)->UseRealTime()->Unit(benchmark::kMillisecond);
BENCHMARK(BM_Spsc_SpscUnboundedQueue)->UseRealTime()->Unit(benchmark::kMillisecond);
BENCHMARK(BM_Spsc_MpmcQueue)->UseRealTime()->Unit(benchmark::kMillisecond);

// Arg = producers = consumers.
BENCHMARK(BM_Mpmc_LockedStdQueue)->Arg(2)->Arg(4)->UseRealTime()->Unit(benchmark::kMillisecond);
BENCHMARK(BM_Mpmc_MpmcQueue)->Arg(2)->Arg(4)->UseRealTime()->Unit(benchmark::kMillisecond);
