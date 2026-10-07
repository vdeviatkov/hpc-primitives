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

**Bandwidth vs message size** (Ryzen 9950X, glibc 2.39). The tables above
use 64-byte messages, so they count messages, not bytes. Larger messages
reach tens of GB/s.

Method: every cell is the median of 5 runs, each in a fresh process, with
the range in brackets. Where two settings are compared, they alternate run
by run so both see the same machine state. The consumer runs on CPU 0 and
the producer on CPU 1 (same CCD) or CPU 8 (other CCD). Each side copies
every message through its own page-aligned buffer, and the consumer polls
again immediately when the queue is empty, unless noted. Shared-memory
figures use glibc with `rep movsb` disabled, because the default is
unstable on this machine (see below). The ring holds 4096 slots, capped at
16 MiB.

| Message | `shm_spsc_queue`, same CCD | `shm_spsc_queue`, across CCDs | Unix socket, same CCD | Unix socket, across CCDs |
|---|---|---|---|---|
| 8 B | 1.2 GB/s (146 M msgs/s) | 0.4 GB/s (45 M/s) | 0.03 GB/s (4.2 M/s) | 0.01 GB/s (1.5 M/s) |
| 64 B | 7.2 GB/s (112 M/s) | 1.8 GB/s (28 M/s) | 0.3 GB/s (4.4 M/s) | 0.1 GB/s (1.5 M/s) |
| 256 B | 15.7 GB/s (61 M/s) | 3.6 GB/s (14 M/s) | 1.0 GB/s (3.8 M/s) | 0.3 GB/s (1.2 M/s) |
| 1 KiB | 120 GB/s [113–126] | 6.1 GB/s | 3.7 GB/s | 1.3 GB/s |
| 4 KiB | 110 GB/s [106–122] | 10.7 GB/s | 12.1 GB/s | 3.8 GB/s |
| 64 KiB (256 slots) | 73 GB/s [72.5–73.8] | 11.1 GB/s | 30 GB/s | 8.7 GB/s |
| 1 MiB (16 slots) | 67 GB/s [66–68] | 11.4 GB/s | 34 GB/s [33–35] | 10.9 GB/s |

Ring size, for large messages, with glibc defaults vs `rep movsb` disabled:

| Message | Ring | Same CCD, default | Same CCD, `rep movsb` off | Across CCDs, default | Across CCDs, off |
|---|---|---|---|---|---|
| 64 KiB | 1 MiB | 73.5 [43–76] | 53.5 [53.3–55.0] | 14.5 [12.2–15.8] | 10.8 |
| 64 KiB | 4 MiB | 79.2 [33.5–79.3] | 76.9 [76.5–77.1] | 17.1 [11.2–17.1] | 11.0 |
| 64 KiB | 16 MiB | 72.9 [41.4–74.0] | 72.8 [72.2–73.2] | 11.3 [11.2–14.7] | 11.1 |
| 64 KiB | 64 MiB | 20.5 | 19.5 | 14.9 | 14.1 |
| 64 KiB | 256 MiB | 16.4 | 15.9 | 11.8 | 11.8 |
| 1 MiB | 1 MiB (1 slot) | 38.0 | 38.2 | 9.3 | 9.3 |
| 1 MiB | 4 MiB | 73.4 | 73.5 | 11.4 | 11.4 |
| 1 MiB | 16 MiB | 67.4 | 67.8 | 12.8 | 12.9 |
| 1 MiB | 64 MiB | 17.8 | 17.9 | 14.7 | 14.7 |
| 1 MiB | 256 MiB | 12.5 | 12.5 | 12.3 | 12.3 |

All values in GB/s. Consumer backoff, small messages, same CCD, 4096
slots, alternating:

| Message | Consumer polls again at once | Consumer pauses 256× `pause` when empty |
|---|---|---|
| 8 B | 145 M msgs/s | 336 M msgs/s |
| 64 B | 109 M msgs/s | 328 M msgs/s |
| 256 B | 61 M msgs/s | 148 M msgs/s |

For reference, one core's `memcpy` runs at 120 GB/s within L1 and 19 GB/s
from DRAM.

