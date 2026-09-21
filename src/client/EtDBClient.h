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
 * EtDB Client — C++ OOP Client Library
 *
 * Provides object-oriented API for connecting to etherdb server,
 * executing SQL queries, and performing parameter-binding inserts.
 *
 * 本文件仅包含类声明，实现位于 EtDBClient.cpp。
 * 客户端自包含、跨平台（Windows / Linux），socket 经 tcpClient.h 封装。
 *
 * Usage:
 *   EtDBClient client;
 *   client.connect("127.0.0.1", 7000);
 *   auto result = client.query("SELECT * FROM test");
 *   result.print();
 *   client.close();
 */

#ifndef ETHERDB_CLIENT_H
#define ETHERDB_CLIENT_H

#include <string>
#include <string_view>
#include <vector>
#include <memory>
#include <unordered_map>
#include <cstdint>
#include <cstring>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>

#include "tcpClient.h"
#include "EtDBLog.h"
#include "EtDBClientProtocol.h"
#include "EtDBMeta.h"

namespace ETDB {
namespace Client {

using Query::Value;

class EtDBConnection;  // forward decl for streaming fetch

// ============================================================================
// Column-storage cell types (matches EtDBResult::_colMeta[].rep)
// ============================================================================
enum ColumnRep : uint8_t {
    COL_REP_NONE   = 0,
    COL_REP_INT    = 1,   // signed integer (1/2/4/8 bytes, host order)
    COL_REP_UINT   = 2,   // unsigned integer (1/2/4/8 bytes, host order)
    COL_REP_FLOAT  = 3,   // floating point (4=float / 8=double)
    COL_REP_BOOL   = 4,   // 1 byte (0/1)
    COL_REP_STRING = 5,   // variable length: byte pool + offset table (see _columnOffsets)
};

namespace detail {
// Types supported by column<T>() → column storage shape (unknown T returns nullptr)
template <typename T> struct ColTypeTraits {
    static constexpr uint8_t rep   = COL_REP_NONE;
    static constexpr uint8_t width = 0;
};
template <> struct ColTypeTraits<int8_t>   { static constexpr uint8_t rep = COL_REP_INT;   static constexpr uint8_t width = 1; };
template <> struct ColTypeTraits<uint8_t>  { static constexpr uint8_t rep = COL_REP_UINT;  static constexpr uint8_t width = 1; };
template <> struct ColTypeTraits<int16_t>  { static constexpr uint8_t rep = COL_REP_INT;   static constexpr uint8_t width = 2; };
template <> struct ColTypeTraits<uint16_t> { static constexpr uint8_t rep = COL_REP_UINT;  static constexpr uint8_t width = 2; };
template <> struct ColTypeTraits<int32_t>  { static constexpr uint8_t rep = COL_REP_INT;   static constexpr uint8_t width = 4; };
template <> struct ColTypeTraits<uint32_t> { static constexpr uint8_t rep = COL_REP_UINT;  static constexpr uint8_t width = 4; };
template <> struct ColTypeTraits<int64_t>  { static constexpr uint8_t rep = COL_REP_INT;   static constexpr uint8_t width = 8; };
template <> struct ColTypeTraits<uint64_t> { static constexpr uint8_t rep = COL_REP_UINT;  static constexpr uint8_t width = 8; };
template <> struct ColTypeTraits<float>    { static constexpr uint8_t rep = COL_REP_FLOAT; static constexpr uint8_t width = 4; };
template <> struct ColTypeTraits<double>   { static constexpr uint8_t rep = COL_REP_FLOAT; static constexpr uint8_t width = 8; };
template <> struct ColTypeTraits<bool>     { static constexpr uint8_t rep = COL_REP_BOOL;  static constexpr uint8_t width = 1; };
} // namespace detail

// ============================================================================
// EtDBResult — Query result wrapper (wire v2 columnar storage + zero-copy cursor)
//
// Results are stored per column (no per-row Value array any more):
//   _columnData[c]    — contiguous bytes of column c. Fixed-size columns = value
//                       array in host byte order; variable-length columns =
//                       byte pool (UTF-8 / binary payload).
//   _columnOffsets[c] — variable-length columns only: rowCount()+1 prefix offsets
//                       (relative to that column's byte pool).
//   _colMeta[c].nullBits — NULL bitmap (1 bit per row, 1=NULL).
//
// Fetching matches TDengine: the library never parses values.
//   · Column-wise: column<T>(col) returns the head pointer of the whole column
//     array (nullptr when T does not match the column's storage shape/width);
//     the caller just casts. String columns use columnStringData/Offsets or
//     stringValue(); NULL via isNull()/nullBitmap().
//   · Row-wise: fetchRow(const void**) returns raw pointers of the next row's
//     cells (zero-copy, no Value construction) — a row-count-only benchmark has
//     zero parse overhead; callers cast, or use column<T>() when parsing.
//   Compat interfaces get()/fetchRow(vector<Value>&) construct Values on demand
//   and are NOT on the zero-copy path.
//
// TDengine-style streaming is preserved: fetchRow() pulls row by row and issues
// the next server FETCH automatically when a batch is exhausted (fetchBlock()
// fetches whole blocks).
// ============================================================================
class EtDBResult {
public:
    EtDBResult() = default;
    ~EtDBResult() = default;

