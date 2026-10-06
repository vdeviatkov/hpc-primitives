# Inter-process communication

`hpc/ipc` has three families of transport. All of them are POSIX-only
(Linux, macOS) and are left out of Windows builds.

| Transport | Header | Shape | Survives process exit | Typical round trip |
|---|---|---|---|---|
| `shm_spsc_queue<T>` | `hpc/ipc/shm_spsc_queue.hpp` | 1 producer → 1 consumer, bounded ring | no (creator unlinks) | ~0.1 µs |
| `shm_mpmc_queue<T>` | `hpc/ipc/shm_mpmc_queue.hpp` | N producers → M consumers, bounded ring | no (creator unlinks) | ~0.1 µs |
| `file_journal<T>` | `hpc/ipc/file_journal.hpp` | 1 writer → any number of readers, append-only log | yes, it is a file | memory speed |
| `stream_socket` | `hpc/ipc/socket.hpp` | byte stream / framed messages, Unix-domain or TCP | no | 2.5–5 µs (Unix), 3.6–14 µs (TCP) |

Pick one:

- **Lowest latency between processes on one machine**: a shared-memory queue.
  It costs no system calls, but the receiver has to poll (busy-wait), and
  both sides must agree on `T`'s layout.
- **Several writers or several readers on the same queue**: `shm_mpmc_queue`.
- **Readers that join late, replay history, or must not lose data when
  slow**: `file_journal`. Records stay in the file until you delete it.
- **Variable-size messages, blocking reads, another machine, or a peer in
  another language without shared memory**: sockets.

