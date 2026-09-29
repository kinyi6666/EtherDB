/*
 * tag_group_test.cpp — GROUPs + free-form TAGs (tag.txt model)
 *
 * Covers: CREATE/DROP GROUP, CREATE TABLE ... IN <group> TAGS (...), group
 * SELECTs with tag predicates (EQ / NE, single + multiple tags, tag + ts),
 * cross-member LIMIT / OFFSET, empty tag match (columns still reported),
 * unsupported forms (tag under OR, ORDER BY over a group), SHOW GROUPS,
 * SHOW TABLES (group column), DESCRIBE (TAG rows), restart persistence.
 *
 * Heterogeneous members (tag.txt design item 1/2): the scanned member set is
 * chosen by COLUMN presence (a member is skipped when a referenced column is
 * missing), same-named columns must agree in type — enforced at CREATE TABLE
 * time (-26) and re-checked defensively at query time. Group AGGREGATION runs
 * the ordinary per-table aggregation on every member and merges the results
 * (COUNT → Σ, SUM → Σ, MIN/MAX → min/max, AVG → Σsum/Σcount).
 *
 * Usage: tag_group_test[.exe] <port> [verify]
 *   (no 2nd arg)  DROP/CREATE the database, insert rows, run all checks
 *   verify        only run the checks (used after a server restart)
 */
#include <client/EtDBClient.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace ETDB::Client;

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  [FAIL] %s\n", msg); g_fail++; } \
    else { printf("  [PASS] %s\n", msg); } \
} while (0)
#define CHECKR(cond, msg, r) do { \
    if (!(cond)) { printf("  [FAIL] %s (rows=%d cols=%d)\n", msg, (r).rowCount(), (r).colCount()); g_fail++; } \
    else { printf("  [PASS] %s\n", msg); } \
} while (0)

static const char* DB = "tagdb";

// t_a: 10 rows v=100..109 | t_b: 20 rows v=200..219 | t_c: 5 rows v=300..304
// Timestamps are 13-digit millisecond values (2023-11-14) so the ts literals in
// the checks compare directly against the stored keys.
static const int64_t BASE_A = 1700000000000LL;
static const int64_t BASE_B = 1700001000000LL;
static const int64_t BASE_C = 1700002000000LL;
// Heterogeneous group g2 (tag.txt item 1):
//   h_a(ts,it1,it2) 3 rows -> it1 1..3,   it2 10..12
//   h_b(ts,it1,it2) 2 rows -> it1 4..5,   it2 13..14
//   h_c(ts,it1,it3) 4 rows -> it1 6..9,   it3 100..103
static const int64_t BASE_H = 1700100000000LL;

// Insert `n` rows (ts = base + i*1000, v = v0 + i) into `table`.
static int insertRows(EtDBClient& c, const std::string& table, int64_t base, int32_t v0, int n) {
    auto* stmt = c.createStmt();
    if (!stmt->prepare("INSERT INTO " + table + " VALUES(?,?)")) {
        printf("  [FAIL] prepare INSERT %s\n", table.c_str());
        return -1;
    }
    for (int i = 0; i < n; ++i) {
        int64_t ts = base + (int64_t)i * 1000;
        int32_t v  = v0 + i;
        EtDBStmt::BindParam bp[2];
        bp[0].type = EtDBStmt::TYPE_TIMESTAMP; bp[0].buffer = &ts; bp[0].length = 8;
        bp[1].type = EtDBStmt::TYPE_INT;       bp[1].buffer = &v;  bp[1].length = 4;
        if (!stmt->bindParam(bp, 2) || !stmt->addBatch()) {
            printf("  [FAIL] bind %s row %d\n", table.c_str(), i);
            stmt->close();
            return -1;
        }
    }
    int aff = stmt->execute();
    stmt->close();
    return aff;
}

