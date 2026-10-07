#pragma once

#include <cstddef>

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#endif

namespace hpc::support {

// Padding unit for data written by different threads. 128 bytes on x86-64 and
// AArch64: Intel's adjacent-line prefetcher pulls 64-byte lines in pairs, and
// Apple M-series cores use 128-byte lines.
#if defined(__x86_64__) || defined(_M_X64) || defined(__aarch64__) || defined(_M_ARM64)
inline constexpr std::size_t cache_line_size = 128;
#else
inline constexpr std::size_t cache_line_size = 64;
#endif

// Spin-wait hint. Reduces power and frees pipeline resources for a sibling
// hyperthread while busy-waiting.
inline void cpu_relax() noexcept
{
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    _mm_pause();
#elif defined(__aarch64__)
    asm volatile("yield" ::: "memory");
#elif defined(_M_ARM64)
    __yield();
#endif
}

// Pins the calling thread to a logical CPU, so the scheduler cannot migrate it
// and its caches stay warm. Linux uses pthread_setaffinity_np with a one-CPU
// mask, and Windows uses SetThreadAffinityMask. macOS has no hard affinity
// API, so the call does nothing there. Returns false on failure or when
// pinning is not supported.
bool pin_current_thread(unsigned cpu) noexcept;

} // namespace hpc::support
