#!/bin/bash
# streaming_verify.sh — 流式查询验证脚本
# 使用 etherdb shell 的 LIMIT+OFFSET 模拟流式分批读取

HOST="127.0.0.1"
PORT="7040"
USER="root"
PASS="etherdbdata"
SHELL="../build/etherdb"

echo "=== 流式查询验证 ==="

# 1. 获取总数
TOTAL=$($SHELL -h $HOST -p $PORT -u $USER -P $PASS \
    -e "select count(*) from perftest20.perf_data;" 2>&1 | \
    grep -E '^\|' | tail -1 | tr -d ' |')

echo "总行数: $TOTAL"

if [ -z "$TOTAL" ] || [ "$TOTAL" = "0" ]; then
    echo "请先运行 perf_20insert_test 插入数据"
    exit 1
fi

# 2. 分批读取 (每次 500 行)
BATCH=500
OFFSET=0
STREAM_TOTAL=0
BATCHES=0

START=$(date +%s%3N)

while [ $OFFSET -lt $TOTAL ]; do
    RESULT=$($SHELL -h $HOST -p $PORT -u $USER -P $PASS \
        -e "select count(*) from (select * from perftest20.perf_data LIMIT $BATCH OFFSET $OFFSET);" 2>&1 | \
        grep -E '^\|' | tail -1 | tr -d ' |')
    
    if [ -z "$RESULT" ]; then
        echo "[FAIL] 批次 $((BATCHES+1)) 查询失败"
        break
    fi
    
    STREAM_TOTAL=$((STREAM_TOTAL + RESULT))
    BATCHES=$((BATCHES + 1))
    
    if [ $BATCHES -le 3 ] || [ $RESULT -eq 0 ]; then
        echo "  批次 $BATCHES: $RESULT 行 (累计 $STREAM_TOTAL)"
    fi
    
    if [ "$RESULT" -lt "$BATCH" ]; then
        break
    fi
    OFFSET=$((OFFSET + BATCH))
done

END=$(date +%s%3N)
ELAPSED=$((END - START))

echo "---"
echo "流式读取: $BATCHES 批次, 共 $STREAM_TOTAL 行, 耗时 ${ELAPSED}ms"

# 3. 一次性读取对比
DIRECT_RESULT=$($SHELL -h $HOST -p $PORT -u $USER -P $PASS \
    -e "select count(*) from perftest20.perf_data;" 2>&1 | \
    grep -E '^\|' | tail -1 | tr -d ' |')

echo "直接读取: $DIRECT_RESULT 行"

# 4. 验证
if [ "$STREAM_TOTAL" = "$DIRECT_RESULT" ]; then
    echo "[PASS] 流式查询结果一致"
    exit 0
else
    echo "[FAIL] 不一致: stream=$STREAM_TOTAL != direct=$DIRECT_RESULT"
    exit 1
fi
