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

/*
 * etdb.c — C API Implementation
 * Thin wrapper around EtDBClient C++ classes.
 */

#include "etdb.h"
#include "EtDBClient.h"
#include <string>
#include <sstream>
#include <cstring>

using namespace ETDB::Client;
using ETDB::Query::Value;
using ETDB::Query::ValType;

// ============================================================================
// Internal handle types (matching opaque typedefs in etdb.h)
// Uses _Impl suffix to avoid name collision with ETDB::Client C++ classes
// ============================================================================
struct EtDBConn_Impl {
    ETDB::Client::EtDBClient* client;
    std::string user;
    std::string password;
    std::string db;
};

struct EtDBResult_Impl {
    ETDB::Client::EtDBResult result;
    mutable char strBuf[64];

    // ── Batch/streaming fetch state (C API level) ──
    std::vector<ETDB::Query::Value> row;   // current row buffer (etdb_fetch_row)
    bool  rowValid = false;                // etdb_fetch_row() produced a row
    int   blockBase = 0;                   // offset of current block within result rows
    int   blockRows = 0;                   // rows in current block (etdb_fetch_block)
    bool  blockValid = false;              // etdb_fetch_block() produced a batch
    std::vector<ETDB_FIELD> fields;        // cached field metadata
};

// Map a user-supplied (row, col) to the underlying result value, honoring the
// active fetch mode:
//   - after etdb_fetch_row():   (0, col) → current row buffer
//   - after etdb_fetch_block(): (r, col) with r in [0, blockRows) → block rows
//   - otherwise (one-shot etdb_query/etdb_use_result): plain result access
static ETDB::Query::Value etdbResultValue(const EtDBResult_Impl* r, int row, int col) {
    if (r->rowValid && row == 0) {
        if (col >= 0 && col < (int)r->row.size()) return r->row[col];
        return ETDB::Query::Value();
    }
    if (r->blockValid) {
        if (row < 0 || row >= r->blockRows) return ETDB::Query::Value();
        return r->result.get(r->blockBase + row, col);
    }
    return r->result.get(row, col);
}

// Refresh the cached ETDB_FIELD metadata from the result's column names/types.
static void etdbRefreshFields(EtDBResult_Impl* r) {
    r->fields.clear();
    int nc = r->result.colCount();
    const auto& names = r->result.columnNames();
    const auto& types = r->result.columnTypes();
    for (int i = 0; i < nc; ++i) {
        ETDB_FIELD f;
        memset(&f, 0, sizeof(f));
        strncpy(f.name, (i < (int)names.size()) ? names[i].c_str() : "", sizeof(f.name) - 1);
        int t = (i < (int)types.size()) ? (int)types[i] : 0;
        if (t < 0 || t > 13) t = 0;
        f.type = t;
        // Column width by type (BINARY/NCHAR declared size unknown here → -1)
        switch (t) {
            case ETDB_TYPE_TIMESTAMP: case ETDB_TYPE_BIGINT:
            case ETDB_TYPE_DOUBLE:    case ETDB_TYPE_UBIGINT: f.bytes = 8; break;
            case ETDB_TYPE_INT:       case ETDB_TYPE_UINT:
            case ETDB_TYPE_FLOAT:                              f.bytes = 4; break;
            case ETDB_TYPE_SMALLINT:  case ETDB_TYPE_USMALLINT: f.bytes = 2; break;
            case ETDB_TYPE_TINYINT:   case ETDB_TYPE_UTINYINT:
            case ETDB_TYPE_BOOL:                                f.bytes = 1; break;
            default:                                            f.bytes = -1; break;
        }
        r->fields.push_back(f);
    }
}

struct EtDBStmt_Impl {
    ETDB::Client::EtDBClient*           client;
    ETDB::Client::EtDBStmt*             stmtObj;
    int                                 numBinds;
    ETDB::Client::EtDBStmt::BindParam   binds[16];
};

// ============================================================================
// Library lifecycle
// ============================================================================
void etdb_init(void) {
    EtDBClient::init();
}

void etdb_cleanup(void) {
}

