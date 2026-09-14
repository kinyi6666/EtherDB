// verify_types_restart.cpp — 重启后验证 typed_test 表数据（磁盘路径）
#include <client/EtDBClient.h>
#include <cstdio>

using namespace ETDB::Client;

int main() {
    EtDBClient c;
    if (!c.connect("127.0.0.1", 7040, "root", "etherdbdata", "")) {
        printf("[FAIL] 连接失败\n");
        return 1;
    }
    auto ru = c.query("USE typetest");
    if (!ru.success()) { printf("[FAIL] USE typetest: %s\n", ru.error().c_str()); return 1; }

    auto r = c.query("SELECT ts, u1, u2, u3, b1, n1 FROM typed_test");
    printf("[%s] SELECT 返回 %d 行\n", r.success() ? "PASS" : "FAIL", r.rowCount());
    for (size_t i = 0; i < r.rows().size(); ++i) {
        auto& row = r.rows()[i];
        printf("  row%zu: ts=%lld u1=%lld u2=%llu u3=%lld b1='%s' n1='%s'\n",
               i, row[0].iVal, row[1].iVal, row[2].uVal, row[3].iVal,
               row[4].sVal.c_str(), row[5].sVal.c_str());
    }
    bool ok = r.success() && r.rowCount() == 2;
    if (r.rowCount() == 2) {
        auto& row0 = r.rows()[0];
        auto& row1 = r.rows()[1];
        ok = row0[1].iVal == 4294967295LL && row0[2].uVal == 18446744073709551615ULL
             && row0[3].iVal == 255
             && row0[4].sVal == "hello world" && row0[5].sVal == "世界"
             && row1[1].iVal == 123 && row1[2].uVal == 456ULL && row1[4].sVal == "abc";
    }
    printf("=== %s ===\n", ok ? "重启持久化验证 PASS" : "重启持久化验证 FAIL");
    return ok ? 0 : 1;
}
