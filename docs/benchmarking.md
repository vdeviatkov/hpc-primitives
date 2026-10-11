# Benchmark methodology and audit

The README retains wins and losses. Treat its ratios as comparisons of specific
workloads on those machines, not general speedups or HFT latency guarantees.
The [2026-10-10 M4 Max audit](../benchmarks/results/apple-m4-max-2026-10-10/README.md)
reruns all 50 cases underlying the Apple tables and losing-case comparisons,
including supplementary baselines. It archives raw output, ranges, machine/build
metadata, and source/binary hashes. Apple table values now use those measurements.
Ryzen values and older before/after or capacity-sweep experiments remain
historical; their raw provenance is unavailable. Current benchmarks cannot
validate performance of older implementations.

## What the reported numbers measure

| Harness | Timed work | Interpretation |
|---|---|---|
| Concurrent queues | Queue construction/destruction, worker creation, transfer of 2²⁰ `uint64_t` items, producer joins, stop sentinels, consumer joins | Completed payload items / wall time; not enqueue or delivery latency |
| Lock + increment | Google Benchmark worker loops acquiring one shared lock and incrementing one shared counter | Reported ns is summed worker wall time / summed acquisitions (mean time per worker iteration), not an individual wait time or aggregate wall-time cost |
| Containers | Push/pop, traversal, or growth loop as named; generally thread CPU time | Small-operation averages over many iterations; instruction layout and compiler optimization matter |
| Allocators | 1024 allocations followed by free, pool deallocation, or arena reset | Objects allocated/reclaimed per CPU second; no payload writes or object construction |
| IPC throughput | Worker creation, message transfer, and joins; MPMC also opens mappings in workers; journal also creates/maps a fresh file | 64-byte messages / wall time; journal measures page-cache visibility, not durable writes |
| IPC ping-pong | Worker creation, 2¹⁵ serial request/reply exchanges, joins | `rtt` = total wall time / exchanges, a batch mean including startup/teardown |

For the pinned Google Benchmark v1.9.1, multithreaded results sum worker
wall times and iteration counts. Thus the lock ns figures are per-worker
iteration averages, including acquisition, increment, and unlock. Their inverse
is not aggregate acquisitions per elapsed second; for equally active N workers,
aggregate throughput is approximately N / reported time.

Google Benchmark's median over repetitions is a **median of batch estimates**.
It is not the median of individual messages. No current harness records
per-message p50, p95, p99, p99.9, maximum latency, or lock-wait distributions.
Inverting throughput does not produce delivery latency. Dividing RTT by two
does not establish measured one-way latency.

## Synchronization and placement

Google Benchmark synchronizes the entry and exit of its registered multithreaded
lock loops. That does not cover threads created inside queue/IPC benchmarks:
those workers start sequentially without a common start barrier. Consumers are
started first; queues start empty. Thread creation and scheduling are included
in wall time and can bias short batches. These are finite-batch tests, not
isolated steady-state service-rate tests.

Queue/IPC workers request logical CPUs starting at 0, but ignore affinity return
values. On macOS the request is a no-op. On Linux it can fail in a restricted
CPU set; logical CPU numbers alone do not establish core, SMT, CCD, or NUMA
relationships. Lock workers do not request affinity. The tables therefore do
not establish a controlled cross-cluster placement experiment. Architecture,
standard library, OS scheduling, mutex implementation, and compiler all change
between the two machines. Cross-cluster CAS and waiter parking are hypotheses
for the observed losses, not separately measured causes.

Producer joins precede insertion of stop sentinels. For SPSC this transfers
producer ownership to the main thread without overlapping producers; consumers
continue draining. For MPMC, producer joins ensure all payloads have been
published before stop records are inserted. Queue benchmarks observe consumed
values but do not validate exact-once delivery; correctness tests are separate.

## Baseline fairness and progress

- The original `std::queue` + mutex baseline is **unbounded**, allocates deque
  blocks, and never rejects a push as full. The bounded queues have 4096 slots
  and producers retry full pushes. Thus the historical comparison includes
  storage, allocation, backpressure, and synchronization differences. The new
  `BM_Spsc_LockedRing` and `BM_Mpmc_LockedRing` baselines preallocate 4096 slots
  and retry full pushes using the same harness. They supplement the original
  baseline; they have no published results yet.