// Insert `n` rows (ts = base + i*1000, x = x0 + i, y = y0 + i) into `table`.
static int insertRows2(EtDBClient& c, const std::string& table, int64_t base,
                       int32_t x0, int32_t y0, int n) {
    auto* stmt = c.createStmt();
    if (!stmt->prepare("INSERT INTO " + table + " VALUES(?,?,?)")) {
        printf("  [FAIL] prepare INSERT %s\n", table.c_str());
        return -1;
    }
    for (int i = 0; i < n; ++i) {
        int64_t ts = base + (int64_t)i * 1000;
        int32_t x  = x0 + i;
        int32_t y  = y0 + i;
        EtDBStmt::BindParam bp[3];
        bp[0].type = EtDBStmt::TYPE_TIMESTAMP; bp[0].buffer = &ts; bp[0].length = 8;
        bp[1].type = EtDBStmt::TYPE_INT;       bp[1].buffer = &x;  bp[1].length = 4;
        bp[2].type = EtDBStmt::TYPE_INT;       bp[2].buffer = &y;  bp[2].length = 4;
        if (!stmt->bindParam(bp, 3) || !stmt->addBatch()) {
            printf("  [FAIL] bind %s row %d\n", table.c_str(), i);
            stmt->close();
            return -1;
        }
    }
    int aff = stmt->execute();
    stmt->close();
    return aff;
}

// Every row's INT column (col 1) outside both ranges?
static bool vOutside(const EtDBResult& r, int64_t a1, int64_t b1, int64_t a2, int64_t b2) {
    for (int i = 0; i < r.rowCount(); ++i) {
        int64_t v = r.get(i, 1).iVal;
        bool inRange = (v >= a1 && v <= b1) || (v >= a2 && v <= b2);
        if (!inRange) return false;
    }
    return true;
}

// How many rows have their value inside [lo, hi] (col defaults to the INT column)?
static int vCount(const EtDBResult& r, int64_t lo, int64_t hi, int col = 1) {
    int n = 0;
    for (int i = 0; i < r.rowCount(); ++i) {
        int64_t v = r.get(i, col).iVal;
        if (v >= lo && v <= hi) ++n;
    }
    return n;
}

// Numeric cell value (integer expressions over a table are returned as FLOAT).
static double numVal(const EtDBResult& r, int row, int col) {
    const auto& v = r.get(row, col);
    return (v.type == ETDB::Query::ValType::FLOAT) ? v.fVal : (double)v.iVal;
}

