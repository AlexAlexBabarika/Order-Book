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
# Only the feed replay uses it, for the index from a feed's order ids to the book's
FetchContent_Declare(
  unordered_dense
  GIT_REPOSITORY https://github.com/martinus/unordered_dense.git
  GIT_SHALLOW TRUE
  GIT_TAG        v5.3.2
  SYSTEM
)
FetchContent_MakeAvailable(googletest benchmark unordered_dense)

# Link-time optimisation, so code in OrderBook.cpp can be inlined into the header templates
# (step 16). Set here, after GoogleTest and Google Benchmark are made, so only this project's
# targets get it and the harness stays as it was. CMake's default for Clang is THIN
set(LOB_LTO OFF CACHE STRING "Link-time optimisation of this project's targets: OFF, THIN or FULL")
set_property(CACHE LOB_LTO PROPERTY STRINGS OFF THIN FULL)
if(NOT LOB_LTO STREQUAL "OFF")
  include(CheckIPOSupported)
  check_ipo_supported(RESULT lob_lto_supported OUTPUT lob_lto_error LANGUAGES CXX)
  if(NOT lob_lto_supported)
    message(FATAL_ERROR "LOB_LTO=${LOB_LTO}, but this toolchain can't do LTO: ${lob_lto_error}")
  endif()
  if(LOB_LTO STREQUAL "FULL" AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(CMAKE_CXX_COMPILE_OPTIONS_IPO -flto=full)
  elseif(NOT LOB_LTO STREQUAL "THIN")
    message(FATAL_ERROR "LOB_LTO must be OFF, THIN or FULL (FULL needs Clang), not ${LOB_LTO}")
  endif()
  set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
endif()

add_library(lob src/OrderBook.cpp src/lob_version.cpp src/CoreTypes.cpp)
target_compile_features(lob PUBLIC cxx_std_20)
target_include_directories(lob PUBLIC include)

option(LOB_PRICE_LADDER "Hold price levels in PriceLadder, not PriceArray" OFF)
if(LOB_PRICE_LADDER)
  target_compile_definitions(lob PUBLIC LOB_PRICE_LADDER)
endif()

add_library(lob_workload STATIC bench/Workload.cpp)
target_include_directories(lob_workload PUBLIC bench)
target_link_libraries(lob_workload PUBLIC lob)

add_library(lob_feed STATIC feed/Lobster.cpp feed/FeedBook.cpp feed/Checker.cpp)
target_include_directories(lob_feed PUBLIC feed)
target_link_libraries(lob_feed PUBLIC lob unordered_dense::unordered_dense)

add_executable(lob_app app/main.cpp)
target_link_libraries(lob_app PRIVATE lob_feed)

add_executable(lob_tests tests/order_book_test.cpp tests/ladder_test.cpp tests/workload_test.cpp
  tests/feed_test.cpp)
target_link_libraries(lob_tests PRIVATE lob_workload lob_feed GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(lob_tests)

# Its own executable, because it replaces the global operator new
add_executable(lob_no_alloc_test tests/no_alloc_test.cpp)
target_link_libraries(lob_no_alloc_test PRIVATE lob_workload lob_feed GTest::gtest_main)
add_test(NAME NoAllocationAfterWarmUp COMMAND lob_no_alloc_test)

# Only the release preset gives meaningful numbers
add_executable(lob_bench bench/order_book_bench.cpp)
target_link_libraries(lob_bench PRIVATE lob_workload benchmark::benchmark_main)

# With LTO, Apple's linker compiles the bitcode into temporary objects and deletes them after
# the link, and their debug info goes with them, so dsymutil and atos can't see the book's
# code. Keep them, one path per executable: ThinLTO writes a folder of objects there
if(APPLE AND NOT LOB_LTO STREQUAL "OFF")
  foreach(exe lob_app lob_tests lob_no_alloc_test lob_bench)
    target_link_options(${exe} PRIVATE
      "LINKER:-object_path_lto,${CMAKE_CURRENT_BINARY_DIR}/lto-${LOB_LTO}-${exe}")
  endforeach()
endif()

