// Copyright (c) 2026 Liu jinwei <kinyi6666@gmail.com>
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef ETHERDB_C_API_H
#define ETHERDB_C_API_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Opaque handle types
// ============================================================================
typedef struct EtDBConn     ETDB_CONN;
typedef struct EtDBResult   ETDB_RESULT;
typedef struct EtDBStmt     ETDB_STMT;

// ============================================================================
// Column type constants
// ============================================================================
#define ETDB_TYPE_TIMESTAMP  0
#define ETDB_TYPE_BOOL       1
#define ETDB_TYPE_TINYINT    2
#define ETDB_TYPE_SMALLINT   3
#define ETDB_TYPE_INT        4
#define ETDB_TYPE_BIGINT     5
#define ETDB_TYPE_FLOAT      6
#define ETDB_TYPE_DOUBLE     7
#define ETDB_TYPE_BINARY     8
#define ETDB_TYPE_NCHAR      9
#define ETDB_TYPE_UTINYINT   10
#define ETDB_TYPE_USMALLINT  11
#define ETDB_TYPE_UINT       12
#define ETDB_TYPE_UBIGINT    13

// ============================================================================
// Library lifecycle
// ============================================================================

// Initialize the client library (call once before any other operations)
void etdb_init(void);

// Cleanup the client library (call once after all operations)
void etdb_cleanup(void);

// ============================================================================
// Connection management
// ============================================================================

// Connect to etherdb server
// host: server hostname or IP address
// port: server port
// user: username for authentication (e.g., "root")
// password: password for authentication (e.g., "etherdbdata")
// db: default database name (can be empty string "")
// Returns connection handle, or NULL on failure
ETDB_CONN* etdb_connect(const char* host, uint16_t port,
                         const char* user, const char* password,
                         const char* db);

// Close connection and free resources
void etdb_close(ETDB_CONN* conn);

// Check if connection is alive
int etdb_is_connected(ETDB_CONN* conn);

// ============================================================================
// Query operations
// ============================================================================

// Execute a SQL query and return results
// The returned result must be freed with etdb_free_result()
ETDB_RESULT* etdb_query(ETDB_CONN* conn, const char* sql);

// Get number of columns in result
int etdb_column_count(ETDB_RESULT* result);

// Get number of rows in result
int etdb_row_count(ETDB_RESULT* result);

// Get column name by index
const char* etdb_column_name(ETDB_RESULT* result, int col);

// Get value at (row, col) as string
// Returns a static buffer, valid until next call
const char* etdb_get_value(ETDB_RESULT* result, int row, int col);

// Check if value at (row, col) is NULL
int etdb_is_null(ETDB_RESULT* result, int row, int col);

// Get value as int64 (returns 0 if not int type)
int64_t etdb_get_int64(ETDB_RESULT* result, int row, int col);

// Get value as double (returns 0.0 if not numeric)
double etdb_get_double(ETDB_RESULT* result, int row, int col);

// Free result
void etdb_free_result(ETDB_RESULT* result);

// ============================================================================
// Batch / streaming fetch (TDengine taos_fetch_row / taos_fetch_block style)
//
// 对标 taos 的流式取数接口。大结果集（行数超过流式阈值）时服务端分批返回，
// etdb_fetch_row() / etdb_fetch_block() 在当前批次耗尽时自动向服务端请求
// 下一批，客户端无需关心批次边界。小结果集则一次性返回（行为与 etdb_query
// 一致）。取完后用 etdb_free_result() 释放。
//
// 典型用法（逐行）:
//   ETDB_RESULT* res = etdb_use_result(conn, "SELECT ts,col_int FROM t WHERE ...");
//   while (etdb_fetch_row(res) == 1) {
//       int64_t ts = etdb_get_int64(res, 0, 0);
//       int     ci = etdb_get_int64(res, 0, 1);
//   }
//   etdb_free_result(res);
//
// 典型用法（批量，推荐——避免逐行复制，吞吐更高）:
//   ETDB_RESULT* res = etdb_use_result(conn, "SELECT ...");
//   int n;
//   while ((n = etdb_fetch_block(res)) > 0) {
//       for (int r = 0; r < n; r++) {
//           for (int c = 0; c < etdb_field_count(res); c++)
//               printf("%s ", etdb_get_value(res, r, c));
//       }
//   }
//   etdb_free_result(res);
// ============================================================================

// 列字段元信息 (type 为 ETDB_TYPE_* 常量, bytes 为该列的定长槽宽,
// BINARY/NCHAR 为声明长度, 未知时为 -1)
typedef struct {
    char name[65];
    int  type;
    int  bytes;
} ETDB_FIELD;

// 执行查询并返回支持批量流式取数的结果句柄（等价于 etdb_query +
// 流式取数能力）。释放用 etdb_free_result()。
ETDB_RESULT* etdb_use_result(ETDB_CONN* conn, const char* sql);

// 结果列数（同 etdb_column_count）
int etdb_field_count(ETDB_RESULT* result);

// 列元信息数组（由 result 持有，下一次取数/释放前有效）
const ETDB_FIELD* etdb_fetch_fields(ETDB_RESULT* result);

// 取下一行。返回 1 = 有行（可用 etdb_get_value(result, 0, col) 等访问），
// 0 = 已取完，-1 = 出错。
int etdb_fetch_row(ETDB_RESULT* result);

// 取下一批行。返回本批行数 (>0)，0 = 已取完，-1 = 出错。
// 本批内第 r 行第 c 列用 etdb_get_value(result, r, c) 访问（r 相对本批）。
// 建议不要与 etdb_fetch_row() 混用。
int etdb_fetch_block(ETDB_RESULT* result);

// ============================================================================
// Prepared statement operations (parameter binding insert)
// ============================================================================

// Create a prepared statement
ETDB_STMT* etdb_stmt_init(ETDB_CONN* conn);

// Prepare an INSERT statement
// sql format: "INSERT INTO <table> VALUES(?, ?, ...)"
// Returns 0 on success, -1 on error
int etdb_stmt_prepare(ETDB_STMT* stmt, const char* sql);

// Bind a parameter by column index (0-based)
// col: column index
// type: ETDB_TYPE_* constant
// value: pointer to the value
// len: size of the value in bytes
// Returns 0 on success, -1 on error
int etdb_stmt_bind_param(ETDB_STMT* stmt, int col, int type, void* value, int len);

// Add current bound row to batch
// Returns 0 on success, -1 on error
int etdb_stmt_add_batch(ETDB_STMT* stmt);

// Execute the batch insert
// Returns number of affected rows, or -1 on error
int etdb_stmt_execute(ETDB_STMT* stmt);

// Get number of affected rows after execute (successfully inserted)
int etdb_stmt_affected_rows(ETDB_STMT* stmt);

// Get number of submitted rows after execute (total attempted)
int etdb_stmt_submitted_rows(ETDB_STMT* stmt);

// Get number of error rows after execute (failed)
int etdb_stmt_error_rows(ETDB_STMT* stmt);

// Get error info for a specific error entry (0-based index among errors)
// Returns 0 on success, -1 if index out of range
// rowIndex: output, 0-based row index in the batch
// errorCode: output, error code
int etdb_stmt_error_entry(ETDB_STMT* stmt, int index, int* rowIndex, int* errorCode);

// Close and free the prepared statement
void etdb_stmt_close(ETDB_STMT* stmt);

#ifdef __cplusplus
}
#endif

#endif // ETHERDB_C_API_H
