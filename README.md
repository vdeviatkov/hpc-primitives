# hpc-primitives

Low-latency C++23 building blocks: concurrent queues, allocators, and
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

M4 Max values were refreshed on 2026-10-10: median of three fresh processes,
`-O3 -march=native`, minimum benchmark time requested as 0.5 s.
[Raw results, observed ranges, and machine/build metadata](benchmarks/results/apple-m4-max-2026-10-10/README.md)
are archived. Ryzen values remain historical measurements, not rerun in this audit.
These are workload-specific results, not latency guarantees; see the
[benchmark methodology](docs/benchmarking.md).

- **M4 Max**: Apple M4 Max, 16 cores, 64 GiB, Apple Clang 17, libc++,
  macOS 26.3.1 (a), AC power, low-power mode off; threads unpinned.
- **Ryzen 9950X**: AMD Ryzen 9 9950X (Zen 5, 16 cores / 32 threads), GCC 13.3,
  libstdc++, Ubuntu 24.04, `performance` governor.

Each cell reads baseline → hpc. Container rates count complete push/pop pairs
or inserted elements as named. For the Apple audit, build `build-bench` as in
[the methodology](docs/benchmarking.md), then run
`python3 benchmarks/results/apple-m4-max-2026-10-10/run.py` followed by
`python3 benchmarks/results/apple-m4-max-2026-10-10/summarize.py` from the repo
root. The runner saves three fresh-process results. Ratios use unrounded medians.

| Benchmark | Baseline vs hpc | M4 Max | Ryzen 9950X |
|---|---|---|---|
| SPSC queue, producer thread → consumer thread | `std::queue` + `std::mutex` vs `spsc_queue` | 68.2 M/s → 168 M/s, **2.5×** | 16 → 397 M/s, **25×** |
| | vs `spsc_unbounded_queue` | 68.2 M/s → 634 M/s, **9.3×** | 16 → 561 M/s, **35×** |
| Lock + increment, 1 thread | `std::mutex` vs `ttas_spinlock` | 4.36 → 1.14 ns, **3.8×** | 7.8 → 3.7 ns, **2.1×** |
| Lock + increment, 4 threads | `std::mutex` vs `ttas_spinlock` | 77.9 → 35.2 ns, **2.2×** | 136 → 54 ns, **2.5×** |
| 64-byte allocations, batch of 1024 | `malloc`/`free` vs `arena` | 82.5 M/s → 790 M/s, **9.6×** | 94 M/s → 1.41 G/s, **15×** |
| | vs `fixed_pool` | 82.5 M/s → 1.07 G/s, **13×** | 94 → 965 M/s, **10×** |
| Steady-state push + pop | `std::queue` vs `queue` | 189 M/s → 1.49 G/s, **7.9×** | 1.29 → 1.38 G/s, 1.07× |
| | `std::queue` vs `fixed_queue<int, 4096>` | 191 M/s → 806 M/s, **4.2×** | 1.29–1.81 → 2.12–2.13 G/s, **1.17–1.65×** |
| Push + pop | `std::stack` vs `fixed_stack<int, 4096>` | 271 M/s → 605 M/s, **2.2×** | 1.38–2.33 → 2.57–2.58 G/s, **1.11–1.86×** |
| Interleaved `push_back` + `pop_front`, 4096 pairs | `std::deque` vs `deque` | 189 M/s → 1.04 G/s, **5.5×** | 1.29–1.95 → 1.29–1.65 G/s, 0.85–1.00× |
| `push_back`, 4096 ints, no reserve | `std::vector` vs `vector` | 2.59 G/s → 2.68 G/s, 1.04× | 2.96–3.17 → 3.24–3.25 G/s, 1.03–1.09× |

Container rows: both sides reach the container through a pointer and read
elements the same way (see [`benchmarks/bench_util.hpp`](benchmarks/bench_util.hpp)),
to reduce differences in compiler treatment. This does not eliminate compiler
or code-layout effects. Some loops average about half a nanosecond per pair
or inserted element, and on the Ryzen the same
source measures up to a third slower or faster from one build to the next
as the loops land at different code addresses. `std::deque` push_back + pop_front,
for example, ran at 1.95 G/s in one build and 1.29 G/s in another. The Ryzen
container cells therefore give the range over two builds, the default one
and one compiled with `-falign-loops=64`, each the median of 3 processes.

The original queue baseline is unbounded and allocates, unlike the bounded
4096-slot rings. New mutex-ring and aligned-allocation baselines supplement
those comparisons; their Apple results are in the linked audit. Lock ns values are
per-worker iteration averages, not individual acquisition/wait latencies
or inverse aggregate throughput. The 4-thread mutex spans 74.3–310 ns
in the Apple audit, so its 2.2× median comparison has substantial run-to-run
variation.

IPC, 64-byte messages between two threads (affinity requested on Linux;
unpinned on macOS), each with its own mapping
or socket end ([details](docs/ipc.md#performance)):

| Transport | Throughput, M4 Max | Throughput, Ryzen 9950X | Mean round trip, M4 Max | Mean round trip, Ryzen 9950X |
|---|---|---|---|---|
| `shm_spsc_queue` | 99.3 M/s | 106 M/s | 119 ns | 97 ns |
| `shm_mpmc_queue`, 1P/1C | 49.1 M/s | 79 M/s | 140 ns | 114 ns |
| `file_journal` | 9.63 M/s | 45 M/s | | |
| Unix-domain socket | 1.58 M/s | 4.2 M/s | 4.74 µs | 2.5 µs |
| TCP loopback | 1.78 M/s | 6.2 M/s | 13.9 µs | 3.6 µs |

Apple IPC throughput also varies: SPSC spans 99.1–206 M/s, and the journal
9.11–14.8 M/s. The tables show medians; the linked audit retains every run.

Where it loses:

- **`deque` against libstdc++.** The 5.5× `push_back` + `pop_front` win is
  measured against libc++'s `std::deque`. libstdc++'s `std::deque` uses the
  same design as this one, and on the Ryzen the two are level within the
  noise of code layout: 0.85–1.23× across builds and sizes for push_back +
  pop_front, and 0.76–1.27× for the single-ended operations.
- **MPMC under heavy symmetric contention on Apple Silicon.** With 4
  producers and 4 consumers busy-polling on the M4 Max, `mpmc_queue` runs at
  5.86 M/s against 37.6 M/s for a locked `std::queue`. Cross-cluster CAS cost and
  mutex parking are plausible explanations, not isolated by this benchmark.
  On the Ryzen, `mpmc_queue`
  wins the same test, 19 M/s against 9.3 M/s. With 1P/1C it runs at
  134 M/s (M4 Max) and 123 M/s (Ryzen). See
  [design notes](docs/design.md#concurrent-queues).
- **`ttas_spinlock` with 8 threads varies between runs on the M4 Max.**
  The original run lost: 159 ns against 122 ns for `std::mutex`. The fresh
  median is 194 ns against 127 ns, with spinlock results spanning
  78.3–204 ns and mutex 123–128 ns. It can win or lose in these
  unpinned runs. The historical Ryzen loss remains 430 ns against 239 ns.
- **`deque` random access and iteration on libc++**: 2.2× and 1.9× slower
  than libc++ at 64 Ki elements. Against libstdc++ on the Ryzen, random
  access runs at the same speed and iteration is 1.2× faster.
- **`vector<std::string>` growth on libc++** is 1.8× slower, because libc++
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
