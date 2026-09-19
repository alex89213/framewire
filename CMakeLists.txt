#
# Description: Build definition for framewire, producing the core library, the
#   producer and consumer binaries, the stress harness, the mock mpv server and
#   the unit tests.
# Author: Alex Wu
# Dependencies: CMake 3.16 or newer, a C++20 compiler
# Usage: cmake -S . -B build && cmake --build build -j
#

cmake_minimum_required(VERSION 3.16)
project(framewire VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "build type" FORCE)
endif()

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(WARNING "framewire targets Linux, other platforms are not supported yet")
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(FRAMEWIRE_BUILD_TESTS "Build the unit tests" ON)
option(FRAMEWIRE_WERROR "Treat warnings as errors" OFF)
option(FRAMEWIRE_NATIVE "Tune for the building machine" OFF)

add_library(framewire_flags INTERFACE)
target_compile_options(framewire_flags INTERFACE
  -Wall
  -Wextra
  -Wpedantic
  -Wshadow
  -Wconversion
  -Wsign-conversion
  -Wcast-qual
  -Wnon-virtual-dtor
  -Wdouble-promotion
)
if(FRAMEWIRE_WERROR)
  target_compile_options(framewire_flags INTERFACE -Werror)
endif()
if(FRAMEWIRE_NATIVE)
  # not on by default, a binary tuned for one machine is a bad default to ship
  target_compile_options(framewire_flags INTERFACE -march=native)
endif()

add_library(framewire_core STATIC
  src/shm.cpp
  src/spsc_ring.cpp
  src/json.cpp
  src/histogram.cpp
  src/ipc_client.cpp
  src/stats.cpp
  src/term.cpp
  src/dashboard.cpp
)
target_include_directories(framewire_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(framewire_core PRIVATE framewire_flags)

# shm_open lives in librt on older glibc and moved into libc on newer ones, so
# the library is linked only when it is actually present
include(CheckLibraryExists)
check_library_exists(rt shm_open "" HAVE_LIBRT)
if(HAVE_LIBRT)
  target_link_libraries(framewire_core PUBLIC rt)
endif()

find_package(Threads REQUIRED)
target_link_libraries(framewire_core PUBLIC Threads::Threads)

# the consumer, which is the binary a user runs directly
add_executable(framewire src/consumer_main.cpp)
target_link_libraries(framewire PRIVATE framewire_core framewire_flags)

add_executable(framewire-producer src/producer_main.cpp)
target_link_libraries(framewire-producer PRIVATE framewire_core framewire_flags)

add_executable(framewire-stress src/stress_main.cpp)
target_link_libraries(framewire-stress PRIVATE framewire_core framewire_flags)

add_executable(framewire-mock-mpv src/mock_mpv_main.cpp)
target_link_libraries(framewire-mock-mpv PRIVATE framewire_core framewire_flags)

install(TARGETS framewire framewire-producer framewire-stress framewire-mock-mpv
        RUNTIME DESTINATION bin)

if(FRAMEWIRE_BUILD_TESTS)
  enable_testing()

  foreach(suite ring json histogram stats)
    add_executable(test_${suite} tests/test_${suite}.cpp)
    target_link_libraries(test_${suite} PRIVATE framewire_core framewire_flags)
    target_include_directories(test_${suite} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    add_test(NAME ${suite} COMMAND test_${suite})
  endforeach()

  # a short stress run in the test suite, so the ring is exercised across real
  # processes on every build rather than only when run by hand
  add_test(NAME stress_smoke
           COMMAND framewire-stress --records 1000000 --capacity 1024
                   --shm /framewire-ctest --quiet)
  set_tests_properties(stress_smoke PROPERTIES TIMEOUT 120)
endif()