    // Result metadata
    int colCount() const;
    int rowCount() const;
    const std::vector<std::string>& columnNames() const;
    const std::vector<ColType>& columnTypes() const;   // per-column type (wire)

    // true when the current batch is a RAW BLOCK (wire layout 1): the rows are
    // the storage image verbatim (host byte order, fixed slots, zero padded
    // strings, no NULLs). The library keeps the block as-is — row pointers go
    // straight into it — and the columnar accessors materialize their column on
    // first use (cached until the next batch).
    bool isRawBlock() const { return _rawMode; }

    // ── Columnar zero-copy access (primary interface) ──
    // Head pointer of the column array; nullptr when the column is absent or T
    // does not match the column storage (shape/width).
    // E.g. INT column → column<int32_t>(); SMALLINT → column<int16_t>();
    //      BIGINT/TIMESTAMP → column<int64_t>(); UINT → column<uint32_t>();
    //      FLOAT → column<float>(); DOUBLE → column<double>(); BOOL → column<bool>().
    // Note: elements are in host byte order; NULL rows carry no valid value —
    // test with isNull() first.
    template <typename T> const T* column(int col) const;

    // Variable-length columns (NCHAR/BINARY): byte-pool base + rowCount()+1 prefix offsets.
    const char*     columnStringData(int col) const;
    const uint32_t* columnStringOffsets(int col) const;
    // Convenience access: zero-copy view [offsets[r], offsets[r+1]); NULL yields an empty view.
    std::string_view stringValue(int row, int col) const;

    // NULL check (bitmap); out-of-range returns true (consistent with get() returning NULL).
    bool isNull(int row, int col) const;
    // NULL-bitmap head pointer (ceil(rowCount/8) bytes; bit=1 marks the row NULL)
    const uint8_t* nullBitmap(int col) const;

    // Access by position (compat interface: constructs a Value on demand; not zero-copy)
    Value get(int row, int col) const;

    // Print formatted table
    void print() const;

    // Deserialize from binary buffer (SQueryRsp from server)
    bool deserialize(const uint8_t* buf, int bufLen);

    // Deserialize SSubmitRspMsg (INSERT response)
    bool deserializeSubmitRsp(const uint8_t* buf, int bufLen);

    // Submit response accessors (for INSERT results)
    int submitSubmitted() const;
    int submitAffected()  const;
    int submitErrors()    const;

    struct SubmitErrorInfo {
        int rowIndex;
        int errorCode;
    };
    const std::vector<SubmitErrorInfo>& submitErrorEntries() const;

    bool success() const;
    const std::string& error() const;
    void setError(const std::string& e);

    // ── Streaming accessors (TDengine-style fetch) ──
    int64_t streamQId() const;
    int64_t streamTotalRows() const;
    bool    streamEnded() const;
    bool    streamError() const;   // a FETCH round trip failed (network/error)

    // ── Row-wise fetch (TAOS_ROW style) ──
    // Zero-copy next row: writes each column's raw-value pointer into
    // cols[0..colCount()-1]; the library does no parsing at all (zero parse
    // overhead for a row-count-only benchmark):
    //   · fixed-size column → points at this row's value in column storage
    //     (host order; caller casts per column type, width = column type width:
    //      INT → int32_t, SMALLINT → int16_t ...)
    //   · string column → points at the payload in the byte pool
    //     (length via valueLength(col))
    //   · NULL cell → nullptr
    // Pointers stay valid until the next fetchRow()/fetchBlock()/deserialize().
    // Returns false when exhausted (streamError() == true means a FETCH failed).
    bool fetchRow(const void** cols);

