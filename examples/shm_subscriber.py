#!/usr/bin/env python3
"""Consumes messages from the C++ shm_publisher example (standard library only).

Layout matches hpc::ipc::shm_queue_header (include/hpc/ipc/shm_spsc_queue.hpp).

Caveat: CPython gives no memory-ordering guarantees, so the acquire/release
pairing of the C++ side is approximated by aligned 8-byte loads and stores,
which are atomic on x86-64 and AArch64. Fine for a demo; a production
consumer should do the index updates in native code.
"""
import signal
import struct
import sys
import time
from multiprocessing import shared_memory

NAME = "hpc_demo_queue"  # Python prepends the leading "/"
MAGIC = struct.unpack("<Q", b"HPCSPSQ1")[0]
U64 = struct.Struct("<Q")
TAIL_OFFSET, HEAD_OFFSET, SLOTS_OFFSET = 128, 256, 384
MESSAGE = struct.Struct("<QQ48s")  # seq, timestamp_ns, payload[48]


def attach(name: str) -> shared_memory.SharedMemory:
    try:
        return shared_memory.SharedMemory(name=name, track=False)  # Python >= 3.13
    except TypeError:
        shm = shared_memory.SharedMemory(name=name)
        # Older versions register attached segments for cleanup and would
        # unlink the publisher's segment on exit.
        from multiprocessing import resource_tracker
        resource_tracker.unregister(shm._name, "shared_memory")  # noqa: SLF001
        return shm


def main() -> int:
    try:
        shm = attach(NAME)
    except FileNotFoundError:
        print(f"/{NAME} not found; start ./build/shm_publisher first")
        return 1

    buf = shm.buf
    magic, capacity, slot_size = struct.unpack_from("<QQQ", buf, 0)
    if magic != MAGIC or slot_size != MESSAGE.size:
        print("incompatible queue layout")
        return 1

    stop = False

    def on_signal(*_):
        nonlocal stop
        stop = True

    signal.signal(signal.SIGINT, on_signal)

    head = U64.unpack_from(buf, HEAD_OFFSET)[0]
    while not stop:
        tail = U64.unpack_from(buf, TAIL_OFFSET)[0]
        if head == tail:
            time.sleep(0.001)
            continue
        while head != tail:
            offset = SLOTS_OFFSET + (head & (capacity - 1)) * MESSAGE.size
            seq, ts_ns, _payload = MESSAGE.unpack_from(buf, offset)
            head += 1
            U64.pack_into(buf, HEAD_OFFSET, head)
            latency_us = (time.time_ns() - ts_ns) / 1e3
            print(f"seq={seq} latency={latency_us:.0f}us")

    del buf
    shm.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
