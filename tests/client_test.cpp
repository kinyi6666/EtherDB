/*
 * ETDB Client Integration Test — Full Data Path via C API
 *
 * Tests the complete client lifecycle:
 *   1. etdb_init() / connect
 *   2. Prepared statement INSERT (parameter binding)
 *   3. SQL SELECT queries
 *   4. Result iteration and verification
 *   5. etdb_close() / cleanup
 *
 * This test connects to an etherdb server (net_raw_test or dnode).
 *
 * Usage:
 *   # Terminal 1: Start the server
 *   ./build/net_raw_test -s -p 7000
 *
 *   # Terminal 2: Run the client test
 *   ./build/client_test [port, default 7000]
 *
 * Build:
 *   g++ -std=c++11 -g -I src -I src/query -I src/etdb -I src/client \
 *       -I src/base -I src/dbnode -I src/wal \
 *       -o build/client_test src/client/test/client_test.cpp src/client/etdb.cpp \
 *       src/base/Logging.cpp src/base/Timestamp.cpp src/base/LogStream.cpp \
 *       src/base/Ascii.cpp -lpthread
 */

#include <client/etdb.h>
#include <client/EtDBClient.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cassert>
#include <thread>
#include <chrono>

// ============================================================================
// Test counter
// ============================================================================
static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  [PASS] %s\n", msg); g_pass++; } \
    else { printf("  [FAIL] %s\n", msg); g_fail++; } \
} while(0)
#define CHECK_EQ(a, b, msg) do { \
    if ((a) == (b)) { printf("  [PASS] %s (%d==%d)\n", msg, (int)(a), (int)(b)); g_pass++; } \
    else { printf("  [FAIL] %s (%d!=%d)\n", msg, (int)(a), (int)(b)); g_fail++; } \
} while(0)

