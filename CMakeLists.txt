cmake_minimum_required(VERSION 3.20)

project(HighPerformanceMPMC LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MPMC_ENABLE_NATIVE "Enable -march=native for local tuning" ON)

if(MPMC_ENABLE_NATIVE)
  add_compile_options(-march=native)
endif()

find_package(Threads REQUIRED)

add_library(mpmc_queue INTERFACE)
target_include_directories(mpmc_queue INTERFACE include)
target_link_libraries(mpmc_queue INTERFACE Threads::Threads)

add_executable(mpmc_tests tests/test_mpmc.cpp)
target_link_libraries(mpmc_tests PRIVATE mpmc_queue)

add_executable(bench_mpmc bench/bench_mpmc.cpp)
target_link_libraries(bench_mpmc PRIVATE mpmc_queue)

enable_testing()
add_test(NAME mpmc_tests COMMAND mpmc_tests)
