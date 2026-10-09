#pragma once

#include <benchmark/benchmark.h>

#include <type_traits>

namespace bench {

// Reads an element's value and keeps that read from being optimized away,
// the same way for every container.
//
// benchmark::DoNotOptimize(x) uses an asm operand with the constraint
// "+m,r", which lets the compiler choose between keeping x in memory and in
// a register. GCC chose differently for different containers in otherwise
// identical loops: for one it reloaded the element and stored it back, or
// spilled the value to the stack, on every iteration; for the other it
// emitted nothing. That alone made hpc::deque and hpc::queue look 10-13%
// slower than libstdc++'s containers when they were 1.2-1.45x faster.
//
// Here the value is copied and handed to an empty asm statement as a
// register-only input, so every container gets the same single load. The
// "memory" clobber keeps the compiler from caching container state across
// iterations, as DoNotOptimize does.
template <class T>
inline void observe(const T& element)
{
    static_assert(std::is_trivially_copyable_v<T>, "observe() is for cheap-to-copy element types");
    T value = element;
#if defined(__GNUC__) || defined(__clang__)
    asm volatile("" : : "r"(value) : "memory");
#else
    benchmark::DoNotOptimize(value); // MSVC: no inline asm on x64
#endif
}

// Returns the container through a pointer the compiler cannot see through,
// so the benchmark loop reaches its fields through a register, as code that
// holds a container as a member or by reference does.
//
// A container declared as a local in the benchmark function lives on its
// stack frame, and GCC then addresses its fields relative to the stack
// pointer. Loops that store a field and reload it next iteration (push and
// pop with a memory clobber in between) then ran at speeds that depended on
// the compiler's frame layout rather than on the container: hpc::queue push +
// pop measured 0.71x of std::queue that way on a Ryzen 9950X, against 1.34x
// with both containers reached through a pointer in an isolated loop. (Zen
// cores can forward stack-relative store/load pairs faster than others,
// which is the likely mechanism; it was not confirmed with counters.)
template <class Container>
inline Container& opaque(Container& c)
{
    Container* p = &c;
    benchmark::DoNotOptimize(p);
    return *p;
}

} // namespace bench
