#!/bin/bash
# 跑全部单元测试
set -e
cd "$(dirname "$0")"

if [ ! -d build ]; then
  echo "请先构建：mkdir build && cd build && cmake .. && make"
  exit 1
fi

fail=0
for t in util wal memtable block bloom table db; do
  printf "%-10s " "$t"
  if ! ./build/test_$t | tail -1; then
    fail=$((fail+1))
  fi
done

echo ""
if [ $fail -eq 0 ]; then
  echo "全部单元测试通过"
else
  echo "$fail 组测试失败"
  exit 1
fi