/*
 * proj_verify_test.cpp — 投影紧凑化重构专项验证
 *
 * 覆盖: 多列投影 / SELECT * (无投影) / WHERE 列不在 SELECT / 聚合+SMA /
 *       LIMIT/OFFSET / 字符串列投影
 *
 * 编译:
 *   g++ -std=c++17 -O2 -w -I<src> -I<deps/cJson> \
 *       -o proj_verify_test proj_verify_test.cpp \
 *       EtDBClient.cpp tcpClient.cpp -lpthread -ldl
 */
#include <client/EtDBClient.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>

using namespace ETDB::Client;
static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  [FAIL] %s\n", msg); g_fail++; } \
    else { printf("  [PASS] %s\n", msg); } \
} while (0)

static int64_t countOf(EtDBClient& c, const std::string& sql) {
    auto r = c.query(sql);
    if (r.rowCount() <= 0) { printf("  [INFO] query failed: %s\n", r.error().c_str()); return -1; }
    return r.get(0, 0).iVal;
}

int main(int argc, char* argv[]) {
    uint16_t port = (argc > 1) ? (uint16_t)atoi(argv[1]) : 7040;
    EtDBClient c;
    if (!c.connect("127.0.0.1", port, "root", "etherdbdata", "")) {
        printf("[FAIL] connect\n");
        return 1;
    }
    c.query("USE perftest20");
    const std::string DB = "perftest20";
    const std::string T  = "perf_data_2";   // 19 列, 102 万行

    printf("=== 1. 多列投影 (2/19 列) ===\n");
    {
        auto r = c.query("SELECT ts,col_int FROM " + DB + "." + T + " WHERE col_int>5");
        int64_t n = (r.streamQId() == 0) ? r.rowCount() : 0;
        if (r.streamQId() != 0) {
            std::vector<Value> row; int64_t cnt = 0;
            while (r.fetchRow(row)) cnt++;
            n = cnt;
        }
        CHECK(n == 1019994, "多列投影行数一致");
    }

    printf("=== 2. 更多列投影 (4/19 列) ===\n");
    {
        auto r = c.query("SELECT ts,col_int,col_float,col_double FROM " + DB + "." + T + " WHERE col_int>100000 LIMIT 10");
        CHECK(r.rowCount() == 10, "4列投影 LIMIT 10 行数");
        if (r.rowCount() > 0) {
            bool nonNull = true;
            for (int i = 0; i < r.rowCount(); ++i)
                if (r.get(i, 2).isNull() && r.get(i, 3).isNull()) nonNull = false;
            CHECK(nonNull, "col_float/col_double 有值");
        }
    }

    printf("=== 3. SELECT * (无投影) ===\n");
    {
        auto r = c.query("SELECT * FROM " + DB + "." + T + " LIMIT 5");
        CHECK(r.rowCount() == 5, "SELECT * LIMIT 5 行数");
        CHECK(r.colCount() == 19, "SELECT * 列数 == 19");
    }

    printf("=== 4. WHERE 列不在 SELECT ===\n");
    {
        // 只投影 ts, WHERE 用 col_int (须被投影以支持过滤)
        auto r = c.query("SELECT ts FROM " + DB + "." + T + " WHERE col_int>100000 LIMIT 100");
        int64_t n = r.rowCount();
        bool ok = true;
        for (int i = 0; i < n; ++i)
            if (r.get(i, 0).isNull()) ok = false;
        CHECK(n == 100 && ok, "WHERE 列不在 SELECT: LIMIT 100 且 ts 有值");
    }

    printf("=== 5. 聚合 + WHERE (SMA 计数路径) ===\n");
    {
        int64_t cnt = countOf(c, "SELECT COUNT(*) FROM " + DB + "." + T + " WHERE col_int>5");
        CHECK(cnt == 1019994, "COUNT WHERE col_int>5 == 1019994");
        int64_t sum = countOf(c, "SELECT SUM(col_int) FROM " + DB + "." + T + " WHERE col_int>100000");
        CHECK(sum > 0, "SUM WHERE col_int>100000 非零");
        int64_t mx = countOf(c, "SELECT MAX(col_int) FROM " + DB + "." + T);
        CHECK(mx == 1019999, "MAX(col_int) == 1019999");
        int64_t mn = countOf(c, "SELECT MIN(col_int) FROM " + DB + "." + T);
        CHECK(mn == 0, "MIN(col_int) == 0");
    }

    printf("=== 6. LIMIT/OFFSET 组合 ===\n");
    {
        auto a = c.query("SELECT ts,col_int FROM " + DB + "." + T + " LIMIT 10 OFFSET 500000");
        CHECK(a.rowCount() == 10, "LIMIT 10 OFFSET 500000 行数");
        auto b = c.query("SELECT ts,col_int FROM " + DB + "." + T + " WHERE col_int>100000 LIMIT 10");
        bool ok = true;
        for (int i = 0; i < b.rowCount(); ++i)
            if (b.get(i, 1).iVal <= 100000) ok = false;
        CHECK(ok, "WHERE+LIMIT 无坏行");
    }

    printf("=== 7. 与 SELECT * COUNT 对拍 (所有行数一致) ===\n");
    {
        // 全表行数基准 (走块索引)
        int64_t all = countOf(c, "SELECT COUNT(*) FROM " + DB + "." + T);
        // 用 2 列投影逐行数
        auto r = c.query("SELECT ts,col_int FROM " + DB + "." + T);
        int64_t n = 0;
        if (r.streamQId() != 0) {
            std::vector<Value> row;
            while (r.fetchRow(row)) n++;
        } else n = r.rowCount();
        CHECK(all == n, "投影全量行数 == COUNT(*)");
        printf("  [INFO] COUNT=%lld 投影=%lld\n", (long long)all, (long long)n);
    }

    c.close();
    printf("\n%s\n", g_fail == 0 ? "全部通过!" : "存在失败项!");
    return g_fail;
}
