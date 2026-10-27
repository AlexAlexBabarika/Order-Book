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
FetchContent_MakeAvailable(googletest)

add_library(lob src/OrderBook.cpp src/lob_version.cpp src/CoreTypes.cpp)
target_compile_features(lob PUBLIC cxx_std_20)
target_include_directories(lob PUBLIC include)

add_executable(lob_app app/main.cpp)
target_link_libraries(lob_app PRIVATE lob)

add_executable(lob_tests tests/order_book_test.cpp)
target_link_libraries(lob_tests PRIVATE lob GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(lob_tests)

