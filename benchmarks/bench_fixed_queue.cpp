#include <benchmark/benchmark.h>

#include "bench_util.hpp"

#include <hpc/containers/fixed_queue.hpp>

#include <cstdint>
#include <queue>
#include <string>

namespace {

// ---------------------------------------------------------------------------
// Push N integers
// ---------------------------------------------------------------------------

void BM_FixedQueueBaseline_StdQueue_Push_Int(benchmark::State& state)
{
    const auto n = static_cast<std::size_t>(state.range(0));
    std::queue<int> q_storage;
    auto& q = bench::opaque(q_storage);
    for (auto _ : state) {
        for (std::size_t i = 0; i < n; ++i) q.push(static_cast<int>(i));
        benchmark::DoNotOptimize(&q);
        state.PauseTiming();
        while (!q.empty()) q.pop();
        state.ResumeTiming();
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(n));
}

template <std::size_t Cap>
void BM_HpcFixedQueue_Push_Int(benchmark::State& state)
{
    hpc::containers::fixed_queue<int, Cap> q_storage;
    auto& q = bench::opaque(q_storage);
    for (auto _ : state) {
        for (std::size_t i = 0; i < Cap; ++i) (void)q.try_push(static_cast<int>(i));
        benchmark::DoNotOptimize(&q);
        state.PauseTiming();
        q.clear();
        state.ResumeTiming();
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(Cap));
}

// ---------------------------------------------------------------------------
// Pop N integers (pre-filled)
// ---------------------------------------------------------------------------

void BM_FixedQueueBaseline_StdQueue_Pop_Int(benchmark::State& state)
{
    const auto n = static_cast<std::size_t>(state.range(0));
    std::queue<int> q_storage;
    auto& q = bench::opaque(q_storage);
    for (auto _ : state) {
        state.PauseTiming();
        for (std::size_t i = 0; i < n; ++i) q.push(static_cast<int>(i));
        state.ResumeTiming();
        while (!q.empty()) {
            bench::observe(q.front());
            q.pop();
        }
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(n));
}

template <std::size_t Cap>
void BM_HpcFixedQueue_Pop_Int(benchmark::State& state)
{
    hpc::containers::fixed_queue<int, Cap> q_storage;
    auto& q = bench::opaque(q_storage);
    for (auto _ : state) {
        state.PauseTiming();
        for (std::size_t i = 0; i < Cap; ++i) (void)q.try_push(static_cast<int>(i));
        state.ResumeTiming();
        while (!q.empty()) {
            bench::observe(q.front());
            (void)q.try_pop();
        }
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(Cap));
}

// ---------------------------------------------------------------------------
// Push + Pop round-trip (interleaved, depth-1)
// ---------------------------------------------------------------------------

void BM_FixedQueueBaseline_StdQueue_PushPop_Int(benchmark::State& state)
{
    const auto n = static_cast<std::size_t>(state.range(0));
    std::queue<int> q_storage;
    auto& q = bench::opaque(q_storage);
    for (auto _ : state) {
        for (std::size_t i = 0; i < n; ++i) {
            q.push(static_cast<int>(i));
            bench::observe(q.front());
            q.pop();
        }
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(n));
}

void BM_HpcFixedQueue_PushPop_Int(benchmark::State& state)
{
    const auto n = static_cast<std::size_t>(state.range(0));
    // Capacity 4096, like the push-only and pop-only benchmarks. With capacity
    // 1 GCC specializes the index math for the single slot, and on a Ryzen
    // 9950X that version ran at 0.71 ns per push + pop against 0.39 ns for any
    // larger capacity doing the same work.
    hpc::containers::fixed_queue<int, 4096> q_storage;
    auto& q = bench::opaque(q_storage);
    for (auto _ : state) {
        for (std::size_t i = 0; i < n; ++i) {
            (void)q.try_push(static_cast<int>(i));
            bench::observe(q.front());
            (void)q.try_pop();
        }
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(n));
}

// ---------------------------------------------------------------------------
// Push N strings (non-trivial type)
// ---------------------------------------------------------------------------

void BM_FixedQueueBaseline_StdQueue_Push_String(benchmark::State& state)
{
    const auto n = static_cast<std::size_t>(state.range(0));
    std::queue<std::string> q_storage;
    auto& q = bench::opaque(q_storage);
    for (auto _ : state) {
        for (std::size_t i = 0; i < n; ++i) q.push("benchmark_string");
        benchmark::DoNotOptimize(&q);
        state.PauseTiming();
        while (!q.empty()) q.pop();
        state.ResumeTiming();
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(n));
}

template <std::size_t Cap>
void BM_HpcFixedQueue_Push_String(benchmark::State& state)
{
    hpc::containers::fixed_queue<std::string, Cap> q_storage;
    auto& q = bench::opaque(q_storage);
    for (auto _ : state) {
        for (std::size_t i = 0; i < Cap; ++i) (void)q.try_push(std::string("benchmark_string"));
        benchmark::DoNotOptimize(&q);
        state.PauseTiming();
        q.clear();
        state.ResumeTiming();
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(Cap));
}

} // namespace

// -- Push int --
BENCHMARK(BM_FixedQueueBaseline_StdQueue_Push_Int)->RangeMultiplier(4)->Range(64, 1 << 16);
BENCHMARK(BM_HpcFixedQueue_Push_Int<64>);
BENCHMARK(BM_HpcFixedQueue_Push_Int<256>);
BENCHMARK(BM_HpcFixedQueue_Push_Int<1024>);
BENCHMARK(BM_HpcFixedQueue_Push_Int<4096>);
BENCHMARK(BM_HpcFixedQueue_Push_Int<16384>);
BENCHMARK(BM_HpcFixedQueue_Push_Int<65536>);

// -- Pop int --
BENCHMARK(BM_FixedQueueBaseline_StdQueue_Pop_Int)->RangeMultiplier(4)->Range(64, 1 << 16);
BENCHMARK(BM_HpcFixedQueue_Pop_Int<64>);
BENCHMARK(BM_HpcFixedQueue_Pop_Int<256>);
BENCHMARK(BM_HpcFixedQueue_Pop_Int<1024>);
BENCHMARK(BM_HpcFixedQueue_Pop_Int<4096>);
BENCHMARK(BM_HpcFixedQueue_Pop_Int<16384>);
BENCHMARK(BM_HpcFixedQueue_Pop_Int<65536>);

// -- Push+Pop round-trip --
BENCHMARK(BM_FixedQueueBaseline_StdQueue_PushPop_Int)->RangeMultiplier(4)->Range(64, 1 << 16);
BENCHMARK(BM_HpcFixedQueue_PushPop_Int)->RangeMultiplier(4)->Range(64, 1 << 16);

// -- Push string --
BENCHMARK(BM_FixedQueueBaseline_StdQueue_Push_String)->RangeMultiplier(4)->Range(64, 1 << 14);
BENCHMARK(BM_HpcFixedQueue_Push_String<64>);
BENCHMARK(BM_HpcFixedQueue_Push_String<256>);
BENCHMARK(BM_HpcFixedQueue_Push_String<1024>);
BENCHMARK(BM_HpcFixedQueue_Push_String<4096>);
BENCHMARK(BM_HpcFixedQueue_Push_String<16384>);

