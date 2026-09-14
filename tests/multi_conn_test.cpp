/*
 * EtherDB 多连接交错逐行插入性能测试（单线程）
 *
 * 打开 N 个独立连接，外层按行循环，内层按连接循环：
 *   对每一行 i，N 个连接各插入一次（相同数据，不同时间戳）。
 *   即：conn0插row0 → conn1插row0 → ... → conn0插row1 → conn1插row1 → ...
 *
 *   - N 个独立连接（默认 5），保持长连接
 *   - 所有连接写入同一张表 perf_data
 *   - 每连接插入 BATCH_SIZE 行
 *   - 总数据量: N × BATCH_SIZE 行
 *
 * Usage:
 *   # 终端 1: 启动服务端
 *   ./src/bin/etherdb_server -p 7040
 *
 *   # 终端 2: 运行测试
 *   ./build/multi_conn_test [port, default 7040]
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

using namespace ETDB::Client;

// ============================================================================
// 测试配置
// ============================================================================
constexpr int  NUM_CONNS     = 5;
constexpr int  BATCH_SIZE    = 500;    // 每个连接插入行数
constexpr int  WARMUP_ROWS   = 50;     // 每连接预热行数
constexpr int  NUM_COLS      = 10;

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
// 每个连接的句柄 + 预编译语句
// ============================================================================
struct ConnHandle {
    EtDBClient* client;
    EtDBStmt*   stmt;
    EtDBStmt::BindParam binds[NUM_COLS];

    // 单行标量变量
    int64_t  v_ts;
    int32_t  v_int;
    int64_t  v_bigint;
    float    v_float;
    double   v_double;
    int16_t  v_smallint;
    int8_t   v_tinyint;
    int8_t   v_bool;
    int64_t  v_extra1;
    double   v_extra2;

    ConnHandle() : client(nullptr), stmt(nullptr) {
        memset(binds, 0, sizeof(binds));
    }
};

// ============================================================================
// 主函数
// ============================================================================
int main(int argc, char* argv[]) {
    uint16_t port = (argc > 1) ? (uint16_t)atoi(argv[1]) : 7040;
    const char* host = "127.0.0.1";

    printf("╔══════════════════════════════════════════════════════╗\n");
    printf("║   EtherDB 多连接交错逐行插入性能测试（单线程）        ║\n");
    printf("║   连接数: %-2d   每连接行数: %-4d   总: %-6d 行      ║\n",
           NUM_CONNS, BATCH_SIZE, NUM_CONNS * BATCH_SIZE);
    printf("║   模式: 外层按行循环，内层按连接循环交错插入          ║\n");
    printf("╚══════════════════════════════════════════════════════╝\n\n");

    // =====================================================================
    // 1. 准备：创建数据库和表
    // =====================================================================
    printf("=== 1. 准备数据库和表 ===\n");
    {
        EtDBClient c;
        if (!c.connect(host, port)) {
            printf("  [FATAL] 无法连接。请先启动: ./src/bin/etherdb_server -p %d\n", port);
            return 1;
        }
        execSQL(c, "DROP DATABASE IF EXISTS perftestmulti");
        if (!execSQL(c, "CREATE DATABASE perftestmulti")) return 1;
        printf("  [OK] CREATE DATABASE perftestmulti\n");

        if (!execSQL(c, "USE perftestmulti")) return 1;
        printf("  [OK] USE perftestmulti\n");

        if (!execSQL(c,
            "CREATE TABLE perf_data ("
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
            ")")) return 1;
        printf("  [OK] CREATE TABLE perf_data (10 fields)\n");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // =====================================================================
    // 2. 打开所有连接 + prepare
    // =====================================================================
    printf("\n=== 2. 打开 %d 个连接并 prepare ===\n", NUM_CONNS);

    std::vector<ConnHandle> handles(NUM_CONNS);
    bool allOk = true;

    for (int i = 0; i < NUM_CONNS; ++i) {
        auto& h = handles[i];
        h.client = new EtDBClient();

        if (!h.client->connect(host, port)) {
            printf("  [连接 %d] 连接失败\n", i);
            allOk = false; continue;
        }
        if (!execSQL(*h.client, "USE perftestmulti")) {
            printf("  [连接 %d] USE perftestmulti 失败\n", i);
            allOk = false; continue;
        }

        h.stmt = h.client->createStmt();
        if (!h.stmt || !h.stmt->prepare(
                "INSERT INTO perf_data VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?)")) {
            printf("  [连接 %d] prepare 失败\n", i);
            allOk = false; continue;
        }

        // 初始化 BindParam（只需一次）
        h.binds[0].type = EtDBStmt::TYPE_TIMESTAMP; h.binds[0].buffer = &h.v_ts;
        h.binds[1].type = EtDBStmt::TYPE_INT;       h.binds[1].buffer = &h.v_int;
        h.binds[2].type = EtDBStmt::TYPE_BIGINT;    h.binds[2].buffer = &h.v_bigint;
        h.binds[3].type = EtDBStmt::TYPE_FLOAT;     h.binds[3].buffer = &h.v_float;
        h.binds[4].type = EtDBStmt::TYPE_DOUBLE;    h.binds[4].buffer = &h.v_double;
        h.binds[5].type = EtDBStmt::TYPE_SMALLINT;  h.binds[5].buffer = &h.v_smallint;
        h.binds[6].type = EtDBStmt::TYPE_TINYINT;   h.binds[6].buffer = &h.v_tinyint;
        h.binds[7].type = EtDBStmt::TYPE_BOOL;      h.binds[7].buffer = &h.v_bool;
        h.binds[8].type = EtDBStmt::TYPE_BIGINT;    h.binds[8].buffer = &h.v_extra1;
        h.binds[9].type = EtDBStmt::TYPE_DOUBLE;    h.binds[9].buffer = &h.v_extra2;

        printf("  [连接 %d] 就绪\n", i);
    }

    if (!allOk) {
        printf("  [FATAL] 部分连接初始化失败\n");
        for (auto& h : handles) { delete h.stmt; delete h.client; }
        return 1;
    }

    // =====================================================================
    // 3. 交错逐行插入 —— 核心逻辑
    // =====================================================================
    // 时间戳策略：每个连接每行一个唯一 TS
    //   TS = baseTs + rowIdx * NUM_CONNS + connIdx
    //   即 conn0 行i 的 TS 与 conn1 行i 的 TS 相邻但不重叠
    int64_t baseTs = 1716364800000LL;
    int64_t tsOffset = 0;
    // 填充某一行数据的 lambda（所有连接对同一 rowIdx 写入相同数据，仅 TS 不同）
    auto fillRowData = [&](ConnHandle& h, int64_t ts) {
        h.v_ts       = ts;
        h.v_int      = (int32_t)(ts % 2147483647);
        h.v_bigint   = ts * 1000;
        h.v_float    = (float)(ts % 1000) * 0.5f + 20.0f;
        h.v_double   = (double)(ts % 10000) * 1.5 + 100.0;
        h.v_smallint = (int16_t)(ts % 32767);
        h.v_tinyint  = (int8_t)(ts % 127);
        h.v_bool     = (int8_t)(ts % 2);
        h.v_extra1   = ts * 777;
        h.v_extra2   = (double)(ts % 5000) * 0.75 + 500.0;
    };

    // 3a) 预热
    printf("\n=== 3. 预热（每连接 %d 行，交错插入） ===\n", WARMUP_ROWS);
    {
        for (int row = 0; row < WARMUP_ROWS; ++row) {
            for (int c = 0; c < NUM_CONNS; ++c) {
                auto& h = handles[c];
                int64_t ts = baseTs + tsOffset;
                fillRowData(h, ts);
                tsOffset++;
                if (!h.stmt->bindParam(h.binds, NUM_COLS)) {
                    printf("  [连接 %d 行 %d] bindParam 失败\n", c, row);
                }
                if (!h.stmt->addBatch()) {
                    printf("  [连接 %d 行 %d] addBatch 失败\n", c, row);
                }
                int aff = h.stmt->execute();
                if (aff < 0) {
                    printf("  [连接 %d 行 %d] execute 失败\n", c, row);
                }
            }
        }
    }
    baseTs += (int64_t)WARMUP_ROWS * NUM_CONNS;
    printf("  预热完成，baseTs 推进到 %lld\n", (long long)baseTs);

    // 3b) 正式测试
    printf("\n=== 4. 正式测试（每连接 %d 行，交错插入） ===\n", BATCH_SIZE);
    printf("  外层 for row in [0, %d), 内层 for conn in [0, %d)\n",
           BATCH_SIZE, NUM_CONNS);

    int64_t  totalRows  = 0;
    int      totalErrs  = 0;
    auto     wallStart  = nowUs();

    for (int row = 0; row < BATCH_SIZE; ++row) {
        for (int c = 0; c < NUM_CONNS; ++c) {
            auto& h = handles[c];
            int64_t ts = baseTs + tsOffset;
            fillRowData(h, ts);
            tsOffset++;

            if (!h.stmt->bindParam(h.binds, NUM_COLS)) {
                totalErrs++; continue;
            }
            if (!h.stmt->addBatch()) {
                totalErrs++; continue;
            }
            int aff = h.stmt->execute();
            if (aff < 0) {
                totalErrs++; continue;
            }
            totalRows++;
        }
    }

    auto     wallEnd    = nowUs();
    int64_t  wallUs     = wallEnd - wallStart;

    // =====================================================================
    // 4. 结果汇总
    // =====================================================================
    printf("\n=== 5. 结果汇总 ===\n");

    double wallRps  = wallUs > 0 ? (totalRows * 1e6 / wallUs) : 0;
    double avgLatUs = totalRows > 0 ? (wallUs / (double)totalRows) : 0;

    printf("\n");
    printf("  ┌─────────────────────────────────────────────────────┐\n");
    printf("  │     多连接交错逐行插入性能测试结果（单线程）        │\n");
    printf("  ├─────────────────────────────────────────────────────┤\n");
    printf("  │ 连接数:           %4d                              │\n", NUM_CONNS);
    printf("  │ 每连接行数:       %4d                              │\n", BATCH_SIZE);
    printf("  │ 总插入行数:       %6lld                            │\n", (long long)totalRows);
    printf("  │ 总错误数:         %6d                              │\n", totalErrs);
    printf("  ├─────────────────────────────────────────────────────┤\n");
    printf("  │ 总体吞吐(墙上):   %8.0f rows/s                     │\n", wallRps);
    printf("  │ 平均延迟/行:      %8.0f us                         │\n", avgLatUs);
    printf("  │ 墙上时间:         %8.2f ms                         │\n", wallUs / 1000.0);
    printf("  └─────────────────────────────────────────────────────┘\n");

    // =====================================================================
    // 5. 数据验证
    // =====================================================================
    printf("\n=== 6. 数据验证 ===\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    {
        EtDBClient c;
        if (c.connect(host, port)) {
            execSQL(c, "USE perftestmulti");
            auto r = c.query("SELECT COUNT(*) FROM perf_data");
            if (r.rowCount() > 0 && r.colCount() > 0) {
                int64_t cnt = r.get(0, 0).iVal;
                int64_t expected = (int64_t)NUM_CONNS * (WARMUP_ROWS + BATCH_SIZE);
                printf("  SELECT COUNT(*): %lld rows (预期 %lld)  %s\n",
                       (long long)cnt, (long long)expected,
                       cnt == expected ? "[OK]" : "[WARN]");
            }

            // 展示前几行看交错效果
            auto r2 = c.query("SELECT * FROM perf_data ORDER BY ts LIMIT 6");
            int cols = r2.colCount(), rows = r2.rowCount();
            if (rows > 0) {
                printf("\n  前 %d 行（按TS排序，可见交错效果）:\n", rows);
                for (int c = 0; c < cols; ++c)
                    printf("    %-14s", r2.columnNames()[c].c_str());
                printf("\n    ");
                for (int c = 0; c < cols; ++c) printf("──────┬────────");
                printf("\n");
                for (int row = 0; row < rows; ++row) {
                    printf("    ");
                    for (int col = 0; col < cols; ++col)
                        printf("%-14s  ", r2.get(row, col).toString().c_str());
                    printf("\n");
                }
            }
        }
    }

    // =====================================================================
    // 6. 清理
    // =====================================================================
    printf("\n=== 7. 清理 ===\n");
    for (auto& h : handles) {
        delete h.stmt;
        h.client->close();
        delete h.client;
    }
    printf("  [OK] %d 个连接已关闭\n", NUM_CONNS);

    {
        EtDBClient c;
        // if (c.connect(host, port)) {
        //     execSQL(c, "DROP DATABASE IF EXISTS perftestmulti");
        //     printf("  [OK] DROP DATABASE perftestmulti\n");
        // }
    }
    printf("\n  测试完成!\n");
    return totalErrs > 0 ? 1 : 0;
}