    // Variable-length length of column col in the current row (the row obtained
    // by the most recent fetchRow); 0 for non-string columns / no current row / NULL.
    uint32_t valueLength(int col) const;

    // Compat interface: builds std::vector<Value> (per-value construction; not zero-copy)
    bool fetchRow(std::vector<Value>& out);

    // Fetch the next BATCH of rows (TDengine taos_fetch_block style). Returns
    // the number of rows in the new batch (>0), 0 when exhausted, -1 on error.
    // The batch replaces the current in-memory rows — access with get(row,col)
    // where row is relative to the batch (0-based). If some rows of the current
    // batch are still unconsumed (mixed fetchRow usage), they are served first.
    int fetchBlock();

    // Index of the next unconsumed row within the current in-memory batch.
    int rowCursor() const;

    // Set the per-FETCH byte budget (default 5 MiB).
    void setFetchBatchBytes(int64_t b);

    // Bind the connection used for subsequent FETCH calls (called by query())
    void setStreamConn(EtDBConnection* conn);

private:
    // Column storage metadata: shape + fixed width + NULL bitmap
    struct ColumnMeta {
        uint8_t rep   = COL_REP_NONE;
        uint8_t width = 0;                 // bytes per value for fixed-size columns (0 for variable-length)
        std::vector<uint8_t> nullBits;     // bitmap: 1 bit per row, 1=NULL
    };

    // Set up the column-storage skeleton (per wire column type); buffers are reused across batches.
    void setupColumns(int numCols, int numRows);
    // Parse one wire value into (col,row); returns bytes consumed, <0 = corrupt stream / type mismatch.
    int  storeValue(int col, int row, const char* p, const char* end);
    // Clear the previous batch (buffer capacity is kept for reuse by streaming FETCH).
    void resetStorage();    // Advance the row cursor: rows left in batch → true; exhausted → triggers FETCH; ended/failed → false.
    bool advanceRow();
    // Raw pointer of cell (row,col) (NULL → nullptr; string → payload in the byte pool).
    const void* cellPtr(int row, int col) const;
    // Common implementation of column<T>(): matches storage shape/width and returns the column data pointer.
    const void* columnRaw(int col, uint8_t rep, uint8_t width) const;

    // ── Raw-block (wire layout 1) ──
    // Decode one raw slot into a Value (host byte order; strings zero-trimmed).
    Value rawSlotValue(int row, int col) const;
    // Trimmed payload length of a raw string slot (0 for non-string columns).
    uint32_t rawSlotLength(int row, int col) const;
    // Build the columnar view of a raw column on demand (byte pool + offsets for
    // strings, strided copy for fixed-size columns); cached until the next batch.
    bool materializeRawCol(int col) const;

    std::vector<std::string> _columnNames;
    std::vector<ColType>     _columnTypes;   // wire column types (server-resolved from actual values)
    mutable std::vector<std::vector<uint8_t>>  _columnData;     // [col] contiguous column bytes (lazy in raw mode)
    mutable std::vector<std::vector<uint32_t>> _columnOffsets;  // [col] variable-length offsets (empty otherwise)
    std::vector<ColumnMeta>            _colMeta;        // [col]
    int _rowCount = 0;
    std::string _error;

    // Raw-block storage: block = _rowCount rows of _rawStride bytes (row-major,
    // host order). _rawRowOff/_rawSlotBytes come from the wire column descriptors.
    std::vector<uint8_t>  _rawBlock;
    std::vector<int32_t>  _rawRowOff;
    std::vector<int32_t>  _rawSlotBytes;
    mutable std::vector<uint8_t> _rawMat;   // [col] columnar materialization done?
    int32_t _rawStride = 0;
    bool    _rawMode   = false;

    // Streaming state (TDengine-style fetch)
    EtDBConnection* _conn = nullptr;
    int64_t _streamQId = 0;
    int64_t _streamTotalRows = 0;
    bool    _streamEnded = true;
    bool    _streamError = false;   // a FETCH failed (not merely exhausted)
    int     _rowCursor = 0;
    int64_t _fetchBatchBytes = ETDB_STREAM_BATCH_BYTES;  // byte budget per FETCH

