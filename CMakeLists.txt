cmake_minimum_required(VERSION 3.25)
project(lob LANGUAGES CXX)

add_library(lob src/lob_version.cpp src/CoreTypes.cpp)
target_include_directories(lob PUBLIC include)

add_executable(lob_app app/main.cpp)
target_link_libraries(lob_app PRIVATE lob)

target_compile_features(lob PUBLIC cxx_std_20)