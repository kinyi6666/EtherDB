/*
 * EtherDB Phase 3 查询优化验证测试
 *
 * 验证:
 *   1. WHERE 时间戳下推 (ts range pushdown to StorageReader)
 *   2. 列投影 (column projection — 只解码需要的列)
 *   3. LIMIT + WHERE 组合优化
 *   4. COUNT(*) 索引快速路径 (Phase 2, 回归验证)
 *
 * Usage:
 *   # 终端 1: 启动服务端
 *   ./src/bin/etherdb_server -p 7040
 *
 *   # 终端 2: 编译 + 运行
 *   cd build && make -j4 p3_verify_test && ./p3_verify_test [port]
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

int main(int argc, char* argv[]) {
    int port = (argc > 1) ? atoi(argv[1]) : 7040;

    EtDBClient c;
    if (!c.connect("127.0.0.1", port)) {
        printf("FAIL: connect to port %d failed\n", port);
        return 1;
    }

    c.query("DROP DATABASE IF EXISTS p3verify");
    c.query("CREATE DATABASE p3verify");
    c.query("USE p3verify");
    c.query("CREATE TABLE qt(ts TIMESTAMP, v1 INT, v2 FLOAT, v3 BIGINT, v4 DOUBLE)");

    int N = 5000;
    int64_t baseTs = 1716364800000LL;
    auto* s = c.createStmt();
    s->prepare("INSERT INTO qt VALUES(?, ?, ?, ?, ?)");
    {
        std::vector<int64_t> ts(N), v1(N), v3(N);
        std::vector<float> v2(N);
        std::vector<double> v4(N);
        for (int i = 0; i < N; ++i) {
            ts[i] = baseTs + i;
            v1[i] = i;
            v2[i] = i * 0.5f;
            v3[i] = i;
            v4[i] = i * 0.25;
        }
        EtDBStmt::MultiBind mb[5] = {};
        mb[0].type = 0; mb[0].buffer = ts.data(); mb[0].stride = 8; mb[0].numRows = N;
        mb[1].type = 4; mb[1].buffer = v1.data(); mb[1].stride = 4; mb[1].numRows = N;
        mb[2].type = 6; mb[2].buffer = v2.data(); mb[2].stride = 4; mb[2].numRows = N;
        mb[3].type = 5; mb[3].buffer = v3.data(); mb[3].stride = 8; mb[3].numRows = N;
        mb[4].type = 7; mb[4].buffer = v4.data(); mb[4].stride = 8; mb[4].numRows = N;
        s->bindParamBatch(mb, 5);
        s->execute();
    }
    delete s;
    printf("Inserted %d rows, waiting for flush...\n", N);
    std::this_thread::sleep_for(std::chrono::seconds(4));

    int passed = 0, failed = 0;
    printf("\n===== Phase 3 Verification =====\n\n");

    // ── Test 1: WHERE ts range pushdown ──
    {
        int64_t startTs = baseTs + 1000;
        int64_t endTs   = baseTs + 1100;
        std::string sql = "SELECT * FROM qt WHERE ts >= " + std::to_string(startTs)
                        + " AND ts < " + std::to_string(endTs);
        auto r = c.query(sql);
        bool ok = (r.rowCount() == 100);
        printf("Test 1: WHERE ts range [1000,1100)  rows=%d  %s\n",
               r.rowCount(), ok ? "PASS" : "FAIL");
        ok ? ++passed : ++failed;
    }

    // ── Test 2: Column projection — SELECT v1, v3 (skip v2, v4) ──
    {
        int64_t startTs = baseTs + 100;
        int64_t endTs   = baseTs + 110;
        std::string sql = "SELECT v1, v3 FROM qt WHERE ts >= " + std::to_string(startTs)
                        + " AND ts < " + std::to_string(endTs);
        auto r = c.query(sql);
        bool ok = (r.rowCount() == 10);
        printf("Test 2: Column projection SELECT v1,v3  rows=%d  %s\n",
               r.rowCount(), ok ? "PASS" : "FAIL");
        ok ? ++passed : ++failed;
    }

    // ── Test 3: LIMIT + WHERE combined ──
    {
        int64_t startTs = baseTs;
        int64_t endTs   = baseTs + 5000;
        std::string sql = "SELECT * FROM qt WHERE ts >= " + std::to_string(startTs)
                        + " AND ts < " + std::to_string(endTs) + " LIMIT 5";
        auto t0 = nowUs();
        auto r = c.query(sql);
        auto t1 = nowUs();
        bool ok = (r.rowCount() == 5);
        printf("Test 3: LIMIT 5 + WHERE ts range  rows=%d time=%lldus  %s\n",
               r.rowCount(), (long long)(t1 - t0), ok ? "PASS" : "FAIL");
        ok ? ++passed : ++failed;
        // Verify first row is ts=baseTs (ascending order)
        if (ok && r.rowCount() > 0) {
            std::string firstTs = r.get(0, 0).toString();
            bool orderOk = (firstTs == std::to_string(baseTs));
            printf("        first row ts=%s  %s\n", firstTs.c_str(),
                   orderOk ? "PASS" : "FAIL");
            orderOk ? ++passed : ++failed;
        }
    }

    // ── Test 4: COUNT(*) via index (Phase 2 regression) ──
    {
        auto r = c.query("SELECT COUNT(*) FROM qt");
        bool ok = (r.rowCount() == 1 && r.get(0, 0).toString() == "5000");
        printf("Test 4: COUNT(*) via index  result=%s  %s\n",
               r.get(0, 0).toString().c_str(), ok ? "PASS" : "FAIL");
        ok ? ++passed : ++failed;
    }

    // ── Test 5: No WHERE, column projection + LIMIT ──
    {
        auto r = c.query("SELECT v1, v2 FROM qt LIMIT 3");
        bool ok = (r.rowCount() == 3);
        printf("Test 5: SELECT v1,v2 LIMIT 3 (no WHERE)  rows=%d  %s\n",
               r.rowCount(), ok ? "PASS" : "FAIL");
        ok ? ++passed : ++failed;
    }

    // ── Test 6: WHERE ts range that matches NO rows (block skip) ──
    {
        int64_t farTs = baseTs + 100000;  // beyond all data
        std::string sql = "SELECT * FROM qt WHERE ts >= " + std::to_string(farTs);
        auto t0 = nowUs();
        auto r = c.query(sql);
        auto t1 = nowUs();
        bool ok = (r.rowCount() == 0);
        printf("Test 6: WHERE ts beyond data (block skip)  rows=%d time=%lldus  %s\n",
               r.rowCount(), (long long)(t1 - t0), ok ? "PASS" : "FAIL");
        ok ? ++passed : ++failed;
    }

    c.query("DROP DATABASE IF EXISTS p3verify");
    printf("\n===== Results: %d passed, %d failed =====\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