    // Submit response fields (for INSERT results)
    int _submitSubmitted = 0;
    int _submitAffected  = 0;
    int _submitErrors    = 0;
    std::vector<SubmitErrorInfo> _submitErrorEntries;
};

// column<T>(): zero-copy head pointer of the column array (nullptr if T mismatches storage shape/width)
template <typename T>
const T* EtDBResult::column(int col) const {
    return static_cast<const T*>(
        columnRaw(col, detail::ColTypeTraits<T>::rep, detail::ColTypeTraits<T>::width));
}

// ============================================================================
// EtDBConnection — Manages one TCP connection to etherdb server
// ============================================================================
class EtDBConnection {
public:
    EtDBConnection() = default;
    ~EtDBConnection();

    // Non-copyable
    EtDBConnection(const EtDBConnection&) = delete;
    EtDBConnection& operator=(const EtDBConnection&) = delete;

    // Connect to server
    bool connect(const std::string& host, uint16_t port,
                 const char* user = "root",
                 const char* password = "etherdbdata",
                 const char* db = "");

    // Close connection
    void close();

    // Execute a SQL query and get results
    // metaCache: optional meta cache for INSERT column type resolution
    EtDBResult query(const std::string& sql, int dbId = 1,
                     class EtDBMetaCache* metaCache = nullptr);

    // Fetch the next batch of a streaming query (TDengine-style FETCH, MSG=5).
    bool fetchStreamRows(int64_t qId, int batchSize, EtDBResult& out);

    // Execute an INSERT statement (via binary submit message)
    int executeInsert(const uint8_t* submitData, int submitLen, int dbId,
                      std::vector<uint8_t>& rspCont);

    // ── Async insert (taos_query_a style: submit returns immediately) ──
    // Build the SUBMIT message with a fresh batch id, send it and return the
    // batch id (>0) right away without waiting for the server. The response is
    // collected by a background receiver thread and indexed by batch id; use
    // waitAsyncResult()/getAsyncResult()/waitAllAsync() to retrieve it later.
    // Returns -1 on failure.
    int64_t executeInsertAsync(const uint8_t* submitData, int submitLen, int dbId);

    // Result of one async insert batch (taos_affected_rows / taos_errno style).
    struct AsyncResult {
        int32_t code      = 0;   // RPC code, 0 = success (like taos_errno)
        int32_t submitted = 0;   // rows submitted to the server
        int32_t affected  = 0;   // rows actually inserted (like taos_affected_rows)
        int32_t errors    = 0;   // rows rejected by the server
        bool    ready     = false;
    };

    // Block until the result of `batchId` is available (timeoutMs<=0 = forever).
    bool waitAsyncResult(uint64_t batchId, int timeoutMs = 0);
    // Non-blocking check whether `batchId`'s result has arrived.
    bool pollAsyncResult(uint64_t batchId) const;
    // Consume the result for `batchId`: returns a copy and REMOVES the entry,
    // freeing one backpressure slot (see setMaxAsyncPending). ready=false if
    // the result is not available yet.
    AsyncResult getAsyncResult(uint64_t batchId);
    // Non-consuming peek: returns a copy but keeps the entry (does not free a
    // backpressure slot). Use getAsyncResult() to actually consume.
    AsyncResult peekAsyncResult(uint64_t batchId) const;
    // Block until all previously submitted async batches have results.
    bool waitAllAsync(int timeoutMs = 0);

    // Bound the number of not-yet-consumed async results. executeInsertAsync()
    // blocks (backpressure) once this many results are pending and unconsumed,
    // so a client that never drains cannot grow memory without bound. Slots are
    // freed by consuming via getAsyncResult(). Default: 1024.
    void setMaxAsyncPending(int n);

    // Counters (for diagnostics / test stats).
    uint64_t asyncSentCount() const;     // batches submitted via executeInsertAsync
    uint64_t asyncReceivedCount() const; // results received so far
    uint64_t asyncPendingCount() const;  // received but not yet consumed

    // Connection state
    bool isConnected() const;

    // Get raw socket (for advanced use)
    int socketFd() const;

    // Send full message (STxHead + content) and receive response
    bool sendRecv(const uint8_t* msg, int msgLen, std::vector<uint8_t>& rspCont,
                  int& rspCode, uint8_t& rspType);

    // Table name→UID cache (populated by EtDBClient on CREATE TABLE or first use)
    uint64_t lookupTableUid(const std::string& tableName);
    void     cacheTableUid(const std::string& tableName, uint64_t uid);

    // Resolve table UID by sending a meta query to the server
    uint64_t resolveTableUidFromServer(const std::string& dbName, const std::string& tableName);

