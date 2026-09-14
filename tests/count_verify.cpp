// Verify which count path is off by one: SMA fast-count vs full scan vs stream.
#include <client/EtDBClient.h>
#include <cstdio>
#include <cstring>
#include <string>

using namespace ETDB::Client;

int main(int argc, char* argv[]) {
    uint16_t port = (argc > 1) ? (uint16_t)atoi(argv[1]) : 7040;
    EtDBClient c;
    if (!c.connect("127.0.0.1", port, "root", "etherdbdata", "")) {
        printf("connect failed\n"); return 1;
    }
    c.query("USE perftest20");

    auto runCount = [&](const char* label, const std::string& sql) {
        auto r = c.query(sql);
        if (!r.success()) { printf("%-46s: ERROR %s\n", label, r.error().c_str()); return; }
        int64_t v = (r.rowCount() > 0) ? r.get(0, 0).iVal : -1;
        printf("%-46s: %lld\n", label, (long long)v);
    };
    auto runStreamCount = [&](const char* label, const std::string& sql) {
        auto r = c.query(sql);
        if (!r.success()) { printf("%-46s: ERROR %s\n", label, r.error().c_str()); return; }
        int64_t n = 0;
        std::vector<Value> row;
        while (r.fetchRow(row)) ++n;
        printf("%-46s: %lld\n", label, (long long)n);
    };

    runCount  ("SMA count  col_int>5",  "SELECT COUNT(*) FROM perftest20.perf_data_1 WHERE col_int>5");
    runCount  ("SMA count  col_int<=5", "SELECT COUNT(*) FROM perftest20.perf_data_1 WHERE col_int<=5");
    runCount  ("SMA count  col_int=5",  "SELECT COUNT(*) FROM perftest20.perf_data_1 WHERE col_int=5");
    runCount  ("SMA count  col_int=0",  "SELECT COUNT(*) FROM perftest20.perf_data_1 WHERE col_int=0");
    runCount  ("full scan count all",   "SELECT COUNT(*) FROM perftest20.perf_data_1 WHERE col_int>5 OR (col_int<=5 AND 0=1)");
    runCount  ("full scan count <=5",   "SELECT COUNT(*) FROM perftest20.perf_data_1 WHERE col_int<=5 OR (col_int>5 AND 0=1)");
    runStreamCount("stream scan col_int<=5", "SELECT col_int FROM perftest20.perf_data_1 WHERE col_int<=5");
    runStreamCount("stream scan col_int>5",  "SELECT col_int FROM perftest20.perf_data_1 WHERE col_int>5");

    c.close();
    return 0;
}