- `spsc_unbounded_queue` has different allocation and reclamation behavior from
  both bounded rings and `std::queue`. Report it as its own workload comparison.
- `ttas_spinlock` has no fairness guarantee. `std::mutex` has no portable FIFO
  guarantee either. A fast aggregate acquisition rate does not rule out long
  waits or starvation. The benchmark's counter and lock layout also influence
  coherence traffic; it exercises only a minimal critical section.
- `malloc(64)` does not promise 64-byte alignment; arena and pool request it.
  `BM_AlignedNew` supplies an aligned new/delete baseline. Arena reset reclaims
  a whole batch and cannot substitute for arbitrary individual frees. Arena
  and pool backing-store creation is outside their timed loops. The original
  malloc figures remain useful only with these semantic differences stated.
- Common pointer opacity and value observation reduce container optimization
  asymmetry, but cannot prove equal compiler treatment. Inspect optimized
  assembly when interpreting sub-nanosecond averages. Build-layout sensitivity
  and the documented losing cases remain part of the results.

SPSC publishes constructed payloads with release stores to slot sequence
numbers, paired with acquire loads by the consumer. The reverse release/acquire
handoff protects slot reuse. MPMC uses relaxed CAS to reserve positions;
**slot sequence acquire/release**, not the position CAS, orders payload access.
The spinlock's successful acquire pairs with release unlock; relaxed polling
only decides when to retry acquisition. No memory-order weakening is needed to
improve benchmark results. Sanitizers and stress tests provide evidence, not a
proof of correctness for every interleaving.

The Vyukov bounded MPMC design uses CAS without a mutex and is not wait-free:
a caller can repeatedly lose reservation attempts. It also has a publication
hole: a producer paused after reserving the head slot prevents consumers from
reaching later published items. `try_pop` returns `false` at that hole even
when a later enqueue has completed; retrying cannot deliver those items until
the owner publishes. This is why the
[algorithm's author distinguishes it from a lock-free queue](https://sites.google.com/site/1024cores/home/lock-free-algorithms/queues/bounded-mpmc-queue).
`MpmcQueue.UnpublishedHeadPreventsDeliveryOfPublishedSuccessor` deterministically
exercises the behavior with a producer paused inside payload construction.

## Reproducing and publishing results

Build without sanitizers, then retain individual repetitions, not just aggregates:

```bash
cmake -S . -B build-bench -DCMAKE_BUILD_TYPE=Release \
    -DHPC_BUILD_BENCHMARKS=ON -DHPC_SANITIZE=
cmake --build build-bench -j
./build-bench/benchmarks/hpc_benchmarks \
    --benchmark_repetitions=10 --benchmark_min_time=1s \
    --benchmark_out=benchmark-results.json --benchmark_out_format=json
```

A repetition is not a fresh process. Repeat the command in fresh processes and
alternate paired baselines/implementations to expose ordering, thermal, and
allocator-state effects. Keep raw JSON alongside the commit, compiler and
standard-library versions, actual build flags, OS/kernel, CPU topology, allowed
CPU mask, affinity success, power settings, and relevant environment tunables.
Report spread as well as medians; do not select only the best build or run.
Google Benchmark's [user guide](https://github.com/google/benchmark/blob/v1.9.1/docs/user_guide.md)
describes repetitions, timing modes, and multithreaded loop synchronization.
The additional bandwidth sweeps in `ipc.md` are historical experiments; the
checked-in 64-byte IPC harness does not reproduce those sweeps.

Before publishing tail-latency claims, add a separate instrumented experiment:
use persistent workers with confirmed placement and a common start barrier,
warm up before measuring, record request-to-completion samples with a monotonic
clock, and measure timer overhead separately. Preallocate sample buffers so
recording does not allocate. Validate sequence/payload delivery outside the timed
region. Report sample count, duration, offered load, payload/capacity, retries,
CPU utilization, and per-worker progress along with p50/p99/p99.9/max.

Ping-pong is closed-loop: the next request waits for the previous reply. It
cannot characterize queues under independent arrivals or overload. For that,
use a scheduled offered-load experiment and include delay from the scheduled
arrival time, so stalls do not silently remove requests from the latency sample
(coordinated omission). Measure several load levels, burst sizes, placements,
and critical-section lengths. Until then, the sub-microsecond figures describe
mean closed-loop two-thread RTT only.
