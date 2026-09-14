/*
 * EtherDB 异步批量插入性能测试（基于 perf_insert_test.cpp 的 10 字段表）
 *
 * 使用 EtDBStmt::bindParamBatch + EtDBStmt::executeAsync：
 *   每批数据 send 完立即返回（不等待服务端响应），服务端返回的插入结果由
 *   后台接收线程按批次 id 收集到结果向量，再通过
 *   EtDBClient::waitAsyncResult(id) / getAsyncResult(id) 获取，
 *   并像同步版一样打印每个异步批次的执行结果（affected 行数）。
 *
 *   - 10 个字段：TIMESTAMP, INT, BIGINT, FLOAT, DOUBLE, SMALLINT, TINYINT, BOOL, BIGINT, DOUBLE
 *   - 每批 10000 行，行宽 52 字节（与 perf_insert_test.cpp 一致，便于对比）
 *   - 异步：预热 2 轮 + 正式测试 100 轮
 *   - 同步基线：20 轮 execute() 对比（参考：同步版 ~1.8M rows/s）
 *
 * Usage:
 *   # 终端 1: 启动服务端
 *   ./src/bin/etherdb_dnode -p 7040
 *
 *   # 终端 2: 运行测试
 *   ./build/perf_insert_test20_1 [port, default 7040]
 */

#include <client/EtDBClient.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <chrono>
#include <thread>
#include <vector>
#include <deque>
#include <string>
#include <cmath>
#include <algorithm>

using namespace ETDB::Client;

// ============================================================================
// 测试配置
// ============================================================================
constexpr int  BATCH_SIZE    = 10000;
constexpr int  NUM_COLS      = 10;
constexpr int  WARMUP_ROUNDS = 2;
constexpr int  ASYNC_ROUNDS  = 100;   // 异步正式测试轮数
constexpr int  SYNC_ROUNDS   = 100;    // 同步基线对比轮数
constexpr int  ROW_BYTES     = 52;    // 行宽（10 字段，与 perf_insert_test.cpp 一致）
constexpr int  MAX_PENDING   = 32;    // 异步在途(未消费)批次上限：控制结果集数量（背压）

using Clock = std::chrono::system_clock;
// using Clock = std::chrono::high_resolution_clock;
using Us    = std::chrono::microseconds;

static int64_t nowUs() {

    return std::chrono::duration_cast<Us>(Clock::now().time_since_epoch()).count();
}

// ============================================================================
// 辅助：执行 SQL
// ============================================================================
static bool execSQL(EtDBClient& client, const std::string& sql) {
    auto r = client.query(sql);
    if (!r.error().empty()) {
        printf("  [ERROR] %s -> %s\n", sql.c_str(), r.error().c_str());
        return false;
    }
    return true;
}