    // Credential accessors
    const std::string& userName() const;
    const std::string& password() const;
    const std::string& dbName() const;

    // dbId of the current USE database (bound to the db, resolved on USE)
    int32_t currentdbId() const;

private:
    bool ensureAsyncRecv();          // lazily start the background receiver thread
    void recvLoop();                 // background thread: read responses, dispatch

    TcpSocket _sock = INVALID_TCP_SOCKET;
    int _lastdbId = 1;  // dbId of the most recent query (for FETCH routing)
    int _currentdbId = 1;  // dbId of the current USE database (routing)
    std::unordered_map<std::string, uint64_t> _tableUidCache;
    std::string _user = "root";
    std::string _password = "etherdbdata";
    std::string _db;

    // ── Async insert state ──
    std::thread              _recvThread;
    std::atomic<bool>        _recvRunning{false};
    std::atomic<uint64_t>    _nextBatchId{1};     // sequential batch ids (start at 1)
    std::atomic<uint64_t>    _sentAsyncCount{0};  // async batches submitted
    std::atomic<uint64_t>    _receivedCount{0};   // async results received
    std::atomic<uint64_t>    _consumedCount{0};   // results consumed by user
    std::mutex               _ioMutex;            // serializes socket sends
    mutable std::mutex       _respMutex;          // guards results + sync slot
    int                      _maxAsyncPending = 1024;  // backpressure cap
    std::condition_variable  _respCond;
    // Results keyed by batch id. A batch id is a correlation handle, NOT a
    // vector subscript — so results live in a map, decoupled from array
    // length. Correlated by the batchId echoed by the server when present,
    // falling back to submission order (responses are in-order today).
    std::unordered_map<uint64_t, AsyncResult> _asyncResults;
    std::vector<uint8_t>     _sendBuf;            // reused send buffer (hot path)
    bool                     _syncWaiting = false;// a synchronous request is in flight
    bool                     _syncReady   = false;// sync response delivered
    int                      _syncCode    = 0;
    uint8_t                  _syncType    = 0;
    std::vector<uint8_t>     _syncCont;
};

// ============================================================================
// EtDBStmt — Prepared statement (parameter binding insert)
// ============================================================================
class EtDBStmt {
public:
    // Types for column binding (matching ColType enum)
    enum BindType {
        TYPE_TIMESTAMP = 0,
        TYPE_BOOL      = 1,
        TYPE_TINYINT   = 2,
        TYPE_SMALLINT  = 3,
        TYPE_INT       = 4,
        TYPE_BIGINT    = 5,
        TYPE_FLOAT     = 6,
        TYPE_DOUBLE    = 7,
        TYPE_BINARY    = 8,
        TYPE_NCHAR     = 9,
    };

    // Single parameter bind descriptor
    struct BindParam {
        int      type;        // BindType enum value
        void*    buffer;      // pointer to value
        uintptr_t length;     // for variable-length types
        uintptr_t* lengthPtr; // pointer to actual length (varlen)
        int*     isNull;      // NULL indicator pointer

        BindParam() : type(0), buffer(nullptr), length(0), lengthPtr(nullptr), isNull(nullptr) {}
        BindParam(int t, void* buf) : type(t), buffer(buf), length(0), lengthPtr(nullptr), isNull(nullptr) {}
    };

    // Multi-row column bind descriptor
    struct MultiBind {
        int      type;
        void*    buffer;       // column data array
        uintptr_t stride;      // bytes between rows
        int32_t* lengths;      // per-row lengths (varlen)
        char*    nullFlags;    // per-row null flags
        int      numRows;

        MultiBind() : type(0), buffer(nullptr), stride(0), lengths(nullptr), nullFlags(nullptr), numRows(0) {}
    };

    EtDBStmt(EtDBConnection* conn, EtDBMetaCache* cache);
    ~EtDBStmt();

    // Prepare an INSERT statement
    bool prepare(const std::string& sql);

    // Bind parameters for one row
    bool bindParam(BindParam* binds, int numBinds);

    // Bind multiple rows in columnar format
    bool bindParamBatch(MultiBind* binds, int numBinds);

    // Add current bound row to batch
    bool addBatch();

    // Execute the batch insert
    int  execute(int dbId = 1);

    // Asynchronously submit the current batch and return immediately (like
    // taos_query_a): returns the batch id (>0) used to retrieve the result via
    // EtDBClient::waitAsyncResult()/getAsyncResult(), or -1 on failure.
    int64_t executeAsync(int dbId = 1);

