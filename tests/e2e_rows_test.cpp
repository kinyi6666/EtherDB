// In-process end-to-end test for the column-major Rows refactor:
//   ETDBRepo (memtable + commit-to-disk) → StorageReader → QueryExecutor
// Build:
//   g++ -O2 -DNDEBUG -DETHERDB_HAS_COMPRESSION -DNO_FSEEKO -std=gnu++17 -w \
//     -I src/dnode -I src/dnode/.. -I src/base -I src/net -I src/rpc \
//     -I src/dbnode -I src/wal -I src/etdb -I src/mnode -I src/query \
//     -I src/client -I deps/lz4/inc -o /tmp/e2e_rows_test e2e_rows_test.cpp \
//     deps/lz4/src/lz4.c -L src/bin -lbase -lpthread -ldl
#include <query/QueryEngine.h>
#include <etdb/ETDBRepo.h>
#include <etdb/ETDBMeta.h>
#include <etdb/ETDBMemTable.h>
#include <etdb/ETDBCommon.h>
#include <etdb/ETDBCommitQ.h>
#include <cstdio>
#include <cstring>
#include <cassert>
#include <vector>

using namespace ETDB;
using namespace ETDB::Query;

static int gFail = 0;
#define CK(cond, msg) do { if (cond) { printf("  [PASS] %s\n", msg); } \
    else { printf("  [FAIL] %s\n", msg); gFail++; } } while (0)

// Row layout: [ts:8][col_int:4][name:8]  (host byte order)
#pragma pack(push, 1)
struct Row3 { int64_t ts; int32_t ci; char nm[8]; };
#pragma pack(pop)

