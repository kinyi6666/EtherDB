/*
 * EtherDB 预绑定（列绑定）批量插入性能测试
 *
 * 使用 EtDBStmt::bindParamBatch — 真正的预绑定方式：
 *   将整列数据一次性绑定（columnar binding），一次 execute 完成批量插入。
 *
 *   - 10 个字段：TIMESTAMP, INT, BIGINT, FLOAT, DOUBLE, SMALLINT, TINYINT, BOOL, BINARY(32), NCHAR(32)
 *   - 每批 500 行
 *   - 预热 2 轮 + 正式测试 5 轮
 *
 * Usage:
 *   # 终端 1: 启动服务端
 *   ./src/bin/etherdb_server -p 7040
 *
 *   # 终端 2: 运行测试
 *   ./build/perf_insert_test20 [port, default 7040]
 */

#include <client/EtDBClient.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <chrono>
#include <thread>
#include <vector>
#include <string>
#include <cmath>
#include <algorithm>

static void print_border(void) {
    int i;
    printf("  +");
    for (i = 0; i < 53; ++i) {
        putchar('-');
    }
    printf("+\n");
}
using namespace ETDB::Client;

// ============================================================================
// 测试配置
// ============================================================================
constexpr int  BATCH_SIZE    = 10000;
constexpr int  NUM_COLS      = 19;
constexpr int  WARMUP_ROUNDS = 2;
int  TEST_ROUNDS   = 100;
constexpr int  BINARY_LEN    = 32;

