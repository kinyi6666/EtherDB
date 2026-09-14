// Value-correctness verification for the row-major Rows fix.
#include <client/EtDBClient.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace ETDB::Client;

static int gFail = 0;
#define CK(cond, msg) do { if (cond) printf("  [PASS] %s\n", msg); \
    else { printf("  [FAIL] %s\n", msg); gFail++; } } while (0)

int main(int argc, char* argv[]) {
    uint16_t port = (argc > 1) ? (uint16_t)atoi(argv[1]) : 7040;
    EtDBClient c;
    if (!c.connect("127.0.0.1", port, "root", "etherdbdata", "")) { printf("connect failed\n"); return 1; }
    c.query("USE perftest20");

    // ── 1. SELECT ts, col_int WHERE col_int<=9 → exactly 10 rows, col_int = 0..9 ──
    {
        auto r = c.query("SELECT ts, col_int FROM perftest20.perf_data_1 WHERE col_int<=9");
        CK(r.success(), "query col_int<=9");
        int64_t n = 0;
        bool ok = true;
        std::vector<Value> row;
        while (r.fetchRow(row)) {
            int64_t ci = row[1].iVal;
            if (ci != n) { ok = false; printf("    row %lld col_int=%lld (expected %lld)\n", (long long)n, (long long)ci, (long long)n); }
            ++n;
        }
        CK(n == 10, "col_int<=9 returns exactly 10 rows");
        CK(ok, "col_int values are 0..9 in order");
    }

    // ── 2. SELECT * full projection: check string/BINARY decode + all columns ──
    {
        auto r = c.query("SELECT * FROM perftest20.perf_data_1 WHERE col_int=1");
        CK(r.success(), "query col_int=1 (SELECT *)");
        int64_t n = 0;
        std::vector<Value> row;
        while (r.fetchRow(row)) { ++n; }
        CK(n == 1, "exactly 1 row with col_int=1");
        // re-fetch to inspect values
        r = c.query("SELECT * FROM perftest20.perf_data_1 WHERE col_int=1");
        if (r.fetchRow(row)) {
            CK(row.size() == 10, "SELECT * returns 10 columns");
            CK(row[0].iVal > 0, "ts > 0");
            CK(row[1].iVal == 1, "col_int == 1");
            CK(row[2].iVal == 1, "col_bigint == 1 (round0 offset=0 i=1)");
        }
    }

    // ── 3. SMA aggregates: MAX/MIN/SUM over col_int ──
    {
        auto r = c.query("SELECT MAX(col_int), MIN(col_int), SUM(col_int) FROM perftest20.perf_data_1");
        CK(r.success(), "MAX/MIN/SUM query");
        if (r.rowCount() > 0) {
            int64_t mx = (int64_t)r.get(0, 0).fVal;
            int64_t mn = (int64_t)r.get(0, 1).fVal;
            int64_t sm = (int64_t)r.get(0, 2).fVal;
            // col_int = 0..1019999 (each once)
            int64_t expectSum = (int64_t)1019999 * 1020000 / 2;
            CK(mx == 1019999, "MAX(col_int)=1019999");
            CK(mn == 0, "MIN(col_int)=0");
            CK(sm == expectSum, "SUM(col_int)=520199490000");
            printf("    MAX=%lld MIN=%lld SUM=%lld (expect %lld)\n", (long long)mx, (long long)mn, (long long)sm, (long long)expectSum);
        }
    }

    // ── 4. GROUP BY over full scan (executor aggregates) ──
    {
        auto r = c.query("SELECT col_int, COUNT(*) FROM perftest20.perf_data_1 WHERE col_int>1019995 GROUP BY col_int");
        CK(r.success(), "GROUP BY query");
        int64_t groups = 0, rows = 0;
        std::vector<Value> row;
        while (r.fetchRow(row)) {
            ++groups;
            rows += row[1].iVal;  // count per group
        }
        CK(groups == 4, "col_int 1019996..1019999 → 4 groups");
        CK(rows == 4, "4 rows total in groups");
    }

    // ── 5. Streaming values across batch boundaries ──
    {
        auto r = c.query("SELECT ts, col_int FROM perftest20.perf_data_1 WHERE col_int>=1019995 AND col_int<=1019999");
        CK(r.success(), "tail-range query");
        int64_t n = 0;
        std::vector<Value> row;
        while (r.fetchRow(row)) { ++n; }
        CK(n == 5, "col_int 1019995..1019999 → 5 rows");
    }

    c.close();
    printf("\n%s (failures=%d)\n", gFail == 0 ? "VALUE VERIFY PASS" : "VALUE VERIFY FAILED", gFail);
    return gFail == 0 ? 0 : 1;
}
