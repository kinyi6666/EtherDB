/*
 * query_perf_test.cpp — 查询性能对比测试
 * 测试 SELECT ... LIMIT ... OFFSET 在不同偏移量下的性能差异
 *
 * Usage:
 *   # Terminal 1: 启动服务端
 *   ./src/bin/etherdb_server -p 7040
 *
 *   # Terminal 2: 编译 + 运行
 *   cd build && make query_perf_test && ./query_perf_test [port]
 */

#include <client/EtDBClient.h>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <thread>
#include <string>

using namespace ETDB::Client;
using Clock = std::chrono::steady_clock;

static int64_t nowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count();
}

static double bench(EtDBClient& c, const std::string& sql, int runs, int* outRows) {
    double totalUs = 0;
    int rows = -1;
    for (int i = 0; i < runs; ++i) {
        auto t0 = nowUs();
        auto r = c.query(sql);
        auto t1 = nowUs();
        totalUs += (t1 - t0);
        if (rows < 0) rows = r.rowCount();
    }
    if (outRows) *outRows = rows;
    return totalUs / runs;
}

int main(int argc, char* argv[]) {
    uint16_t port = (argc > 1) ? (uint16_t)atoi(argv[1]) : 7040;
    const int RUNS = 3;

    printf("=== 查询性能测试（不同 OFFSET） ===\n\n");

    EtDBClient c;
    if (!c.connect("127.0.0.1", port)) {
        printf("FAIL: connect failed\n");
        return 1;
    }

    c.query("USE perftest20");

    struct TestCase {
        const char* desc;
        const char* sql;
    };

    TestCase cases[] = {
        {"OFFSET 0",      "SELECT * FROM perf_data_1 LIMIT 10 OFFSET 0"},
        {"OFFSET 100",    "SELECT * FROM perf_data_1 LIMIT 10 OFFSET 100"},
        {"OFFSET 1000",   "SELECT * FROM perf_data_1 LIMIT 10 OFFSET 1000"},
        {"OFFSET 10000",  "SELECT * FROM perf_data_1 LIMIT 10 OFFSET 10000"},
        {"OFFSET 100000", "SELECT * FROM perf_data_1 LIMIT 10 OFFSET 100000"},
        {"OFFSET 200000", "SELECT * FROM perf_data_1 LIMIT 10 OFFSET 200000"},
        {"OFFSET 300000", "SELECT * FROM perf_data_1 LIMIT 10 OFFSET 300000"},
        {"OFFSET 400000", "SELECT * FROM perf_data_1 LIMIT 10 OFFSET 400000"},
        {"OFFSET 500000", "SELECT * FROM perf_data_1 LIMIT 10 OFFSET 500000"},
        {"COUNT(*)",      "SELECT COUNT(*) FROM perf_data_1"},
        {"COUNT col_int<5","SELECT COUNT(*) FROM perf_data_1 WHERE col_int < 5"},
        {"WHERE col_int<5","SELECT * FROM perf_data_1 WHERE col_int < 5 LIMIT 10"},
    };

    printf("%-16s | %8s | %10s | %6s\n", "测试", "耗时(ms)", "平均(us)", "行数");
    printf("%-16s-+-%8s-+-%10s-+-%6s\n", "----------------", "--------", "----------", "------");

    for (auto& tc : cases) {
        int rows = 0;
        double avgUs = bench(c, tc.sql, RUNS, &rows);
        printf("%-16s | %8.2f | %10.0f | %6d\n", tc.desc, avgUs/1000.0, avgUs, rows);
        fflush(stdout);
    }

    printf("\n=== 完成 ===\n");
    return 0;
}