// ============================================================================
// Connection management
// ============================================================================
ETDB_CONN* etdb_connect(const char* host, uint16_t port,
                         const char* user, const char* password,
                         const char* db) {
    auto* conn = new EtDBConn_Impl();
    conn->client = new EtDBClient();
    conn->client->init();
    conn->user = user ? user : "root";
    conn->password = password ? password : "etherdbdata";
    conn->db = db ? db : "";
    if (!conn->client->connect(host, port, conn->user.c_str(),
                                conn->password.c_str(), conn->db.c_str())) {
        delete conn->client;
        delete conn;
        return nullptr;
    }
    return (ETDB_CONN*)conn;
}

void etdb_close(ETDB_CONN* h) {
    auto* conn = (EtDBConn_Impl*)h;
    if (!conn) return;
    conn->client->close();
    delete conn->client;
    delete conn;
}

int etdb_is_connected(ETDB_CONN* h) {
    auto* conn = (EtDBConn_Impl*)h;
    return conn && conn->client->isConnected() ? 1 : 0;
}

// ============================================================================
// Query operations
// ============================================================================
ETDB_RESULT* etdb_query(ETDB_CONN* h, const char* sql) {
    auto* conn = (EtDBConn_Impl*)h;
    if (!conn || !sql) return nullptr;
    auto* r = new EtDBResult_Impl();
    r->result = conn->client->query(sql);
    return (ETDB_RESULT*)r;
}

int etdb_column_count(ETDB_RESULT* h) {
    auto* r = (EtDBResult_Impl*)h;
    return r ? r->result.colCount() : 0;
}

int etdb_row_count(ETDB_RESULT* h) {
    auto* r = (EtDBResult_Impl*)h;
    return r ? r->result.rowCount() : 0;
}

const char* etdb_column_name(ETDB_RESULT* h, int col) {
    auto* r = (EtDBResult_Impl*)h;
    if (!r || col < 0 || col >= r->result.colCount()) return "";
    const auto& names = r->result.columnNames();
    if (col >= (int)names.size()) return "";
    return names[col].c_str();
}

const char* etdb_get_value(ETDB_RESULT* h, int row, int col) {
    auto* r = (EtDBResult_Impl*)h;
    if (!r) return "NULL";
    Value v = etdbResultValue(r, row, col);
    if (v.isNull()) return "NULL";
    std::string s = v.toString();
    strncpy(r->strBuf, s.c_str(), sizeof(r->strBuf) - 1);
    r->strBuf[sizeof(r->strBuf) - 1] = '\0';
    return r->strBuf;
}

int etdb_is_null(ETDB_RESULT* h, int row, int col) {
    auto* r = (EtDBResult_Impl*)h;
    if (!r) return 1;
    Value v = etdbResultValue(r, row, col);
    return v.isNull() ? 1 : 0;
}

int64_t etdb_get_int64(ETDB_RESULT* h, int row, int col) {
    auto* r = (EtDBResult_Impl*)h;
    if (!r) return 0;
    Value v = etdbResultValue(r, row, col);
    if (v.type == ValType::INT) return v.iVal;
    if (v.type == ValType::UINT64) return (int64_t)v.uVal;  // bit pattern
    if (v.type == ValType::FLOAT) return (int64_t)v.fVal;
    if (v.type == ValType::BOOL) return v.bVal ? 1 : 0;
    return 0;
}

double etdb_get_double(ETDB_RESULT* h, int row, int col) {
    auto* r = (EtDBResult_Impl*)h;
    if (!r) return 0.0;
    Value v = etdbResultValue(r, row, col);
    if (v.type == ValType::FLOAT) return v.fVal;
    if (v.type == ValType::INT) return (double)v.iVal;
    if (v.type == ValType::UINT64) return (double)v.uVal;
    if (v.type == ValType::BOOL) return v.bVal ? 1.0 : 0.0;
    return 0.0;
}

// ============================================================================
// Batch / streaming fetch implementation
// ============================================================================
ETDB_RESULT* etdb_use_result(ETDB_CONN* h, const char* sql) {
    return etdb_query(h, sql);
}

int etdb_field_count(ETDB_RESULT* h) {
    auto* r = (EtDBResult_Impl*)h;
    return r ? r->result.colCount() : 0;
}

const ETDB_FIELD* etdb_fetch_fields(ETDB_RESULT* h) {
    auto* r = (EtDBResult_Impl*)h;
    if (!r) return nullptr;
    if (r->fields.empty()) etdbRefreshFields(r);
    return r->fields.data();
}