    // Get number of affected rows (after execute)
    int  affectedRows() const;
    int  submittedRows() const;
    int  errorRows() const;

    // Per-row error information
    struct RowError {
        int rowIndex;    // 0-based row index
        int errorCode;   // error code
    };
    const std::vector<RowError>& errorEntries() const;

    // Close the statement
    void close();

private:
    int  parseInsertSql(const std::string& sql);
    int  buildInsertRow(const BindParam* binds, int numBinds, uint8_t* rowBuf, int rowBufSize);
    int  buildSubmitMsg(int numRows, int rowBytes, const uint8_t* rowsData);
    int  sendToServer(int dbId);

    EtDBConnection* _conn;
    std::string     _tableName;
    int             _numCols     = 0;
    int             _rowBytes    = 0;
    int             _batchRows   = 0;
    int             _affectedRows = 0;
    int             _submittedRows = 0;
    int             _errorRows    = 0;
    std::vector<RowError> _errorEntries;

    // Column types (resolved from table metadata)
    std::vector<int> _colTypes;
    // Storage bytes per column (BINARY/NCHAR use declared-size fixed slots)
    std::vector<int> _colBytes;

    // Current row buffer (host byte order)
    std::vector<uint8_t> _rowBuf;

    // Batch data buffer (network byte order for SDataBlock)
    std::vector<uint8_t> _batchData;

    // State
    enum State { INIT, PREPARED, BINDING, BATCHED };
    State _state = INIT;

    // Table meta (fetched on prepare)
    const TableMeta* _tableMeta = nullptr;
    EtDBMetaCache*   _metaCache = nullptr;
    int32_t          _targetdbId = 1;      // dbId from meta for routing
    uint64_t         _tableUid   = 0;      // uid from meta
    int32_t          _tableTid   = 0;      // tid from meta
};

// ============================================================================
// EtDBClient — High-level client API
// ============================================================================
class EtDBClient {
public:
    EtDBClient();
    ~EtDBClient();

    // Initialize client library (must be called once)
    static void init();

    // Connect to etherdb server
    bool connect(const std::string& host, uint16_t port,
                 const char* user = "root",
                 const char* password = "etherdbdata",
                 const char* db = "");

    // Execute a SQL query. dbId<=0 → route to the query's target database's dbId:
    // the SQL's `FROM/INTO db.table` prefix wins, otherwise the current USE db.
    EtDBResult query(const std::string& sql, int dbId = 0);

    // Resolve the dbId for a given database (for query routing)
    int32_t resolveDBdbId(const std::string& dbName);

    // Meta operations
    TableMeta* fetchTableMeta(const std::string& dbName, const std::string& tableName);
    TableMeta* getTableMeta(const std::string& dbName, const std::string& tableName);
    int fetchAllTableMeta(const std::string& dbName);
    void invalidateMeta(const std::string& dbName, const std::string& tableName);

    // Create a prepared statement
    EtDBStmt* createStmt();

    // ── Async insert results ──
    // `batchId` is returned by EtDBStmt::executeAsync().
    bool waitAsyncResult(int64_t batchId, int timeoutMs = 0);   // block until ready
    bool pollAsyncResult(int64_t batchId) const;                // non-blocking check
    EtDBConnection::AsyncResult getAsyncResult(int64_t batchId);   // consume (remove)
    EtDBConnection::AsyncResult peekAsyncResult(int64_t batchId) const;  // peek
    bool waitAllAsync(int timeoutMs = 0);                       // all sent batches
    void setMaxAsyncPending(int n);                             // backpressure cap
    int64_t asyncSentCount() const;     // batches submitted via executeAsync
    int64_t asyncReceivedCount() const; // results received so far
    int64_t asyncPendingCount() const;  // received but not consumed

    // Close connection
    void close();

    // Check connection state
    bool isConnected() const;

    // Get underlying connection (advanced use)
    EtDBConnection* connection() { return &_conn; }

private:
    EtDBConnection _conn;
    EtDBMetaCache _metaCache;
    // db(小写) → dbId 缓存。只缓存解析成功(>0)的结果；失败不缓存，建表后可重试。
    std::unordered_map<std::string, int32_t> _dbIdCache;
};

} // namespace Client
} // namespace ETDB

#endif // ETHERDB_CLIENT_H
