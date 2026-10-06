// Lock throughput under contention: N threads each repeatedly take the lock
// and increment a shared counter (a minimal critical section).

#include <hpc/concurrency/ttas_spinlock.hpp>

#include <benchmark/benchmark.h>

#include <cstdint>
#include <mutex>

namespace {

template <class Lock>
void BM_Lock(benchmark::State& state)
{
    static Lock          lock;
    static std::uint64_t counter = 0;

    for (auto _ : state) {
        std::lock_guard guard(lock);
        benchmark::DoNotOptimize(++counter);
    }
    state.SetItemsProcessed(state.iterations());
}

} // namespace

BENCHMARK_TEMPLATE(BM_Lock, std::mutex)->ThreadRange(1, 8)->UseRealTime();
BENCHMARK_TEMPLATE(BM_Lock, hpc::concurrency::ttas_spinlock)->ThreadRange(1, 8)->UseRealTime();
