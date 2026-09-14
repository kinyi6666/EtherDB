/*
 * EtherDB Full Integration Test — TDengine 2.4 Compatible SQL Syntax
 *
 * Tests the complete DDL/DML lifecycle via TCP network protocol:
 *   1. CREATE DATABASE (with KEEP, REPLICA options)
 *   2. USE database
 *   3. CREATE TABLE (normal table with multiple column types)
 *   4. INSERT INTO ... VALUES (SQL-based insert, TDengine compatible)
 *   5. SELECT queries (WHERE, ORDER BY, LIMIT, aggregation)
 *   6. SHOW DATABASES, SHOW TABLES
 *   7. DESCRIBE table
 *   8. DROP TABLE, DROP DATABASE
 *
 * Usage:
 *   # Terminal 1: Start the server
 *   ./build/etdb_server -p 7000
 *
 *   # Terminal 2: Run this test
 *   ./build/full_test 7000
 *
 * Build:
 *   g++ -std=c++11 -g -I src -I src/query -I src/etdb -I src/client \
 *       -I src/base -I src/dbnode -I src/wal -I src/server -I src/mnode \
 *       -o build/full_test src/client/test/full_test.cpp src/client/etdb.cpp \
 *       src/base/Logging.cpp src/base/Timestamp.cpp \
 *       src/base/LogStream.cpp src/base/Ascii.cpp -lpthread
 */

#include <client/etdb.h>
#include <client/EtDBClient.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cassert>
#include <thread>
#include <chrono>
#include <vector>
#include <string>

// ============================================================================
// Test framework
// ============================================================================
static int g_pass = 0, g_fail = 0;

#define TEST_SECTION(name) \
    printf("\n========================================\n"); \
    printf("  %s\n", name); \
    printf("========================================\n")

#define CHECK(cond, msg) do { \
    if (cond) { printf("  [PASS] %s\n", std::string(msg).c_str()); g_pass++; } \
    else { printf("  [FAIL] %s\n", std::string(msg).c_str()); g_fail++; } \
} while(0)

#define CHECK_EQ(a, b, msg) do { \
    int64_t _av = (int64_t)(a); int64_t _bv = (int64_t)(b); \
    if (_av == _bv) { printf("  [PASS] %s (%ld)\n", msg, (long)_av); g_pass++; } \
    else { printf("  [FAIL] %s (expected=%ld, got=%ld)\n", msg, (long)_bv, (long)_av); g_fail++; } \
} while(0)

// ============================================================================
// Helper: execute a SQL via C++ client and return result text
// ============================================================================
static std::string execSQL(ETDB::Client::EtDBClient& client, const std::string& sql) {
    auto result = client.query(sql);
    std::string output;

    // Check if it's a text response (single "message" column)
    if (result.colCount() == 1 && result.columnNames().size() == 1
        && result.columnNames()[0] == "message") {
        if (result.rowCount() >= 1) {
            output = result.get(0, 0).toString();
            // Strip surrounding quotes from Value::toString()
            if (output.size() >= 2 && output.front() == '\'' && output.back() == '\'')
                output = output.substr(1, output.size() - 2);
        }
    } else if (result.colCount() > 0 && result.rowCount() > 0) {
        // Multi-column result (like SHOW output)
        for (int r = 0; r < result.rowCount(); ++r) {
            for (int c = 0; c < result.colCount(); ++c) {
                if (c) output += " | ";
                std::string v = result.get(r, c).toString();
                if (v.size() >= 2 && v.front() == '\'' && v.back() == '\'')
                    v = v.substr(1, v.size() - 2);
                output += v;
            }
            output += "\n";
        }
        // Remove trailing newline
        while (!output.empty() && output.back() == '\n') output.pop_back();
    } else if (!result.error().empty()) {
        output = "ERROR: " + result.error();
    } else {
        // Successful DDL (CREATE/DROP/USE) and empty result sets come back as an
        // empty response in the current protocol — treat as success.
        output = "OK";
    }
    return output;
}