A CCD (core complex die) is one of the chiplets an AMD Ryzen or EPYC CPU is
built from. The 9950X has two, each with 8 cores (16 threads) and its own
32 MiB L3. No L3 is shared between them. On this machine CPUs 0–7 and 16–23
sit on CCD 0, and 8–15 and 24–31 on CCD 1 (`lscpu -e=CPU,CORE,CACHE` shows
the L3 id). The two CCDs talk through the Infinity Fabric on a separate I/O
die, which is much slower than a shared L3.

What the numbers say:

- **Small messages are bound by per-message cost, not bandwidth.** Every
  message is a slot handoff between two cores.
- **For small messages, let the producer get ahead.** A consumer that
  polls again the instant it finds the queue empty chases the producer
  through the ring, pulling each cache line across as soon as it is
  written. Pausing (here 256 `pause` instructions) before polling again
  lets the producer run ahead, and the consumer then drains a batch:
  2.3–3× more messages per second at 8–256 B. The price is latency; a
  consumer that is pausing reacts late to the next message. It made no
  difference at 1 KiB and 64 KiB.
- **Up to the L3 size, the handoff runs at cache speed.** When the ring
  fits in the L3 both cores share, the consumer reads the producer's data
  from that L3 at 67–120 GB/s, faster than DRAM. A ring larger than the L3
  (64 MiB or more here) spills to DRAM and drops to 12–20 GB/s. Size the
  ring to fit: slots × message size of a few MiB, at most 16 MiB here.
- **Give the ring more than one slot.** With one 1 MiB slot, the producer
  waits while the consumer copies, and the rate halves (38 against 73 GB/s
  with 4 slots).
- **Keep both processes on one CCD.** Across CCDs every cache line crosses
  the Infinity Fabric. Small messages get 3–4.5× slower, and 1 KiB–1 MiB
  messages lose 6–20× in bandwidth. Once the ring is too big for L3, both
  cases are DRAM-bound and the gap shrinks to 1.0–1.4×. Pin both ends to
  the same CCD (on Linux, `taskset`, or
  `hpc::support::pin_current_thread`).
- **Sockets are bound by system calls.** A Unix socket manages about
  4 M `send` calls per second on one CCD and 1.2–1.5 M across CCDs for
  messages up to 256 B, and 3.0–3.6 M per second at 1–4 KiB; beyond that
  the copy dominates. With 64 KiB–1 MiB per `send` that becomes 30–34 GB/s,
  about half of shared memory with an L3-sized ring. Across CCDs, large-message
  sockets (8.7–10.9 GB/s) come close to shared memory (11.1–11.4 GB/s).
- **glibc's `rep movsb` is unstable on this machine.** glibc's `memcpy`
  switches to the `rep movsb` instruction at 2112 bytes. With that default,
  4 KiB and 64 KiB same-CCD runs occasionally drop to 24–65 GB/s, and
  these slow episodes last for minutes. With `rep movsb` disabled
  (`GLIBC_TUNABLES=glibc.cpu.x86_rep_movsb_threshold=0xffffffff`, so glibc
  uses vector loads and stores), every same-CCD cell was stable, run after
  run. Disabling it is not free, though: `rep movsb` was faster for 64 KiB
  across CCDs (14.5–17.1 against 10.8–11.0 GB/s), and for 64 KiB messages
  on a 1 MiB ring (73.5 against 53.5 GB/s). Use the tunable for steady same-CCD throughput
  at 4–64 KiB; otherwise keep the default. 1 MiB messages showed no
  difference.
- **Open questions.** What triggers the `rep movsb` slow episodes is not
  known. Finding out needs CPU performance counters, which this machine
  blocks for normal users (`kernel.perf_event_paranoid=4`). In earlier,
  shorter runs, 1 KiB messages (which do not use `rep movsb`) also
  sometimes ran at about 65 instead of 123 GB/s for a whole process. That
  did not recur in the runs above, and its cause is also unknown. The
  following did not explain either effect when tested run by run: the page
  offset of the copy buffers or of the ring slots, `shm_open` memory versus
  anonymous memory, transparent huge pages, false sharing between the two
  queue handles, and consumer backoff.

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
