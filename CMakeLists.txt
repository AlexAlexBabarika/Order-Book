cmake_minimum_required(VERSION 3.25)
project(lob LANGUAGES CXX)

enable_testing()

include(FetchContent)

FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_SHALLOW TRUE
  GIT_TAG        v1.18.0
)

set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  benchmark
  GIT_REPOSITORY https://github.com/google/benchmark.git
  GIT_SHALLOW TRUE
  GIT_TAG        v1.9.5
)
FetchContent_MakeAvailable(googletest benchmark)

add_library(lob src/OrderBook.cpp src/lob_version.cpp src/CoreTypes.cpp)
target_compile_features(lob PUBLIC cxx_std_20)
target_include_directories(lob PUBLIC include)

add_library(lob_workload STATIC bench/Workload.cpp)
target_include_directories(lob_workload PUBLIC bench)
target_link_libraries(lob_workload PUBLIC lob)

add_executable(lob_app app/main.cpp)
target_link_libraries(lob_app PRIVATE lob)

add_executable(lob_tests tests/order_book_test.cpp tests/workload_test.cpp)
target_link_libraries(lob_tests PRIVATE lob_workload GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(lob_tests)

# Its own executable, because it replaces the global operator new
add_executable(lob_no_alloc_test tests/no_alloc_test.cpp)
target_link_libraries(lob_no_alloc_test PRIVATE lob_workload GTest::gtest_main)
add_test(NAME NoAllocationAfterWarmUp COMMAND lob_no_alloc_test)
# The hot path still allocates. Remove WILL_FAIL once it stops (readme, "Order of work", step 4)
set_tests_properties(NoAllocationAfterWarmUp PROPERTIES WILL_FAIL TRUE)

# Only the release preset gives meaningful numbers
add_executable(lob_bench bench/order_book_bench.cpp)
target_link_libraries(lob_bench PRIVATE lob_workload benchmark::benchmark_main)