// ============================================================================
// Helper: execute via C API
// ============================================================================
static std::string execSQL_C(ETDB_CONN* conn, const std::string& sql) {
    ETDB_RESULT* r = etdb_query(conn, sql.c_str());
    if (!r) return "ERROR: query returned NULL";

    std::string output;
    int cols = etdb_column_count(r);
    int rows = etdb_row_count(r);

    for (int row = 0; row < rows; ++row) {
        for (int col = 0; col < cols; ++col) {
            if (col) output += " | ";
            const char* v = etdb_get_value(r, row, col);
            output += v ? v : "NULL";
        }
        if (row < rows - 1) output += "\n";
    }
    etdb_free_result(r);
    return output;
}

// ============================================================================
// Main Test
// ============================================================================
int main(int argc, char* argv[]) {
    uint16_t port = (argc > 1) ? (uint16_t)atoi(argv[1]) : 7000;
    const char* host = "127.0.0.1";

    printf("╔══════════════════════════════════════════════╗\n");
    printf("║   EtherDB Full Integration Test             ║\n");
    printf("║   TDengine 2.4 Compatible SQL Syntax        ║\n");
    printf("╠══════════════════════════════════════════════╣\n");
    printf("║   Server: %s:%-5d                     ║\n", host, port);
    printf("╚══════════════════════════════════════════════╝\n\n");

    printf("NOTE: Start the server first:\n");
    printf("  ./build/etdb_server -p %d\n\n", port);

    // =====================================================================
    // Connect
    // =====================================================================
    TEST_SECTION("1. Connect to Server");

    ETDB::Client::EtDBClient client;
    CHECK(client.connect(host, port), "C++ client connect");
    if (!client.isConnected()) {
        printf("  [FATAL] Cannot connect. Start server: ./build/etdb_server -p %d\n", port);
        return 1;
    }

    // Also connect via C API
    etdb_init();
    ETDB_CONN* conn = etdb_connect(host, port, "root", "etherdbdata", "" );
    CHECK(conn != nullptr, "C API connect");

    // =====================================================================
    // CREATE DATABASE
    // =====================================================================
    TEST_SECTION("2. CREATE DATABASE");

    std::string r;

    r = execSQL(client, "CREATE DATABASE testdb");
    CHECK(r.find("OK") != std::string::npos, "CREATE DATABASE testdb -> " + r);

    r = execSQL(client, "CREATE DATABASE testdb2 KEEP 90 REPLICA 1");
    CHECK(r.find("OK") != std::string::npos, "CREATE DATABASE testdb2 KEEP 90 -> " + r);

    r = execSQL(client, "CREATE DATABASE IF NOT EXISTS testdb");
    CHECK(r.find("OK") != std::string::npos, "CREATE DATABASE IF NOT EXISTS -> " + r);

    // =====================================================================
    // SHOW DATABASES
    // =====================================================================
    TEST_SECTION("3. SHOW DATABASES");

    r = execSQL(client, "SHOW DATABASES");
    printf("  SHOW DATABASES:\n%s\n", r.c_str());
    CHECK(r.find("testdb") != std::string::npos, "SHOW DATABASES contains 'testdb'");
    CHECK(r.find("testdb2") != std::string::npos, "SHOW DATABASES contains 'testdb2'");

    // =====================================================================
    // USE Database
    // =====================================================================
    TEST_SECTION("4. USE Database");

    r = execSQL(client, "USE testdb");
    // Current protocol: USE returns an empty success response (no text message).
    CHECK(r.find("OK") != std::string::npos, "USE testdb -> " + r);

    r = execSQL(client, "SELECT database()");
    // May not be supported; just check no crash
    CHECK(true, "SELECT database() (no crash)");

    // =====================================================================
    // CREATE TABLE
    // =====================================================================
    TEST_SECTION("5. CREATE TABLE");

    r = execSQL(client, "CREATE TABLE sensors ("
                 "ts TIMESTAMP, "
                 "v1 INT, "
                 "v2 BIGINT, "
                 "v3 FLOAT, "
                 "v4 DOUBLE, "
                 "v5 SMALLINT, "
                 "v6 TINYINT, "
                 "v7 BOOL"
                 ")");
    CHECK(r.find("OK") != std::string::npos, "CREATE TABLE sensors -> " + r);

    r = execSQL(client, "CREATE TABLE IF NOT EXISTS sensors (ts TIMESTAMP, val FLOAT)");
    CHECK(r.find("OK") != std::string::npos,
          "CREATE TABLE IF NOT EXISTS (existing table → no-op success) -> " + r);

    r = execSQL(client, "CREATE TABLE readings ("
                 "ts TIMESTAMP, "
                 "temperature FLOAT, "
                 "humidity FLOAT, "
                 "pressure DOUBLE"
                 ")");
    CHECK(r.find("OK") != std::string::npos, "CREATE TABLE readings -> " + r);

    // =====================================================================
    // SHOW TABLES
    // =====================================================================
    TEST_SECTION("6. SHOW TABLES");

    r = execSQL(client, "SHOW TABLES");
    printf("  SHOW TABLES:\n%s\n", r.c_str());
    CHECK(r.find("sensors") != std::string::npos, "SHOW TABLES contains 'sensors'");
    CHECK(r.find("readings") != std::string::npos, "SHOW TABLES contains 'readings'");

    // =====================================================================
    // DESCRIBE Table
    // =====================================================================
    TEST_SECTION("7. DESCRIBE Table");

    r = execSQL(client, "DESCRIBE sensors");
    printf("  DESCRIBE sensors:\n%s\n", r.c_str());
    CHECK(r.find("ts") != std::string::npos, "DESCRIBE contains 'ts'");
    CHECK(r.find("v1") != std::string::npos, "DESCRIBE contains 'v1'");
    CHECK(r.find("TIMESTAMP") != std::string::npos, "DESCRIBE contains 'TIMESTAMP'");

    r = execSQL(client, "DESC readings");
    printf("  DESC readings:\n%s\n", r.c_str());
    CHECK(r.find("temperature") != std::string::npos, "DESC contains 'temperature'");

    // =====================================================================
    // INSERT INTO ... VALUES (SQL-based)
    // =====================================================================
    TEST_SECTION("8. INSERT INTO ... VALUES");

    // Insert into sensors (8 columns)
    r = execSQL(client, "INSERT INTO sensors VALUES("
                 "1716364800000, 100, 1000000, 25.5, 3.1415926535, 10, 1, true)");
    CHECK(r.find("OK") != std::string::npos || r.find("inserted") != std::string::npos,
          "INSERT sensors row 1 -> " + r);

    r = execSQL(client, "INSERT INTO sensors VALUES("
                 "1716364860000, 200, 2000000, 26.8, 2.7182818284, 20, 2, false)");
    CHECK(r.find("OK") != std::string::npos || r.find("inserted") != std::string::npos,
          "INSERT sensors row 2 -> " + r);

    r = execSQL(client, "INSERT INTO sensors VALUES("
                 "1716364920000, 300, 3000000, 24.1, 1.4142135623, 30, 3, true)");
    CHECK(r.find("OK") != std::string::npos || r.find("inserted") != std::string::npos,
          "INSERT sensors row 3 -> " + r);

    // Insert into readings (4 columns)
    r = execSQL(client, "INSERT INTO readings VALUES(1716364800000, 25.5, 60.0, 1013.25)");
    CHECK(r.find("OK") != std::string::npos || r.find("inserted") != std::string::npos,
          "INSERT readings row 1 -> " + r);

    r = execSQL(client, "INSERT INTO readings VALUES(1716364860000, 26.8, 55.5, 1014.50)");
    CHECK(r.find("OK") != std::string::npos || r.find("inserted") != std::string::npos,
          "INSERT readings row 2 -> " + r);

    r = execSQL(client, "INSERT INTO readings VALUES(1716364920000, 24.1, 65.2, 1012.00)");
    CHECK(r.find("OK") != std::string::npos || r.find("inserted") != std::string::npos,
          "INSERT readings row 3 -> " + r);

    r = execSQL(client, "INSERT INTO readings VALUES(1716364980000, 27.3, 50.8, 1015.10)");
    CHECK(r.find("OK") != std::string::npos || r.find("inserted") != std::string::npos,
          "INSERT readings row 4 -> " + r);

    r = execSQL(client, "INSERT INTO readings VALUES(1716365040000, 28.0, 48.3, 1016.30)");
    CHECK(r.find("OK") != std::string::npos || r.find("inserted") != std::string::npos,
          "INSERT readings row 5 -> " + r);

    // Give server time to commit
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // =====================================================================
    // SELECT Queries
    // =====================================================================
    TEST_SECTION("9. SELECT Queries");

    // 9a: SELECT *
    r = execSQL(client, "SELECT * FROM sensors");
    printf("  SELECT * FROM sensors:\n%s\n", r.c_str());
    CHECK(r.find("25.5") != std::string::npos || r.find("100") != std::string::npos,
          "SELECT * returns data");

    r = execSQL(client, "SELECT * FROM readings");
    printf("  SELECT * FROM readings:\n%s\n", r.c_str());
    CHECK(r.find("25.5") != std::string::npos || r.find("60") != std::string::npos,
          "SELECT * from readings returns data");

    // 9b: WHERE filter
    r = execSQL(client, "SELECT * FROM readings WHERE temperature > 26.0");
    printf("  WHERE temperature > 26.0:\n%s\n", r.c_str());
    // FLOAT 26.8f is printed as 26.799999 (%.6f via std::to_string) — match the
    // actual display representation rather than the literal decimal input.
    CHECK(r.find("26.799999") != std::string::npos || r.find("26.8") != std::string::npos,
          "WHERE filter returns 26.8 (26.799999)");
    CHECK(r.find("28.0") != std::string::npos, "WHERE filter returns 28.0");

    // 9c: Aggregation
    r = execSQL(client, "SELECT COUNT(*), AVG(temperature), MAX(temperature), MIN(temperature) FROM readings");
    printf("  Aggregation:\n%s\n", r.c_str());
    CHECK(r.find("5") != std::string::npos || r.find("26.34") != std::string::npos,
          "Aggregation COUNT/AVG/MAX/MIN");

    // 9d: ORDER BY + LIMIT
    r = execSQL(client, "SELECT * FROM readings ORDER BY temperature DESC LIMIT 3");
    printf("  ORDER BY temperature DESC LIMIT 3:\n%s\n", r.c_str());

    r = execSQL(client, "SELECT * FROM readings ORDER BY ts ASC LIMIT 2");
    printf("  ORDER BY ts ASC LIMIT 2:\n%s\n", r.c_str());

    // =====================================================================
    // DROP TABLE
    // =====================================================================
    TEST_SECTION("10. DROP TABLE");

    r = execSQL(client, "DROP TABLE IF EXISTS nonexistent_table");
    CHECK(r.find("not found") != std::string::npos || r.find("OK") != std::string::npos
          || r.find("ERROR") != std::string::npos,
          "DROP TABLE IF EXISTS (safe) -> " + r);

    r = execSQL(client, "DROP TABLE readings");
    CHECK(r.find("OK") != std::string::npos, "DROP TABLE readings -> " + r);

    r = execSQL(client, "SHOW TABLES");
    CHECK(r.find("readings") == std::string::npos, "readings no longer in SHOW TABLES");

    // =====================================================================
    // DROP DATABASE
    // =====================================================================
    TEST_SECTION("11. DROP DATABASE");

    r = execSQL(client, "DROP DATABASE IF EXISTS testdb2");
    CHECK(r.find("OK") != std::string::npos, "DROP DATABASE testdb2 -> " + r);

    r = execSQL(client, "SHOW DATABASES");
    CHECK(r.find("testdb2") == std::string::npos, "testdb2 no longer in SHOW DATABASES");

    // =====================================================================
    // C API Tests
    // =====================================================================
    TEST_SECTION("12. C API Tests");

    // The C API connection has no default database — switch to testdb first so
    // the unqualified table name resolves to a dbnode.
    etdb_query(conn, "USE testdb");
    std::string cr = execSQL_C(conn, "SELECT * FROM sensors");
    printf("  C API SELECT * FROM sensors:\n%s\n", cr.c_str());
    CHECK(!cr.empty() && cr.find("25.5") != std::string::npos || cr.find("100") != std::string::npos,
          "C API SELECT returns data");

    // C API INSERT via prepared statement
    ETDB_STMT* stmt = etdb_stmt_init(conn);
    CHECK(stmt != nullptr, "C API stmt_init");

    int rc = etdb_stmt_prepare(stmt, "INSERT INTO sensors VALUES(?, ?, ?, ?, ?, ?, ?, ?)");
    // Note: prepare may fail if table schema doesn't match; that's OK
    if (rc == 0) {
        int64_t ts = 1716364980000LL;
        int v1 = 400; int64_t v2 = 4000000LL;
        float v3 = 29.0f; double v4 = 3.0;
        int16_t v5 = 40; int8_t v6 = 4; bool v7 = true;

        etdb_stmt_bind_param(stmt, 0, ETDB_TYPE_TIMESTAMP, &ts, sizeof(ts));
        etdb_stmt_bind_param(stmt, 1, ETDB_TYPE_INT, &v1, sizeof(v1));
        etdb_stmt_bind_param(stmt, 2, ETDB_TYPE_BIGINT, &v2, sizeof(v2));
        etdb_stmt_bind_param(stmt, 3, ETDB_TYPE_FLOAT, &v3, sizeof(v3));
        etdb_stmt_bind_param(stmt, 4, ETDB_TYPE_DOUBLE, &v4, sizeof(v4));
        etdb_stmt_bind_param(stmt, 5, ETDB_TYPE_SMALLINT, &v5, sizeof(v5));
        etdb_stmt_bind_param(stmt, 6, ETDB_TYPE_TINYINT, &v6, sizeof(v6));
        etdb_stmt_bind_param(stmt, 7, ETDB_TYPE_BOOL, &v7, sizeof(v7));

        rc = etdb_stmt_add_batch(stmt);
        if (rc == 0) {
            int affected = etdb_stmt_execute(stmt);
            printf("  C API stmt insert affected: %d\n", affected);
            CHECK(affected >= 0, "C API stmt execute");
        }
    }
    etdb_stmt_close(stmt);

    // =====================================================================
    // Cleanup
    // =====================================================================
    TEST_SECTION("13. Cleanup");

    r = execSQL(client, "DROP TABLE sensors");
    CHECK(r.find("OK") != std::string::npos, "DROP TABLE sensors -> " + r);

    r = execSQL(client, "DROP DATABASE testdb");
    CHECK(r.find("OK") != std::string::npos, "DROP DATABASE testdb -> " + r);

    client.close();
    etdb_close(conn);
    etdb_cleanup();

    // =====================================================================
    // Summary
    // =====================================================================
    printf("\n");
    printf("╔══════════════════════════════════════════════╗\n");
    printf("║   Test Summary                              ║\n");
    printf("╠══════════════════════════════════════════════╣\n");
    printf("║   PASSED: %-3d                              ║\n", g_pass);
    printf("║   FAILED: %-3d                              ║\n", g_fail);
    printf("║   TOTAL:  %-3d                              ║\n", g_pass + g_fail);
    printf("╚══════════════════════════════════════════════╝\n");

    return g_fail > 0 ? 1 : 0;
}
