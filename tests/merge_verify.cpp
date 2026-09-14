// Verify values across the disk/memtable merge boundary region.
#include <client/EtDBClient.h>
#include <cstdio>
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

    // col_int 809990..810010 — straddles whatever disk/mem boundary exists.
    auto r = c.query("SELECT ts, col_int FROM perftest20.perf_data_1 WHERE col_int>=809990 AND col_int<=810010");
    CK(r.success(), "boundary query");
    int64_t n = 0;
    bool monotonic = true, exact = true;
    int64_t prev = -1;
    std::vector<Value> row;
    while (r.fetchRow(row)) {
        int64_t ci = row[1].iVal;
        if (ci != 809990 + n) { exact = false; printf("    row %lld col_int=%lld (expect %lld)\n", (long long)n, (long long)ci, (long long)(809990 + n)); }
        if (prev >= 0 && row[0].iVal <= prev) monotonic = false;
        prev = row[0].iVal;
        ++n;
    }
    CK(n == 21, "21 rows (809990..810010)");
    CK(exact, "col_int values exact in order");
    CK(monotonic, "ts monotonically increasing");

    // Full-stream ordered check: every batch boundary must keep ts ascending.
    auto r2 = c.query("SELECT ts, col_int FROM perftest20.perf_data_1 WHERE col_int>=1019000");
    CK(r2.success(), "tail query");
    int64_t prevTs = -1; int64_t prevCi = -1;
    bool asc = true, seq = true;
    std::vector<Value> row2;
    while (r2.fetchRow(row2)) {
        if (prevTs >= 0 && row2[0].iVal < prevTs) asc = false;
        if (prevCi >= 0 && row2[1].iVal != prevCi + 1) seq = false;
        prevTs = row2[0].iVal; prevCi = row2[1].iVal;
    }
    CK(asc, "ts ascending across batches");
    CK(seq, "col_int sequential (1019000..1019999)");

    c.close();
    printf("\n%s (failures=%d)\n", gFail == 0 ? "MERGE BOUNDARY PASS" : "MERGE BOUNDARY FAILED", gFail);
    return gFail == 0 ? 0 : 1;
}
