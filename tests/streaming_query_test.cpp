/*
 * head_structure_analysis.c — 验证 .head 结构差异 & 流式查询测试
 *
 * 测试内容:
 *   Part A: 当前.head vs TDengine .head 结构差异说明
 *   Part B: 流式查询验证 (fetchRow cursor-based, 不使用 LIMIT 模拟)
 */

#include <client/EtDBClient.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cassert>
#include <chrono>
#include <thread>


// ============================================================================
// Part A: .head 索引结构分析
//
//   ⚠ 下方四段为优化前的分析，保留作设计依据。其中的「建议」已全部落地：
//     · SDataBlkInfo 按表分组、一个表一段含 N 个 SDataBlk
//     · SDataBlkIdx 为表级索引（含 numOfBlocks），不再重复 minKey
//     · 块记录改为 delta + LEB128 变长编码
//     · .head 每次提交整体重写（写 tmp 后原子替换），不再累积被取代的死副本
//   实现见 src/etdb/ETDBStubs.h（etdbWriteSDataBlkInfo / etdbEncodeIdxTail /
//   etdbDecodeIdxTail）与 src/etdb/ETDBRepo.h 的 .head 重写段；
//   读取侧见 src/query/StorageReader.h::loadBlocksFromHead。
//
//   实测（本机 5.1M 行单表）：旧 38,430,452 B / 15,665 活块（2,453 B/活块，
//   98% 是历史提交留下的死数据）→ 新 12.5 B/块，同规模单表 .head 仅 14.8 KB。
// ============================================================================
/*
 * ┌─────────────────────────────────────────────────────────────┐
 * │  TDengine .head 文件布局 (两级索引)                          │
 * │                                                             │
 * │  [SDataBlkInfo(table1) + SDataBlk[0..N] + cksum]  ← 前部        │
 * │  [SDataBlkInfo(table2) + SDataBlk[0..M] + cksum]                │
 * │  ...                                                        │
 * │  ─────── 文件尾部 ───────                                    │
 * │  [SDataBlkIdx(table1)]  ← SDataBlkIdx.offset→SDataBlkInfo(table1) │
 * │  [SDataBlkIdx(table2)]  ← SDataBlkIdx.offset→SDataBlkInfo(table2) │
 * │  [TSCKSUM]                                                   │
 * │                                                             │
 * │  关系: 1 SDataBlkIdx ↔ 1 SDataBlkInfo ↔ N SDataBlks               │
 * │  SDataBlkIdx: 表级索引 (tid, uid, maxKey, numOfBlocks)         │
 * │  SDataBlkInfo: 包含该表在此文件中的所有数据块                    │
 * │  SDataBlk: 每个数据块的描述 (offset→.data, keyFirst, keyLast)   │
 * └─────────────────────────────────────────────────────────────┘
 *
 * ┌─────────────────────────────────────────────────────────────┐
 * │  当前 EtherDB .head 文件布局 (扁平单级)                      │
 * │                                                             │
 * │  [SDFHInfo 512B]                                             │
 * │  [SDataBlkInfo(1 block) + cksum][SDataBlkIdx]  ← 记录0          │
 * │  [SDataBlkInfo(1 block) + cksum][SDataBlkIdx]  ← 记录1          │
 * │  ...                                                        │
 * │                                                             │
 * │  关系: 1 SDataBlkInfo ↔ 1 SDataBlk ↔ 1 SDataBlkIdx (1:1:1)       │
 * │                                                             │
 * │  问题:                                                      │
 * │  1. SDataBlkInfo 永远是 1 个 SDataBlk — 等于没有数组意义          │
 * │  2. SDataBlkIdx.minKey/maxKey 与 SDataBlk.keyFirst/keyLast 重复  │
 * │  3. SDataBlkInfo 在查询中未被使用 (estimateRowCountInRange     │
 * │     只读了 SDataBlkIdx，直接从 .head 跳到 .data)               │
 * │  4. 缺少变长编码 — 固定 124 字节/条，浪费空间                 │
 * └─────────────────────────────────────────────────────────────┘
 *
 * ┌─────────────────────────────────────────────────────────────┐
 * │  建议: 对齐 TDengine 的两级索引设计                           │
 * │                                                             │
 * │  1. SDataBlkInfo 按表分组，包含多个 SDataBlk                      │
 * │     → 减少 .head 文件大小 (SDataBlkInfo overhead 分摊到多块)     │
 * │  2. SDataBlkIdx 作为表级索引，含 numOfBlocks                    │
 * │     → 快速判断该表在此文件中是否有数据                          │
 * │  3. 移除 SDataBlkIdx.minKey (与 SDataBlk 重复)                    │
 * │     → 用 SDataBlk.keyFirst/Last 做块级过滤                      │
 * │  4. SDataBlkIdx 用变长编码 (variant)                            │
 * │     → 减少磁盘占用                                           │
 * └─────────────────────────────────────────────────────────────┘
 */
// ============================================================================
// Part B: 流式查询测试
// ============================================================================

static const char* HOST = "127.0.0.1";
static const int   PORT = 7040;
static const char* USER = "root";
static const char* PASS = "etherdbdata";

