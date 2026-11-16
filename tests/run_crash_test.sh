#!/bin/bash
# 100 次随机时刻 kill -9，验证「已确认（sync=true 返回 OK）的写入不会丢」
#
# 判定标准：
#   每一轮 writer 打印出的最后一个序号 = 已确认写入的条数 acked
#   杀了之后 verify 恢复出的条数 recovered
#   必须 recovered >= acked（可以多，因为可能有未确认但已落盘的）

DIR=/tmp/ldb_crash
BIN=./build/crash_test
OUT=/tmp/crash_writer_out.txt
ROUNDS=${1:-100}

fail=0
lost=0

echo "=== kill -9 崩溃恢复测试（$ROUNDS 轮）==="
for round in $(seq 1 $ROUNDS); do
    rm -rf "$DIR"

    # 启动 writer，stdout 写到文件
    "$BIN" writer "$DIR" > "$OUT" 2>/dev/null &
    pid=$!

    # 随机等 20–220 ms
    ms=$(( (RANDOM % 200) + 20 ))
    sleep "$(printf "0.%03d" $ms)"

    kill -9 $pid 2>/dev/null
    wait $pid 2>/dev/null

    # 最后一个完整确认的序号
    last=$(grep -E '^[0-9]+$' "$OUT" | tail -1)
    if [ -z "$last" ]; then
        acked=0
    else
        acked=$((last + 1))
    fi

    # 恢复并检查
    res=$("$BIN" verify "$DIR" 2>/dev/null)
    recovered=$(echo "$res" | awk '/^RECOVERED/{print $2}')
    gap=$(echo "$res" | awk '/^GAP_TAIL/{print $2}')

    if [ -z "$recovered" ] || [ "$recovered" -lt 0 ] 2>/dev/null; then
        echo "round $round: OPEN FAILED"
        fail=$((fail+1))
        continue
    fi

    if [ "$gap" != "0" ]; then
        echo "round $round: GAP DETECTED (tail=$gap)  acked=$acked recovered=$recovered"
        fail=$((fail+1))
        continue
    fi

    if [ "$recovered" -lt "$acked" ]; then
        echo "round $round: LOST DATA  acked=$acked recovered=$recovered  <-- LOST $((acked-recovered))"
        lost=$((lost+1))
        fail=$((fail+1))
        continue
    fi

    printf "round %3d: acked=%5d recovered=%5d  OK\n" "$round" "$acked" "$recovered"
done

echo "============================================"
if [ $fail -eq 0 ]; then
    echo "全部 $ROUNDS 轮通过：已确认的写入一条没丢，且没有数据空洞"
    exit 0
else
    echo "失败 $fail 轮（其中丢数据 $lost 轮）"
    exit 1
fi