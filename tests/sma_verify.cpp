// Verify SMA aggregate fast paths (SUM/MIN/MAX via .sma, with/without filters).
#include <client/EtDBClient.h>
#include <cstdio>
#include <string>

using namespace ETDB::Client;
static int gFail = 0;
#define CK(cond, msg) do { if (cond) printf("  [PASS] %s\n", msg); \
    else { printf("  [FAIL] %s\n", msg); gFail++; } } while (0)

int main(int argc, char* argv[]) {
    uint16_t port = (argc > 1) ? (uint16_t)atoi(argv[1]) : 7040;
    EtDBClient c;
    if (!c.connect("127.0.0.1", port, "root", "etherdbdata", "")) { printf("connect failed\n"); return 1; }
    c.query("USE perftest20");

    auto runAggr = [&](const char* label, const std::string& sql, double expect) {
        auto r = c.query(sql);
        if (!r.success()) { printf("  [FAIL] %s: %s\n", label, r.error().c_str()); gFail++; return; }
        int64_t v = r.get(0, 0).iVal;
        bool ok = (v == (int64_t)expect);
        CK(ok, label);
        if (!ok) printf("    got %f expect %f\n", v, expect);
    };

    // Single-aggregate → SMA fast path (no WHERE)
    runAggr("SMA SUM(col_int) = 520199490000", "SELECT SUM(col_int) FROM perftest20.perf_data_1", 520199490000.0);
    runAggr("SMA MAX(col_int) = 1019999",      "SELECT MAX(col_int) FROM perftest20.perf_data_1", 1019999.0);
    runAggr("SMA MIN(col_int) = 0",            "SELECT MIN(col_int) FROM perftest20.perf_data_1", 0.0);

    // SMA with column filter: col_int>100000  → col_int 100001..1019999 = 919999 rows
    // sum(100001..1019999) = (100001+1019999)*919999/2 = 1120000*919999/2 = 560000*919999
    double sumHi = (100001.0 + 1019999.0) * 919999.0 / 2.0;
    runAggr("SMA SUM(col_int) WHERE col_int>100000", "SELECT SUM(col_int) FROM perftest20.perf_data_1 WHERE col_int>100000", sumHi);
    runAggr("SMA MAX(col_int) WHERE col_int>100000", "SELECT MAX(col_int) FROM perftest20.perf_data_1 WHERE col_int>100000", 1019999.0);

    c.close();
    printf("\n%s (failures=%d)\n", gFail == 0 ? "SMA AGGR PASS" : "SMA AGGR FAILED", gFail);
    return gFail == 0 ? 0 : 1;
}
