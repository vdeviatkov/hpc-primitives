# hpc-primitives

Low-latency C++23 building blocks: lock-free queues, allocators, and
cache-conscious containers, with tests run under ASan, UBSan and TSan, and
benchmarks against the standard library.

| Module | Types |
|---|---|
| `hpc/concurrency` | `spsc_queue`, `spsc_unbounded_queue`, `mpmc_queue`, `ttas_spinlock` |
| `hpc/memory` | `arena` + `arena_allocator`, `fixed_pool` + `object_pool`, `map_pages` (huge pages), `numa_arena` |
| `hpc/containers` | `vector`, `deque`, `queue`, `fixed_queue`, `fixed_stack` |
| `hpc/ipc` | `shm_spsc_queue`, `shm_mpmc_queue` (shared memory), `file_journal` (memory-mapped file), `stream_socket` (Unix-domain / TCP); see the [IPC guide](docs/ipc.md) |

Design rationale and trade-offs are in [docs/design.md](docs/design.md).
How to use and benchmark the IPC transports is in [docs/ipc.md](docs/ipc.md).

## Performance

Median of 3 runs, `-O3 -march=native`, on two machines:

- **M4 Max**: Apple M4 Max, Apple Clang 17, libc++, macOS.
- **Ryzen 9950X**: AMD Ryzen 9 9950X (Zen 5, 16 cores / 32 threads), GCC 13.3,
  libstdc++, Ubuntu 24.04, `performance` governor.

Reproduce with `./build/benchmarks/hpc_benchmarks --benchmark_repetitions=3
--benchmark_report_aggregates_only=true`. Each cell reads baseline → hpc.

| Benchmark | Baseline vs hpc | M4 Max | Ryzen 9950X |
|---|---|---|---|
| SPSC queue, producer thread → consumer thread | `std::queue` + `std::mutex` vs `spsc_queue` | 71 → 184 M/s, **2.6×** | 16 → 397 M/s, **25×** |
| | vs `spsc_unbounded_queue` | 71 → 624 M/s, **8.8×** | 16 → 561 M/s, **35×** |
| Lock + increment, 1 thread | `std::mutex` vs `ttas_spinlock` | 4.3 → 1.1 ns, **3.8×** | 7.8 → 3.7 ns, **2.1×** |
| Lock + increment, 4 threads | `std::mutex` vs `ttas_spinlock` | 129 → 32 ns, **4.1×** | 136 → 54 ns, **2.5×** |
| 64-byte allocations, batch of 1024 | `malloc`/`free` vs `arena` | 84 → 798 M/s, **9.5×** | 94 M/s → 1.41 G/s, **15×** |
| | vs `fixed_pool` | 84 M/s → 1.10 G/s, **13×** | 94 → 965 M/s, **10×** |
| Steady-state push + pop | `std::queue` vs `queue` | 188 M/s → 1.67 G/s, **8.9×** | 1.27 G/s → 421 M/s, 0.33× |
| `push_back` + `pop_front`, 4096 elements | `std::deque` vs `deque` | 201 M/s → 1.07 G/s, **5.3×** | 1.90 → 1.66 G/s, 0.87× |
| `push_back`, 4096 ints, no reserve | `std::vector` vs `vector` | 2.60 → 2.70 G/s, 1.04× | 3.17 → 3.00 G/s, 0.94× |

IPC, 64-byte messages between two pinned threads, each with its own mapping
or socket end ([details](docs/ipc.md#performance)):

| Transport | Throughput, M4 Max | Throughput, Ryzen 9950X | Round trip, M4 Max | Round trip, Ryzen 9950X |
|---|---|---|---|---|
| `shm_spsc_queue` | 112 M/s | 106 M/s | 116 ns | 97 ns |
| `shm_mpmc_queue`, 1P/1C | 51 M/s | 79 M/s | 125 ns | 114 ns |
| `file_journal` | 16 M/s | 45 M/s | | |
| Unix-domain socket | 1.7 M/s | 4.2 M/s | 4.7 µs | 2.5 µs |
| TCP loopback | 1.9 M/s | 6.2 M/s | 14.1 µs | 3.6 µs |

Where it loses:

- **`queue` and `fixed_queue` steady-state push + pop under GCC.** On the
  Ryzen, `queue` runs at 421 M/s and `fixed_queue` at 406 M/s, against
  1.27 G/s for `std::queue`. Both store `head` and `size`, so push and pop
  both write `size`. When the compiler keeps the members in memory, which
  GCC does in this benchmark, every operation waits for a store-to-load
  forward of `size`. In a standalone test, the same ring with `head` and
  `tail` instead ran 6× faster on the Ryzen and 3.6× faster on the M4 Max.
- **`deque` against libstdc++.** The 5.3× `push_back` + `pop_front` win is
  measured against libc++'s `std::deque`. Against libstdc++'s on the Ryzen,
  `deque` is 0.87× as fast.
- **MPMC under heavy symmetric contention on Apple Silicon.** With 4
  producers and 4 consumers busy-polling on the M4 Max, `mpmc_queue` runs at
  5.8 M/s against 37 M/s for a locked `std::queue`. Cross-cluster CAS is
  expensive there, and the mutex parks waiters. On the Ryzen, `mpmc_queue`
  wins the same test, 19 M/s against 9.3 M/s. With 1P/1C it runs at
  142 M/s (M4 Max) and 123 M/s (Ryzen). See
  [design notes](docs/design.md#concurrent-queues).
- **`ttas_spinlock` with 8 threads**: 159 ns against 122 ns for `std::mutex`
  on the M4 Max, and 430 ns against 239 ns on the Ryzen. Once 8 threads
  fight over one cache line, the mutex wins by parking waiters.
- **`deque` random access and iteration on libc++**: 2.5× and 2.0× slower
  than libc++ at 64 Ki elements. Against libstdc++ on the Ryzen, random
  access runs at the same speed and iteration is 1.2× faster.
- **`vector<std::string>` growth on libc++** is 1.9× slower, because libc++
  marks `std::string` trivially relocatable and moves it with `memcpy`,
  while this library moves element by element. libstdc++ does not, and the
  two are level on the Ryzen (379 µs against 372 µs for 16 Ki strings).

## Build

Requires CMake ≥ 3.25 and a C++23 compiler (GCC 13+, Clang 17+, MSVC 2022).
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

As a dependency: `add_subdirectory(hpc-primitives)` and link
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
    (void)q.pop();           // returns false only on an empty queue
}
```
