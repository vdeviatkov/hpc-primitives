add_executable(hpc_benchmarks
    bench_concurrent_queues.cpp
    bench_spinlock.cpp
    bench_memory.cpp
    bench_vector.cpp
    bench_deque.cpp
    bench_queue.cpp
    bench_fixed_queue.cpp
    bench_fixed_stack.cpp
)
if(NOT WIN32)
    target_sources(hpc_benchmarks PRIVATE bench_ipc.cpp)
endif()

target_link_libraries(hpc_benchmarks PRIVATE hpc::primitives benchmark::benchmark_main)
hpc_configure_target(hpc_benchmarks)

option(HPC_BENCH_NATIVE "Compile benchmarks with -march=native" ON)
if(HPC_BENCH_NATIVE AND NOT MSVC)
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag(-march=native HPC_HAS_MARCH_NATIVE)
    if(HPC_HAS_MARCH_NATIVE)
        target_compile_options(hpc_benchmarks PRIVATE -march=native)
    endif()
endif()