// ============================================================================
// 主测试
// ============================================================================
int main(int argc, char* argv[]) {
    uint16_t port = (argc > 1) ? (uint16_t)atoi(argv[1]) : 7040;
    const char* host = "192.168.10.200";

    printf("------------------------------------------------------\n");
    printf("║   EtherDB asyn batch insert  performance                       ║\n");
    printf("║   fields: %-2d    batch: %-4d row                       ║\n", NUM_COLS, BATCH_SIZE);
    printf("║   warmup: %-2d    async rounds: %-3d  sync rounds: %-3d         ║\n", WARMUP_ROUNDS, ASYNC_ROUNDS, SYNC_ROUNDS);
    printf("║   server: %s:%-5d                               ║\n", host, port);
    printf("------------------------------------------------------\n");

    // =====================================================================
    // 1. 连接
    // =====================================================================
    printf("=== 1. server ===\n");

    EtDBClient client;
    if (!client.connect(host, port)) {
        printf("  [FATAL] 无法连接。请先启动: ./src/bin/etherdb_dnode -p %d\n", port);
        return 1;
    }
    printf("  [OK] connected to %s:%d\n", host, port);

    // =====================================================================
    // 2. 创建数据库和表
    // =====================================================================
    printf("\n=== 2. create database and table ===\n");

    // execSQL(client, "DROP DATABASE IF EXISTS perftest20");
     if (!execSQL(client, "CREATE DATABASE IF NOT EXISTS perftest20 KEEP 3650 REPLICA 1 PRECISION us")) return 1;
     printf("  [OK] CREATE DATABASE perftest20\n");

    if (!execSQL(client, "USE perftest20")) return 1;
    printf("  [OK] USE perftest20\n");

    const char* createSQL =
        "CREATE TABLE IF NOT EXISTS perf_data_1 ("
        "ts TIMESTAMP, "
        "col_int INT, "
        "col_bigint BIGINT, "
        "col_float FLOAT, "
        "col_double DOUBLE, "
        "col_smallint SMALLINT, "
        "col_tinyint TINYINT, "
        "col_bool BOOL, "
        "col_extra1 BIGINT, "
        "col_extra2 DOUBLE"
        ")";
    if (!execSQL(client, createSQL)) return 1;
    printf("  [OK] CREATE TABLE IF NOT EXISTS perf_data_1 (10 fields)\n");

    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // =====================================================================
    // 3. 预绑定列绑定批量插入
    // =====================================================================
    printf("\n=== 3. 预绑定(列绑定)批量插入性能测试 ===\n");

    EtDBStmt* stmt = client.createStmt();
    if (!stmt) { printf("  [FATAL] createStmt failed\n"); return 1; }

    bool ok = stmt->prepare("INSERT INTO perf_data_1 VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
    if (!ok) { printf("  [FATAL] prepare failed\n"); delete stmt; return 1; }
    printf("  [OK] Prepared: INSERT INTO perf_data_1 VALUES(...10 params...)\n");

    // 预分配列数据缓冲区（10 字段，与 perf_insert_test.cpp 一致）
    std::vector<int64_t>  col_ts(BATCH_SIZE);
    std::vector<int32_t>  col_int(BATCH_SIZE);
    std::vector<int64_t>  col_bigint(BATCH_SIZE);
    std::vector<float>    col_float(BATCH_SIZE);
    std::vector<double>   col_double(BATCH_SIZE);
    std::vector<int16_t>  col_smallint(BATCH_SIZE);
    std::vector<int8_t>   col_tinyint(BATCH_SIZE);
    std::vector<int8_t>   col_bool(BATCH_SIZE);
    std::vector<int64_t>  col_extra1(BATCH_SIZE);
    std::vector<double>   col_extra2(BATCH_SIZE);

    int64_t baseTs = nowUs() ;  // 当前时间戳（us秒）
    printf("20 basets = %lld \r\n",baseTs);
    int64_t tsOffset = 0;

    // 填充一批数据
    auto fillData = [&](int64_t offset) {
        for (int i = 0; i < BATCH_SIZE; ++i) {
            col_ts[i]      = baseTs + offset + i;
            col_int[i]     = (int32_t)(offset + i);
            col_bigint[i]  = offset * 1000 + i;
            col_float[i]   = (float)i * 0.5f + 20.0f;
            col_double[i]  = (double)i * 1.5 + 100.0;
            col_smallint[i]= (int16_t)(i % 32767);
            col_tinyint[i] = (int8_t)(i % 127);
            col_bool[i]    = (int8_t)(i % 2);
            col_extra1[i]  = (int64_t)(offset * 777 + i * 13);
            col_extra2[i]  = (double)i * 0.75 + 500.0;
        }
    };

    // MultiBind 描述符
    EtDBStmt::MultiBind multiBinds[NUM_COLS];

    // 统一设置列绑定描述符（10 字段）
    auto setupBinds = [&]() {
        memset(multiBinds, 0, sizeof(multiBinds));
        multiBinds[0].type = EtDBStmt::TYPE_TIMESTAMP; multiBinds[0].buffer = col_ts.data();      multiBinds[0].stride = sizeof(int64_t);  multiBinds[0].numRows = BATCH_SIZE;
        multiBinds[1].type = EtDBStmt::TYPE_INT;       multiBinds[1].buffer = col_int.data();     multiBinds[1].stride = sizeof(int32_t);  multiBinds[1].numRows = BATCH_SIZE;
        multiBinds[2].type = EtDBStmt::TYPE_BIGINT;    multiBinds[2].buffer = col_bigint.data();  multiBinds[2].stride = sizeof(int64_t);  multiBinds[2].numRows = BATCH_SIZE;
        multiBinds[3].type = EtDBStmt::TYPE_FLOAT;     multiBinds[3].buffer = col_float.data();   multiBinds[3].stride = sizeof(float);    multiBinds[3].numRows = BATCH_SIZE;
        multiBinds[4].type = EtDBStmt::TYPE_DOUBLE;    multiBinds[4].buffer = col_double.data();  multiBinds[4].stride = sizeof(double);   multiBinds[4].numRows = BATCH_SIZE;
        multiBinds[5].type = EtDBStmt::TYPE_SMALLINT;  multiBinds[5].buffer = col_smallint.data();multiBinds[5].stride = sizeof(int16_t);  multiBinds[5].numRows = BATCH_SIZE;
        multiBinds[6].type = EtDBStmt::TYPE_TINYINT;   multiBinds[6].buffer = col_tinyint.data(); multiBinds[6].stride = sizeof(int8_t);   multiBinds[6].numRows = BATCH_SIZE;
        multiBinds[7].type = EtDBStmt::TYPE_BOOL;      multiBinds[7].buffer = col_bool.data();    multiBinds[7].stride = sizeof(int8_t);   multiBinds[7].numRows = BATCH_SIZE;
        multiBinds[8].type = EtDBStmt::TYPE_BIGINT;    multiBinds[8].buffer = col_extra1.data();  multiBinds[8].stride = sizeof(int64_t);  multiBinds[8].numRows = BATCH_SIZE;
        multiBinds[9].type = EtDBStmt::TYPE_DOUBLE;    multiBinds[9].buffer = col_extra2.data();  multiBinds[9].stride = sizeof(double);   multiBinds[9].numRows = BATCH_SIZE;
    };

    struct RoundStat {
        int round; int rows;
        int64_t sendStartUs;   // 本批开始时间（send 起点）
        int64_t sendUs;        // 仅 send 耗时（bind + executeAsync）
        int64_t totalUs;       // send 起点 → 结果就绪（含等待服务端）
        int64_t batchId;
        int affected;
    };

    // =====================================================================
    // 4. 同步基线对比（execute）
    // =====================================================================
    printf("\n=== 4. 同步基线对比 (execute) ===\n");
    std::vector<RoundStat> syncStats;

    auto runRoundSync = [&](int roundId) -> RoundStat {
        RoundStat s; s.round = roundId; s.rows = BATCH_SIZE;

        fillData(tsOffset);
        setupBinds();

        s.sendStartUs = nowUs();
        bool bindOk = stmt->bindParamBatch(multiBinds, NUM_COLS);
        if (!bindOk) { printf("  [ERROR] bindParamBatch failed\n"); s.sendUs = -1; return s; }

        int affected = stmt->execute();      // 同步：等待服务端响应
        int64_t t1 = nowUs();
        if (affected < 0) { printf("  [ERROR] execute returned %d\n", affected); s.sendUs = -1; return s; }

        s.sendUs  = t1 - s.sendStartUs;
        s.totalUs = s.sendUs;   // 同步场景 send 即 total
        s.affected = affected;
        tsOffset += BATCH_SIZE;
        return s;
    };

    printf("\n  --- 同步基准 (Sync baseline) ---\n");
    for (int r = 0; r < SYNC_ROUNDS; ++r) {
        auto s = runRoundSync(r + 1);
        if (s.sendUs < 0) { printf("  [FATAL] sync round failed\n"); delete stmt; return 1; }
        syncStats.push_back(s);
        printf("  Round %2d: %d rows, %6.2f ms, %8.0f rows/s, %7.2f MB/s\n",
               s.round, s.rows, s.sendUs/1000.0, (s.affected * 1e6) / (double)s.sendUs,
               (s.affected * ROW_BYTES / 1e6) * 1e6 / (double)s.sendUs);
    }
    // =====================================================================
    // 3. 异步批量插入（send 完即返回，结果由后台线程按批次 id 收集）
    //    滑动窗口：最多 MAX_PENDING 批在途（未消费），超限即消费最旧批次，
    //    从而控制结果集数量（内存有界）。
    // =====================================================================
    std::vector<RoundStat> asyncStats;
    std::deque<int> inflight;              // 在途批次在 asyncStats 中的下标（FIFO）

    client.setMaxAsyncPending(MAX_PENDING);  // 背压上限：控制结果集数量

    auto runRoundAsync = [&](int roundId) -> RoundStat {
        RoundStat s; s.round = roundId; s.rows = BATCH_SIZE;

        fillData(tsOffset);
        setupBinds();

        s.sendStartUs = nowUs();
        bool bindOk = stmt->bindParamBatch(multiBinds, NUM_COLS);
        if (!bindOk) { printf("  [ERROR] bindParamBatch failed\n"); s.sendUs = -1; return s; }

        int64_t bid = stmt->executeAsync();      // send 完立即返回
        int64_t t1 = nowUs();
        if (bid <= 0) { printf("  [ERROR] executeAsync returned %lld\n", (long long)bid); s.sendUs = -1; return s; }

        s.sendUs = t1 - s.sendStartUs;   // 仅 send 耗时（不含等待服务端响应）
        s.batchId = bid;
        tsOffset += BATCH_SIZE;
        return s;
    };

    // 消费并打印一个批次的结果（getAsyncResult 会消费：移出结果集，释放背压槽位）
    int64_t asyncAffected = 0, asyncErrors = 0, asyncFail = 0;
    auto consumeAndPrint = [&](int idx) {
        RoundStat& s = asyncStats[idx];
        client.waitAsyncResult(s.batchId);
        auto r = client.getAsyncResult(s.batchId);
        if (!r.ready || r.code != 0 || r.errors != 0) {
            printf("  [WARN] batch %lld: ready=%d code=%d submitted=%d affected=%d errors=%d\n",
                   (long long)s.batchId, r.ready, r.code, r.submitted, r.affected, r.errors);
            asyncFail++;
        }
        s.affected = r.affected;
        s.totalUs  = nowUs() - s.sendStartUs;   // send 起点 → 结果就绪
        double rps  = (s.affected * 1e6) / (double)(s.totalUs ? s.totalUs : 1);
        double mbps = (s.affected * ROW_BYTES / 1e6) * 1e6 / (double)(s.totalUs ? s.totalUs : 1);
        asyncAffected += r.affected;
        asyncErrors  += r.errors;
        printf("  Round %2d: %d rows, send %5.2f ms, total %6.2f ms, %8.0f rows/s, %7.2f MB/s, affected=%d%s\n",
               s.round, s.rows, s.sendUs/1000.0, s.totalUs/1000.0, rps, mbps, r.affected,
               (r.errors == 0 && r.code == 0) ? "" : "  [ERROR]");
    };

    // 预热（异步）
    printf("\n  --- 预热 (Warmup, async) ---\n");
    int64_t warmupIds[WARMUP_ROUNDS];
    for (int r = 0; r < WARMUP_ROUNDS; ++r) {
        auto s = runRoundAsync(r + 1);
        if (s.sendUs < 0) { printf("  [FATAL] warmup failed\n"); delete stmt; return 1; }
        warmupIds[r] = s.batchId;
        printf("  Round %2d: %d rows, send %5.2f ms\n", s.round, s.rows, s.sendUs/1000.0);
    }
    client.waitAllAsync();   // 等待预热批次结果
    // 消费预热结果（getAsyncResult 会移出结果集、释放背压槽位）
    for (int r = 0; r < WARMUP_ROUNDS; ++r) client.getAsyncResult((uint64_t)warmupIds[r]);

    // 正式测试（异步）：滑动窗口 —— 最多 MAX_PENDING 批在途，超过即消费最旧批次
    printf("\n  --- test (Benchmark, async, window=%d) ---\n", MAX_PENDING);
    int64_t tSend0 = nowUs();
    for (int r = 0; r < ASYNC_ROUNDS; ++r) {
        auto s = runRoundAsync(r + 1);
        if (s.sendUs < 0) { printf("  [FATAL] bench round failed\n"); delete stmt; return 1; }
        asyncStats.push_back(s);
        inflight.push_back((int)asyncStats.size() - 1);
        if ((int)inflight.size() >= MAX_PENDING) {   // 窗口满 → 消费最旧批次
            consumeAndPrint(inflight.front());
            inflight.pop_front();
        }
    }
    int64_t tSend1 = nowUs();
    int64_t sendTotalUs = tSend1 - tSend0;

    // 排空剩余在途批次
    int64_t tDrain0 = nowUs();
    while (!inflight.empty()) {
        consumeAndPrint(inflight.front());
        inflight.pop_front();
    }
    int64_t tDrain1 = nowUs();
    int64_t drainUs = tDrain1 - tDrain0;
    printf("  asy result: %d batch,affected %lld, err rows %lld，exception batchs %lld（result wait %6.2f ms, pending %lld）\n",
           (int)asyncStats.size(), (long long)asyncAffected, (long long)asyncErrors,
           (long long)asyncFail, drainUs/1000.0, (long long)client.asyncPendingCount());

    

    delete stmt;

    // =====================================================================
    // 5. 统计汇总
    // =====================================================================
    printf("\n=== 5. 统计汇总 ===\n");

    int64_t asyncRows = (int64_t)asyncStats.size() * BATCH_SIZE;
    double asyncSendRps  = asyncRows * 1e6 / (double)sendTotalUs;
    double asyncTotalRps = asyncRows * 1e6 / (double)(sendTotalUs + drainUs);
    double asyncSendMbps = asyncRows * ROW_BYTES / 1e6 * 1e6 / (double)sendTotalUs;
    double asyncTotalMbps = asyncRows * ROW_BYTES / 1e6 * 1e6 / (double)(sendTotalUs + drainUs);

    // 每批 send 延迟 + 每批总延迟（send→结果就绪）
    double avgSendUs = 0, minSendUs = 1e18, maxSendUs = 0;
    double avgTotalUs = 0, minTotalUs = 1e18, maxTotalUs = 0;
    for (auto& s : asyncStats) {
        avgSendUs += s.sendUs;
        if (s.sendUs < minSendUs) minSendUs = s.sendUs;
        if (s.sendUs > maxSendUs) maxSendUs = s.sendUs;
        avgTotalUs += s.totalUs;
        if (s.totalUs < minTotalUs) minTotalUs = s.totalUs;
        if (s.totalUs > maxTotalUs) maxTotalUs = s.totalUs;
    }
    if (!asyncStats.empty()) { avgSendUs /= (double)asyncStats.size(); avgTotalUs /= (double)asyncStats.size(); }

    double syncRps = 0, syncMin = 1e18, syncMax = 0;
    int64_t syncRows = 0, syncUs = 0;
    for (auto& s : syncStats) {
        double rps = (s.affected * 1e6) / (double)s.totalUs;
        syncRps += rps; syncRows += s.affected; syncUs += s.totalUs;
        if (rps < syncMin) syncMin = rps;
        if (rps > syncMax) syncMax = rps;
    }
    double avgSyncRps = syncStats.empty() ? 0 : syncRps / (double)syncStats.size();

    printf("\n");
    printf("  ------------------------------------------------------\n");
    printf("  │           asyn batch insert performance results                      │\n");
    printf("  ------------------------------------------------------\n");
    printf("  │ asyn rounds: %4d   batch: %4d rows   total rows: %6lld      │\n", ASYNC_ROUNDS, BATCH_SIZE, (long long)asyncRows);
    printf("  ------------------------------------------------------\n");
    printf("  │ [asyn] send total time:    %9.2f ms                      │\n", sendTotalUs/1000.0);
    printf("  │ [asyn] wait result time:  %9.2f ms                      │\n", drainUs/1000.0);
    printf("  │ [asyn] send throughput:      %10.0f rows/s  %7.2f MB/s  │\n", asyncSendRps, asyncSendMbps);
    printf("  │ [asyn] total throughput (with wait): %10.0f rows/s  %7.2f MB/s │\n", asyncTotalRps, asyncTotalMbps);
    printf("  │ [asyn] per-batch send latency: average %7.2f ms min %6.2f ms max %6.2f ms │\n", avgSendUs/1000.0, minSendUs/1000.0, maxSendUs/1000.0);
    printf("  │ [asyn] per-batch total latency:     average %7.2f ms min %6.2f ms max %6.2f ms │\n", avgTotalUs/1000.0, minTotalUs/1000.0, maxTotalUs/1000.0);
    printf("  ------------------------------------------------------\n");
    printf("  │ [sync] %3d rounds  average throughput: %10.0f rows/s            │\n", SYNC_ROUNDS, avgSyncRps);
    printf("  │ [sync] min/max:       %10.0f / %10.0f rows/s   │\n", syncMin, syncMax);
    printf("  ------------------------------------------------------\n");
    if (avgSyncRps > 0)
        printf("  │ asyn send / sync加速比:    %6.2f x                │\n", asyncSendRps / avgSyncRps);
    else
        printf("  │ asyn send / sync加速比:       N/A                  │\n");
    printf("  ------------------------------------------------------\n");

    // 等待提交完成
    printf("\n  waiting for commit to complete...\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));

    // =====================================================================
    // 6. 数据验证
    // =====================================================================
    int64_t totRows = (int64_t)(WARMUP_ROUNDS + ASYNC_ROUNDS + SYNC_ROUNDS) * BATCH_SIZE;
    printf("\n=== 6. data validation ===\n");
    {
        auto r = client.query("SELECT COUNT(*) FROM perf_data_1");
        if (r.rowCount() > 0 && r.colCount() > 0) {
            int64_t cnt = r.get(0, 0).iVal;
            printf("  SELECT COUNT(*): %lld rows\n", (long long)cnt);
            printf("  %s %d - %d \n", cnt == totRows ? "[OK] data validation passed" : "[WARN] row count mismatch (table may contain historical data)", (int)cnt, (int)totRows);
        } else {
            printf("  SELECT COUNT(*) failed or no data returned\n");
        }
    }
    {
        auto r = client.query("SELECT * FROM perf_data_1 LIMIT 3");
        int cols = r.colCount(), rows = r.rowCount();
        if (rows > 0) {
            printf("\n  first %d rows:\n", rows);
            for (int c = 0; c < cols; ++c) printf("    %-14s", r.columnNames()[c].c_str());
            printf("\n    ");
            for (int c = 0; c < cols; ++c) printf("------------------------------------------------------");
            printf("\n");
            for (int row = 0; row < rows; ++row) {
                printf("    ");
                for (int col = 0; col < cols; ++col) printf("%-14s  ", r.get(row, col).toString().c_str());
                printf("\n");
            }
        }
    }

    // =====================================================================
    // 7. 清理
    // =====================================================================
    printf("\n=== 7. cleanup ===\n");
    //execSQL(client, "DROP DATABASE IF EXISTS perftest");
    //printf("  [OK] DROP DATABASE perftest\n");
    client.close();
    printf("  [OK] connection closed\n");
    printf("\n  test completed!\n");
    return 0;
}
