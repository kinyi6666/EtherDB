/*
 * partial_filter_test.cpp — 部分提取 WHERE 验证 (executor 兜底过滤)
 *
 * 第二项优化(跳过 executor 双重过滤)的安全前提: 仅当 extractColFiltersFromExpr
 * 完整提取 WHERE (filtersComplete) 时才可跳过 evalWhere。本测试验证:
 *   1. WHERE 含 BINARY 字符串列 → 部分提取 → executor 必须完整 evalWhere
 *   2. 纯字符串 WHERE → 不可提取 → executor 全过滤
 *   3. 完整提取的数值 WHERE + LIMIT → 双重过滤跳过路径正确
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

int main(int argc, char* argv[]) {
    uint16_t port = (argc > 1) ? (uint16_t)atoi(argv[1]) : 7040;
    EtDBClient c;
    if (!c.connect("127.0.0.1", port, "root", "etherdbdata", "")) {
        printf("[FAIL] connect\n");
        return 1;
    }

    // 独立库（不 USE），全程用 db.table 前缀 → 同时验证跨库查询路由修复。
    c.query("CREATE DATABASE strtest PRECISION us");
    c.query("CREATE TABLE strtest.t1 (ts TIMESTAMP, col_i INT, col_s BINARY(16))");

    // 插入 6 行: col_i=1..6, col_s 交替 aaa/bbb
    auto* stmt = c.createStmt();
    if (!stmt->prepare("INSERT INTO strtest.t1 VALUES(?,?,?)")) {
        printf("[FAIL] prepare\n");
        return 1;
    }
    for (int i = 1; i <= 6; ++i) {
        int64_t ts = 1750000000000000LL + i * 1000;
        int32_t vi = i;
        char    vs[16] = {0};
        strncpy(vs, (i % 2) ? "aaa" : "bbb", 15);
        EtDBStmt::BindParam bp[3];
        bp[0].type = EtDBStmt::TYPE_TIMESTAMP; bp[0].buffer = &ts; bp[0].length = 8;
        bp[1].type = EtDBStmt::TYPE_INT;       bp[1].buffer = &vi; bp[1].length = 4;
        bp[2].type = EtDBStmt::TYPE_BINARY;    bp[2].buffer = vs; bp[2].length = 16;
        if (!stmt->bindParam(bp, 3) || !stmt->addBatch()) {
            printf("[FAIL] bind row %d\n", i);
            return 1;
        }
    }
    int aff = stmt->execute();
    stmt->close();
    CHECK(aff == 6, "插入 6 行");

    // 1. 混合 WHERE: col_i>3 (可提取) AND col_s='bbb' (字符串, 不可提取)
    //    → filtersComplete=false → executor 必须完整 evalWhere
    //    预期: col_i>3 且 col_s=bbb → i=4,6 → 2 行
    {
        auto r = c.query("SELECT ts,col_i,col_s FROM strtest.t1 WHERE col_i>3 AND col_s='bbb'");
        int n = r.rowCount();
        bool ok = (n == 2);
        if (n == 2) {
            for (int i = 0; i < n; ++i)
                if (r.get(i, 1).iVal != 4 && r.get(i, 1).iVal != 6) ok = false;
        }
        CHECK(ok, "混合 WHERE(数值+字符串): 2 行且 col_i∈{4,6}");
        CHECK(n == 2, "混合 WHERE 行数 == 2");
    }

    // 2. 纯字符串 WHERE: col_s='aaa' → 不可提取 → executor 全过滤
    //    预期: i=1,3,5 → 3 行
    {
        auto r = c.query("SELECT ts,col_i,col_s FROM strtest.t1 WHERE col_s='aaa'");
        int n = r.rowCount();
        bool ok = (n == 3);
        if (n == 3) {
            for (int i = 0; i < n; ++i)
                if (r.get(i, 1).iVal != 1 && r.get(i, 1).iVal != 3 && r.get(i, 1).iVal != 5) ok = false;
        }
        CHECK(ok, "纯字符串 WHERE: 3 行且 col_i∈{1,3,5}");
    }

    // 3. 完整提取数值 WHERE + LIMIT: col_i>2 LIMIT 2 → 双重过滤跳过路径
    //    预期: col_i=3,4 → 2 行 (存储层过滤+LIMIT, executor 跳过 evalWhere)
    {
        auto r = c.query("SELECT ts,col_i FROM strtest.t1 WHERE col_i>2 LIMIT 2");
        int n = r.rowCount();
        bool ok = (n == 2 && r.get(0, 1).iVal == 3 && r.get(1, 1).iVal == 4);
        CHECK(ok, "完整数值 WHERE+LIMIT: 2 行 {3,4}");
    }

    // 4. 完整提取 + 字符串列不在 WHERE (col_s 投影, 数值过滤)
    //    预期: col_i>4 → i=5,6 → 2 行, col_s 值正确
    {
        auto r = c.query("SELECT ts,col_i,col_s FROM strtest.t1 WHERE col_i>4");
        int n = r.rowCount();
        bool ok = (n == 2);
        if (n == 2) {
            if (r.get(0, 1).iVal == 5 && r.get(1, 1).iVal == 6) {
                // col_s 是投影列 (因 SELECT 引用), 值应正确
                ok = (r.get(0, 2).toString().find("aaa") != std::string::npos)
                  && (r.get(1, 2).toString().find("bbb") != std::string::npos);
            } else ok = false;
        }
        CHECK(ok, "数值过滤+字符串投影: 2 行且 col_s 值正确");
    }

    c.query("DROP DATABASE strtest");
    c.close();
    printf("\n%s\n", g_fail == 0 ? "全部通过!" : "存在失败项!");
    return g_fail;
}
