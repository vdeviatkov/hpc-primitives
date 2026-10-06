cmake_minimum_required(VERSION 3.25)
project(hpc_primitives VERSION 0.2.0 LANGUAGES CXX)

option(HPC_BUILD_TESTS      "Build unit tests"                 ${PROJECT_IS_TOP_LEVEL})
option(HPC_BUILD_BENCHMARKS "Build benchmarks"                 ${PROJECT_IS_TOP_LEVEL})
option(HPC_BUILD_EXAMPLES   "Build examples"                   ${PROJECT_IS_TOP_LEVEL})
option(HPC_ENABLE_NUMA      "Build numa_arena if libnuma found" ON)
set(HPC_SANITIZE "" CACHE STRING "Comma-separated sanitizers, e.g. address,undefined or thread")

if(PROJECT_IS_TOP_LEVEL AND NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerOptions)

find_package(Threads REQUIRED)

add_library(hpc_primitives
    src/memory/huge_pages.cpp
    src/support/platform.cpp
)
add_library(hpc::primitives ALIAS hpc_primitives)

target_compile_features(hpc_primitives PUBLIC cxx_std_23)
target_include_directories(hpc_primitives PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(hpc_primitives PUBLIC Threads::Threads)
hpc_configure_target(hpc_primitives)

# IPC: POSIX shared memory, memory-mapped files and sockets (not on Windows).
if(NOT WIN32)
    target_sources(hpc_primitives PRIVATE
        src/ipc/shm_region.cpp
        src/ipc/mapped_file.cpp
        src/ipc/socket.cpp
    )
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_link_libraries(hpc_primitives PUBLIC rt)
    endif()
endif()

# NUMA (Linux + libnuma).
set(HPC_HAS_NUMA OFF)
if(HPC_ENABLE_NUMA)
    find_path(NUMA_INCLUDE_DIR numa.h)
    find_library(NUMA_LIBRARY numa)
    if(NUMA_INCLUDE_DIR AND NUMA_LIBRARY)
        set(HPC_HAS_NUMA ON)
        target_sources(hpc_primitives PRIVATE src/memory/numa_arena.cpp)
        target_include_directories(hpc_primitives PRIVATE ${NUMA_INCLUDE_DIR})
        target_link_libraries(hpc_primitives PUBLIC ${NUMA_LIBRARY})
    else()
        message(STATUS "libnuma not found: building without numa_arena")
    endif()
endif()
target_compile_definitions(hpc_primitives PUBLIC HPC_HAS_NUMA=$<BOOL:${HPC_HAS_NUMA}>)

if(HPC_BUILD_TESTS OR HPC_BUILD_BENCHMARKS)
    include(Dependencies)
endif()

if(HPC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(HPC_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

if(HPC_BUILD_EXAMPLES AND NOT WIN32)
    add_executable(shm_publisher examples/shm_publisher.cpp)
    target_link_libraries(shm_publisher PRIVATE hpc::primitives)
    hpc_configure_target(shm_publisher)
endif()
