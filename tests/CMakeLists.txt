add_executable(hpc_tests
    test_vector.cpp
    test_deque.cpp
    test_queue.cpp
    test_fixed_queue.cpp
    test_fixed_stack.cpp
    test_spsc_queue.cpp
    test_mpmc_queue.cpp
    test_spsc_unbounded_queue.cpp
    test_ttas_spinlock.cpp
    test_arena.cpp
    test_pool.cpp
    test_huge_pages.cpp
)

if(HPC_HAS_NUMA)
    target_sources(hpc_tests PRIVATE test_numa_arena.cpp)
endif()

if(NOT WIN32)
    target_sources(hpc_tests PRIVATE test_shm_spsc_queue.cpp)
endif()

target_link_libraries(hpc_tests PRIVATE hpc::primitives GTest::gtest_main)
hpc_configure_target(hpc_tests)

include(GoogleTest)
gtest_discover_tests(hpc_tests DISCOVERY_TIMEOUT 30)