Measured numbers are under [Performance](#performance).

## Shared-memory queues

Both queues live in a POSIX shared-memory object (`shm_open` + `mmap`). The
creator sizes and initializes it, then publishes a magic number with release
semantics. `open()` checks the magic number and `sizeof(T)`, so it refuses
a half-initialized queue or one with a different element type. `T` must be
trivially copyable and contain no pointers, since pointers mean nothing in
another address space.

```cpp
#include <hpc/ipc/shm_spsc_queue.hpp>

struct tick { std::uint64_t seq; double bid, ask; };

// process A (producer)
auto q = hpc::ipc::shm_spsc_queue<tick>::create("/md_ticks", 4096);
if (!q.try_push({seq, bid, ask})) { /* full: drop, retry or back off */ }

// process B (consumer)
auto q = hpc::ipc::shm_spsc_queue<tick>::open("/md_ticks");
tick t;
while (running) {
    if (q.try_pop(t)) handle(t);
    else hpc::support::cpu_relax();
}
```

`shm_mpmc_queue` has the same API, with any number of `try_push` and
`try_pop` callers across processes:

```cpp
#include <hpc/ipc/shm_mpmc_queue.hpp>

// dispatcher
auto jobs = hpc::ipc::shm_mpmc_queue<job>::create("/jobs", 1024);

// in each worker process
auto jobs = hpc::ipc::shm_mpmc_queue<job>::open("/jobs");
job j;
if (jobs.try_pop(j)) run(j);
```

Rules and caveats:

- **Names** start with `/` and contain no other slash. macOS limits them to
  31 characters.
- **Lifetime.** The handle returned by `create()` unlinks the name when it
  is destroyed. Processes that already have the queue open keep working, but
  nobody new can `open()` it. `create()` replaces a stale object left by a
  crashed run.
- **Capacity** is rounded up to a power of two (minimum 2 for MPMC).
- **SPSC is strict.** Exactly one process (or thread) pushes and one pops.
  Use `shm_mpmc_queue` for anything else.
- **Crashes.** If an MPMC producer dies between claiming a slot and filling
  it, that slot stays busy forever, and consumers stall when they reach it.
  Recreate the queue after a crash. An SPSC producer that dies loses nothing
  already pushed.
- **Polling.** Neither queue sleeps. For a consumer that should block, pair
  the queue with a wakeup (a socket, an `eventfd`, or a futex), or use
  sockets.

### Layouts

The layouts are fixed, so other languages can attach.
[`examples/shm_subscriber.py`](../examples/shm_subscriber.py) reads
`shm_spsc_queue` using only the Python standard library.

```text
shm_spsc_queue                         shm_mpmc_queue
  0  u64 magic "HPCSPSQ1"               0  u64 magic "HPCMPMQ1"
  8  u64 capacity                       8  u64 capacity
 16  u64 sizeof(T)                     16  u64 sizeof(T)
128  u64 tail (producer)               24  u64 slot stride
256  u64 head (consumer)              128  u64 tail (producers, CAS)
384  T slots[capacity]                256  u64 head (consumers, CAS)
                                      384  { u64 seq; T value; } slots[capacity]
```

`shm_spsc_queue` caches the other side's index in each handle and re-reads
the shared one only when the queue looks full or empty. That keeps the
producer and consumer off each other's cache line, and it is invisible
in the layout.

`shm_mpmc_queue` is Vyukov's bounded queue, the same algorithm as the
in-process `hpc::concurrency::mpmc_queue`. See
[design notes](design.md#concurrent-queues).

## File journal

`file_journal<T>` is an append-only array of fixed-size records in a
memory-mapped file. One process appends, and any number of processes read,
each at its own position. Nothing is ever overwritten, so a slow reader
cannot miss records, and a reader started later can replay from record 0.

```cpp
#include <hpc/ipc/file_journal.hpp>

struct fill { std::uint64_t order_id; std::int64_t px, qty; };

// writer
auto log = hpc::ipc::file_journal<fill>::create("/var/tmp/fills.journal", 1'000'000);
if (!log.try_append({id, px, qty})) { /* file full: roll to a new one */ }
log.flush(); // optional: msync, only needed to survive power loss

// reader (another process, possibly started later)
auto log = hpc::ipc::file_journal<fill>::open("/var/tmp/fills.journal");
std::uint64_t next = 0;           // or a position you persisted earlier
fill f;
for (;;) {
    while (log.try_read(next, f)) { handle(f); ++next; }
    wait_a_bit();                 // new records appear as log.size() grows
}
```

Semantics:

- **Visibility.** The writer copies a record and then publishes it with a
  release store to `committed`. Readers acquire `committed` and only read
  below it. Visibility goes through the page cache, so it does not wait for
  the disk.
- **Durability.** Data reaches disk on normal kernel writeback. Call
  `flush()` (an `msync`) when a record has to survive a power loss or a
  kernel crash, not just a process crash.
- **Crash consistency.** A writer that dies mid-append leaves `committed`
  at the last complete record. `open()` followed by `try_append()` resumes
  from there.
- **Capacity** is fixed at `create()`. `try_append` returns `false` when the
  file is full; roll over to a new file, the way Chronicle Queue or Kafka do.
  The file is created sparse, so unused capacity takes no disk space.
- **One writer.** Two processes appending to the same journal will corrupt
  it. Put an `shm_mpmc_queue` in front of a single writer to merge streams.
- `create()` truncates an existing file. Use `open()` to append to an
  existing journal.

Layout (little-endian): `u64 magic "HPCJRNL1"` at 0, `u64 capacity` at 8,
`u64 sizeof(T)` at 16, `u64 committed` at 128, records from offset 256.
Reading it from Python:

```python
import mmap, struct
with open("/var/tmp/fills.journal", "rb") as f:
    m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
    committed = struct.unpack_from("<Q", m, 128)[0]
    rec = struct.Struct("<Qqq")
    for i in range(committed):
        order_id, px, qty = rec.unpack_from(m, 256 + i * rec.size)
```

## Sockets

`stream_socket` is a connected, blocking stream socket that owns its
descriptor. `stream_listener` accepts connections on a Unix-domain path or
a TCP port. Errors throw `std::system_error`. Writing to a closed peer
throws `EPIPE` instead of killing the process with `SIGPIPE`.

```cpp
#include <hpc/ipc/socket.hpp>
using namespace hpc::ipc;

// server
auto listener = stream_listener::listen_unix("/tmp/pricer.sock");
// or: stream_listener::listen_tcp("127.0.0.1", 9000)   (port 0 = any free port)
stream_socket conn = listener.accept();
std::vector<std::byte> frame;
while (conn.recv_frame(frame)) {               // false = peer closed cleanly
    conn.send_frame(frame.data(), frame.size());
}

// client
stream_socket s = connect_unix("/tmp/pricer.sock");
// or: connect_tcp("127.0.0.1", 9000); then s.set_nodelay(true) for latency
s.send_frame(buf.data(), buf.size());
s.recv_frame(frame);
```

| Call | Meaning |
|---|---|
| `send_all(p, n)` / `recv_all(p, n)` | exactly `n` bytes; `recv_all` returns `false` on a clean close before the first byte, and throws on a close mid-message |
| `send_frame` / `recv_frame` | message framing: little-endian `u32` length, then the payload |
| `send_value<T>` / `recv_value<T>` | raw bytes of a trivially copyable `T` |
| `set_nodelay(true)` | TCP only: turn off Nagle so small messages leave at once |
| `shutdown_write()` | half-close; the peer's next read returns end of stream |
| `socket_pair()` | two connected Unix-domain sockets, for a parent and a `fork()`ed child |

The framing is easy to speak from other languages. In Python:

```python
import socket, struct
s = socket.socket(socket.AF_UNIX); s.connect("/tmp/pricer.sock")
s.sendall(struct.pack("<I", len(msg)) + msg)
n = struct.unpack("<I", s.recv(4, socket.MSG_WAITALL))[0]
reply = s.recv(n, socket.MSG_WAITALL)
```

A Unix-domain listener deletes a stale socket file when it binds and
deletes its path when it is destroyed. Socket paths are limited to about
104 bytes.

## Running the tests

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build -R 'Shm|FileJournal|Socket' --output-on-failure
```

Each transport has a test that crosses a real process boundary with `fork()`.
The others use threads, and each thread opens its own mapping or socket end.
They also run under ThreadSanitizer and ASan/UBSan
(`-DHPC_SANITIZE=thread` or `address,undefined`).

## Running the benchmarks

```bash
./build/benchmarks/hpc_benchmarks --benchmark_filter=Ipc
# steadier numbers:
./build/benchmarks/hpc_benchmarks --benchmark_filter=Ipc \
    --benchmark_repetitions=3 --benchmark_report_aggregates_only=true
```

- `BM_Ipc_Throughput_*` sends 2¹⁸ 64-byte messages from a producer to a
  consumer and reports `items_per_second`. The sockets make one `send` and
  one `recv` system call per message, without batching.
  `ShmMpmc/N` runs N producers and N consumers.
- `BM_Ipc_PingPong_*` bounces one 64-byte message back and forth. `rtt` is
  the time per round trip.

The two sides are threads pinned to CPUs 0 and 1 (pinning is a no-op on
macOS). Each side opens the queue, file or socket itself, so the hardware
and kernel paths are the same as between two processes.

## Performance

Median of 3 runs, `-O3 -march=native`, 64-byte messages. Machines as in the
[README](../README.md#performance): Apple M4 Max (Apple Clang 17, macOS) and
AMD Ryzen 9 9950X (GCC 13.3, Ubuntu 24.04, kernel 7.0).

**Throughput**, producer → consumer:

| Transport | M4 Max | Ryzen 9950X |
|---|---|---|
| `shm_spsc_queue` | 112 M/s (6.7 GiB/s) | 106 M/s (6.3 GiB/s) |
| `shm_mpmc_queue`, 1 producer / 1 consumer | 51 M/s | 79 M/s |
| `shm_mpmc_queue`, 2 / 2 | 17 M/s | 27 M/s |
| `shm_mpmc_queue`, 4 / 4 | 6.5 M/s | 16 M/s |
| `file_journal` (fresh file each run) | 16 M/s | 45 M/s |
| Unix-domain socket, one syscall per message | 1.7 M/s | 4.2 M/s |
| TCP loopback, one syscall per message | 1.9 M/s | 6.2 M/s |

**Round trip**, one message there and back:

| Transport | M4 Max | Ryzen 9950X |
|---|---|---|
| `shm_spsc_queue` | 116 ns | 97 ns |
| `shm_mpmc_queue` | 125 ns | 114 ns |
| Unix-domain socket | 4.7 µs | 2.5 µs |
| TCP loopback, `TCP_NODELAY` | 14.1 µs | 3.6 µs |

How to read these numbers:

- Shared memory is 25–120× faster than a socket on the same machine, since
  a round trip is two cache-line transfers rather than four system calls and
  two wakeups. In exchange, the receiver burns a core polling.
- `file_journal` throughput mostly pays for first-touch page faults on
  pages the file has not used yet. Readers see records through the page
  cache, so its latency should be close to shared memory's, but it has no
  round-trip benchmark yet.
- Socket throughput here is bound by one system call per 64-byte message.
  Sending several messages per `send_all` spreads that cost (not
  benchmarked here).
- `shm_spsc_queue` caches the other side's index per handle. Before that
  change, throughput on the M4 Max was 20 M/s and the round trip 192 ns.
