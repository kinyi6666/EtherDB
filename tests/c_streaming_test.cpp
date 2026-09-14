/*
 * c_streaming_test.cpp — C API 批量流式取数验证 (etdb_fetch_row / etdb_fetch_block)
 *
 * 对标 taos_perf_query 的取数方式（taos_fetch_row 逐行 / taos_fetch_block 批量），
 * 验证 etdb_use_result + etdb_fetch_block 的批量流式返回接口。
 *
 * 编译:
 *   g++ -std=c++17 -O2 -w -I<src> -I<deps/cJson> \
 *       -o c_streaming_test c_streaming_test.cpp \
 *       etdb.cpp EtDBClient.cpp tcpClient.cpp -lpthread -ldl
 *
 * 运行:
 *   ./c_streaming_test [port] [table] [db]
 */

#include <client/etdb.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <chrono>

using Clock = std::chrono::high_resolution_clock;
static int64_t nowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               Clock::now().time_since_epoch()).count();
}

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  [FAIL] %s\n", msg); g_fail++; } \
    else { printf("  [PASS] %s\n", msg); } \
} while (0)

int main(int argc, char* argv[]) {
    uint16_t port  = (argc > 1) ? (uint16_t)atoi(argv[1]) : 7040;
    std::string table = (argc > 2) ? argv[2] : "perf_data_2";
    std::string db    = (argc > 3) ? argv[3] : "perftest20";

    printf("╔══════════════════════════════════════════════╗\n");
    printf("║  C API 批量流式取数测试 (etdb_fetch_*)        ║\n");
    printf("╚══════════════════════════════════════════════╝\n\n");

    etdb_init();
    ETDB_CONN* conn = etdb_connect("127.0.0.1", port, "root", "etherdbdata", "");
    CHECK(conn != nullptr, "etdb_connect");

    ETDB_RESULT* u = etdb_query(conn, ("USE " + db).c_str());
    etdb_free_result(u);

    std::string sql = "SELECT ts,col_int FROM " + db + "." + table + " WHERE col_int>5";

    // ── 1. 预期行数 (COUNT, 走块索引) ──
    ETDB_RESULT* cr = etdb_query(conn, ("SELECT COUNT(*) FROM " + db + "." + table + " WHERE col_int>5").c_str());
    int64_t expected = (cr && etdb_row_count(cr) > 0) ? etdb_get_int64(cr, 0, 0) : -1;
    etdb_free_result(cr);
    printf("[INFO] 预期匹配行数: %lld\n\n", (long long)expected);

    // ── 2. 批量流式取数 (etdb_fetch_block, 对标 taos_fetch_block) ──
    printf("=== 批量流式取数 (etdb_fetch_block) ===\n");
    int64_t t0 = nowUs();
    ETDB_RESULT* res = etdb_use_result(conn, sql.c_str());
    int ncols = etdb_field_count(res);
    CHECK(ncols == 2, "field_count == 2");
    const ETDB_FIELD* fields = etdb_fetch_fields(res);
    CHECK(fields != nullptr && strcmp(fields[0].name, "ts") == 0, "field[0].name == 'ts'");
    CHECK(fields != nullptr && strcmp(fields[1].name, "col_int") == 0, "field[1].name == 'col_int'");
    printf("  [INFO] 字段: %s(type=%d,bytes=%d), %s(type=%d,bytes=%d)\n",
           fields ? fields[0].name : "?", fields ? fields[0].type : -1, fields ? fields[0].bytes : -1,
           fields ? fields[1].name : "?", fields ? fields[1].type : -1, fields ? fields[1].bytes : -1);

    int64_t total = 0;
    int blocks = 0;
    int bad = 0;
    int n;
    while ((n = etdb_fetch_block(res)) > 0) {
        blocks++;
        for (int r = 0; r < n; ++r) {
            int64_t ts  = etdb_get_int64(res, r, 0);
            int64_t ci  = etdb_get_int64(res, r, 1);
            //printf("  [INFO1] row %lld: col_int=%lld ts=%lld\n", (long long)total, (long long)ci, (long long)ts);
            if (ci <= 5) bad++;
            (void)ts;
            total++;
        }
    }
    int64_t t1 = nowUs();
    printf("  [INFO] 共 %lld 行, %d 批, 耗时 %.2f ms\n",
           (long long)total, blocks, (t1 - t0) / 1000.0);
    CHECK(n == 0, "fetch_block 最终返回 0 (取完)");
    CHECK(total == expected, "批量流式行数一致");
    CHECK(bad == 0, "无坏行 (col_int<=5)");
    etdb_free_result(res);

    // ── 3. 逐行流式取数 (etdb_fetch_row, 对标 taos_fetch_row) ──
    printf("\n=== 逐行流式取数 (etdb_fetch_row) ===\n");
    t0 = nowUs();
    res = etdb_use_result(conn, sql.c_str());
    total = 0; bad = 0;
    int rc;
    while ((rc = etdb_fetch_row(res)) == 1) {
        int64_t ci = etdb_get_int64(res, 0, 1);
        //printf("  [INFO] row %lld: col_int=%lld\n", (long long)total, (long long)ci);
        if (ci <= 5) bad++;
        total++;
    }
    t1 = nowUs();
    printf("  [INFO] 共 %lld 行, 耗时 %.2f ms\n", (long long)total, (t1 - t0) / 1000.0);
    CHECK(rc == 0, "fetch_row 最终返回 0 (取完)");
    CHECK(total == expected, "逐行流式行数一致");
    CHECK(bad == 0, "无坏行 (col_int<=5)");
    etdb_free_result(res);

    // ── 4. 字符串/空值访问不崩溃 (小结果) ──
    printf("\n=== 小结果 one-shot 回归 ===\n");
    res = etdb_query(conn, ("SELECT ts,col_int FROM " + db + "." + table + " LIMIT 3").c_str());
    CHECK(etdb_row_count(res) == 3, "LIMIT 3 行数");
    for (int r = 0; r < etdb_row_count(res); ++r) {
        const char* v0 = etdb_get_value(res, r, 0);
        const char* v1 = etdb_get_value(res, r, 1);
        CHECK(v0 && v1 && v0[0] && v1[0], "get_value 有值");
    }
    etdb_free_result(res);

    etdb_close(conn);
    etdb_cleanup();

    printf("\n%s\n", g_fail == 0 ? "全部通过!" : "存在失败项!");
    return g_fail;
}
