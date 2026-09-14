/*
 * cross_db_test.cpp — 跨库查询路由验证
 *
 * 客户端 query() 之前只用当前 USE 库解析 dbId，SELECT ... FROM other_db.tbl
 * 会路由到错误 dbnode（新库建表后查询返回 -1/查不到数据）。修复后：
 *   - 解析 SQL 的 db.table 前缀 → 用目标库 dbId 路由
 *   - INSERT 本已按表 meta 的 dbId 路由
 * 本测试不 USE 目标库，直接跨库建表/插入/查询验证。
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

    const std::string DB = "crossdb";

    // 不 USE crossdb — 全程用 db.table 前缀
    c.query("CREATE DATABASE " + DB + " PRECISION us");
    auto cr = c.query("CREATE TABLE " + DB + ".t1 (ts TIMESTAMP, col_i INT, col_s BINARY(16))");
    CHECK(!cr.success() || cr.error().empty(), "CREATE TABLE crossdb.t1");

    // INSERT (EtDBStmt 按表 meta dbId 路由)
    auto* stmt = c.createStmt();
    if (!stmt->prepare("INSERT INTO " + DB + ".t1 VALUES(?,?,?)")) {
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
        if (!stmt->bindParam(bp, 3) || !stmt->addBatch()) { printf("[FAIL] bind\n"); return 1; }
    }
    int aff = stmt->execute();
    stmt->close();
    CHECK(aff == 6, "跨库 INSERT 6 行");

    // 1. 跨库全量查询 (未 USE crossdb)
    {
        auto r = c.query("SELECT ts,col_i,col_s FROM " + DB + ".t1");
        CHECK(r.error().empty(), "跨库 SELECT 无错误");
        CHECK(r.rowCount() == 6, "跨库 SELECT 行数 == 6");
    }

    // 2. 跨库 + 过滤
    {
        auto r = c.query("SELECT ts,col_i FROM " + DB + ".t1 WHERE col_i>3");
        CHECK(r.rowCount() == 3, "跨库 WHERE col_i>3 行数 == 3");
    }

    // 3. 跨库 COUNT
    {
        auto r = c.query("SELECT COUNT(*) FROM " + DB + ".t1");
        CHECK(r.rowCount() == 1 && r.get(0, 0).iVal == 6, "跨库 COUNT == 6");
    }

    // 4. 跨库 LIMIT
    {
        auto r = c.query("SELECT ts,col_i FROM " + DB + ".t1 LIMIT 2");
        CHECK(r.rowCount() == 2, "跨库 LIMIT 2 行数 == 2");
    }

    // 5. USE 到 crossdb 后无前缀查询仍正常
    {
        auto u = c.query("USE " + DB);
        CHECK(u.error().empty(), "USE crossdb");
        auto r = c.query("SELECT COUNT(*) FROM t1");
        CHECK(r.rowCount() == 1 && r.get(0, 0).iVal == 6, "USE 后无前缀 COUNT == 6");
    }

    // 6. 切回 perftest20 后跨库查 crossdb 仍正确
    {
        c.query("USE perftest20");
        auto r = c.query("SELECT COUNT(*) FROM " + DB + ".t1");
        CHECK(r.rowCount() == 1 && r.get(0, 0).iVal == 6, "切库后跨库 COUNT == 6");
    }

    c.query("DROP DATABASE " + DB);
    c.close();
    printf("\n%s\n", g_fail == 0 ? "全部通过!" : "存在失败项!");
    return g_fail;
}
