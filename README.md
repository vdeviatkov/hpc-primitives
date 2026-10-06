# cpp-hpc-primitives

Low-latency C++20 building blocks: lock-free queues, allocators, and
cache-conscious containers, with tests run under ASan, UBSan and TSan, and
benchmarks against the standard library.

| Module | Types |
|---|---|
| `hpc/concurrency` | `spsc_queue`, `spsc_unbounded_queue`, `mpmc_queue`, `ttas_spinlock` |
| `hpc/memory` | `arena` + `arena_allocator`, `fixed_pool` + `object_pool`, `map_pages` (huge pages), `numa_arena` |
| `hpc/containers` | `vector`, `deque`, `queue`, `fixed_queue`, `fixed_stack` |
| `hpc/ipc` | `shm_spsc_queue`: cross-process queue with a fixed layout ([Python consumer](examples/shm_subscriber.py)) |

Design rationale and trade-offs are in [docs/design.md](docs/design.md).

## Performance

Apple M4 Max, Apple Clang 17, `-O3 -march=native`, median of 3 runs. Reproduce
with `./build/benchmarks/hpc_benchmarks`.

| Benchmark | Baseline | hpc | Speedup |
|---|---|---|---|
| SPSC queue, producer thread → consumer thread | `std::queue` + `std::mutex`: 28 M/s | `spsc_queue`: 166 M/s | **6.0×** |
| | | `spsc_unbounded_queue`: 573 M/s | **21×** |
| Lock + increment, 1 thread | `std::mutex`: 4.8 ns | `ttas_spinlock`: 1.2 ns | **4.1×** |
| Lock + increment, 4 threads | `std::mutex`: 196 ns | `ttas_spinlock`: 30 ns | **6.5×** |
| 64-byte allocations, batch of 1024 | `malloc`/`free`: 76 M/s | `arena`: 795 M/s | **10×** |
| | | `fixed_pool`: 1.08 G/s | **14×** |
| Steady-state push + pop | `std::queue`: 132 M/s | `queue`: 1.17 G/s | **8.8×** |
| `push_back` + `pop_front`, 4096 elements | `std::deque`: 120 M/s | `deque`: 775 M/s | **6.5×** |
| `push_back`, 4096 ints, no reserve | `std::vector`: 2.44 G/s | `vector`: 2.57 G/s | 1.05× |

Where it loses:

- **MPMC under heavy symmetric contention.** With 4 producers and 4 consumers
  busy-polling, `mpmc_queue` runs at 4.6 M/s against 28 M/s for a locked
  `std::queue`. Cross-cluster CAS is expensive on Apple Silicon, and the
  mutex parks waiters. With 1P/1C it runs at 130 M/s. See
  [design notes](docs/design.md#concurrent-queues).
- **`ttas_spinlock` with 8 threads**: 294 ns against 131 ns for
  `std::mutex`. Spinning stops paying off once threads land on efficiency
  cores.
- **`deque` random access and iteration** are about 2.5× slower than libc++.
- **`vector<std::string>` growth** is about 2× slower: libc++ relocates
  `std::string` with `memcpy`, while this library moves element by element.

## Build

Requires CMake ≥ 3.25 and a C++20 compiler (GCC 12+, Clang 16+, MSVC 2022).
GoogleTest and Google Benchmark are fetched automatically.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/benchmarks/hpc_benchmarks
```

| Option | Default | |
|---|---|---|
| `HPC_BUILD_TESTS` / `HPC_BUILD_BENCHMARKS` / `HPC_BUILD_EXAMPLES` | ON when top-level | |
| `HPC_SANITIZE` | empty | e.g. `address,undefined` or `thread` |
| `HPC_ENABLE_NUMA` | ON | builds `numa_arena` when libnuma is found |
| `HPC_BENCH_NATIVE` | ON | `-march=native` for benchmarks |

As a dependency: `add_subdirectory(cpp-hpc-primitives)` and link
`hpc::primitives`.

## Example

```cpp
#include <hpc/concurrency/spsc_queue.hpp>

hpc::concurrency::spsc_queue<Order> q(4096);

// producer thread
if (!q.try_emplace(id, price, qty)) { /* full: drop or retry */ }

// consumer thread
if (Order* o = q.front()) {  // zero-copy
    handle(*o);
    q.pop();
}
```
