#include <client/EtDBClient.h>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <vector>
using namespace ETDB::Client;

static int bulkInsert(EtDBClient& cli, const char* tbl, const std::vector<int64_t>& ts, const std::vector<int32_t>& val) {
    EtDBStmt* stmt = cli.createStmt();
    if (!stmt->prepare(std::string("INSERT INTO ") + tbl + " VALUES(?, ?)")) { printf("prepare failed\n"); return -1; }
    EtDBStmt::MultiBind binds[2];
    memset(binds, 0, sizeof(binds));
    binds[0].type = EtDBStmt::TYPE_TIMESTAMP;
    binds[0].buffer = (void*)ts.data();
    binds[0].stride = sizeof(int64_t);
    binds[0].numRows = (int)ts.size();
    binds[1].type = EtDBStmt::TYPE_INT;
    binds[1].buffer = (void*)val.data();
    binds[1].stride = sizeof(int32_t);
    binds[1].numRows = (int)val.size();
    if (!stmt->bindParamBatch(binds, 2)) { printf("bind failed\n"); return -1; }
    return stmt->execute();
}

int main() {
    EtDBClient cli;
    if (!cli.connect("127.0.0.1", 7040)) { printf("connect failed\n"); return 1; }
    cli.query("CREATE DATABASE IF NOT EXISTS histdb");
    cli.query("USE histdb");
    cli.query("CREATE TABLE IF NOT EXISTS hist_bulk (ts TIMESTAMP, val INT)");

    time_t now = time(nullptr);
    int N = 150000;
    // Normal: ts in future within 1 day (ms precision, i ms apart)
    std::vector<int64_t> tsN; std::vector<int32_t> valN;
    for (int i = 0; i < N; i++) { tsN.push_back((long long)(now)*1000 + i); valN.push_back(i); }
    int aN = bulkInsert(cli, "hist_bulk", tsN, valN);
    printf("normal bulk affected=%d\n", aN);

    // Historical: ts 1 hour ago (within 1-day window)
    long long histBase = (long long)(now - 3600) * 1000;
    std::vector<int64_t> tsH; std::vector<int32_t> valH;
    for (int i = 0; i < N; i++) { tsH.push_back(histBase + i); valH.push_back(i+1000); }
    int aH = bulkInsert(cli, "hist_bulk", tsH, valH);
    printf("hist bulk affected=%d\n", aH);

    sleep(3);
    auto c = cli.query("SELECT COUNT(*) FROM hist_bulk");
    printf("COUNT(*): %s\n", c.rowCount()>0 ? c.get(0,0).toString().c_str() : "N/A");
    cli.close();
    return 0;
}