static int runChecks(EtDBClient& c) {
    printf("\n== group + tag query checks ==\n");
    c.query(std::string("USE ") + DB);

    // 1. Full group scan: all members concatenated in creation order.
    {
        auto r = c.query("SELECT ts,v FROM g1");
        CHECK(r.error().empty(), "SELECT ts,v FROM g1 no error");
        CHECK(r.colCount() == 2, "SELECT ts,v FROM g1 -> 2 columns");
        CHECK(r.rowCount() == 35, "SELECT ts,v FROM g1 -> 35 rows (10+20+5)");
        if (r.rowCount() == 35) {
            CHECK(r.get(0, 1).iVal == 100,  "row 0  comes from member t_a");
            CHECK(r.get(10, 1).iVal == 200, "row 10 comes from member t_b");
            CHECK(r.get(34, 1).iVal == 304, "row 34 comes from member t_c");
        }
    }

    // 1b. SELECT * over a group is REJECTED (members may differ in column count,
    //     the result would not be rectangular). SELECT * on one table still works.
    {
        auto r = c.query("SELECT * FROM g1");
        CHECK(!r.error().empty(), "SELECT * over a group -> error reported");
        auto r2 = c.query("SELECT * FROM t_a");
        CHECK(r2.error().empty() && r2.rowCount() == 10 && r2.colCount() == 2,
              "SELECT * over a plain table still works (10 rows, 2 cols)");
    }

    // 2. Tag EQ -> inverted index picks the members (t_a + t_c).
    {
        auto r = c.query("SELECT ts,v FROM g1 WHERE location='A'");
        CHECK(r.error().empty(), "tag EQ location='A' no error");
        CHECK(r.rowCount() == 15, "location='A' -> 15 rows (10+5)");
        CHECK(vOutside(r, 100, 109, 300, 304), "location='A' rows are t_a + t_c only");
        CHECK(vCount(r, 100, 109) == 10 && vCount(r, 300, 304) == 5,
              "location='A' returns 10 rows from t_a and 5 from t_c");
    }

    // 3. Two tags ANDed (index intersection).
    {
        auto r = c.query("SELECT ts,v FROM g1 WHERE location='A' AND model='T100'");
        CHECK(r.error().empty(), "two tag EQ no error");
        CHECK(r.rowCount() == 10, "location='A' AND model='T100' -> 10 rows");
        CHECK(vCount(r, 100, 109) == 10, "two tag EQ rows are t_a only");
    }

    // 4. Tag EQ on the second member.
    {
        auto r = c.query("SELECT ts,v FROM g1 WHERE location='B'");
        CHECKR(r.rowCount() == 20, "location='B' -> 20 rows", r);
        CHECKR(vCount(r, 200, 219) == 20, "location='B' rows are t_b only", r);
    }

    // 5. Tag NE.
    {
        auto r = c.query("SELECT ts,v FROM g1 WHERE model!='T100'");
        CHECKR(r.rowCount() == 5, "model!='T100' -> 5 rows (t_c)", r);
        CHECKR(vCount(r, 300, 304) == 5, "model!='T100' rows are t_c only", r);
    }

    // 6. Tag + timestamp: the tag is stripped from the row path, ts still filters.
    {
        auto r = c.query("SELECT ts,v FROM g1 WHERE location='A' AND ts > 1700002001000");
        CHECK(r.error().empty(), "tag + ts no error");
        CHECKR(r.rowCount() == 3, "location='A' AND ts > t_c+1000 -> 3 rows", r);
        CHECKR(vCount(r, 302, 304) == 3, "tag + ts rows are the t_c tail", r);
    }

    // 6b. Timestamp only, over the whole group (no tag).
    {
        auto r = c.query("SELECT ts,v FROM g1 WHERE ts > 1700002001000");
        CHECKR(r.rowCount() == 3, "group WHERE ts > t_c+1000 -> 3 rows", r);
    }

    // 6c. Same ts predicate on one plain table (no group) - reference result.
    {
        auto r = c.query("SELECT ts,v FROM t_c WHERE ts > 1700002001000");
        CHECKR(r.rowCount() == 3, "t_c WHERE ts > t_c+1000 -> 3 rows", r);
    }

    // 7. LIMIT across member boundaries.
    {
        auto r = c.query("SELECT ts,v FROM g1 LIMIT 12");
        CHECK(r.rowCount() == 12, "GROUP LIMIT 12 -> 12 rows");
        if (r.rowCount() == 12) {
            CHECK(r.get(9, 1).iVal == 109,  "LIMIT row 9  is last of t_a");
            CHECK(r.get(10, 1).iVal == 200, "LIMIT row 10 is first of t_b");
        }
    }

    // 8. OFFSET across member boundaries (not pushed down for groups).
    {
        auto r = c.query("SELECT ts,v FROM g1 LIMIT 5 OFFSET 8");
        CHECK(r.rowCount() == 5, "GROUP LIMIT 5 OFFSET 8 -> 5 rows");
        if (r.rowCount() == 5) {
            CHECK(r.get(0, 1).iVal == 108, "OFFSET 8 first row is t_a v=108");
            CHECK(r.get(2, 1).iVal == 200, "OFFSET 8 crosses into t_b");
        }
    }

    // 8b. Value path (computed column is not raw-eligible): same tag filtering,
    //     cross-member LIMIT and empty-result column metadata.
    {
        auto r = c.query("SELECT v+1 FROM g1 WHERE location='A'");
        CHECK(r.error().empty(), "computed column over a group no error");
        CHECKR(r.rowCount() == 15, "computed column, location='A' -> 15 rows", r);
        int fromA = 0, fromC = 0;
        for (int i = 0; i < r.rowCount(); ++i) {
            double n = numVal(r, i, 0);
            if (n >= 101.0 && n <= 110.0) ++fromA;
            if (n >= 301.0 && n <= 305.0) ++fromC;
        }
        CHECK(fromA == 10 && fromC == 5, "computed column returns t_a + t_c rows (v+1)");
        auto r2 = c.query("SELECT v+1 FROM g1 LIMIT 12");
        CHECKR(r2.rowCount() == 12, "computed column, LIMIT 12 -> 12 rows", r2);
        auto r3 = c.query("SELECT v+1 FROM g1 WHERE location='Z'");
        CHECK(r3.error().empty(), "computed column, unknown tag no error");
        CHECK(r3.rowCount() == 0 && r3.colCount() == 1,
              "computed column, unknown tag -> 0 rows with 1 column");
    }

    // 8c. A group member that lacks the selected column is not scanned (the
    //     rest of the group keeps working).
    {
        c.query("CREATE TABLE t_x (ts TIMESTAMP, w DOUBLE) IN g1 TAGS (location='A')");
        auto r = c.query("SELECT ts,v FROM g1 WHERE location='A'");
        CHECKR(r.rowCount() == 15, "group scan skips the member without column v (t_x)", r);
    }

    // 9. Tag filter on a single table (no group): matching and non-matching.
    {
        auto r = c.query("SELECT ts,v FROM t_a WHERE model='T100'");
        CHECK(r.rowCount() == 10, "single table, matching tag -> all rows");
        auto r2 = c.query("SELECT ts,v FROM t_a WHERE model='T200'");
        CHECK(r2.error().empty(), "single table, non-matching tag no error");
        CHECK(r2.rowCount() == 0, "single table, non-matching tag -> 0 rows");
        CHECK(r2.colCount() == 2, "empty result still reports 2 columns");
    }

    // 10. Tag value nobody has -> empty result, columns preserved.
    {
        auto r = c.query("SELECT ts,v FROM g1 WHERE location='Z'");
        CHECK(r.error().empty(), "unknown tag value no error");
        CHECK(r.rowCount() == 0, "unknown tag value -> 0 rows");
        CHECK(r.colCount() == 2, "unknown tag value keeps columns");
    }

    // 11. Unsupported forms must fail cleanly (no crash / no wrong rows).
    {
        auto r = c.query("SELECT ts,v FROM g1 WHERE location='A' AND (v > 105 OR model='T200')");
        CHECK(!r.error().empty(), "tag under OR -> error reported");
        auto r2 = c.query("SELECT v FROM g1 ORDER BY v DESC LIMIT 3");
        CHECK(!r2.error().empty(), "ORDER BY over a group -> error reported");
    }

    // 12. SHOW GROUPS.
    {
        auto r = c.query("SHOW GROUPS");
        CHECK(r.error().empty(), "SHOW GROUPS no error");
        CHECK(r.colCount() == 3, "SHOW GROUPS -> 3 columns");
        bool found = false;
        for (int i = 0; i < r.rowCount(); ++i) {
            if (r.get(i, 0).sVal == "g1" && r.get(i, 2).sVal == "4") found = true;
        }
        CHECK(found, "SHOW GROUPS lists g1 with 4 tables (t_a,t_b,t_c,t_x)");
    }

    // 13. SHOW TABLES carries the group name.
    {
        auto r = c.query("SHOW TABLES");
        CHECK(r.colCount() == 4, "SHOW TABLES -> 4 columns");
        bool found = false;
        for (int i = 0; i < r.rowCount(); ++i) {
            if (r.get(i, 0).sVal == "t_a") found = (r.get(i, 3).sVal == "g1");
        }
        CHECK(found, "SHOW TABLES reports t_a group=g1");
    }

    // 14. DESCRIBE lists the tags as TAG rows.
    {
        auto r = c.query("DESCRIBE t_a");
        int tagRows = 0;
        bool locOk = false;
        for (int i = 0; i < r.rowCount(); ++i) {
            if (r.get(i, 3).sVal == "TAG") {
                ++tagRows;
                if (r.get(i, 0).sVal == "location" && r.get(i, 2).sVal == "A") locOk = true;
            }
        }
        CHECK(tagRows == 2, "DESCRIBE t_a -> 2 TAG rows");
        CHECK(locOk, "DESCRIBE t_a shows location='A'");
    }

    // 15. Heterogeneous members: the scanned member set is chosen by COLUMN
    //     presence — it1 exists in all three, it2 only in h_a/h_b, it3 only in h_c.
    {
        auto r = c.query("SELECT it1 FROM g2");
        CHECKR(r.rowCount() == 9, "g2: SELECT it1 -> 9 rows (all members)", r);
        CHECK(r.colCount() == 1, "g2: SELECT it1 -> 1 column");
        auto r2 = c.query("SELECT it1, it2 FROM g2");
        CHECKR(r2.rowCount() == 5, "g2: SELECT it1, it2 -> 5 rows (h_a+h_b)", r2);
        auto r3 = c.query("SELECT it3 FROM g2");
        CHECKR(r3.rowCount() == 4, "g2: SELECT it3 -> 4 rows (h_c only)", r3);
        CHECK(r3.rowCount() == 4 && r3.get(0, 0).iVal == 100 && r3.get(3, 0).iVal == 103,
              "g2: SELECT it3 values 100..103");
        auto r4 = c.query("SELECT it1+100 FROM g2");          // computed → value path
        CHECKR(r4.rowCount() == 9, "g2: computed column -> 9 rows", r4);
        auto r5 = c.query("SELECT it9 FROM g2");              // no member has it9
        CHECK(r5.error().empty() && r5.rowCount() == 0 && r5.colCount() == 1,
              "g2: column no member has -> 0 rows, 1 column");
        auto r6 = c.query("SELECT it1, it2 FROM g2 WHERE kind='c'");
        CHECKR(r6.rowCount() == 0, "g2: it1,it2 with tag kind='c' -> 0 rows (no column)", r6);
    }

    // 16. Group aggregation = per-member aggregation + merge:
    //     COUNT → Σ, SUM → Σ, MIN/MAX → min/max, AVG → Σsum/Σcount,
    //     COUNT(*) → all members; column aggregates → members with the column.
    {
        auto r = c.query("SELECT count(it1) FROM g2");
        CHECK(r.rowCount() == 1 && r.get(0, 0).iVal == 9, "g2 agg: count(it1) = 9");
        auto r2 = c.query("SELECT count(it2) FROM g2");
        CHECK(r2.rowCount() == 1 && r2.get(0, 0).iVal == 5, "g2 agg: count(it2) = 5 (h_a+h_b)");
        auto r3 = c.query("SELECT count(it3) FROM g2");
        CHECK(r3.rowCount() == 1 && r3.get(0, 0).iVal == 4, "g2 agg: count(it3) = 4 (h_c)");
        auto r4 = c.query("SELECT count(*) FROM g2");
        CHECK(r4.rowCount() == 1 && r4.get(0, 0).iVal == 9, "g2 agg: count(*) = 9 (all members)");
        auto r5 = c.query("SELECT sum(it2) FROM g2");
        CHECK(r5.rowCount() == 1 && numVal(r5, 0, 0) == 60.0, "g2 agg: sum(it2) = 60");
        auto r6 = c.query("SELECT min(it3), max(it3) FROM g2");
        CHECK(r6.rowCount() == 1 && numVal(r6, 0, 0) == 100.0 && numVal(r6, 0, 1) == 103.0,
              "g2 agg: min(it3), max(it3) = 100, 103");
        // avg-of-avgs would be 12.25 → the merged Σsum/Σcount must be exactly 12.0
        auto r7 = c.query("SELECT avg(it2) FROM g2");
        CHECK(r7.rowCount() == 1 && numVal(r7, 0, 0) > 11.99 && numVal(r7, 0, 0) < 12.01,
              "g2 agg: avg(it2) = 12.0 (merged, not avg-of-avgs)");
        auto r8 = c.query("SELECT count(it2), sum(it2) FROM g2");
        CHECK(r8.rowCount() == 1 && r8.get(0, 0).iVal == 5 && numVal(r8, 0, 1) == 60.0,
              "g2 agg: two aggregates in one statement");
        auto r9 = c.query("SELECT count(it2) FROM g2 WHERE kind='a'");
        CHECK(r9.rowCount() == 1 && r9.get(0, 0).iVal == 3, "g2 agg: count(it2) WHERE kind='a' = 3");
        auto r10 = c.query("SELECT count(it2) FROM g2 WHERE kind='c'");
        CHECK(r10.rowCount() == 1 && r10.get(0, 0).iVal == 0,
              "g2 agg: count(it2) WHERE kind='c' = 0 (member lacks it2)");
        auto r11 = c.query("SELECT count(it1) FROM g2 WHERE ts >= " + std::to_string(BASE_H + 2000000));
        CHECK(r11.rowCount() == 1 && r11.get(0, 0).iVal == 4, "g2 agg: count(it1) with ts range = 4");
        // Unsupported over a group: mixed select list / GROUP BY.
        auto r12 = c.query("SELECT it2, count(it2) FROM g2");
        CHECK(!r12.error().empty(), "g2 agg: mixed column+aggregate -> error reported");
        auto r13 = c.query("SELECT count(it2) FROM g2 GROUP BY it2");
        CHECK(!r13.error().empty(), "g2 agg: GROUP BY over a group -> error reported");
    }

    return g_fail;
}

