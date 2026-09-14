/*
 * typed_types_test.cpp — 验证 UNSIGNED + BINARY + NCHAR 类型支持
 *
 * 测试:
 *   1. 建表: ts + UNSIGNED INT/BIGINT/TINYINT + BINARY(16) + NCHAR(8)
 *   2. 插入: SQL INSERT（客户端打包路径）
 *   3. 查询: SELECT * / WHERE 过滤（UNSIGNED 数值 + BINARY 字符串）
 */

#include <client/EtDBClient.h>
#include <cstdio>
#include <cstring>
#include <string>

using namespace ETDB::Client;

static int failures = 0;
static void check(const char* name, bool ok, const std::string& detail = "") {
    if (ok) printf("[PASS] %s\n", name);
    else    { printf("[FAIL] %s %s\n", name, detail.c_str()); failures++; }
}

int main() {
    printf("=== 类型支持测试 (UNSIGNED + BINARY + NCHAR) ===\n");

    EtDBClient c;
    if (!c.connect("127.0.0.1", 7040, "root", "etherdbdata", "")) {
        printf("[FAIL] 连接失败\n");
        return 1;
    }
    printf("[OK] 已连接\n");

    // 1. 建库建表
    c.query("CREATE DATABASE typetest");
    auto ru = c.query("USE typetest");
    check("USE typetest", ru.success(), ru.error());

    // 删除旧表（若存在）后重建
    c.query("DROP TABLE typed_test");
    auto r1 = c.query("CREATE TABLE typed_test (ts TIMESTAMP, u1 UNSIGNED INT, "
                      "u2 UNSIGNED BIGINT, u3 UNSIGNED TINYINT, "
                      "b1 BINARY(16), n1 NCHAR(8))");
    check("CREATE TABLE typed_test", r1.success(), r1.error());

    // 2. 插入两行
    int64_t ts = 1750000000000LL;
    char sql[1024];
    // 行1: UNSIGNED 最大值 + 中文 NCHAR
    snprintf(sql, sizeof(sql),
        "INSERT INTO typed_test VALUES(%lld, 4294967295, 18446744073709551615, 255, 'hello world', '世界')",
        (long long)ts);
    auto r2 = c.query(sql);
    check("INSERT 行1 (UNSIGNED最大值+中文NCHAR)", r2.success(), r2.error());

    // 行2: 小值 + 英文
    snprintf(sql, sizeof(sql),
        "INSERT INTO typed_test VALUES(%lld, 123, 456, 7, 'abc', 'test')", (long long)ts + 1);
    auto r3 = c.query(sql);
    check("INSERT 行2 (小值+BINARY)", r3.success(), r3.error());

    // 3. 查询全部
    auto r4 = c.query("SELECT ts, u1, u2, u3, b1, n1 FROM typed_test");
    check("SELECT 6列返回2行", r4.success() && r4.rowCount() == 2, r4.error());
    if (r4.rowCount() == 2) {
        // 行1: u1=4294967295, u2=18446744073709551615, u3=255, b1='hello world', n1='世界'
        auto& row0 = r4.rows()[0];
        check("u1=4294967295", row0.size() > 1 && row0[1].iVal == 4294967295LL,
              "u1=" + std::to_string(row0.size() > 1 ? row0[1].iVal : -1));
        check("u2=18446744073709551615 (uint64)", row0.size() > 2 && row0[2].uVal == 18446744073709551615ULL,
              "u2=" + std::to_string(row0.size() > 2 ? (long long)row0[2].uVal : -1));
        check("u3=255", row0.size() > 3 && row0[3].iVal == 255,
              "u3=" + std::to_string(row0.size() > 3 ? row0[3].iVal : -1));
        check("b1='hello world'", row0.size() > 4 && row0[4].sVal == "hello world",
              "b1='" + (row0.size() > 4 ? row0[4].sVal : "") + "'");
        check("n1='世界'", row0.size() > 5 && row0[5].sVal == "世界",
              "n1='" + (row0.size() > 5 ? row0[5].sVal : "") + "'");
        // 行2: u1=123, u2=456
        auto& row1 = r4.rows()[1];
        check("行2 u1=123", row1.size() > 1 && row1[1].iVal == 123);
        check("行2 u2=456 (uint64)", row1.size() > 2 && row1[2].uVal == 456ULL);
        check("行2 b1='abc'", row1.size() > 4 && row1[4].sVal == "abc");
        check("行2 n1='test'", row1.size() > 5 && row1[5].sVal == "test");
    }

    // 4. WHERE 过滤 UNSIGNED 数值（u1 > 1000 → 只返回行1）
    auto r5 = c.query("SELECT ts, u1 FROM typed_test WHERE u1 > 1000");
    check("WHERE u1>1000 返回1行", r5.rowCount() == 1,
          "rows=" + std::to_string(r5.rowCount()));
    if (r5.rowCount() == 1) {
        check("WHERE 结果 u1=4294967295", r5.get(0, 1).iVal == 4294967295LL);
    }

    // 5. WHERE 过滤 BINARY 字符串（executor 层，b1='abc' → 行2）
    auto r6 = c.query("SELECT ts, b1 FROM typed_test WHERE b1 = 'abc'");
    check("WHERE b1='abc' 返回1行", r6.rowCount() == 1,
          "rows=" + std::to_string(r6.rowCount()));
    if (r6.rowCount() == 1) {
        check("WHERE b1 结果='abc'", r6.get(0, 1).sVal == "abc");
    }

    // 6. UNSIGNED 范围过滤（u3 >= 200 → 行1）
    auto r7 = c.query("SELECT ts, u3 FROM typed_test WHERE u3 >= 200");
    check("WHERE u3>=200 返回1行", r7.rowCount() == 1,
          "rows=" + std::to_string(r7.rowCount()));

    // 7. UBIGINT 过滤（u2 > 1000 → 行1；u2 = 最大值 → 行1）
    auto r8 = c.query("SELECT ts, u2 FROM typed_test WHERE u2 > 1000");
    check("WHERE u2>1000 返回1行", r8.rowCount() == 1,
          "rows=" + std::to_string(r8.rowCount()));
    if (r8.rowCount() == 1) {
        check("WHERE u2 结果=18446744073709551615", r8.get(0, 1).uVal == 18446744073709551615ULL);
    }
    // 大数字面量（> INT64_MAX）直接比较
    auto r9 = c.query("SELECT ts, u2 FROM typed_test WHERE u2 = 18446744073709551615");
    check("WHERE u2=最大值 返回1行", r9.rowCount() == 1,
          "rows=" + std::to_string(r9.rowCount()));
    if (r9.rowCount() == 1) {
        check("WHERE u2=最大值 结果正确", r9.get(0, 1).uVal == 18446744073709551615ULL);
    }

    c.close();
    printf("\n=== %s (%d failures) ===\n", failures == 0 ? "全部通过" : "有失败", failures);
    return failures;
}
