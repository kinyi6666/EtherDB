// 1) insert normal rows (establish lastKey)  2) insert historical rows (trigger)
#include <client/EtDBClient.h>
#include <cstdio>
#include <cstdlib>
#include <ctime>
using namespace ETDB::Client;

static bool exec(EtDBClient& cli, const char* sql) {
    auto r = cli.query(sql);
    bool ok = r.error().empty();
    if (!ok) printf("  ERR: %s -> %s\n", sql, r.error().c_str());
    return ok;
}

int main() {
    EtDBClient cli;
    if (!cli.connect("127.0.0.1", 7040)) { printf("connect failed\n"); return 1; }
    exec(cli, "USE perftest20");
    exec(cli, "CREATE TABLE IF NOT EXISTS hist_t (ts TIMESTAMP, val INT)");

    time_t now = time(nullptr);
    // Normal rows (current ts) — establish lastKey
    for (int i = 0; i < 3; i++) {
        char sql[256];
        snprintf(sql, sizeof(sql), "INSERT INTO hist_t VALUES(%lld, %d)", (long long)(now+i)*1000, i);
        if (!exec(cli, sql)) return 1;
    }
    printf("  normal 3 rows OK\n");

    // Historical rows (1 hour ago) — should trigger historical mode
    long long histBase = (long long)(now - 3600) * 1000;
    for (int i = 0; i < 3; i++) {
        char sql[256];
        snprintf(sql, sizeof(sql), "INSERT INTO hist_t VALUES(%lld, %d)", histBase + i, i+100);
        if (!exec(cli, sql)) return 1;
        printf("  hist insert %d OK\n", i);
    }
    sleep(1);
    auto c = cli.query("SELECT COUNT(*) FROM hist_t");
    printf("COUNT(*): %s\n", c.rowCount()>0 ? c.get(0,0).toString().c_str() : "N/A");
    cli.close();
    return 0;
}
