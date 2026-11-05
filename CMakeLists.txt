cmake_minimum_required(VERSION 3.14)
project(midi2jianpu CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

include_directories(${CMAKE_SOURCE_DIR}/include)

# 主程序
add_executable(midi2jianpu
  src/main.cpp
  src/midifile.cpp
  src/notation.cpp
  src/html_writer.cpp
)

# 单元测试
enable_testing()
add_executable(test_basic tests/test_basic.cpp src/midifile.cpp src/notation.cpp)
add_test(NAME basic COMMAND test_basic)
