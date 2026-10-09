# Design notes

Why the primitives look the way they do. Per-function contracts live in the
headers.

## Concurrent queues

| Type | Producers / consumers | Bounded | Progress |
|---|---|---|---|
| `spsc_queue<T>` | 1 / 1 | yes | wait-free |
| `spsc_unbounded_queue<T>` | 1 / 1 | no (segments) | wait-free pop; push allocates once per segment |
| `mpmc_queue<T>` | N / M | yes | lock-free |

**Per-slot sequence numbers, even for SPSC.** The textbook SPSC ring (shared
`head`/`tail`, optionally with cached copies) collapses when one side polls:
a consumer spinning on an empty queue keeps pulling the `tail` cache line
away from the producer, which slows the producer, which keeps the queue empty.
On an Apple M4 Max that feedback loop held a 64 Ki-slot ring at about 14 M items/s
(the consumer failed ~80 polls per item) against about 250 M/s at 256 slots.
With a sequence number in each slot, each side polls only the slot it is
waiting for and never reads the other side's counter. Throughput becomes
insensitive to capacity (167–470 M/s across 256–64 Ki slots). The cost is
8 bytes per slot.

**`mpmc_queue` is Vyukov's bounded queue.** A CAS on `head`/`tail` reserves
a position; the release store to the slot's sequence publishes the data.
Positions are 64-bit, so there is no ABA. It is not linearizable as a whole:
`try_pop` can report empty while a producer that claimed an earlier position
is still writing.

Under heavy symmetric contention (4 producers + 4 consumers busy-polling)
on Apple Silicon, this queue is slower than `std::mutex` + `std::queue`,
because cross-cluster CAS is expensive and the mutex parks waiters. Neither
padding slots onto separate cache lines nor CAS backoff changed that. Use
it when threads do real work between operations, or when you need bounded
memory and no syscalls. Do not use it for an 8-thread ping-pong on Apple
Silicon. On a Ryzen 9 9950X the same 4P/4C test goes the other way:
`mpmc_queue` runs at 19 M/s against 9.3 M/s for the mutex.

**No unbounded MPMC.** A linked-segment MPMC queue needs safe memory
reclamation (hazard pointers or epochs) before a consumer can free a drained
segment. An earlier version skipped that and leaked every segment.

## Memory

- `arena` is header-only so that `allocate` inlines to a few instructions
  (align, compare, bump). The bounds check is written to be overflow-safe.
- `fixed_pool` rounds blocks up to the requested alignment, so the intrusive
  free-list pointer stored in each free block is always aligned. The free
  list is built so the first allocations walk the slab forwards.
- `map_pages(size, page_kind)` takes a *preferred* page size and falls back
  rather than failing: `huge_1g` → `huge_2m` → regular pages. Explicit huge
  pages exist only when an admin has reserved them, so a hard requirement
  would fail on most machines. `page_region::huge` and `page_size` report
  what was mapped, so a caller that must not run on small pages can check
  them at startup. With no huge pages available, Linux falls back to regular
  pages plus an `MADV_HUGEPAGE` hint. `page_kind::regular` sets
  `MADV_NOHUGEPAGE` instead, so the reported page size stays true even with
  THP set to `always`. Mapping is a syscall, so the benefit is fewer TLB
  misses afterwards, not faster allocation. Usage is in the
  [README](../README.md#huge-pages).
- `numa_arena` takes its memory from `numa_alloc_onnode`, which is
  page-aligned and bound with `MPOL_BIND`. That is the only reliable way to
  place an arena on a node: `mbind` on `operator new` memory fails when the
  address is not page-aligned.

## Containers

These are drop-in shapes of their `std` counterparts, so the differences are
the point:

- `queue<T>` is one power-of-two ring buffer. `std::queue` defaults to
  `std::deque`, which allocates chunks as the queue moves.
- `queue<T>` and `fixed_queue<T, N>` track the ring with two free-running
  counters, `head` (written only by pop) and `tail` (written only by push),
  and compute `size()` as `tail - head`. An earlier version kept `head` and a
  `size` count instead. Push and pop then both read-modify-wrote `size`, so
  whenever the queue lived in memory (a member, or used across a call) each
  operation waited for a store-to-load forward of the previous one's `size`.
  Under GCC on a Ryzen 9950X that made steady push + pop 3x slower than
  libstdc++'s `std::queue`. With separate counters, push and pop form two
  independent chains, and both queues went 4-11x faster. The read of the
  other counter in the full and empty checks feeds only a predicted branch,
  so nothing waits on it. `queue` relies on its power-of-two capacity to stay
  correct when a counter wraps; `fixed_queue` takes any capacity, so it uses
  64-bit counters and `pos % N`, which compiles to a mask or a multiply.
- `fixed_queue<T, N>` and `fixed_stack<T, N>` have inline storage and never
  allocate. Their `try_*` operations report full or empty instead of throwing.
- `vector<T>` and `queue<T>` construct the new element before relocating on
  growth, so `v.push_back(v[0])` is safe. Relocation uses
  `std::uninitialized_move`, which lowers to `memmove` for trivially copyable
  `T`, and falls back to copying when `T`'s move can throw.
- A default-constructed or moved-from `deque<T>` owns no memory. The chunk
  map is allocated on first insertion.

## IPC

Usage, layouts and benchmark numbers for every transport are in
[ipc.md](ipc.md). This section covers the shared-memory design.

`shm_spsc_queue<T>` has a fixed layout, documented in
[`shm_spsc_queue.hpp`](../include/hpc/ipc/shm_spsc_queue.hpp), so that
processes in other languages can attach:

```text
  0  u64 magic ("HPCSPSQ1"), u64 capacity, u64 sizeof(T)
128  u64 tail   (producer)
256  u64 head   (consumer)
384  T slots[capacity]
```

The creator writes `magic` last, with release semantics, and `open()`
validates it together with `sizeof(T)`, so a reader can't attach to a
half-initialized or incompatible queue. Head and tail are lock-free
`std::atomic<uint64_t>`, which is address-free and therefore valid across
processes. `T` must be trivially copyable and pointer-free.
[`examples/shm_subscriber.py`](../examples/shm_subscriber.py) consumes from
Python with only the standard library.

Unlike the in-process `spsc_queue`, the shared-memory SPSC queue keeps shared
`head`/`tail` counters, because external readers depend on that layout. Each
handle caches the other side's counter and re-reads it only when the queue
looks full or empty. Without that cache, every push read `head` and every pop
read `tail`, the two cache lines bounced on every operation, and throughput
on an M4 Max was 20 M/s instead of 110 M/s.

`shm_mpmc_queue<T>` is the cross-process version of `mpmc_queue`: the same
sequence-per-slot algorithm, with 64-bit positions and a fixed layout. It is
lock-free but not crash-safe: a process that dies between its CAS and its
publishing store leaves a slot that never becomes ready.

`file_journal<T>` gives up bounded memory in exchange for history. Nothing is
overwritten, so the writer never waits for readers, and readers need no
shared state beyond `committed`. The writer publishes each record with a
release store after copying it, so a crash cannot expose a torn record.