// 获取当前系统毫秒时间戳（保证单调递增）
static int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// ============================================================================
// Main
// ============================================================================
int main(int argc, char* argv[]) {
    uint16_t port = (argc > 1) ? (uint16_t)atoi(argv[1]) : 7040;

    printf("╔══════════════════════════════════════════════╗\n");
    printf("║   ETDB Client API Integration Test          ║\n");
    printf("║   C API: create/insert/query full data path ║\n");
    printf("╠══════════════════════════════════════════════╣\n");
    printf("║   Server: 127.0.0.1:%-5d                    ║\n", port);
    printf("╚══════════════════════════════════════════════╝\n\n");

    printf("NOTE: Start the server first:\n");
    printf("  ./build/net_raw_test -s -p %d\n\n", port);

    // =====================================================================
    // Test 1: C API — Connect
    // =====================================================================
    printf("=== Test 1: C API Connect ===\n");
    etdb_init();
    ETDB_CONN* conn = etdb_connect("127.0.0.1", port, "root", "etherdbdata", "");
    CHECK(conn != nullptr, "etdb_connect succeeds");
    if (!conn) {
        printf("  [FATAL] Cannot connect to server. Is the server running?\n");
        printf("  Start it with: ./build/net_raw_test -s -p %d\n", port);
        etdb_cleanup();
        return 1;
    }
    CHECK(etdb_is_connected(conn), "etdb_is_connected returns true");

    // =====================================================================
    // Test 2: C API — Prepared Statement INSERT
    // =====================================================================
    printf("\n=== Test 2: C API Prepared Statement INSERT ===\n");

    ETDB_STMT* stmt = etdb_stmt_init(conn);
    CHECK(stmt != nullptr, "etdb_stmt_init succeeds");

    int rc = etdb_stmt_prepare(stmt, "INSERT INTO test VALUES(?, ?, ?)");
    CHECK_EQ(rc, 0, "etdb_stmt_prepare");

    // Insert 5 rows one by one (TS from system clock, monotonically increasing)
    int64_t baseTs = nowMs();
    struct { int64_t ts; float temp; float pres; } rows[] = {
        {baseTs + 0,     25.5f, 1013.2f},
        {baseTs + 1,     26.8f, 1014.5f},
        {baseTs + 2,     24.1f, 1012.0f},
        {baseTs + 3,     27.3f, 1015.1f},
        {baseTs + 4,     28.0f, 1016.3f},
    };
    int numRows = 5;

    for (int i = 0; i < numRows; ++i) {
        rc  = etdb_stmt_bind_param(stmt, 0, ETDB_TYPE_TIMESTAMP, &rows[i].ts, sizeof(int64_t));
        rc |= etdb_stmt_bind_param(stmt, 1, ETDB_TYPE_FLOAT, &rows[i].temp, sizeof(float));
        rc |= etdb_stmt_bind_param(stmt, 2, ETDB_TYPE_FLOAT, &rows[i].pres, sizeof(float));
        CHECK_EQ(rc, 0, "bind_param row");

        rc = etdb_stmt_add_batch(stmt);
        CHECK_EQ(rc, 0, "add_batch row");
    }

    int affected = etdb_stmt_execute(stmt);
    CHECK_EQ(affected, numRows, "stmt_execute affected rows");

    int totalAffected = etdb_stmt_affected_rows(stmt);
    CHECK_EQ(totalAffected, numRows, "stmt_affected_rows total");

    etdb_stmt_close(stmt);
    printf("  [INFO] Inserted %d rows via prepared statement\n", numRows);

    // Give server time to commit
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // =====================================================================
    // Test 3: C API — SELECT queries
    // =====================================================================
    printf("\n=== Test 3: C API SELECT Queries ===\n");

    // Test 3a: SELECT *
    {
        ETDB_RESULT* r = etdb_query(conn, "SELECT * FROM test");
        CHECK(r != nullptr, "SELECT * returns result");
        int cols = etdb_column_count(r);
        int rows = etdb_row_count(r);
        printf("  [INFO] SELECT *: %d cols, %d rows\n", cols, rows);
        CHECK(cols >= 3, "SELECT * has >= 3 columns");
        CHECK_EQ(rows, numRows, "SELECT * returns correct row count");

        // Verify first row values
        printf("  Row0: ts=%s temp=%s pres=%s\n",
               etdb_get_value(r, 0, 0),
               etdb_get_value(r, 0, 1),
               etdb_get_value(r, 0, 2));
        int64_t ts0 = etdb_get_int64(r, 0, 0);
        CHECK_EQ(ts0, baseTs, "row0 timestamp correct");
        etdb_free_result(r);
    }

    // Test 3b: WHERE filter
    {
        ETDB_RESULT* r = etdb_query(conn, "SELECT * FROM test WHERE temperature > 26.0");
        CHECK(r != nullptr, "WHERE query returns result");
        int rows = etdb_row_count(r);
        printf("  [INFO] WHERE temperature>26.0: %d rows\n", rows);
        CHECK(rows >= 3, "WHERE filter returns correct count");
        etdb_free_result(r);
    }

    // Test 3c: Aggregation
    {
        ETDB_RESULT* r = etdb_query(conn, "SELECT COUNT(*), AVG(temperature), MAX(temperature) FROM test");
        CHECK(r != nullptr, "Aggregation query returns result");
        int rows = etdb_row_count(r);
        CHECK_EQ(rows, 1, "Aggregation returns 1 row");
        int64_t cnt = etdb_get_int64(r, 0, 0);
        CHECK_EQ(cnt, numRows, "COUNT(*) correct");
        printf("  COUNT(*)=%ld  AVG=%.1f  MAX=%.1f\n",
               cnt, etdb_get_double(r, 0, 1), etdb_get_double(r, 0, 2));
        etdb_free_result(r);
    }

    // Test 3d: ORDER BY + LIMIT
    {
        ETDB_RESULT* r = etdb_query(conn, "SELECT * FROM test ORDER BY temperature DESC LIMIT 2");
        CHECK(r != nullptr, "ORDER BY LIMIT returns result");
        int rows = etdb_row_count(r);
        CHECK_EQ(rows, 2, "LIMIT 2 returns 2 rows");
        double t0 = etdb_get_double(r, 0, 1);
        double t1 = etdb_get_double(r, 1, 1);
        CHECK(t0 >= t1, "rows ordered by temperature DESC");
        printf("  Top temps: %.1f, %.1f\n", t0, t1);
        etdb_free_result(r);
    }

    // Test 3e: Column name and null check
    {
        ETDB_RESULT* r = etdb_query(conn, "SELECT * FROM test LIMIT 1");
        CHECK(r != nullptr, "Single row query returns result");
        printf("  Columns: %s, %s, %s\n",
               etdb_column_name(r, 0), etdb_column_name(r, 1), etdb_column_name(r, 2));
        CHECK(!etdb_is_null(r, 0, 0), "value is not null");
        etdb_free_result(r);
    }

    // =====================================================================
    // Test 4: C++ API (EtDBClient) direct usage
    // =====================================================================
    printf("\n=== Test 4: C++ API (EtDBClient) ===\n");

    // Close C API connection first
    etdb_close(conn);

    {
        ETDB::Client::EtDBClient client;
        bool ok = client.connect("127.0.0.1", port);
        CHECK(ok, "C++ client connect succeeds");

        auto result = client.query("SELECT * FROM test");
        CHECK(result.colCount() >= 3, "C++ query returns columns");

        int count = 0;
        for (int r = 0; r < result.rowCount(); ++r) {
            auto ts = result.get(r, 0);
            if (!ts.isNull()) count++;
        }
        CHECK_EQ(count, numRows, "C++ row count matches");
        printf("  C++ API queried %d rows\n", count);

        // Create and use stmt
        auto* stmt2 = client.createStmt();
        CHECK(stmt2 != nullptr, "C++ createStmt succeeds");
        stmt2->prepare("INSERT INTO test VALUES(?, ?, ?)");

        int64_t newTs = baseTs + 10;
        float newTemp = 30.0f, newPres = 1020.0f;
        ETDB::Client::EtDBStmt::BindParam binds[3] = {
            ETDB::Client::EtDBStmt::BindParam(ETDB::Client::EtDBStmt::TYPE_TIMESTAMP, &newTs),
            ETDB::Client::EtDBStmt::BindParam(ETDB::Client::EtDBStmt::TYPE_FLOAT, &newTemp),
            ETDB::Client::EtDBStmt::BindParam(ETDB::Client::EtDBStmt::TYPE_FLOAT, &newPres),
        };
        CHECK(stmt2->bindParam(binds, 3), "C++ bindParam succeeds");
        CHECK(stmt2->addBatch(), "C++ addBatch succeeds");
        int affected2 = stmt2->execute();
        CHECK_EQ(affected2, 1, "C++ stmt execute returns 1");

        delete stmt2;
    }

    // =====================================================================
    // Cleanup
    // =====================================================================
    printf("\n=== Cleanup ===\n");
    etdb_cleanup();

    // Summary
    printf("\n╔══════════════════════════════════════════════╗\n");
    printf("║   Test Summary                              ║\n");
    printf("╠══════════════════════════════════════════════╣\n");
    printf("║   PASSED: %-3d                              ║\n", g_pass);
    printf("║   FAILED: %-3d                              ║\n", g_fail);
    printf("╚══════════════════════════════════════════════╝\n");

    return g_fail > 0 ? 1 : 0;
}
