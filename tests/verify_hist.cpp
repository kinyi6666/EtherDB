// Verify historical insert: files named by historical ts, COUNT correct
#include <client/EtDBClient.h>
#include <cstdio>
#include <cstdlib>
#include <ctime>
using namespace ETDB::Client;

int main() {
    EtDBClient cli;
    if (!cli.connect("127.0.0.1", 7040)) { printf("connect failed\n"); return 1; }
    cli.query("CREATE DATABASE IF NOT EXISTS histdb");
    cli.query("USE histdb");
    cli.query("CREATE TABLE IF NOT EXISTS hist_t (ts TIMESTAMP, val INT)");

    // Insert NORMAL data with current ts
    time_t now = time(nullptr);
    for (int i = 0; i < 5; i++) {
        char sql[256];
        snprintf(sql, sizeof(sql), "INSERT INTO hist_t VALUES(%lld, %d)", (long long)(now+i)*1000, i);
        auto r = cli.query(sql);
        printf("normal insert %d: %s\n", i, r.error().empty() ? "OK" : r.error().c_str());
    }

    // Insert HISTORICAL data (ts 1 hour ago)
    long long histBase = (long long)(now - 3600) * 1000;
    for (int i = 0; i < 5; i++) {
        char sql[256];
        snprintf(sql, sizeof(sql), "INSERT INTO hist_t VALUES(%lld, %d)", histBase + i*1000, i+100);
        auto r = cli.query(sql);
        printf("hist insert %d: %s\n", i, r.error().empty() ? "OK" : r.error().c_str());
    }

    auto c = cli.query("SELECT COUNT(*) FROM hist_t");
    printf("COUNT(*): %s\n", c.rowCount()>0 ? c.get(0,0).toString().c_str() : "N/A");
    auto m = cli.query("SELECT MIN(ts), MAX(ts) FROM hist_t");
    if (m.rowCount()>0) printf("ts range: %s ~ %s\n", m.get(0,0).toString().c_str(), m.get(0,1).toString().c_str());
    cli.close();
    return 0;
}
