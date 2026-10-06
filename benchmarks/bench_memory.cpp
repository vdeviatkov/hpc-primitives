// Small-object allocation: allocate kBatch 64-byte objects, then release them.

#include <hpc/memory/arena.hpp>
#include <hpc/memory/pool.hpp>

#include <benchmark/benchmark.h>

#include <array>
#include <cstdint>
#include <cstdlib>

namespace {

constexpr std::size_t kBatch = 1024;

struct alignas(64) object {
    std::uint64_t data[8];
};

void BM_Malloc(benchmark::State& state)
{
    std::array<void*, kBatch> ptrs{};
    for (auto _ : state) {
        for (auto& p : ptrs) benchmark::DoNotOptimize(p = std::malloc(sizeof(object)));
        for (void* p : ptrs) std::free(p);
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(kBatch));
}

void BM_Arena(benchmark::State& state)
{
    hpc::memory::arena arena(kBatch * sizeof(object));
    for (auto _ : state) {
        for (std::size_t i = 0; i < kBatch; ++i)
            benchmark::DoNotOptimize(arena.allocate(sizeof(object), alignof(object)));
        arena.reset();
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(kBatch));
}

void BM_FixedPool(benchmark::State& state)
{
    hpc::memory::fixed_pool pool(sizeof(object), kBatch, alignof(object));
    std::array<void*, kBatch> ptrs{};
    for (auto _ : state) {
        for (auto& p : ptrs) benchmark::DoNotOptimize(p = pool.allocate());
        for (void* p : ptrs) pool.deallocate(p);
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(kBatch));
}

} // namespace

BENCHMARK(BM_Malloc);
BENCHMARK(BM_Arena);
BENCHMARK(BM_FixedPool);
