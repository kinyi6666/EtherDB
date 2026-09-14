/*
 * EtherDB Phase 3 查询优化性能对比测试
 *
 * 对比 Phase 3 优化前后的查询性能：
 *   1. WHERE 时间戳下推：对比范围查询 vs 全表扫描
 *   2. 列投影：对比 SELECT 1列 vs SELECT *
 *   3. LIMIT + WHERE 组合：Phase 3 独有优化
 *   4. COUNT(*) 索引快速路径
 *
 * Usage:
 *   # 终端 1: 启动服务端
 *   ./src/bin/etherdb_server -p 7040
 *
 *   # 终端 2: 编译 + 运行
 *   cd build && make -j4 p3_perf_test && ./p3_perf_test [port]
 */

#include <client/EtDBClient.h>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <thread>
#include <string>

using namespace ETDB::Client;
using Clock = std::chrono::high_resolution_clock;
using Us = std::chrono::microseconds;

static int64_t nowUs() {
    return std::chrono::duration_cast<Us>(Clock::now().time_since_epoch()).count();
}

static int64_t bench(EtDBClient& c, const std::string& sql, int runs, int* outRows) {
    int64_t total = 0;
    int rows = -1;
    for (int i = 0; i < runs; ++i) {
        auto t0 = nowUs();
        auto r = c.query(sql);
        auto t1 = nowUs();
        total += (t1 - t0);
        if (rows < 0) rows = r.rowCount();
    }
    if (outRows) *outRows = rows;
    return total / runs;
}

static void printResult(const char* label, int64_t avgUs, int rows) {
    printf("  %-24s  avg=%8lld us  rows=%d\n", label, (long long)avgUs, rows);
}

int main(int argc, char* argv[]) {
    int port = (argc > 1) ? atoi(argv[1]) : 7040;
    const int RUNS = 5;

    EtDBClient c;
    if (!c.connect("127.0.0.1", port)) {
        printf("FAIL: connect to port %d failed\n", port);
        return 1;
    }

    c.query("DROP DATABASE IF EXISTS p3perf");
    c.query("CREATE DATABASE p3perf");
    c.query("USE p3perf");
    c.query("CREATE TABLE t(ts TIMESTAMP, v1 INT, v2 FLOAT, v3 BIGINT, v4 DOUBLE, v5 BIGINT)");

    int N = 10000;
    int64_t baseTs = 1716364800000LL;
    auto* s = c.createStmt();
    s->prepare("INSERT INTO t VALUES(?, ?, ?, ?, ?, ?)");
    for (int batch = 0; batch < 2; ++batch) {
        int cnt = std::min(5000, N - batch * 5000);
        std::vector<int64_t> ts(cnt), v1(cnt), v3(cnt), v5(cnt);
        std::vector<float> v2(cnt);
        std::vector<double> v4(cnt);
        for (int i = 0; i < cnt; ++i) {
            ts[i] = baseTs + batch * 5000LL + i;
            v1[i] = batch * 5000 + i;
            v2[i] = (batch * 5000 + i) * 0.5f;
            v3[i] = batch * 5000 + i;
            v4[i] = (batch * 5000 + i) * 0.25;
            v5[i] = batch * 5000 + i;
        }
        EtDBStmt::MultiBind mb[6] = {};
        mb[0].type = 0; mb[0].buffer = ts.data(); mb[0].stride = 8; mb[0].numRows = cnt;
        mb[1].type = 4; mb[1].buffer = v1.data(); mb[1].stride = 4; mb[1].numRows = cnt;
        mb[2].type = 6; mb[2].buffer = v2.data(); mb[2].stride = 4; mb[2].numRows = cnt;
        mb[3].type = 5; mb[3].buffer = v3.data(); mb[3].stride = 8; mb[3].numRows = cnt;
        mb[4].type = 7; mb[4].buffer = v4.data(); mb[4].stride = 8; mb[4].numRows = cnt;
        mb[5].type = 5; mb[5].buffer = v5.data(); mb[5].stride = 8; mb[5].numRows = cnt;
        s->bindParamBatch(mb, 6);
        s->execute();
    }
    delete s;
    printf("Inserted %d rows (6 cols), waiting for flush...\n", N);
    std::this_thread::sleep_for(std::chrono::seconds(5));

    printf("\n===== Phase 3 Performance Comparison (%d runs each) =====\n\n", RUNS);

    // ── Test 1: WHERE pushdown vs full scan ──
    printf("--- Test 1: WHERE ts range pushdown ---\n");
    {
        std::string qRange = "SELECT * FROM t WHERE ts >= " + std::to_string(baseTs + 5000)
                           + " AND ts < " + std::to_string(baseTs + 5100);
        int rows1, rows2;
        int64_t t1 = bench(c, qRange, RUNS, &rows1);
        int64_t t2 = bench(c, "SELECT * FROM t", RUNS, &rows2);
        printResult("WHERE ts range (Ph3)", t1, rows1);
        printResult("Full scan baseline", t2, rows2);
        printf("  => Range scan reads ~%d rows vs %d full scan (%.1fx fewer)\n",
               rows1, rows2, (double)rows2 / std::max(1, rows1));
    }

    // ── Test 2: LIMIT + WHERE combo ──
    printf("\n--- Test 2: LIMIT + WHERE (Ph3 unique combo) ---\n");
    {
        std::string qCombo = "SELECT * FROM t WHERE ts >= " + std::to_string(baseTs)
                           + " AND ts < " + std::to_string(baseTs + 5000) + " LIMIT 10";
        int rows1, rows2;
        int64_t t1 = bench(c, qCombo, RUNS, &rows1);
        int64_t t2 = bench(c, "SELECT * FROM t LIMIT 10", RUNS, &rows2);
        printResult("LIMIT+WHERE (Ph3)", t1, rows1);
        printResult("LIMIT only", t2, rows2);
    }

    // ── Test 3: Column projection ──
    printf("\n--- Test 3: Column projection (SELECT v1 vs SELECT *) ---\n");
    {
        std::string qProj = "SELECT v1 FROM t WHERE ts >= " + std::to_string(baseTs + 2000)
                          + " AND ts < " + std::to_string(baseTs + 3000);
        std::string qAll  = "SELECT * FROM t WHERE ts >= " + std::to_string(baseTs + 2000)
                          + " AND ts < " + std::to_string(baseTs + 3000);
        int rows1, rows2;
        int64_t t1 = bench(c, qProj, RUNS, &rows1);
        int64_t t2 = bench(c, qAll, RUNS, &rows2);
        printResult("SELECT v1 (2/6 cols)", t1, rows1);
        printResult("SELECT * (6/6 cols)", t2, rows2);
    }

    // ── Test 4: COUNT(*) index ──
    printf("\n--- Test 4: COUNT(*) index (Phase 2) ---\n");
    {
        int rows;
        int64_t t = bench(c, "SELECT COUNT(*) FROM t", RUNS, &rows);
        printResult("COUNT(*) index", t, rows);
    }

    c.query("DROP DATABASE IF EXISTS p3perf");
    printf("\nDone.\n");
    return 0;
}
