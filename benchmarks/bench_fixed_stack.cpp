#include <benchmark/benchmark.h>

#include "bench_util.hpp"

#include <hpc/containers/fixed_stack.hpp>

#include <cstdint>
#include <stack>
#include <string>

namespace {

// ---------------------------------------------------------------------------
// Push N integers
// ---------------------------------------------------------------------------

void BM_FixedStackBaseline_StdStack_Push_Int(benchmark::State& state)
{
    const auto n = static_cast<std::size_t>(state.range(0));
    std::stack<int> s_storage;
    auto& s = bench::opaque(s_storage);
    for (auto _ : state) {
        for (std::size_t i = 0; i < n; ++i) s.push(static_cast<int>(i));
        benchmark::DoNotOptimize(&s);
        state.PauseTiming();
        while (!s.empty()) s.pop();
        state.ResumeTiming();
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(n));
}

template <std::size_t Cap>
void BM_HpcFixedStack_Push_Int(benchmark::State& state)
{
    hpc::containers::fixed_stack<int, Cap> s_storage;
    auto& s = bench::opaque(s_storage);
    for (auto _ : state) {
        for (std::size_t i = 0; i < Cap; ++i) (void)s.try_push(static_cast<int>(i));
        benchmark::DoNotOptimize(&s);
        state.PauseTiming();
        s.clear();
        state.ResumeTiming();
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(Cap));
}

// ---------------------------------------------------------------------------
// Pop N integers (pre-filled)
// ---------------------------------------------------------------------------

void BM_FixedStackBaseline_StdStack_Pop_Int(benchmark::State& state)
{
    const auto n = static_cast<std::size_t>(state.range(0));
    std::stack<int> s_storage;
    auto& s = bench::opaque(s_storage);
    for (auto _ : state) {
        state.PauseTiming();
        for (std::size_t i = 0; i < n; ++i) s.push(static_cast<int>(i));
        state.ResumeTiming();
        while (!s.empty()) {
            bench::observe(s.top());
            s.pop();
        }
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(n));
}

template <std::size_t Cap>
void BM_HpcFixedStack_Pop_Int(benchmark::State& state)
{
    hpc::containers::fixed_stack<int, Cap> s_storage;
    auto& s = bench::opaque(s_storage);
    for (auto _ : state) {
        state.PauseTiming();
        for (std::size_t i = 0; i < Cap; ++i) (void)s.try_push(static_cast<int>(i));
        state.ResumeTiming();
        while (!s.empty()) {
            bench::observe(s.top());
            (void)s.try_pop();
        }
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(Cap));
}

// ---------------------------------------------------------------------------
// Push + Pop round-trip (interleaved, depth-1)
// ---------------------------------------------------------------------------

void BM_FixedStackBaseline_StdStack_PushPop_Int(benchmark::State& state)
{
    const auto n = static_cast<std::size_t>(state.range(0));
    std::stack<int> s_storage;
    auto& s = bench::opaque(s_storage);
    for (auto _ : state) {
        for (std::size_t i = 0; i < n; ++i) {
            s.push(static_cast<int>(i));
            bench::observe(s.top());
            s.pop();
        }
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(n));
}

void BM_HpcFixedStack_PushPop_Int(benchmark::State& state)
{
    const auto n = static_cast<std::size_t>(state.range(0));
    // Capacity 4096, like the push-only and pop-only benchmarks. With capacity
    // 1 GCC specializes the index math for the single slot, and on a Ryzen
    // 9950X that version ran at 0.71 ns per push + pop against 0.39 ns for any
    // larger capacity doing the same work.
    hpc::containers::fixed_stack<int, 4096> s_storage;
    auto& s = bench::opaque(s_storage);
    for (auto _ : state) {
        for (std::size_t i = 0; i < n; ++i) {
            (void)s.try_push(static_cast<int>(i));
            bench::observe(s.top());
            (void)s.try_pop();
        }
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(n));
}

// ---------------------------------------------------------------------------
// Push N strings (non-trivial type)
// ---------------------------------------------------------------------------

void BM_FixedStackBaseline_StdStack_Push_String(benchmark::State& state)
{
    const auto n = static_cast<std::size_t>(state.range(0));
    std::stack<std::string> s_storage;
    auto& s = bench::opaque(s_storage);
    for (auto _ : state) {
        for (std::size_t i = 0; i < n; ++i) s.push("benchmark_string");
        benchmark::DoNotOptimize(&s);
        state.PauseTiming();
        while (!s.empty()) s.pop();
        state.ResumeTiming();
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(n));
}

template <std::size_t Cap>
void BM_HpcFixedStack_Push_String(benchmark::State& state)
{
    hpc::containers::fixed_stack<std::string, Cap> s_storage;
    auto& s = bench::opaque(s_storage);
    for (auto _ : state) {
        for (std::size_t i = 0; i < Cap; ++i) (void)s.try_push(std::string("benchmark_string"));
        benchmark::DoNotOptimize(&s);
        state.PauseTiming();
        s.clear();
        state.ResumeTiming();
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(Cap));
}

} // namespace

// -- Push int (capacity == N so each instantiation is right-sized) --
BENCHMARK(BM_FixedStackBaseline_StdStack_Push_Int)->RangeMultiplier(4)->Range(64, 1 << 16);
BENCHMARK(BM_HpcFixedStack_Push_Int<64>);
BENCHMARK(BM_HpcFixedStack_Push_Int<256>);
BENCHMARK(BM_HpcFixedStack_Push_Int<1024>);
BENCHMARK(BM_HpcFixedStack_Push_Int<4096>);
BENCHMARK(BM_HpcFixedStack_Push_Int<16384>);
BENCHMARK(BM_HpcFixedStack_Push_Int<65536>);

// -- Pop int --
BENCHMARK(BM_FixedStackBaseline_StdStack_Pop_Int)->RangeMultiplier(4)->Range(64, 1 << 16);
BENCHMARK(BM_HpcFixedStack_Pop_Int<64>);
BENCHMARK(BM_HpcFixedStack_Pop_Int<256>);
BENCHMARK(BM_HpcFixedStack_Pop_Int<1024>);
BENCHMARK(BM_HpcFixedStack_Pop_Int<4096>);
BENCHMARK(BM_HpcFixedStack_Pop_Int<16384>);
BENCHMARK(BM_HpcFixedStack_Pop_Int<65536>);

// -- Push+Pop round-trip int --
BENCHMARK(BM_FixedStackBaseline_StdStack_PushPop_Int)->RangeMultiplier(4)->Range(64, 1 << 16);
BENCHMARK(BM_HpcFixedStack_PushPop_Int)->RangeMultiplier(4)->Range(64, 1 << 16);

// -- Push string --
BENCHMARK(BM_FixedStackBaseline_StdStack_Push_String)->RangeMultiplier(4)->Range(64, 1 << 14);
BENCHMARK(BM_HpcFixedStack_Push_String<64>);
BENCHMARK(BM_HpcFixedStack_Push_String<256>);
BENCHMARK(BM_HpcFixedStack_Push_String<1024>);
BENCHMARK(BM_HpcFixedStack_Push_String<4096>);
BENCHMARK(BM_HpcFixedStack_Push_String<16384>);