int main(int argc, char* argv[]) {
    uint16_t port = (argc > 1) ? (uint16_t)atoi(argv[1]) : 7040;
    bool verifyOnly = (argc > 2 && strcmp(argv[2], "verify") == 0);

    EtDBClient c;
    if (!c.connect("127.0.0.1", port, "root", "etherdbdata", "")) {
        printf("[FAIL] connect 127.0.0.1:%u\n", (unsigned)port);
        return 1;
    }

    if (!verifyOnly) {
        printf("== create schema + data ==\n");
        c.query(std::string("DROP DATABASE IF EXISTS ") + DB);
        // Default (ms) precision: the ts literals in the checks compare directly
        // against the stored keys (µs/ns dbs scale timestamps, unrelated to tags).
        auto cr = c.query(std::string("CREATE DATABASE ") + DB);
        CHECK(cr.error().empty(), "CREATE DATABASE tagdb");
        c.query(std::string("USE ") + DB);

        auto g = c.query("CREATE GROUP g1");
        CHECK(g.error().empty(), "CREATE GROUP g1");
        auto g2 = c.query("CREATE GROUP g1");
        CHECK(!g2.error().empty(), "duplicate CREATE GROUP -> error");
        auto g3 = c.query("CREATE GROUP IF NOT EXISTS g1");   // accepted, no-op
        CHECK(g3.error().empty(), "CREATE GROUP IF NOT EXISTS (already exists) no error");

        auto c1 = c.query("CREATE TABLE t_a (ts TIMESTAMP, v INT) IN g1 TAGS (location='A', model='T100')");
        auto c2 = c.query("CREATE TABLE t_b (ts TIMESTAMP, v INT) IN g1 TAGS (location='B', model='T100')");
        auto c3 = c.query("CREATE TABLE t_c (ts TIMESTAMP, v INT) IN g1 TAGS (location='A', model='T200')");
        CHECK(c1.error().empty(), "CREATE TABLE t_a IN g1 TAGS");
        CHECK(c2.error().empty(), "CREATE TABLE t_b IN g1 TAGS");
        CHECK(c3.error().empty(), "CREATE TABLE t_c IN g1 TAGS");
        auto c4 = c.query("CREATE TABLE t_d (ts TIMESTAMP, v INT) IN nosuchgroup TAGS (location='A')");
        CHECK(!c4.error().empty(), "CREATE TABLE IN missing group -> error");

        CHECK(insertRows(c, "t_a", BASE_A, 100, 10) == 10, "insert 10 rows into t_a");
        CHECK(insertRows(c, "t_b", BASE_B, 200, 20) == 20, "insert 20 rows into t_b");
        CHECK(insertRows(c, "t_c", BASE_C, 300, 5)  == 5,  "insert 5 rows into t_c");

        // ── g2: HETEROGENEOUS members (tag.txt item 1/2) ──
        // Members may differ in layout; the member set of a query is chosen by
        // COLUMN presence (it2 only in h_a/h_b, it3 only in h_c), and the same
        // column name must agree in type (checked at CREATE TABLE time).
        auto g2n = c.query("CREATE GROUP g2");
        CHECK(g2n.error().empty(), "CREATE GROUP g2");
        auto h1 = c.query("CREATE TABLE h_a (ts TIMESTAMP, it1 INT, it2 INT) IN g2 TAGS (kind='a')");
        auto h2 = c.query("CREATE TABLE h_b (ts TIMESTAMP, it1 INT, it2 INT) IN g2 TAGS (kind='b')");
        auto h3 = c.query("CREATE TABLE h_c (ts TIMESTAMP, it1 INT, it3 INT) IN g2 TAGS (kind='c')");
        CHECK(h1.error().empty() && h2.error().empty() && h3.error().empty(),
              "CREATE TABLE heterogeneous members IN g2");
        // Same-named column with a different type/width → DDL rejected.
        auto bad1 = c.query("CREATE TABLE h_bad (ts TIMESTAMP, it1 BIGINT) IN g2");
        CHECK(!bad1.error().empty(), "member re-typing it1 (BIGINT vs INT) -> rejected");
        auto bad2 = c.query("CREATE TABLE h_bad2 (ts TIMESTAMP, it2 DOUBLE) IN g2");
        CHECK(!bad2.error().empty(), "member re-typing it2 (DOUBLE vs INT) -> rejected");
        // A member may introduce columns nobody else has (no conflict).
        auto h4 = c.query("CREATE TABLE h_d (ts TIMESTAMP, it1 INT, it9 INT) IN g2");
        CHECK(h4.error().empty(), "member may add its own columns");
        c.query("DROP TABLE h_d");

        CHECK(insertRows2(c, "h_a", BASE_H,             1, 10, 3) == 3, "insert 3 rows into h_a");
        CHECK(insertRows2(c, "h_b", BASE_H + 1000000,   4, 13, 2) == 2, "insert 2 rows into h_b");
        CHECK(insertRows2(c, "h_c", BASE_H + 2000000,   6, 100, 4) == 4, "insert 4 rows into h_c");
    }

    runChecks(c);

    c.close();
    printf("\n%s\n", g_fail == 0 ? "全部通过!" : "存在失败项!");
    return g_fail;
}
