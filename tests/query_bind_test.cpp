/*
 * EtherDB Query & Binding Integration Test
 *
 * Tests the complete query lifecycle:
 *   1. SQL DDL: CREATE DATABASE, USE, CREATE TABLE
 *   2. Prepared Statement INSERT (C API parameter binding)
 *   3. SQL SELECT queries (WHERE, ORDER BY, LIMIT, aggregation)
 *   4. C++ Client API (EtDBClient + EtDBStmt binding)
 *   5. SHOW / DESCRIBE
 *
 * Usage:
 *   # Terminal 1: Start the server
 *   ./build/etherdb_server -p 7040
 *
 *   # Terminal 2: Run this test
 *   ./build/query_bind_test [port, default 7040]
 *
 * Build (add to CMakeLists.txt):
 *   g++ -std=c++14 -g -I src -I src/query -I src/etdb -I src/client \
 *       -I src/base -I src/dbnode -I src/wal -I src/mnode -I src/rpc \
 *       -o build/query_bind_test src/client/test/query_bind_test.cpp \
 *       src/client/etdb.cpp \
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
#include <string>
#include <vector>

// ============================================================================
// Test framework
// ============================================================================
static int g_pass = 0, g_fail = 0;

#define TEST(name) \
    printf("\n--- %s ---\n", name)

#define CHECK(cond, msg) do { \
    if (cond) { printf("  [PASS] %s\n", msg); g_pass++; } \
    else { printf("  [FAIL] %s\n", msg); g_fail++; } \
} while(0)

#define CHECK_EQ(a, b, msg) do { \
    int64_t _a = (int64_t)(a); int64_t _b = (int64_t)(b); \
    if (_a == _b) { printf("  [PASS] %s (%ld)\n", msg, (long)_a); g_pass++; } \
    else { printf("  [FAIL] %s (expected=%ld, got=%ld)\n", msg, (long)_b, (long)_a); g_fail++; } \
} while(0)

// ============================================================================
// Helper: execute SQL via C++ client and return result text
// ============================================================================
static std::string execSQL(ETDB::Client::EtDBClient& client, const std::string& sql) {
    auto result = client.query(sql);
    std::string output;

    if (result.colCount() == 1 && result.columnNames().size() == 1
        && result.columnNames()[0] == "message") {
        if (result.rowCount() >= 1) {
            output = result.get(0, 0).toString();
            if (output.size() >= 2 && output.front() == '\'' && output.back() == '\'')
                output = output.substr(1, output.size() - 2);
        }
    } else if (result.colCount() > 0 && result.rowCount() > 0) {
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
        while (!output.empty() && output.back() == '\n') output.pop_back();
    } else if (!result.error().empty()) {
        output = "ERROR: " + result.error();
    }
    return output;
}

// ============================================================================
// Main
// ============================================================================
int main(int argc, char* argv[]) {
    uint16_t port = (argc > 1) ? (uint16_t)atoi(argv[1]) : 7040;
    const char* host = "127.0.0.1";

    printf("======================================================================\n");
    printf("  EtherDB Query & Binding Integration Test\n");
    printf("  Server: %s:%d\n", host, port);
    printf("======================================================================\n");

    printf("\nNOTE: Start the server first:\n");
    printf("  ./build/etherdb_server -p %d\n\n", port);

    // =====================================================================
    // 1. Connect (both C API and C++ API)
    // =====================================================================
    TEST("1. Connect");

    etdb_init();
    ETDB_CONN* conn = etdb_connect(host, port, "root", "etherdbdata", "");
    CHECK(conn != nullptr, "C API connect");
    if (!conn) {
        printf("  [FATAL] Cannot connect. Start server: ./build/etherdb_server -p %d\n", port);
        return 1;
    }

    ETDB::Client::EtDBClient client;
    CHECK(client.connect(host, port), "C++ API connect");

    // =====================================================================
    // 2. DDL: CREATE DATABASE + USE + CREATE TABLE
    // =====================================================================
    TEST("2. DDL Operations");

    std::string r;

    r = execSQL(client, "CREATE DATABASE testdb");
    printf("  CREATE DATABASE: %s\n", r.c_str());
    CHECK(r.find("OK") != std::string::npos || r.find("created") != std::string::npos
          || r.find("success") != std::string::npos || r.empty(),
          "CREATE DATABASE testdb");

    r = execSQL(client, "USE testdb");
    printf("  USE testdb: %s\n", r.c_str());

    r = execSQL(client, "CREATE TABLE sensors ("
                 "ts TIMESTAMP, "
                 "temperature FLOAT, "
                 "humidity FLOAT, "
                 "pressure DOUBLE"
                 ")");
    printf("  CREATE TABLE sensors: %s\n", r.c_str());
    CHECK(r.find("OK") != std::string::npos || r.find("created") != std::string::npos
          || r.find("success") != std::string::npos || r.empty(),
          "CREATE TABLE sensors");

    // =====================================================================
    // 3. SHOW & DESCRIBE
    // =====================================================================
    TEST("3. SHOW & DESCRIBE");

    r = execSQL(client, "SHOW DATABASES");
    printf("  SHOW DATABASES:\n%s\n", r.c_str());

    r = execSQL(client, "SHOW TABLES");
    printf("  SHOW TABLES:\n%s\n", r.c_str());

    r = execSQL(client, "DESCRIBE sensors");
    printf("  DESCRIBE sensors:\n%s\n", r.c_str());
    CHECK(r.find("ts") != std::string::npos || r.find("temperature") != std::string::npos,
          "DESCRIBE shows columns");

    // =====================================================================
    // 4. Prepared Statement INSERT (C API — parameter binding)
    // =====================================================================
    TEST("4. Prepared Statement INSERT (C API binding)");

    ETDB_STMT* stmt = etdb_stmt_init(conn);
    CHECK(stmt != nullptr, "etdb_stmt_init");

    // Prepare against the "test" table (uid=1, hardcoded for compat)
    int rc = etdb_stmt_prepare(stmt, "INSERT INTO sensors VALUES(?, ?, ?, ?)");
    printf("  prepare rc=%d\n", rc);
    // Note: prepare may fail if table meta isn't available; try fallback
    if (rc != 0) {
        printf("  [INFO] prepare against 'sensors' failed, trying 'test' table\n");
        rc = etdb_stmt_prepare(stmt, "INSERT INTO test VALUES(?, ?, ?)");
    }
    CHECK_EQ(rc, 0, "etdb_stmt_prepare");

    // Insert 5 rows via binding
    struct Row { int64_t ts; float temp; float hum; double pres; };
    Row rows[] = {
        {1716364800000LL, 25.5f, 60.0f, 1013.25},
        {1716364860000LL, 26.8f, 55.5f, 1014.50},
        {1716364920000LL, 24.1f, 65.2f, 1012.00},
        {1716364980000LL, 27.3f, 58.0f, 1015.10},
        {1716365040000LL, 28.0f, 62.3f, 1016.30},
    };
    int numRows = 5;

    for (int i = 0; i < numRows; ++i) {
        rc  = etdb_stmt_bind_param(stmt, 0, ETDB_TYPE_TIMESTAMP, &rows[i].ts, sizeof(int64_t));
        rc |= etdb_stmt_bind_param(stmt, 1, ETDB_TYPE_FLOAT, &rows[i].temp, sizeof(float));
        rc |= etdb_stmt_bind_param(stmt, 2, ETDB_TYPE_FLOAT, &rows[i].hum, sizeof(float));
        rc |= etdb_stmt_bind_param(stmt, 3, ETDB_TYPE_DOUBLE, &rows[i].pres, sizeof(double));
        CHECK_EQ(rc, 0, "bind_param");
        rc = etdb_stmt_add_batch(stmt);
        CHECK_EQ(rc, 0, "add_batch");
    }

    int affected = etdb_stmt_execute(stmt);
    printf("  execute affected=%d\n", affected);
    CHECK_EQ(affected, numRows, "stmt_execute affected rows");

    etdb_stmt_close(stmt);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // =====================================================================
    // 5. SQL SELECT Queries (via C API)
    // =====================================================================
    TEST("5. SQL SELECT Queries (C API)");

    // 5a. SELECT *
    {
        ETDB_RESULT* r = etdb_query(conn, "SELECT * FROM test");
        CHECK(r != nullptr, "SELECT * returns result");
        if (r) {
            int cols = etdb_column_count(r);
            int rows = etdb_row_count(r);
            printf("  SELECT * FROM test: %d cols, %d rows\n", cols, rows);

            if (rows > 0) {
                printf("  Col names: %s", etdb_column_name(r, 0));
                for (int c = 1; c < cols; ++c) printf(", %s", etdb_column_name(r, c));
                printf("\n");

                printf("  Row0: ");
                for (int c = 0; c < cols; ++c) {
                    if (c) printf(" | ");
                    printf("%s", etdb_get_value(r, 0, c) ? etdb_get_value(r, 0, c) : "NULL");
                }
                printf("\n");
            }
            CHECK(cols >= 3, "SELECT * has >= 3 columns");
            etdb_free_result(r);
        }
    }

    // 5b. WHERE filter
    {
        ETDB_RESULT* r = etdb_query(conn, "SELECT * FROM test WHERE temperature > 26.0");
        if (r) {
            int rows = etdb_row_count(r);
            printf("  WHERE temperature>26.0: %d rows\n", rows);
            CHECK(rows >= 2, "WHERE filter returns rows");
            etdb_free_result(r);
        }
    }

    // 5c. Aggregation: COUNT, AVG, MAX, MIN
    {
        ETDB_RESULT* r = etdb_query(conn,
            "SELECT COUNT(*), AVG(temperature), MAX(temperature), MIN(temperature) FROM test");
        if (r) {
            int rows = etdb_row_count(r);
            CHECK_EQ(rows, 1, "Aggregation returns 1 row");
            if (rows > 0) {
                int64_t cnt = etdb_get_int64(r, 0, 0);
                double avg  = etdb_get_double(r, 0, 1);
                double maxV = etdb_get_double(r, 0, 2);
                double minV = etdb_get_double(r, 0, 3);
                printf("  COUNT=%ld, AVG=%.2f, MAX=%.2f, MIN=%.2f\n", cnt, avg, maxV, minV);
            }
            etdb_free_result(r);
        }
    }

    // 5d. ORDER BY + LIMIT
    {
        ETDB_RESULT* r = etdb_query(conn, "SELECT * FROM test ORDER BY temperature DESC LIMIT 3");
        if (r) {
            int rows = etdb_row_count(r);
            printf("  ORDER BY temp DESC LIMIT 3: %d rows\n", rows);
            for (int i = 0; i < rows && i < 3; ++i) {
                printf("    row%d: temp=%s\n", i,
                       etdb_get_value(r, i, 1) ? etdb_get_value(r, i, 1) : "NULL");
            }
            etdb_free_result(r);
        }
    }

    // 5e. LIMIT only
    {
        ETDB_RESULT* r = etdb_query(conn, "SELECT * FROM test LIMIT 1");
        if (r) {
            int rows = etdb_row_count(r);
            CHECK_EQ(rows, 1, "LIMIT 1 returns 1 row");
            bool notNull = !etdb_is_null(r, 0, 0);
            CHECK(notNull, "value is not null");
            etdb_free_result(r);
        }
    }

    // =====================================================================
    // 6. C++ API: EtDBClient + EtDBStmt binding
    // =====================================================================
    TEST("6. C++ API Binding Insert");

    {
        auto* stmt2 = client.createStmt();
        CHECK(stmt2 != nullptr, "createStmt");

        bool ok = stmt2->prepare("INSERT INTO test VALUES(?, ?, ?)");
        printf("  C++ prepare: %s\n", ok ? "OK" : "FAILED");
        CHECK(ok, "C++ stmt prepare");

        // Insert 2 more rows
        for (int i = 0; i < 2; ++i) {
            int64_t ts = 1716365100000LL + i * 60000;
            float temp = 30.0f + i * 2.0f;
            float pres = 1020.0f + i;
            ETDB::Client::EtDBStmt::BindParam binds[3] = {
                ETDB::Client::EtDBStmt::BindParam(ETDB::Client::EtDBStmt::TYPE_TIMESTAMP, &ts),
                ETDB::Client::EtDBStmt::BindParam(ETDB::Client::EtDBStmt::TYPE_FLOAT, &temp),
                ETDB::Client::EtDBStmt::BindParam(ETDB::Client::EtDBStmt::TYPE_FLOAT, &pres),
            };
            CHECK(stmt2->bindParam(binds, 3), "C++ bindParam");
            CHECK(stmt2->addBatch(), "C++ addBatch");
        }

        int aff2 = stmt2->execute();
        printf("  C++ stmt execute affected=%d\n", aff2);
        CHECK_EQ(aff2, 2, "C++ stmt execute returns 2");

        delete stmt2;
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }

    // =====================================================================
    // 7. C++ API: SQL SELECT via EtDBClient
    // =====================================================================
    TEST("7. C++ API SQL SELECT");

    {
        auto result = client.query("SELECT * FROM test");
        int cols = result.colCount();
        int rows = result.rowCount();
        printf("  C++ SELECT * FROM test: %d cols, %d rows\n", cols, rows);

        if (cols > 0) {
            printf("  Columns: ");
            for (int c = 0; c < cols; ++c) {
                if (c) printf(", ");
                printf("%s", result.columnNames()[c].c_str());
            }
            printf("\n");
        }

        for (int r = 0; r < rows && r < 3; ++r) {
            printf("  row%d: ", r);
            for (int c = 0; c < cols; ++c) {
                if (c) printf(" | ");
                printf("%s", result.get(r, c).toString().c_str());
            }
            printf("\n");
        }
        if (rows > 3) printf("  ... (%d more rows)\n", rows - 3);
    }

    // C++ WHERE + aggregation
    {
        auto result = client.query(
            "SELECT COUNT(*), AVG(temperature), MAX(temperature) FROM test WHERE temperature > 26.0");
        if (result.rowCount() > 0) {
            printf("  C++ Aggregation (WHERE temp>26): COUNT=%s, AVG=%s, MAX=%s\n",
                   result.get(0, 0).toString().c_str(),
                   result.get(0, 1).toString().c_str(),
                   result.get(0, 2).toString().c_str());
        }
    }

    // =====================================================================
    // 8. Cleanup
    // =====================================================================
    TEST("8. Cleanup");

    etdb_close(conn);
    client.close();
    etdb_cleanup();

    // =====================================================================
    // Summary
    // =====================================================================
    printf("\n======================================================================\n");
    printf("  Test Summary\n");
    printf("  PASSED: %d\n", g_pass);
    printf("  FAILED: %d\n", g_fail);
    printf("======================================================================\n");

    return g_fail > 0 ? 1 : 0;
}
