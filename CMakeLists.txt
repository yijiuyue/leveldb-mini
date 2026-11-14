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
  src/skiplist.cpp
  src/memtable.cpp
)

target_include_directories(ldb PUBLIC include src)

add_executable(test_util tests/test_util.cpp)
target_link_libraries(test_util ldb)

add_executable(test_wal tests/test_wal.cpp)
target_link_libraries(test_wal ldb)

add_executable(test_memtable tests/test_memtable.cpp)
target_link_libraries(test_memtable ldb)

enable_testing()
add_test(NAME util COMMAND test_util)
add_test(NAME wal COMMAND test_wal)
add_test(NAME memtable COMMAND test_memtable)