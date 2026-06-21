cmake_minimum_required(VERSION 3.14)
project(ThreadsafeQueueLib)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
enable_testing()

find_package(Threads REQUIRED)
find_package(GTest REQUIRED)
include_directories(include)

# ---------------------------------------------------------------------------
# Sanitizer options. Only one of TSAN / ASAN can be active per build.
#   cmake -DENABLE_TSAN=ON ..   -> ThreadSanitizer  (data races)
#   cmake -DENABLE_ASAN=ON ..   -> AddressSanitizer (UAF, leaks, OOB)
#   cmake -DENABLE_UBSAN=ON ..  -> UndefinedBehaviorSanitizer
# Sanitizer builds use -O1 -g -fno-omit-frame-pointer for usable stacks.
# ---------------------------------------------------------------------------
option(ENABLE_TSAN  "Enable ThreadSanitizer"             OFF)
option(ENABLE_ASAN  "Enable AddressSanitizer"            OFF)
option(ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer"  OFF)

if(ENABLE_TSAN AND ENABLE_ASAN)
    message(FATAL_ERROR "TSAN and ASAN cannot be enabled together; pick one")
endif()

if(ENABLE_TSAN)
    message(STATUS "ThreadSanitizer enabled")
    add_compile_options(-fsanitize=thread -O1 -g -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
    add_compile_definitions(TSFQUEUE_TSAN_BUILD=1)
endif()

if(ENABLE_ASAN)
    message(STATUS "AddressSanitizer enabled")
    add_compile_options(-fsanitize=address -O1 -g -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
endif()

if(ENABLE_UBSAN)
    message(STATUS "UndefinedBehaviorSanitizer enabled")
    add_compile_options(-fsanitize=undefined -O1 -g -fno-omit-frame-pointer)
    add_link_options(-fsanitize=undefined)
endif()

# ---------------------------------------------------------------------------
# 128-bit atomics (used by lockfree_mpmc_bounded packed entries).
# On x86_64, -mcx16 lets the compiler emit `cmpxchg16b` inline instead of
# generating __sync_*_16 libcalls that require linking libatomic.
# ---------------------------------------------------------------------------
if(CMAKE_SYSTEM_PROCESSOR MATCHES "(x86_64|AMD64)")
    add_compile_options(-mcx16)
endif()

# SPSC tests
add_executable(test_spsc tests/test_spsc.cpp)
target_link_libraries(test_spsc GTest::GTest GTest::Main Threads::Threads)

# MPMC unbounded blocking tests
add_executable(test_mpmc_unbounded_blocking tests/test_mpmc_unbounded_blocking.cpp)
target_link_libraries(test_mpmc_unbounded_blocking GTest::GTest GTest::Main Threads::Threads)

# MPMC bounded lockfree tests
add_executable(test_mpmc_bounded_lockfree tests/test_mpmc_bounded_lockfree.cpp)
target_link_libraries(test_mpmc_bounded_lockfree GTest::GTest GTest::Main Threads::Threads)

# Register tests
add_test(NAME SPSC COMMAND test_spsc)
add_test(NAME MPMC_UNBOUNDED_BLOCKING COMMAND test_mpmc_unbounded_blocking)
add_test(NAME MPMC_BOUNDED_LOCKFREE COMMAND test_mpmc_bounded_lockfree)
