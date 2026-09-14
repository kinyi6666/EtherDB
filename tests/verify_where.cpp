// Verify COUNT(*) with WHERE col filter after append
#include <client/EtDBClient.h>
#include <cstdio>
using namespace ETDB::Client;

static void run(EtDBClient& cli, const char* sql) {
    auto r = cli.query(sql);
    if (r.error().empty() && r.rowCount() > 0) {
        printf("%-55s => %s\n", sql, r.get(0,0).toString().c_str());
    } else {
        printf("%-55s => ERROR: %s (rows=%d)\n", sql, r.error().c_str(), r.rowCount());
    }
}

int main() {
    EtDBClient cli;
    if (!cli.connect("127.0.0.1", 7040)) { printf("connect failed\n"); return 1; }
    run(cli, "USE perftest20");
    run(cli, "SELECT COUNT(*) FROM perf_data_1");
    run(cli, "SELECT COUNT(*) FROM perf_data_1 WHERE col_int > 5");
    run(cli, "SELECT COUNT(*) FROM perf_data_1 WHERE col_int < 5");
    run(cli, "SELECT COUNT(*) FROM perf_data_1 WHERE col_int <= 5");
    run(cli, "SELECT COUNT(*) FROM perf_data_1 WHERE col_int >= 5");
    run(cli, "SELECT COUNT(*) FROM perf_data_1 WHERE col_int = 5");
    run(cli, "SELECT COUNT(*) FROM perf_data_1 WHERE col_int < 0");
    cli.close();
    return 0;
}