int main() {
    ETDBConfig cfg;
    cfg.repoId = 901;
    cfg.cacheBlockSize = 1;
    cfg.totalDataBlks = 8;
    ETDBAppH appH;
    ETDBRepo repo(cfg, appH);
    CK(repo.init() == 0, "repo init");
    CommitQueue::instance().init(1);

    // ── Create table: ts, col_int, name(BINARY(8)) ──
    uint64_t uid = 901;
    TableDef* td = repo.meta()->createTable(uid, "perftest.test", 0);
    CK(td != nullptr, "create table");
    repo.meta()->addColumn(uid, 0, ColType::TIMESTAMP, "ts");
    repo.meta()->addColumn(uid, 1, ColType::INT, "col_int");
    repo.meta()->addColumn(uid, 2, ColType::BINARY, "name", 8);
    td->schema.recalcRowBytes();
    CK(td->colCount() == 3 && td->rowBytes() == 20, "schema 3 cols / 20B row");

    auto insertMem = [&](int64_t ts, int32_t ci, const char* nm) {
        Row3 r; r.ts = ts; r.ci = ci; memset(r.nm, 0, 8);
        if (nm) strncpy(r.nm, nm, 7);
        int32_t tid = 0;
        TableData* t = repo.mem()->getOrCreate(uid, &tid);
        assert(t);
        assert(t->insert(ts, &r, (int)sizeof(Row3)) == 0);
    };

    // 5 rows → commit to disk
    insertMem(100, 5,  "aa");
    insertMem(200, 7,  "bb");
    insertMem(300, 12, "cc");
    insertMem(400, 3,  "dd");
    insertMem(500, 20, "ee");
    CK(repo.syncCommit() == 0, "syncCommit (5 rows to disk)");

    // 2 more rows into memtable (newer) → forces disk+mem merge
    insertMem(600, 9,  "ff");
    insertMem(700, 15, "gg");

    StorageReader reader(&repo, td);

    // ── Test 1: readAllRows (disk + mem merged, sorted) ──
    printf("\n--- Test 1: readAllRows (disk+mem merge) ---\n");
    Rows all = reader.readAllRows();
    CK(all.numOfRows == 7, "7 rows total");
    bool sorted = true;
    for (int i = 1; i < all.numOfRows; ++i) if (all.tsAt(i-1) > all.tsAt(i)) sorted = false;
    CK(sorted, "rows sorted by ts");
    CK(all.tsAt(0) == 100 && all.tsAt(6) == 700, "min=100 max=700");
    CK(all.getValue(0, 1).iVal == 5, "row0 col_int=5");
    CK(all.getValue(6, 1).iVal == 15, "row6 col_int=15");
    CK(all.getValue(1, 2).sVal == "bb", "row1 name=bb (string decode)");
    CK(all.minTs == 100 && all.maxTs == 700, "Rows.minTs/maxTs");

    // ── Test 2: projection (ts, col_int) ──
    printf("\n--- Test 2: projection ---\n");
    std::vector<bool> proj = {true, true, false};
    Rows p = reader.readRowsInRange(TSKEY_NULL, TSKEY_MAX, -1, &proj, 0);
    CK(p.colCount == 2 && p.numOfRows == 7, "2 cols, 7 rows");
    CK(p.hasCol(1) && !p.hasCol(2), "hasCol correct");
    CK(p.getValue(0, 1).iVal == 5, "proj col_int=5");
    CK(p.getValue(0, 2).isNull(), "dropped col reads NULL");

    // ── Test 3: WHERE col_int < 10 (column filter pushdown) ──
    printf("\n--- Test 3: column filter ---\n");
    ColFilter f; f.colIndex = 1; f.colType = (int8_t)ColType::INT; f.op = 3; f.val = 10;
    std::vector<ColFilter> filters = {f};
    Rows filtered = reader.readRowsInRange(TSKEY_NULL, TSKEY_MAX, -1, nullptr, 0, &filters);
    CK(filtered.numOfRows == 4, "4 rows with col_int<10");
    bool allLt = true;
    for (int i = 0; i < filtered.numOfRows; ++i) if (filtered.getValue(i,1).iVal >= 10) allLt = false;
    CK(allLt, "all col_int<10");
    CK(filtered.tsAt(3) == 600, "last match ts=600");

    // ── Test 4: LIMIT + OFFSET ──
    printf("\n--- Test 4: LIMIT / OFFSET ---\n");
    Rows lim = reader.readRowsInRange(TSKEY_NULL, TSKEY_MAX, 3, nullptr, 0);
    CK(lim.numOfRows == 3 && lim.tsAt(0) == 100 && lim.tsAt(2) == 300, "LIMIT 3 (earliest)");
    Rows off = reader.readRowsInRange(TSKEY_NULL, TSKEY_MAX, -1, nullptr, 2);
    CK(off.numOfRows == 5 && off.tsAt(0) == 300, "OFFSET 2");

    // ── Test 5: QueryExecutor (parse directly, bypass MNode layer) ──
    printf("\n--- Test 5: QueryExecutor ---\n");
    {
        SqlParser parser("SELECT * FROM perftest.test");
        ParsedSql parsed = parser.parse();
        CK(parsed.valid && parsed.isSelect(), "parse SELECT *");
        QueryExecutor exec(parsed.selectStmt.get(), &td->schema, all);
        QueryResult res = exec.execute();
        CK(res.success && res.rowsReturned == 7, "SELECT * returns 7 rows");
        CK(res.rows[0][0].iVal == 100 && res.rows[0][1].iVal == 5, "row0 values");
        CK(res.rows[0][2].sVal == "aa", "row0 name string");
    }
    {
        SqlParser parser("SELECT ts, col_int FROM perftest.test WHERE col_int < 10");
        ParsedSql parsed = parser.parse();
        CK(parsed.valid, "parse WHERE");
        QueryExecutor exec(parsed.selectStmt.get(), &td->schema, all);
        exec.setFilterHandled(false);  // executor re-applies WHERE
        QueryResult res = exec.execute();
        CK(res.success && res.rowsReturned == 4, "WHERE col_int<10 returns 4");
        CK(res.rows[3][1].iVal == 9, "last row col_int=9");
    }
    {
        SqlParser parser("SELECT ts, col_int FROM perftest.test ORDER BY col_int DESC LIMIT 2");
        ParsedSql parsed = parser.parse();
        CK(parsed.valid, "parse ORDER BY");
        QueryExecutor exec(parsed.selectStmt.get(), &td->schema, all);
        QueryResult res = exec.execute();
        CK(res.success && res.rowsReturned == 2, "ORDER BY DESC LIMIT 2");
        CK(res.rows[0][1].iVal == 20, "max col_int=20 first");
    }
    {
        SqlParser parser("SELECT COUNT(*) FROM perftest.test");
        ParsedSql parsed = parser.parse();
        CK(parsed.valid, "parse COUNT");
        QueryExecutor exec(parsed.selectStmt.get(), &td->schema, all);
        QueryResult res = exec.execute();
        CK(res.success && res.rowsReturned == 1 && res.rows[0][0].iVal == 7, "COUNT(*)=7");
    }

    // ── Test 6: streaming readNextBatch ──
    printf("\n--- Test 6: streaming ---\n");
    {
        StorageReader sr(&repo, td);
        std::vector<int64_t> tss;
        Rows b;
        do {
            b = sr.readNextBatch(3, TSKEY_NULL, TSKEY_MAX);
            for (int i = 0; i < b.numOfRows; ++i) tss.push_back(b.tsAt(i));
        } while (b.numOfRows > 0);
        CK((int)tss.size() == 7, "streaming total 7");
        bool s = true;
        for (size_t i = 1; i < tss.size(); ++i) if (tss[i-1] > tss[i]) s = false;
        CK(s, "streaming rows sorted across batches");
        CK(tss[0] == 100 && tss[6] == 700, "streaming first/last ts");
    }
    // Streaming with column filter
    {
        StorageReader sr(&repo, td);
        std::vector<int64_t> tss;
        Rows b;
        do {
            b = sr.readNextBatch(2, TSKEY_NULL, TSKEY_MAX, -1, nullptr, &filters);
            for (int i = 0; i < b.numOfRows; ++i) tss.push_back(b.tsAt(i));
        } while (b.numOfRows > 0);
        CK((int)tss.size() == 4, "streaming filtered total 4 (no dupes)");
        bool s = true;
        for (size_t i = 1; i < tss.size(); ++i) if (tss[i-1] >= tss[i]) s = false;
        CK(s, "streaming filtered rows unique+sorted");
    }

    repo.close(true);
    CommitQueue::instance().shutdown();
    printf("\n%s (failures=%d)\n", gFail == 0 ? "ALL E2E PASS" : "E2E FAILED", gFail);
    return gFail == 0 ? 0 : 1;
}
