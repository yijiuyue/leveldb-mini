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
  src/block.cpp
)

target_include_directories(ldb PUBLIC include src)

foreach(t util wal memtable block)
  add_executable(test_${t} tests/test_${t}.cpp)
  target_link_libraries(test_${t} ldb)
endforeach()

enable_testing()
add_test(NAME util COMMAND test_util)
add_test(NAME wal COMMAND test_wal)
add_test(NAME memtable COMMAND test_memtable)
add_test(NAME block COMMAND test_block)