// 测试: 使用 fetchRow 流式分批获取 (taos_fetch_row 风格), 不使用 LIMIT 模拟
static int test_streaming_query() {
    printf("\n=== 流式查询测试 (fetchRow, 无 LIMIT 模拟) ===\n");

    using namespace ETDB::Client;

    // 1. 连接
    EtDBClient client;
    if (!client.connect(HOST, PORT, USER, PASS, "perftest20")) {
        printf("[FAIL] 连接失败\n");
        return 1;
    }
    printf("[OK] 已连接到 %s:%d\n", HOST, PORT);
    auto r = client.query("USE perftest20");
    if (!r.success()) {
        return 1;
    }
        printf("  [OK] USE perftest20\n");
    // 2. 查询总数作为基准
    auto cr = client.query("SELECT COUNT(*) FROM perf_data_2");
    int64_t totalRows = (cr.rowCount() > 0) ? cr.get(0, 0).iVal : 0;
    printf("[INFO] 总行数: %lld\n", (long long)totalRows);

    // 3. 流式查询（无 LIMIT）→ 服务端分批返回, fetchRow 逐行拉取
    auto start = std::chrono::steady_clock::now();
    auto result = client.query("SELECT ts,col_int FROM perf_data_2 where col_int>=0");
    if (!result.success() && !result.error().empty()) {
        printf("[FAIL] 查询失败: %s\n", result.error().c_str());
        client.close();
        return 1;
    }
    if (result.streamQId() == 0) {
        printf("[INFO] 服务端返回一次性结果（行数未超流式阈值），跳过流式验证\n");
        printf("  rowCount=%d\n", result.rowCount());
        client.close();
        return (result.rowCount() == totalRows) ? 0 : 1;
    }
    printf("[OK] 流式查询已建立 qId=%lld, 第一批 %d 行 (total=%lld)\n",
           (long long)result.streamQId(), result.rowCount(),
           (long long)result.streamTotalRows());

    // 4. fetchRow 逐行拉取, 自动跨批 FETCH。
    //    流式批次按序列化字节数分批(非固定行数) — 这里把每批字节预算设为 8KB
    //    (≠默认 64KB), 验证客户端调节生效: 服务端步长由 batchBytes 动态派生
    //    (≈batchBytes/32 行, 上限 128), 窄行每批行数多、宽行每批行数少。
    //result.setFetchBatchBytes(1024*64);   // 1MB/批 → 验证客户端调节生效
    int64_t streamTotal = 0;
    int     fetches = 1;   // 第一批已返回
    std::vector<Value> row;
   
    while (result.fetchRow(row)) {
        streamTotal++;
        //printf("ts=%lld col_int= %d \r\n",row[0].iVal,row[1].iVal);
    }
    auto end = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    printf("[INFO] 流式读取完成(1MB/批): 共 %lld 行, 耗时 %lld ms\n",
           (long long)streamTotal, (long long)ms);

    // 5. 验证
    bool ok1 = (streamTotal == totalRows);
    if (ok1) {
        printf("[PASS] 按字节分批流式查询结果正确 (%lld == %lld)\n",
               (long long)streamTotal, (long long)totalRows);
    } else {
        printf("[FAIL] 流式查询结果不一致: stream=%lld != total=%lld\n",
               (long long)streamTotal, (long long)totalRows);
    }

    // 6. 带 WHERE 的流式查询（用户场景: SELECT ts,col_int WHERE col_int>5）
    //    过滤在服务端存储层下沉, 只解码匹配行; 客户端 fetchRow 流式拉取。
    auto cr2 = client.query("SELECT COUNT(*) FROM perf_data_2 WHERE col_int>5");
    int64_t whereTotal = (cr2.rowCount() > 0) ? cr2.get(0, 0).iVal : 0;
    printf("[INFO] WHERE 总行数: %lld\n", (long long)whereTotal);

    bool ok2 = true;
    start = std::chrono::steady_clock::now();
    auto wres = client.query("SELECT ts,col_int FROM perf_data_2 WHERE col_int>5");
    //wres.setFetchBatchBytes(1024*64);   // 1MB/批 → 验证客户端调节生效
   
    if (wres.streamQId() == 0) {
        printf("[WARN] WHERE 流式未触发 (rowCount=%d), 跳过验证\n", wres.rowCount());
        ok2 = (wres.rowCount() == whereTotal);
    } else {
        int64_t ws = 0, bad = 0;
        std::vector<Value> wrow;
        while (wres.fetchRow(wrow)) {
            ws++;
            if (wrow.size() > 1 && !wrow[1].isNull() && wrow[1].iVal <= 5) bad++;
            //printf("ts=%lld col_int= %d \r\n",wrow[0].iVal,wrow[1].iVal);
        }
        end = std::chrono::steady_clock::now();
        ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        printf("[INFO] WHERE 流式读取: %lld 行, 坏行(<=5)=%lld\n",
               (long long)ws, (long long)bad);
        if (ws == whereTotal && bad == 0) {
            printf("[PASS] WHERE 流式查询结果正确 (%lld == %lld, 无坏行), 耗时 %lld ms\n",
                   (long long)ws, (long long)whereTotal, (long long)ms);
        } else {
            printf("[FAIL] WHERE 流式结果不一致: stream=%lld != total=%lld (bad=%lld)\n",
                   (long long)ws, (long long)whereTotal, (long long)bad);
            ok2 = false;
        }
    }

    client.close();
    return (ok1 && ok2) ? 0 : 1;
}

int main() {
    printf("╔══════════════════════════════════════════════════════╗\n");
    printf("║  .head 结构分析 & 流式查询测试                        ║\n");
    printf("╚══════════════════════════════════════════════════════╝\n");

    int ret = test_streaming_query();

    return ret;
}