int etdb_fetch_row(ETDB_RESULT* h) {
    auto* r = (EtDBResult_Impl*)h;
    if (!r) return -1;
    // Switching to row mode invalidates a previous block.
    r->blockValid = false;
    r->row.clear();
    if (!r->result.fetchRow(r->row)) {
        r->rowValid = false;
        return r->result.streamError() ? -1 : 0;
    }
    r->rowValid = true;
    return 1;
}

int etdb_fetch_block(ETDB_RESULT* h) {
    auto* r = (EtDBResult_Impl*)h;
    if (!r) return -1;
    // Switching to block mode invalidates a previous row.
    r->rowValid = false;
    int n = r->result.fetchBlock();
    if (n <= 0) {
        r->blockValid = false;
        r->blockRows = 0;
        return n;   // 0 = exhausted, -1 = error
    }
    // fetchBlock() consumes the block (rowCursor advanced past it); the block's
    // rows are the last n rows of the current in-memory batch.
    r->blockBase = r->result.rowCursor() - n;
    r->blockRows = n;
    r->blockValid = true;
    return n;
}

void etdb_free_result(ETDB_RESULT* h) {
    delete (EtDBResult_Impl*)h;
}

// ============================================================================
// Prepared statement operations
// ============================================================================
ETDB_STMT* etdb_stmt_init(ETDB_CONN* h) {
    auto* conn = (EtDBConn_Impl*)h;
    if (!conn) return nullptr;
    auto* s = new EtDBStmt_Impl();
    s->client = conn->client;
    s->stmtObj = conn->client->createStmt();
    s->numBinds = 0;
    memset(s->binds, 0, sizeof(s->binds));
    return (ETDB_STMT*)s;
}

int etdb_stmt_prepare(ETDB_STMT* h, const char* sql) {
    auto* s = (EtDBStmt_Impl*)h;
    if (!s || !sql) return -1;
    return s->stmtObj->prepare(sql) ? 0 : -1;
}

int etdb_stmt_bind_param(ETDB_STMT* h, int col, int type, void* value, int len) {
    auto* s = (EtDBStmt_Impl*)h;
    if (!s || col < 0 || col >= 16) return -1;
    s->binds[col].type   = type;
    s->binds[col].buffer = value;
    s->binds[col].length = (uintptr_t)len;
    if (col + 1 > s->numBinds) s->numBinds = col + 1;
    return 0;
}

int etdb_stmt_add_batch(ETDB_STMT* h) {
    auto* s = (EtDBStmt_Impl*)h;
    if (!s) return -1;
    if (!s->stmtObj->bindParam(s->binds, s->numBinds)) return -1;
    if (!s->stmtObj->addBatch()) return -1;
    return 0;
}

int etdb_stmt_execute(ETDB_STMT* h) {
    auto* s = (EtDBStmt_Impl*)h;
    if (!s) return -1;
    return s->stmtObj->execute();
}

int etdb_stmt_affected_rows(ETDB_STMT* h) {
    auto* s = (EtDBStmt_Impl*)h;
    return s ? s->stmtObj->affectedRows() : 0;
}

int etdb_stmt_submitted_rows(ETDB_STMT* h) {
    auto* s = (EtDBStmt_Impl*)h;
    return s ? s->stmtObj->submittedRows() : 0;
}

int etdb_stmt_error_rows(ETDB_STMT* h) {
    auto* s = (EtDBStmt_Impl*)h;
    return s ? s->stmtObj->errorRows() : 0;
}

int etdb_stmt_error_entry(ETDB_STMT* h, int index, int* rowIndex, int* errorCode) {
    auto* s = (EtDBStmt_Impl*)h;
    if (!s || !rowIndex || !errorCode) return -1;
    const auto& entries = s->stmtObj->errorEntries();
    if (index < 0 || index >= (int)entries.size()) return -1;
    *rowIndex  = entries[index].rowIndex;
    *errorCode = entries[index].errorCode;
    return 0;
}

void etdb_stmt_close(ETDB_STMT* h) {
    auto* s = (EtDBStmt_Impl*)h;
    if (!s) return;
    s->stmtObj->close();
    delete s->stmtObj;
    delete s;
}
