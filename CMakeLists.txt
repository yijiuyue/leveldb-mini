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
  src/bloom.cpp
  src/table.cpp
  src/db.cpp
)

target_include_directories(ldb PUBLIC include src)

foreach(t util wal memtable block bloom table db)
  add_executable(test_${t} tests/test_${t}.cpp)
  target_link_libraries(test_${t} ldb)
endforeach()

# 崩溃恢复测试的被测程序（不由 ctest 驱动，由 tests/run_crash_test.sh 驱动）
add_executable(crash_test tests/crash_test.cpp)
target_link_libraries(crash_test ldb)

enable_testing()
foreach(t util wal memtable block bloom table db)
  add_test(NAME ${t} COMMAND test_${t})
endforeach()