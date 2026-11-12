cmake_minimum_required(VERSION 3.10)
project(leveldb_mini CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

add_compile_options(-Wall -Wextra -Wno-unused-parameter)

add_library(ldb
  src/util/coding.cpp
  src/util/crc32c.cpp
  src/wal.cpp
)

target_include_directories(ldb PUBLIC include src)

# 工具层测试
add_executable(test_util tests/test_util.cpp)
target_link_libraries(test_util ldb)

# WAL 测试
add_executable(test_wal tests/test_wal.cpp)
target_link_libraries(test_wal ldb)

enable_testing()
add_test(NAME util COMMAND test_util)
add_test(NAME wal COMMAND test_wal)