using Clock = std::chrono::high_resolution_clock;
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
    TEST_ROUNDS = (argc > 2) ? atoi(argv[2]) : TEST_ROUNDS;
    std::string table = (argc > 3) ? argv[3] : "perf_data";
    const char* host = "127.0.0.1";

    printf("|-------------------------------------------------------------------- |\n");
    printf("║   EtherDB Pre-bound (column binding) Batch Insert Performance Test  ║\n");
    printf("║   Number of columns: %-2d    Rows per batch: %-4d                   ║\n", NUM_COLS, BATCH_SIZE);
    printf("║   Warmup rounds: %-2d    Test rounds: %-2d                          ║\n", WARMUP_ROUNDS, TEST_ROUNDS);
    printf("║   Server: %s:%-5d                                                   ║\n", host, port);
    printf("|---------------------------------------------------------------------|\n\n");


    // =====================================================================
    // 1. 连接
    // =====================================================================
    printf("=== 1. 连接服务端 ===\n");

    EtDBClient client;
    if (!client.connect(host, port)) {
        printf("  [FATAL] 无法连接。请先启动: ./src/bin/etherdb_dserver -p %d\n", port);
        return 1;
    }
    printf("  [OK] 已连接到 %s:%d\n", host, port);

    // =====================================================================
    // 2. 创建数据库和表
    // =====================================================================
    printf("\n=== 2. 创建数据库和表 ===\n");

    // execSQL(client, "DROP DATABASE IF EXISTS perftest20");
     if (!execSQL(client, "CREATE DATABASE IF NOT EXISTS perftest20 REPLICA 1 PRECISION 'us' ")) return 1;
     printf("  [OK] CREATE DATABASE perftest20\n");

    if (!execSQL(client, "USE perftest20")) return 1;
    printf("  [OK] USE perftest20\n");

    std::string createSQL =
        "CREATE TABLE IF NOT EXISTS " + table + " ("
        "ts TIMESTAMP, "
        "col_int INT, "
        "col_bigint BIGINT, "
        "col_float FLOAT, "
        "col_double DOUBLE, "
        "col_smallint SMALLINT, "
        "col_tinyint TINYINT, "
        "col_bool BOOL, "
        "col_extra1 BIGINT, "
        "col_extra2 DOUBLE, "
        "col_int1 INT, "
        "col_bigint1 BIGINT, "
        "col_float1 FLOAT, "
        "col_double1 DOUBLE, "
        "col_smallint1 SMALLINT, "
        "col_tinyint1 TINYINT, "
        "col_bool1 BOOL, "
        "col_extra11 BIGINT, "
        "col_extra12 DOUBLE"
        ")";
     if (!execSQL(client, createSQL)) return 1;
     printf("  [OK] CREATE TABLE IF NOT EXISTS %s (19 fields)\n", table.c_str());

    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // =====================================================================
    // 3. 预绑定列绑定批量插入
    // =====================================================================
    printf("\n=== 3. 预绑定(列绑定)批量插入性能测试 ===\n");

    EtDBStmt* stmt = client.createStmt();
    if (!stmt) { printf("  [FATAL] createStmt failed\n"); return 1; }

    std::string insertSQL = "INSERT INTO " + table + " VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
    bool ok = stmt->prepare(insertSQL);
    if (!ok) { printf("  [FATAL] prepare failed\n"); delete stmt; return 1; }
    printf("  [OK] Prepared: INSERT INTO %s VALUES(...19 params...)\n", table.c_str());

    // 预分配列数据缓冲区
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
     std::vector<int32_t>  col_int1(BATCH_SIZE);
    std::vector<int64_t>  col_bigint1(BATCH_SIZE);
    std::vector<float>    col_float1(BATCH_SIZE);
    std::vector<double>   col_double1(BATCH_SIZE);
    std::vector<int16_t>  col_smallint1(BATCH_SIZE);
    std::vector<int8_t>   col_tinyint1(BATCH_SIZE);
    std::vector<int8_t>   col_bool1(BATCH_SIZE);
    std::vector<int64_t>  col_extra11(BATCH_SIZE);
    std::vector<double>   col_extra12(BATCH_SIZE);

    int64_t baseTs = nowUs() ;  // 当前时间戳（us秒）
    printf("20 basets = %ld \r\n",baseTs);
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
            col_int1[i]     = (int32_t)(offset + i);
            col_bigint1[i]  = offset * 1000 + i;
            col_float1[i]   = (float)i * 0.5f + 20.0f;
            col_double1[i]  = (double)i * 1.5 + 100.0;
            col_smallint1[i]= (int16_t)(i % 32767);
            col_tinyint1[i] = (int8_t)(i % 127);
            col_bool1[i]    = (int8_t)(i % 2);
            col_extra11[i]  = (int64_t)(offset * 777 + i * 13);
            col_extra12[i]  = (double)i * 0.75 + 500.0;
        }
    };

    // MultiBind 描述符
    EtDBStmt::MultiBind multiBinds[NUM_COLS];

    struct RoundStat {
        int round; int rows; int64_t elapsedUs; double rowsPerSec; double mbPerSec;
    };
    std::vector<RoundStat> stats;

    auto runRound = [&](int roundId) -> RoundStat {
        RoundStat s; s.round = roundId; s.rows = BATCH_SIZE;

        fillData(tsOffset);

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
        multiBinds[10].type = EtDBStmt::TYPE_INT;       multiBinds[10].buffer = col_int1.data();     multiBinds[10].stride = sizeof(int32_t);  multiBinds[10].numRows = BATCH_SIZE;
        multiBinds[11].type = EtDBStmt::TYPE_BIGINT;    multiBinds[11].buffer = col_bigint1.data();  multiBinds[11].stride = sizeof(int64_t);  multiBinds[11].numRows = BATCH_SIZE;
        multiBinds[12].type = EtDBStmt::TYPE_FLOAT;     multiBinds[12].buffer = col_float1.data();   multiBinds[12].stride = sizeof(float);    multiBinds[12].numRows = BATCH_SIZE;
        multiBinds[13].type = EtDBStmt::TYPE_DOUBLE;    multiBinds[13].buffer = col_double1.data();  multiBinds[13].stride = sizeof(double);   multiBinds[13].numRows = BATCH_SIZE;
        multiBinds[14].type = EtDBStmt::TYPE_SMALLINT;  multiBinds[14].buffer = col_smallint1.data();multiBinds[14].stride = sizeof(int16_t);  multiBinds[14].numRows = BATCH_SIZE;
        multiBinds[15].type = EtDBStmt::TYPE_TINYINT;   multiBinds[15].buffer = col_tinyint1.data(); multiBinds[15].stride = sizeof(int8_t);   multiBinds[15].numRows = BATCH_SIZE;
        multiBinds[16].type = EtDBStmt::TYPE_BOOL;      multiBinds[16].buffer = col_bool1.data();    multiBinds[16].stride = sizeof(int8_t);   multiBinds[16].numRows = BATCH_SIZE;
        multiBinds[17].type = EtDBStmt::TYPE_BIGINT;    multiBinds[17].buffer = col_extra11.data();  multiBinds[17].stride = sizeof(int64_t);  multiBinds[17].numRows = BATCH_SIZE;
        multiBinds[18].type = EtDBStmt::TYPE_DOUBLE;    multiBinds[18].buffer = col_extra12.data();  multiBinds[18].stride = sizeof(double);   multiBinds[18].numRows = BATCH_SIZE;

        int64_t t0 = nowUs();
        bool bindOk = stmt->bindParamBatch(multiBinds, NUM_COLS);
        if (!bindOk) { printf("  [ERROR] bindParamBatch failed\n"); s.elapsedUs = 0; return s; }

        int affected = stmt->execute();
        int64_t t1 = nowUs();

        if (affected < 0) {
            printf("  [ERROR] execute returned %d\n", affected);
            s.elapsedUs = 0;
            return s;
        }

        s.elapsedUs  = t1 - t0;
        s.rowsPerSec = (affected * 1e6) / s.elapsedUs;
        s.mbPerSec   = (affected * 96.0 / 1e6) * 1e6 / s.elapsedUs;
        tsOffset += BATCH_SIZE;
        return s;
    };

    // 预热
    printf("\n  --- 预热 (Warmup) ---\n");
    for (int r = 0; r < WARMUP_ROUNDS; ++r) {
        auto s = runRound(r + 1);
        if (s.elapsedUs == 0) { printf("  [FATAL] warmup failed\n"); delete stmt; return 1; }
        printf("  Round %2d: %d rows, %6.2f ms, %8.0f rows/s, %7.2f MB/s\n",
               s.round, s.rows, s.elapsedUs/1000.0, s.rowsPerSec, s.mbPerSec);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // 正式测试
    printf("\n  --- 正式测试 (Benchmark) ---\n");
    for (int r = 0; r < TEST_ROUNDS; ++r) {
        auto s = runRound(r + 1);
        if (s.elapsedUs == 0) { printf("  [FATAL] bench round failed\n"); delete stmt; return 1; }
        stats.push_back(s);
        printf("  Round %2d: %d rows, %6.2f ms, %8.0f rows/s, %7.2f MB/s\n",
               s.round, s.rows, s.elapsedUs/1000.0, s.rowsPerSec, s.mbPerSec);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    delete stmt;

    // =====================================================================
    // 4. 统计
    // =====================================================================
    printf("\n=== 4. 统计汇总 ===\n");

    double trps = 0, tmbps = 0;
    double minRps = 1e18, maxRps = 0, minMbps = 1e18, maxMbps = 0;
    int64_t totRows = 0, totUs = 0;
    for (auto& s : stats) {
        trps += s.rowsPerSec; tmbps += s.mbPerSec;
        totRows += s.rows; totUs += s.elapsedUs;
        if (s.rowsPerSec < minRps) minRps = s.rowsPerSec;
        if (s.rowsPerSec > maxRps) maxRps = s.rowsPerSec;
        if (s.mbPerSec   < minMbps) minMbps = s.mbPerSec;
        if (s.mbPerSec   > maxMbps) maxMbps = s.mbPerSec;
    }
    int sn = (int)stats.size();
    double avgRps = trps / sn, avgMbps = tmbps / sn;
    double vr = 0, vm = 0;
    for (auto& s : stats) { double dr = s.rowsPerSec - avgRps; vr += dr*dr; double dm = s.mbPerSec - avgMbps; vm += dm*dm; }
    double stdRps = sqrt(vr/sn), stdMbps = sqrt(vm/sn);

    

    /* 替换原来的 printf 块 */
    printf("\n");
    print_border();
    printf("  | %-51s |\n", "Pre-bound (column binding) Insert Performance");
    printf("  | %-51s |\n", "Test Results");
    print_border();
    printf("  | %-20s %4d%*s |\n", "Test rounds:", sn, 26, "");
    printf("  | %-20s %4d%*s |\n", "Rows per batch:", BATCH_SIZE, 26, "");
    printf("  | %-20s %6lld%*s |\n", "Total rows inserted:", (long long)totRows, 24, "");
    print_border();
    printf("  | %-51s |\n", "Throughput (rows/s):");
    printf("  |   %-18s %10.0f%*s |\n", "Average:", avgRps, 20, "");
    printf("  |   %-18s %10.0f%*s |\n", "Min:", minRps, 20, "");
    printf("  |   %-18s %10.0f%*s |\n", "Max:", maxRps, 20, "");
    printf("  |   %-18s %10.0f%*s |\n", "Std dev:", stdRps, 20, "");
    print_border();
    printf("  | %-51s |\n", "Throughput (MB/s):");
    printf("  |   %-18s %10.2f%*s |\n", "Average:", avgMbps, 20, "");
    printf("  |   %-18s %10.2f%*s |\n", "Min:", minMbps, 20, "");
    printf("  |   %-18s %10.2f%*s |\n", "Max:", maxMbps, 20, "");
    printf("  |   %-18s %10.2f%*s |\n", "Std dev:", stdMbps, 20, "");
    print_border();
    printf("  | %-24s %8.2f ms%*s |\n", "Average latency/batch:", (totUs / (double)sn) / 1000.0, 15, "");
    print_border();

    // 等待异步提交完成
    printf("\n  等待提交完成...\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));

    // =====================================================================
    // 5. 验证
    // =====================================================================
    totRows += WARMUP_ROUNDS * BATCH_SIZE;  // 加上预热轮的行数
    printf("\n=== 5. Data Validation ===\n");
    {
        auto r = client.query("SELECT COUNT(*) FROM " + table);
        if (r.rowCount() > 0 && r.colCount() > 0) {
            int64_t cnt = r.get(0, 0).iVal;
            printf("  SELECT COUNT(*): %lld rows\n", (long long)cnt);
            printf("  %s %d - %d \n", cnt == totRows ? "[OK] data integrity verified" : "[WARN] row count mismatch", (int)cnt, (int)totRows);
        } else {
            printf("  SELECT COUNT(*) query failed or no data returned\n");
        }
    }
    {
        auto r = client.query("SELECT * FROM " + table + " LIMIT 3");
        int cols = r.colCount(), rows = r.rowCount();
        if (rows > 0) {
            printf("\n  First %d rows:\n", rows);
            for (int c = 0; c < cols; ++c) printf("    %-14s", r.columnNames()[c].c_str());
            printf("\n    ");
            for (int c = 0; c < cols; ++c) printf("----------------");
            printf("\n");
            for (int row = 0; row < rows; ++row) {
                printf("    ");
                for (int col = 0; col < cols; ++col) printf("%-14s  ", r.get(row, col).toString().c_str());
                printf("\n");
            }
        }
    }

    // =====================================================================
    // 6. 清理
    // =====================================================================
    printf("\n=== 6. Cleanup ===\n");
    //execSQL(client, "DROP DATABASE IF EXISTS perftest");
    //printf("  [OK] DROP DATABASE perftest\n");
    client.close();
    printf("  [OK] close connection\n");
    printf("\n  test completed!\n");
    return 0;
}
