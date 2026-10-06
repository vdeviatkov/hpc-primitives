#include <hpc/support/platform.hpp>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#elif defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#endif

namespace hpc::support {

bool pin_current_thread(unsigned cpu) noexcept
{
#if defined(__linux__)
    if (cpu >= CPU_SETSIZE) return false;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return ::pthread_setaffinity_np(::pthread_self(), sizeof(set), &set) == 0;
#elif defined(_WIN32)
    if (cpu >= sizeof(DWORD_PTR) * 8) return false;
    return ::SetThreadAffinityMask(::GetCurrentThread(), DWORD_PTR{1} << cpu) != 0;
#else
    (void)cpu;
    return false;
#endif
}

} // namespace hpc::support
