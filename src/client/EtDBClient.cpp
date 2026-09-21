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
 * EtDBClient.cpp — EtDBResult / EtDBConnection / EtDBStmt / EtDBClient /
 *                  EtDBMetaCache::fetchTableMeta 实现
 *
 * Migrated from the inline implementation in the original EtDBClient.h; 
 * socket calls are now uniformly routed through the TcpClient wrapper,
 * supporting compilation on both Windows and Linux platforms.
 */

#include "EtDBClient.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <algorithm>
#include <chrono>

namespace ETDB {
namespace Client {
constexpr int MAX_SQL_LEN = 128*1024;
// ============================================================================
// Query phase timing (performance analysis)
// Enable with ETDB_QTIME=1; zero-cost otherwise. Breaks down where query time
// goes: network RTT vs result deserialization vs per-row fetchRow copies.
// ============================================================================
namespace {
struct QPerf {
    bool enabled = false;
    int64_t queryRttUs  = 0;   // sendRecv (QUERY request/response) wall time
    int64_t queryDeserUs = 0;  // EtDBResult::deserialize (first batch)
    int64_t fetchRttUs  = 0;   // sendRecv (FETCH request/response) wall time
    int64_t fetchDeserUs = 0;  // EtDBResult::deserialize (subsequent batches)
    int64_t fetchBatches = 0;  // number of FETCH round trips
    int64_t rowCopyUs   = 0;   // EtDBResult::fetchRow per-row copy
    int64_t rowsFetched = 0;   // rows served through fetchRow
};
QPerf g_qperf;

inline int64_t qnowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::high_resolution_clock::now().time_since_epoch()).count();
}

inline bool qtimeEnabled() {
    static const bool on = (getenv("ETDB_QTIME") != nullptr &&
                            std::string(getenv("ETDB_QTIME")) == "1");
    return on;
}

// Print the accumulated breakdown (called once per process on close()).
inline void qtimeReport() {
    if (!g_qperf.enabled) return;
    fprintf(stderr,
        "[QTIME] queryRTT=%lldus queryDeser=%lldus | fetchRTT=%lldus fetchDeser=%lldus "
        "(batches=%lld) | fetchRowCopy=%lldus rows=%lld\n",
        (long long)g_qperf.queryRttUs, (long long)g_qperf.queryDeserUs,
        (long long)g_qperf.fetchRttUs, (long long)g_qperf.fetchDeserUs,
        (long long)g_qperf.fetchBatches,
        (long long)g_qperf.rowCopyUs, (long long)g_qperf.rowsFetched);
    g_qperf = QPerf{};
}
} // namespace

// ============================================================================
// EtDBResult Implementation — wire v2 columnar storage
//
// Deserialization is "read row-major, write column-major": each parsed value is
// written straight into its column's contiguous buffer (byte swap happens here
// exactly once), constructing no Query::Value; fetching uses zero-copy pointers
// via column<T>()/stringValue(). Streaming FETCH reuses each column's buffer.
// ============================================================================

namespace {

// Column type → storage shape/width. The server-side header type byte is already
// resolved from "declared type × actual values" (expression columns get their
// real encoding type too), so the client maps it directly:
//   SMALLINT→2-byte short, INT→4-byte int, BIGINT/TIMESTAMP→8 bytes;
//   unsigned at the same widths; FLOAT/DOUBLE→float/double; NCHAR/BINARY→variable string column.
inline void colTypeStore(ColType t, uint8_t& rep, uint8_t& width) {
    switch (t) {
        case ColType::BOOL:      rep = COL_REP_BOOL;  width = 1; break;
        case ColType::TINYINT:   rep = COL_REP_INT;   width = 1; break;
        case ColType::UTINYINT:  rep = COL_REP_UINT;  width = 1; break;
        case ColType::SMALLINT:  rep = COL_REP_INT;   width = 2; break;  // 2-byte short
        case ColType::USMALLINT: rep = COL_REP_UINT;  width = 2; break;
        case ColType::INT:       rep = COL_REP_INT;   width = 4; break;  // 4-byte int
        case ColType::UINT:      rep = COL_REP_UINT;  width = 4; break;
        case ColType::BIGINT:
        case ColType::TIMESTAMP: rep = COL_REP_INT;   width = 8; break;
        case ColType::UBIGINT:   rep = COL_REP_UINT;  width = 8; break;
        case ColType::FLOAT:     rep = COL_REP_FLOAT; width = 4; break;
        case ColType::DOUBLE:    rep = COL_REP_FLOAT; width = 8; break;
        case ColType::NCHAR:
        case ColType::BINARY:    rep = COL_REP_STRING; width = 0; break;
        default:                 rep = COL_REP_INT;   width = 8; break;  // fallback for unknown types
    }
}

// Fixed-slot writes (saturating, defensive; values never exceed the column range in normal streams)
inline void putSigned(uint8_t* dst, int w, int64_t v) {
    switch (w) {
        case 1: { int8_t  x = (int8_t)(v < -128 ? -128 : (v > 127 ? 127 : v)); memcpy(dst, &x, 1); break; }
        case 2: { int16_t x = (int16_t)(v < -32768 ? -32768 : (v > 32767 ? 32767 : v)); memcpy(dst, &x, 2); break; }
        case 4: { int32_t x = (int32_t)(v < INT32_MIN ? INT32_MIN : (v > INT32_MAX ? INT32_MAX : v)); memcpy(dst, &x, 4); break; }
        default:{ int64_t x = v; memcpy(dst, &x, 8); break; }
    }
}
inline void putUnsigned(uint8_t* dst, int w, uint64_t v) {
    switch (w) {
        case 1: { uint8_t  x = (uint8_t)(v > 0xFFULL ? 0xFFULL : v); memcpy(dst, &x, 1); break; }
        case 2: { uint16_t x = (uint16_t)(v > 0xFFFFULL ? 0xFFFFULL : v); memcpy(dst, &x, 2); break; }
        case 4: { uint32_t x = (uint32_t)(v > 0xFFFFFFFFULL ? 0xFFFFFFFFULL : v); memcpy(dst, &x, 4); break; }
        default:{ uint64_t x = v; memcpy(dst, &x, 8); break; }
    }
}
inline void putFloat(uint8_t* dst, int w, double d) {
    if (w == 4) { float f = (float)d; memcpy(dst, &f, 4); }
    else        { memcpy(dst, &d, 8); }
}

// Fixed-slot reads (host byte order)
inline int64_t getSigned(const uint8_t* p, int w) {
    switch (w) {
        case 1: { int8_t  x; memcpy(&x, p, 1); return x; }
        case 2: { int16_t x; memcpy(&x, p, 2); return x; }
        case 4: { int32_t x; memcpy(&x, p, 4); return x; }
        default:{ int64_t x; memcpy(&x, p, 8); return x; }
    }
}
inline uint64_t getUnsigned(const uint8_t* p, int w) {
    switch (w) {
        case 1: { uint8_t  x; memcpy(&x, p, 1); return x; }
        case 2: { uint16_t x; memcpy(&x, p, 2); return x; }
        case 4: { uint32_t x; memcpy(&x, p, 4); return x; }
        default:{ uint64_t x; memcpy(&x, p, 8); return x; }
    }
}
inline double getFloat(const uint8_t* p, int w) {
    if (w == 4) { float f; memcpy(&f, p, 4); return (double)f; }
    double d; memcpy(&d, p, 8); return d;
}

// Parsed result of one wire v2 value (byte order already converted to host; no Query::Value constructed)
struct WireVal {
    uint8_t     tag   = VAL_NULL;
    bool        isNum = false;    // numeric / boolean value
    bool        isU   = false;    // unsigned origin
    bool        isF   = false;    // floating-point origin
    int64_t     i     = 0;        // signed view (unsigned values truncated as needed)
    uint64_t    u     = 0;        // unsigned view
    double      d     = 0.0;      // floating-point view
    const char* s     = nullptr;  // string payload
    uint32_t    slen  = 0;
};

// Parse one value; returns bytes consumed, <0 = corrupt stream / unknown tag.
inline int parseWireValue(const char* p, const char* end, WireVal& out) {
    if (p >= end) return -1;
    const uint8_t tag = (uint8_t)p[0];
    out.tag = tag; out.isNum = false; out.isU = false; out.isF = false;
    out.s = nullptr; out.slen = 0;
    switch (tag) {
        case VAL_NULL: return 1;
        case VAL_BOOL:
            if (end - p < 2) return -1;
            out.isNum = true; out.i = (p[1] != 0) ? 1 : 0; out.u = (uint64_t)out.i;
            return 2;
        case VAL_TINYINT:
            if (end - p < 2) return -1;
            out.isNum = true; out.i = (int8_t)p[1]; out.u = (uint64_t)out.i;
            return 2;
        case VAL_UTINYINT:
            if (end - p < 2) return -1;
            out.isNum = true; out.isU = true; out.u = (uint8_t)p[1]; out.i = (int64_t)out.u;
            return 2;
        case VAL_SHORT: {
            if (end - p < 3) return -1;
            uint16_t x; memcpy(&x, p + 1, 2);
            out.isNum = true; out.i = (int64_t)(int16_t)ntohs(x); out.u = (uint64_t)out.i;
            return 3;
        }
        case VAL_USHORT: {
            if (end - p < 3) return -1;
            uint16_t x; memcpy(&x, p + 1, 2);
            out.isNum = true; out.isU = true; out.u = (uint16_t)ntohs(x); out.i = (int64_t)out.u;
            return 3;
        }
        case VAL_INT: {
            if (end - p < 5) return -1;
            uint32_t x; memcpy(&x, p + 1, 4);
            out.isNum = true; out.i = (int64_t)(int32_t)ntohl(x); out.u = (uint64_t)out.i;
            return 5;
        }
        case VAL_UINT32: {
            if (end - p < 5) return -1;
            uint32_t x; memcpy(&x, p + 1, 4);
            out.isNum = true; out.isU = true; out.u = (uint32_t)ntohl(x); out.i = (int64_t)out.u;
            return 5;
        }
        case VAL_BIGINT: {
            if (end - p < 9) return -1;
            uint64_t x; memcpy(&x, p + 1, 8);
            out.isNum = true; out.i = (int64_t)be64toh(x);
            out.u = (out.i < 0) ? 0 : (uint64_t)out.i;
            return 9;
        }
        case VAL_UINT64: {
            if (end - p < 9) return -1;
            uint64_t x; memcpy(&x, p + 1, 8);
            out.isNum = true; out.isU = true; out.u = be64toh(x);
            out.i = (out.u > (uint64_t)INT64_MAX) ? INT64_MAX : (int64_t)out.u;
            return 9;
        }
        case VAL_FLOAT: {
            if (end - p < 9) return -1;
            uint64_t x; memcpy(&x, p + 1, 8); x = be64toh(x);
            memcpy(&out.d, &x, 8);
            out.isNum = true; out.isF = true;
            return 9;
        }
        case VAL_STRING: {
            if (end - p < 5) return -1;
            uint32_t l; memcpy(&l, p + 1, 4); l = ntohl(l);
            if ((uint64_t)(end - p - 5) < l) return -1;
            out.s = p + 5; out.slen = l;
            return 5 + (int)l;
        }
        default: return -1;
    }
}

} // namespace

// ============================================================================
// Raw-block accessors (wire layout 1)
// ============================================================================
// A raw block is the storage row image: _rawStride bytes per row, each column
// at _rawRowOff[col] with _rawSlotBytes[col] bytes (host byte order, zero padded
// strings, no NULLs). Nothing is parsed when the batch arrives; the accessors
// below only compute addresses, and the columnar views are materialized lazily.

uint32_t EtDBResult::rawSlotLength(int row, int col) const {
    if (row < 0 || row >= _rowCount || col < 0 || col >= (int)_rawSlotBytes.size()) return 0;
    const ColType ct = (ColType)(uint8_t)_columnTypes[(size_t)col];
    if (ct != ColType::NCHAR && ct != ColType::BINARY) return 0;
    const uint8_t* p = _rawBlock.data() + (int64_t)row * _rawStride + _rawRowOff[(size_t)col];
    int32_t n = _rawSlotBytes[(size_t)col];
    while (n > 0 && p[n - 1] == 0) n--;      // slots are zero padded
    return (uint32_t)n;
}

Value EtDBResult::rawSlotValue(int row, int col) const {
    if (row < 0 || row >= _rowCount || col < 0 || col >= (int)_rawSlotBytes.size()) return Value();
    const uint8_t* p = _rawBlock.data() + (int64_t)row * _rawStride + _rawRowOff[(size_t)col];
    switch ((ColType)(uint8_t)_columnTypes[(size_t)col]) {
        case ColType::TIMESTAMP: { int64_t v; memcpy(&v, p, 8); return Value(v); }
        case ColType::BIGINT:    { int64_t v; memcpy(&v, p, 8); return Value(v); }
        case ColType::DOUBLE:    { double  v; memcpy(&v, p, 8); return Value(v); }
        case ColType::INT:       { int32_t v; memcpy(&v, p, 4); return Value((int64_t)v); }
        case ColType::FLOAT:     { float   v; memcpy(&v, p, 4); return Value((double)v); }
        case ColType::SMALLINT:  { int16_t v; memcpy(&v, p, 2); return Value((int64_t)v); }
        case ColType::TINYINT:   { int8_t  v; memcpy(&v, p, 1); return Value((int64_t)v); }
        case ColType::BOOL:      { int8_t  v; memcpy(&v, p, 1); return Value(v != 0); }
        case ColType::UTINYINT:  { uint8_t  v; memcpy(&v, p, 1); return Value((int64_t)v); }
        case ColType::USMALLINT: { uint16_t v; memcpy(&v, p, 2); return Value((int64_t)v); }
        case ColType::UINT:      { uint32_t v; memcpy(&v, p, 4); return Value((int64_t)v); }
        case ColType::UBIGINT:   { uint64_t v; memcpy(&v, p, 8); return Value::fromUInt64(v); }
        case ColType::BINARY:
        case ColType::NCHAR:
            return Value(std::string((const char*)p, (size_t)rawSlotLength(row, col)));
        default: return Value();
    }
}

bool EtDBResult::materializeRawCol(int col) const {
    if (!_rawMode || col < 0 || col >= (int)_rawSlotBytes.size()) return false;
    if ((size_t)col < _rawMat.size() && _rawMat[(size_t)col]) return true;
    const ColumnMeta& m = _colMeta[(size_t)col];
    const int32_t off  = _rawRowOff[(size_t)col];
    const int32_t slot = _rawSlotBytes[(size_t)col];
    if (m.rep == COL_REP_STRING) {
        // Same shape as the tag path: byte pool + rowCount()+1 prefix offsets.
        std::vector<uint8_t>&  pool = _columnData[(size_t)col];
        std::vector<uint32_t>& offs = _columnOffsets[(size_t)col];
        pool.clear();
        offs.assign((size_t)_rowCount + 1, 0);
        for (int r = 0; r < _rowCount; ++r) {
            const uint8_t* p = _rawBlock.data() + (int64_t)r * _rawStride + off;
            int32_t n = slot;
            while (n > 0 && p[n - 1] == 0) n--;
            pool.insert(pool.end(), p, p + n);
            offs[(size_t)r + 1] = (uint32_t)pool.size();
        }
    } else if (m.rep != COL_REP_NONE) {
        std::vector<uint8_t>& data = _columnData[(size_t)col];
        data.resize((size_t)_rowCount * (size_t)m.width);
        for (int r = 0; r < _rowCount; ++r)
            memcpy(data.data() + (size_t)r * m.width,
                   _rawBlock.data() + (int64_t)r * _rawStride + off, (size_t)m.width);
        _columnOffsets[(size_t)col].clear();
    } else {
        return false;
    }
    if (_rawMat.size() < _rawSlotBytes.size()) _rawMat.resize(_rawSlotBytes.size(), 0);
    _rawMat[(size_t)col] = 1;
    return true;
}

int EtDBResult::colCount() const { return (int)_columnNames.size(); }
int EtDBResult::rowCount() const { return _rowCount; }
const std::vector<std::string>& EtDBResult::columnNames() const { return _columnNames; }
const std::vector<ColType>& EtDBResult::columnTypes() const { return _columnTypes; }

// Clear the previous batch (column buffer capacity is kept for repeated streaming FETCH reuse)
void EtDBResult::resetStorage() {
    _columnNames.clear();
    _columnTypes.clear();
    _rowCount = 0;
    for (auto& c : _columnData) c.clear();
    for (auto& o : _columnOffsets) o.clear();
    for (auto& m : _colMeta) { m.rep = COL_REP_NONE; m.width = 0; m.nullBits.clear(); }
    _rawMode = false;
    _rawStride = 0;
    _rawBlock.clear();
    _rawRowOff.clear();
    _rawSlotBytes.clear();
    _rawMat.clear();
}

// Set up/reset column storage per wire column type (fixed-size columns preallocate rowCount*width; variable-length columns clear the byte pool)
void EtDBResult::setupColumns(int numCols, int numRows) {
    _rowCount = numRows;
    _columnData.resize((size_t)numCols);
    _columnOffsets.resize((size_t)numCols);
    _colMeta.resize((size_t)numCols);
    const size_t nullBytes = ((size_t)numRows + 7) / 8;
    for (int ci = 0; ci < numCols; ++ci) {
        uint8_t rep = COL_REP_NONE, width = 0;
        colTypeStore(_columnTypes[(size_t)ci], rep, width);
        ColumnMeta& m = _colMeta[(size_t)ci];
        m.rep   = rep;
        m.width = width;
        m.nullBits.assign(nullBytes, 0);
        std::vector<uint8_t>& data  = _columnData[(size_t)ci];
        std::vector<uint32_t>& offs = _columnOffsets[(size_t)ci];
        if (rep == COL_REP_STRING) {
            data.clear();                               // keep capacity
            offs.assign((size_t)numRows + 1, 0);
        } else {
            data.resize((size_t)numRows * width);       // new bytes are zero-initialized
            offs.clear();
        }
    }
}

// Parse one wire value into (col,row); returns bytes consumed, <0 = corrupt stream / type mismatch.
int EtDBResult::storeValue(int col, int row, const char* p, const char* end) {
    WireVal wv;
    int adv = parseWireValue(p, end, wv);
    if (adv < 0) return -1;

    ColumnMeta& m = _colMeta[(size_t)col];
    if (wv.tag == VAL_NULL) {
        m.nullBits[(size_t)row >> 3] |= (uint8_t)(1u << (row & 7));
        if (m.rep == COL_REP_STRING) {
            _columnOffsets[(size_t)col][(size_t)row + 1] = _columnOffsets[(size_t)col][(size_t)row];
        } else {
            memset(_columnData[(size_t)col].data() + (size_t)row * m.width, 0, m.width);
        }
        return 1;
    }

    if (m.rep == COL_REP_STRING) {
        if (wv.s == nullptr) return -1;   // numeric value into a string column → protocol violation
        std::vector<uint8_t>& pool = _columnData[(size_t)col];
        if ((uint64_t)pool.size() + wv.slen > 0xFFFFFFFFULL) return -1;  // offsets are uint32
        pool.insert(pool.end(), (const uint8_t*)wv.s, (const uint8_t*)wv.s + wv.slen);
        _columnOffsets[(size_t)col][(size_t)row + 1] = (uint32_t)pool.size();
        return adv;
    }
    if (wv.s != nullptr) return -1;       // string into a fixed-size column → protocol violation

    uint8_t* dst = _columnData[(size_t)col].data() + (size_t)row * m.width;
    switch (m.rep) {
        case COL_REP_INT:
            putSigned(dst, m.width, wv.i);
            break;
        case COL_REP_UINT: {
            uint64_t v = wv.isF ? ((wv.d <= 0.0) ? 0ULL : (uint64_t)wv.d)
                     : wv.isU ? wv.u
                     : ((wv.i <= 0) ? 0ULL : (uint64_t)wv.i);
            putUnsigned(dst, m.width, v);
            break;
        }
        case COL_REP_FLOAT: {
            double d = wv.isF ? wv.d : (wv.isU ? (double)wv.u : (double)wv.i);
            putFloat(dst, m.width, d);
            break;
        }
        case COL_REP_BOOL:
            dst[0] = (wv.isF ? (wv.d != 0.0) : (wv.isU ? (wv.u != 0) : (wv.i != 0))) ? 1 : 0;
            break;
        default:
            return -1;   // unknown fixed-size shape (should not happen)
    }
    return adv;
}

const void* EtDBResult::columnRaw(int col, uint8_t rep, uint8_t width) const {
    if (col < 0 || col >= (int)_colMeta.size()) return nullptr;
    if (_rawMode && !materializeRawCol(col)) return nullptr;   // lazy columnar view
    const ColumnMeta& m = _colMeta[(size_t)col];
    if (m.rep != rep || m.width != width) return nullptr;
    if (m.rep == COL_REP_STRING || m.rep == COL_REP_NONE) return nullptr;
    const std::vector<uint8_t>& data = _columnData[(size_t)col];
    return data.empty() ? nullptr : (const void*)data.data();
}

const char* EtDBResult::columnStringData(int col) const {
    if (col < 0 || col >= (int)_colMeta.size()) return nullptr;
    if (_colMeta[(size_t)col].rep != COL_REP_STRING) return nullptr;
    if (_rawMode && !materializeRawCol(col)) return nullptr;
    const std::vector<uint8_t>& data = _columnData[(size_t)col];
    return data.empty() ? nullptr : (const char*)data.data();
}

const uint32_t* EtDBResult::columnStringOffsets(int col) const {
    if (col < 0 || col >= (int)_colMeta.size()) return nullptr;
    if (_colMeta[(size_t)col].rep != COL_REP_STRING) return nullptr;
    if (_rawMode && !materializeRawCol(col)) return nullptr;
    const std::vector<uint32_t>& offs = _columnOffsets[(size_t)col];
    return offs.empty() ? nullptr : offs.data();
}

std::string_view EtDBResult::stringValue(int row, int col) const {
    if (row < 0 || row >= _rowCount) return std::string_view();
    if (col < 0 || col >= (int)_colMeta.size()) return std::string_view();
    if (_colMeta[(size_t)col].rep != COL_REP_STRING) return std::string_view();
    if (isNull(row, col)) return std::string_view();
    if (_rawMode) {
        // Zero-copy view of the (zero-padded) slot with the padding trimmed.
        const uint8_t* p = _rawBlock.data() + (int64_t)row * _rawStride + _rawRowOff[(size_t)col];
        return std::string_view((const char*)p, (size_t)rawSlotLength(row, col));
    }
    const std::vector<uint32_t>& offs = _columnOffsets[(size_t)col];
    if ((size_t)row + 1 >= offs.size()) return std::string_view();
    const std::vector<uint8_t>& data = _columnData[(size_t)col];
    return std::string_view((const char*)data.data() + offs[(size_t)row],
                            (size_t)(offs[(size_t)row + 1] - offs[(size_t)row]));
}

bool EtDBResult::isNull(int row, int col) const {
    if (row < 0 || row >= _rowCount || col < 0 || col >= (int)_colMeta.size()) return true;
    const std::vector<uint8_t>& nb = _colMeta[(size_t)col].nullBits;
    size_t byte = (size_t)row >> 3;
    if (byte >= nb.size()) return true;
    return ((nb[byte] >> (row & 7)) & 1u) != 0;
}

const uint8_t* EtDBResult::nullBitmap(int col) const {
    if (col < 0 || col >= (int)_colMeta.size()) return nullptr;
    const std::vector<uint8_t>& nb = _colMeta[(size_t)col].nullBits;
    return nb.empty() ? nullptr : nb.data();
}

Value EtDBResult::get(int row, int col) const {
    if (row < 0 || row >= _rowCount || col < 0 || col >= (int)_colMeta.size()) return Value();
    if (_rawMode) return rawSlotValue(row, col);
    const ColumnMeta& m = _colMeta[(size_t)col];
    if (isNull(row, col)) return Value();
    if (m.rep == COL_REP_STRING) return Value(std::string(stringValue(row, col)));
    const uint8_t* p = _columnData[(size_t)col].data() + (size_t)row * m.width;
    switch (m.rep) {
        case COL_REP_INT:   return Value(getSigned(p, m.width));
        case COL_REP_UINT: {
            uint64_t v = getUnsigned(p, m.width);
            // Keep the legacy wire variant convention: unsigned values of ≤4 bytes
            // are carried as the INT variant (old callers read .iVal); 8-byte ones
            // as UINT64 (read .uVal).
            return (m.width <= 4) ? Value((int64_t)v) : Value::fromUInt64(v);
        }
        case COL_REP_FLOAT: return Value(getFloat(p, m.width));
        case COL_REP_BOOL:  return Value(p[0] != 0);
        default:            return Value();
    }
}

void EtDBResult::print() const {
    if (_columnNames.empty()) { printf("(empty)\n"); return; }
    // Header
    for (size_t i = 0; i < _columnNames.size(); ++i) {
        if (i) printf(" | ");
        printf("%s", _columnNames[i].c_str());
    }
    printf("\n");
    // Separator
    for (size_t i = 0; i < _columnNames.size(); ++i) {
        if (i) printf("-+-");
        printf("---");
    }
    printf("\n");
    // Rows (the print path constructs Values on demand; not on the zero-copy hot path)
    for (int r = 0; r < _rowCount; ++r) {
        for (int ci = 0; ci < (int)_columnNames.size(); ++ci) {
            if (ci) printf(" | ");
            printf("%s", get(r, ci).toString().c_str());
        }
        printf("\n");
    }
    printf("(%d rows)\n", _rowCount);
}

bool EtDBResult::deserialize(const uint8_t* buf, int bufLen) {
    int parseOff = (int)sizeof(SMsgHead);
    if (bufLen < parseOff + (int)sizeof(SQueryRsp)) return false;

    const auto* rsp = (const SQueryRsp*)(buf + parseOff);
    int numCols = ntohl(rsp->numCols);
    int numRows = ntohl(rsp->numRows);
    int dataLen = ntohl(rsp->dataLen);
    if (numCols < 0 || numCols > 4096 || numRows < 0 || numRows > 1000000) return false;
    if (dataLen < 0 || parseOff + (int)sizeof(SQueryRsp) + dataLen > bufLen) return false;

    resetStorage();

    const char* d   = rsp->data;
    const char* end = rsp->data + dataLen;

    // Detect streaming metadata prefix (TDengine-style fetch)
    _streamQId = 0; _streamTotalRows = 0; _streamEnded = true;
    if (dataLen >= (int)sizeof(SStreamMeta)) {
        uint32_t magic;
        memcpy(&magic, d, 4);
        if (ntohl(magic) == ETDB_STREAM_MAGIC) {
            const auto* sm = (const SStreamMeta*)d;
            _streamQId       = (int64_t)be64toh((uint64_t)sm->qId);
            _streamTotalRows = (int64_t)be64toh((uint64_t)sm->totalRows);
            _streamEnded     = (sm->isEnd != 0);
            d += sizeof(SStreamMeta);
        }
    }
    _rowCursor = 0;

    // ── Payload layout (1 byte, after the optional streaming metadata) ──
    // Layout 1 = raw row block: the storage image verbatim. The client keeps the
    // block as-is (a single memcpy) and serves pointers into it — nothing is
    // parsed per value. Columnar accessors materialize on demand.
    if (d >= end) return false;
    const uint8_t layout = (uint8_t)*d++;
    if (layout == ETDB_LAYOUT_RAW_BLOCK) {
        if (end - d < 4) return false;
        int32_t stride; memcpy(&stride, d, 4); stride = (int32_t)ntohl((uint32_t)stride); d += 4;
        if (stride <= 0 || numCols <= 0 || numRows < 0) return false;
        _rawStride = stride;
        _rawRowOff.assign((size_t)numCols, 0);
        _rawSlotBytes.assign((size_t)numCols, 0);
        _columnNames.reserve((size_t)numCols);
        _columnTypes.reserve((size_t)numCols);
        for (int ci = 0; ci < numCols; ++ci) {
            if (end - d < 2) return false;
            uint16_t nameLen; memcpy(&nameLen, d, 2); nameLen = ntohs((uint16_t)nameLen); d += 2;
            if (end - d < (int)nameLen + 1 + 8) return false;
            _columnNames.emplace_back(d, nameLen);
            d += nameLen;
            _columnTypes.push_back((ColType)(uint8_t)*d++);
            int32_t sb; memcpy(&sb, d, 4); sb = (int32_t)ntohl((uint32_t)sb); d += 4;
            int32_t ro; memcpy(&ro, d, 4); ro = (int32_t)ntohl((uint32_t)ro); d += 4;
            _rawSlotBytes[(size_t)ci] = sb;
            _rawRowOff[(size_t)ci]    = ro;
        }
        const int64_t need = (int64_t)stride * (int64_t)numRows;
        if (end - d < need) { resetStorage(); return false; }
        _rawBlock.assign(d, d + (size_t)need);      // ONE memcpy for the whole batch
        _rowCount = numRows;
        _colMeta.assign((size_t)numCols, ColumnMeta());
        _columnData.assign((size_t)numCols, std::vector<uint8_t>());
        _columnOffsets.assign((size_t)numCols, std::vector<uint32_t>());
        _rawMat.assign((size_t)numCols, 0);
        const size_t nullBytes = ((size_t)numRows + 7) / 8;
        for (int ci = 0; ci < numCols; ++ci) {
            uint8_t rep = COL_REP_NONE, width = 0;
            colTypeStore((ColType)(uint8_t)_columnTypes[(size_t)ci], rep, width);
            ColumnMeta& m = _colMeta[(size_t)ci];
            m.rep = rep;
            m.width = width;
            m.nullBits.assign(nullBytes, 0);   // raw blocks have no NULLs
        }
        _rawMode = true;
        return true;
    }
    if (layout != ETDB_LAYOUT_TAG_ROWS) { resetStorage(); return false; }

    // Column names + type bytes. The type byte is the server's actual wire
    // encoding type (schema type for plain column refs; server-resolved by value
    // range for expression columns) and decides the column storage shape/width.
    _columnNames.reserve((size_t)numCols);
    _columnTypes.reserve((size_t)numCols);
    for (int ci = 0; ci < numCols; ++ci) {
        if (end - d < 2) return false;
        uint16_t nameLen; memcpy(&nameLen, d, 2); nameLen = ntohs(nameLen);
        d += 2;
        if (end - d < (int)nameLen + 1) return false;
        _columnNames.emplace_back(d, nameLen);
        _columnTypes.push_back((ColType)(uint8_t)d[nameLen]);
        d += (int)nameLen + 1;
    }

    // Single-pass parse: the stream is row-major, but each value goes straight
    // into its column's contiguous buffer (column-major storage) with no
    // Query::Value constructed — for large result sets the dominant cost is just
    // this one byte copy + byte-order conversion.
    setupColumns(numCols, numRows);
    for (int ri = 0; ri < numRows; ++ri) {
        for (int ci = 0; ci < numCols; ++ci) {
            int adv = storeValue(ci, ri, d, end);
            if (adv < 0) {
                _error = "malformed query response (col " + std::to_string(ci) +
                         ", row " + std::to_string(ri) + ")";
                resetStorage();
                return false;
            }
            d += adv;
        }
    }
    return true;
}

bool EtDBResult::deserializeSubmitRsp(const uint8_t* buf, int bufLen) {
    int minSize = (int)(sizeof(SSubmitRspMsg) - 1);
    if (bufLen < minSize) return false;

    const auto* rsp = (const SSubmitRspMsg*)buf;
    int submitted = (int)ntohl(rsp->numOfRows);
    int affected  = (int)ntohl(rsp->affectedRows);
    int errors    = (int)ntohl(rsp->errorRows);

    resetStorage();
    _streamQId = 0; _streamTotalRows = 0; _streamEnded = true;
    _rowCursor = 0;
    _submitSubmitted = submitted;
    _submitAffected  = affected;
    _submitErrors    = errors;
    _submitErrorEntries.clear();

    // Parse error entries
    if (errors > 0) {
        const auto* errEntry = (const SSubmitErrEntry*)rsp->errorData;
        for (int ei = 0; ei < errors; ++ei) {
            SubmitErrorInfo sei;
            sei.rowIndex  = (int)ntohl(errEntry[ei].rowIndex);
            sei.errorCode = (int)ntohl(errEntry[ei].errorCode);
            _submitErrorEntries.push_back(sei);
        }
    }

    // Represented as a single "status" string column (1 row), columnar storage: byte pool + offsets
    char msg[256];
    if (errors == 0) {
        snprintf(msg, sizeof(msg), "OK: %d row(s) inserted", affected);
    } else {
        snprintf(msg, sizeof(msg), "%d inserted, %d failed (total %d submitted)",
                 affected, errors, submitted);
    }
    _columnNames.push_back("status");
    _columnTypes.push_back(ColType::NCHAR);
    setupColumns(1, 1);
    size_t len = strlen(msg);
    _columnData[0].assign((const uint8_t*)msg, (const uint8_t*)msg + len);
    _columnOffsets[0][1] = (uint32_t)len;
    return true;
}

int EtDBResult::submitSubmitted() const { return _submitSubmitted; }
int EtDBResult::submitAffected()  const { return _submitAffected; }
int EtDBResult::submitErrors()    const { return _submitErrors; }

const std::vector<EtDBResult::SubmitErrorInfo>& EtDBResult::submitErrorEntries() const {
    return _submitErrorEntries;
}

bool EtDBResult::success() const { return !_columnNames.empty() || _error.empty(); }
const std::string& EtDBResult::error() const { return _error; }
void EtDBResult::setError(const std::string& e) { _error = e; }

int64_t EtDBResult::streamQId() const      { return _streamQId; }
int64_t EtDBResult::streamTotalRows() const { return _streamTotalRows; }
bool    EtDBResult::streamEnded() const    { return _streamEnded; }
bool    EtDBResult::streamError() const    { return _streamError; }

void EtDBResult::setFetchBatchBytes(int64_t b) {
    _fetchBatchBytes = (b > 0) ? b : ETDB_STREAM_BATCH_BYTES;
}

void EtDBResult::setStreamConn(EtDBConnection* conn) { _conn = conn; }

// Advance the row cursor: rows left in batch → true; exhausted → triggers FETCH; ended/failed → false
bool EtDBResult::advanceRow() {
    if (_rowCursor < _rowCount) return true;
    // Current batch drained — FETCH next batch if streaming & not ended.
    if (_streamEnded || _streamQId == 0 || !_conn) return false;
    if (!_conn->fetchStreamRows(_streamQId, (int)_fetchBatchBytes, *this)) {
        _streamEnded = true;
        _streamError = true;
        return false;
    }
    _rowCursor = 0;
    return _rowCursor < _rowCount;
}

// Raw pointer of cell (row,col) (NULL → nullptr; string → payload in the byte pool)
const void* EtDBResult::cellPtr(int row, int col) const {
    if (_rawMode) {
        // Pointers go straight into the raw block (no parse, no copy).
        if (row < 0 || row >= _rowCount || col < 0 || col >= (int)_rawSlotBytes.size()) return nullptr;
        return (const void*)(_rawBlock.data() + (int64_t)row * _rawStride + _rawRowOff[(size_t)col]);
    }
    const ColumnMeta& m = _colMeta[(size_t)col];
    if (isNull(row, col)) return nullptr;
    if (m.rep == COL_REP_STRING)
        return (const void*)(_columnData[(size_t)col].data() + _columnOffsets[(size_t)col][(size_t)row]);
    return (const void*)(_columnData[(size_t)col].data() + (size_t)row * m.width);
}

// Zero-copy row interface: fills raw pointers only — constructs no Value and
// performs no type conversion. A row-count-only benchmark has zero parse
// overhead; parsing is done by the caller's casts or via column<T>().
bool EtDBResult::fetchRow(const void** cols) {
    if (!advanceRow()) return false;
    const int n = (int)_colMeta.size();
    for (int c = 0; c < n; ++c) cols[c] = cellPtr(_rowCursor, c);
    ++_rowCursor;
    return true;
}

uint32_t EtDBResult::valueLength(int col) const {
    int row = _rowCursor - 1;   // row obtained by the most recent fetchRow
    if (row < 0 || row >= _rowCount || col < 0 || col >= (int)_colMeta.size()) return 0;
    if (_colMeta[(size_t)col].rep != COL_REP_STRING) return 0;
    if (_rawMode) return rawSlotLength(row, col);
    if (isNull(row, col)) return 0;
    const std::vector<uint32_t>& offs = _columnOffsets[(size_t)col];
    if ((size_t)row + 1 >= offs.size()) return 0;
    return offs[(size_t)row + 1] - offs[(size_t)row];
}

// Compat interface: builds Value rows from columnar storage on demand (not zero-copy, not on the hot path)
bool EtDBResult::fetchRow(std::vector<Value>& out) {
    if (!advanceRow()) return false;
    bool qon = qtimeEnabled();
    int64_t ts = qon ? qnowUs() : 0;
    out.clear();
    out.reserve(_colMeta.size());
    for (int ci = 0; ci < (int)_colMeta.size(); ++ci) out.push_back(get(_rowCursor, ci));
    ++_rowCursor;
    if (qon) { g_qperf.rowCopyUs += qnowUs() - ts; g_qperf.rowsFetched++; }
    return true;
}

int EtDBResult::fetchBlock() {
    // If the current in-memory batch still has unconsumed rows (mixed fetchRow
    // usage), serve the remainder as this block first — they were already
    // pulled from the server, no extra FETCH round trip needed. The block is
    // CONSUMED: _rowCursor advances past it. Callers use rowCursor()-n to find
    // the block's start within the current batch (rows remain accessible via
    // get()/column<T>() until the next FETCH replaces them).
    if (_rowCursor < _rowCount) {
        int n = _rowCount - _rowCursor;
        _rowCursor = _rowCount;
        return n;
    }
    // Batch drained — FETCH the next batch if streaming & not ended.
    if (_streamEnded || _streamQId == 0 || !_conn) return 0;
    if (!_conn->fetchStreamRows(_streamQId, (int)_fetchBatchBytes, *this)) {
        _streamEnded = true;
        _streamError = true;
        return -1;
    }
    _rowCursor = _rowCount;  // consume the whole new batch
    return _rowCount;
}

int EtDBResult::rowCursor() const { return _rowCursor; }

// ============================================================================
// EtDBConnection Implementation
// ============================================================================

EtDBConnection::~EtDBConnection() { close(); }

bool EtDBConnection::connect(const std::string& host, uint16_t port,
                             const char* user, const char* password,
                             const char* db) {
    close();

    _user = user ? user : "root";
    _password = password ? password : "etherdbdata";
    _db = db ? db : "";

    // 跨平台初始化（Windows 下 WSAStartup）
    TcpClient::init();

    TcpSocket s = TcpClient::create();
    if (!TcpClient::valid(s)) return false;
    _sock = s;

    TcpClient::setNoDelay(s);
    if (!TcpClient::connect(s, host.c_str(), port)) {
        TcpClient::close(s);
        _sock = INVALID_TCP_SOCKET;
        return false;
    }

    // === Authentication handshake: CM_CONNECT (msgType=49) ===
    // Body format: user\0password\0
    {
        int userLen = (int)_user.size();
        int passLen = (int)_password.size();
        int bodyLen = userLen + 1 + passLen + 1;  // user + \0 + password + \0
        int totalLen = (int)sizeof(STxHead) + bodyLen;

        std::vector<uint8_t> authMsg(totalLen, 0);
        STxHead* head = (STxHead*)authMsg.data();
        head->version = 0x01;
        head->msgType = ETDB::Proto::MSG_CM_CONNECT; // CM_CONNECT=49
        head->msgLen  = htonl(totalLen);
        strncpy(head->user, _user.c_str(), ETDB_USER_LEN - 1);

        char* body = (char*)authMsg.data() + sizeof(STxHead);
        memcpy(body, _user.c_str(), userLen);
        body[userLen] = '\0';
        memcpy(body + userLen + 1, _password.c_str(), passLen);
        body[userLen + 1 + passLen] = '\0';  // null-terminate password

        // Send
        int sent = TcpClient::sendAll(s, authMsg.data(), totalLen);
        if (sent != totalLen) { TcpClient::close(s); _sock = INVALID_TCP_SOCKET; return false; }

        // Receive response
        STxHead rspHead;
        memset(&rspHead, 0, sizeof(rspHead));
        int n = TcpClient::recv(s, &rspHead, (int)sizeof(STxHead), MSG_WAITALL);
        if (n != (int)sizeof(STxHead)) { TcpClient::close(s); _sock = INVALID_TCP_SOCKET; return false; }

        int rspCode = ntohl(rspHead.rtCode);
        if (rspCode != 0) {
            // Auth failed — server returned error code
            fprintf(stderr, "ERROR: Authentication failed for user '%s' (code:%d)\n",
                    _user.c_str(), rspCode);
            TcpClient::close(s);
            _sock = INVALID_TCP_SOCKET;
            return false;
        }
    }

    return true;
}

void EtDBConnection::close() {
    // Stop the async receiver thread first (it may be blocked in recv()).
    bool wasRunning = _recvRunning.exchange(false);
    if (wasRunning || _recvThread.joinable()) {
        // Unblock a receiver blocked in recv(): shutdown() is the portable
        // way, but on Windows a pending recv() may not wake from shutdown()
        // alone — closing the socket (closesocket) reliably cancels pending
        // I/O. Do both BEFORE join() so close() can never hang on the
        // receiver thread.
        if (TcpClient::valid(_sock)) {
            TcpClient::shutdown(_sock, 2);
            TcpClient::close(_sock);
            _sock = INVALID_TCP_SOCKET;
        }
    }
    if (_recvThread.joinable()) _recvThread.join();

    // Wake any waiter that is blocked on a never-arriving response.
    {
        std::lock_guard<std::mutex> lk(_respMutex);
        if (_syncWaiting && !_syncReady) { _syncCode = -1; _syncReady = true; }
        _asyncResults.clear();
        _sentAsyncCount.store(0);
        _receivedCount.store(0);
        _consumedCount.store(0);
        _nextBatchId.store(1);
    }
    _respCond.notify_all();

    if (TcpClient::valid(_sock)) {
        TcpClient::close(_sock);
        _sock = INVALID_TCP_SOCKET;
    }
    qtimeReport();
}

bool EtDBConnection::isConnected() const { return TcpClient::valid(_sock); }
int  EtDBConnection::socketFd() const    { return (int)_sock; }
int32_t EtDBConnection::currentdbId() const { return _currentdbId; }

const std::string& EtDBConnection::userName() const { return _user; }
const std::string& EtDBConnection::password() const { return _password; }
const std::string& EtDBConnection::dbName()    const { return _db; }

bool EtDBConnection::sendRecv(const uint8_t* msg, int msgLen,
                              std::vector<uint8_t>& rspCont,
                              int& rspCode, uint8_t& rspType) {
    if (!TcpClient::valid(_sock)) return false;

    // If the async receiver thread is active it owns socket reads. Route the
    // synchronous exchange through it so responses never get misread: async
    // INSERT responses arrive in submission order, so drain them first and the
    // next response on the wire belongs to this sync request.
    if (_recvRunning.load()) {
        waitAllAsync(0);
        if (!_recvRunning.load() || !TcpClient::valid(_sock)) return false;

        {
            std::lock_guard<std::mutex> lk(_respMutex);
            _syncWaiting = true;
            _syncReady   = false;
        }
        {
            std::lock_guard<std::mutex> io(_ioMutex);
            if (TcpClient::sendAll(_sock, msg, msgLen) != msgLen) {
                std::lock_guard<std::mutex> lk(_respMutex);
                _syncWaiting = false;
                return false;
            }
        }
        {
            std::unique_lock<std::mutex> lk(_respMutex);
            _respCond.wait(lk, [&]{ return _syncReady || !_recvRunning.load(); });
            if (!_syncReady) { _syncWaiting = false; return false; }  // conn lost
            _syncWaiting = false;
            rspCode = _syncCode;
            rspType = _syncType;
            rspCont = std::move(_syncCont);
        }
        return true;
    }

    // Normal synchronous path (receiver thread not started).
    {
        std::lock_guard<std::mutex> io(_ioMutex);
        int sent = TcpClient::sendAll(_sock, msg, msgLen);
        if (sent != msgLen) return false;
    }

    // Receive STxHead
    STxHead rspHead;
    memset(&rspHead, 0, sizeof(rspHead));
    int n = TcpClient::recv(_sock, &rspHead, (int)sizeof(STxHead), MSG_WAITALL);
    if (n != (int)sizeof(STxHead)) return false;

    rspCode = ntohl(rspHead.rtCode);
    rspType = rspHead.msgType;
    int contLen = ntohl(rspHead.msgLen) - (int)sizeof(STxHead);

    if (contLen <= 0) { rspCont.clear(); return true; }

    rspCont.resize(contLen);
    n = TcpClient::recv(_sock, rspCont.data(), contLen, MSG_WAITALL);
    return n == contLen;
}

// ============================================================================
// Async insert — background response receiver
// ============================================================================
bool EtDBConnection::ensureAsyncRecv() {
    if (_recvRunning.load()) return true;
    if (!TcpClient::valid(_sock)) return false;
    bool expected = false;
    if (!_recvRunning.compare_exchange_strong(expected, true)) return true;
    try {
        _recvThread = std::thread(&EtDBConnection::recvLoop, this);
    } catch (...) {
        _recvRunning.store(false);
        return false;
    }
    return true;
}

void EtDBConnection::recvLoop() {
    // Poll for readability with a short timeout so close() can stop this
    // thread promptly even if shutdown()/closesocket() fail to wake a blocked
    // recv() (a documented Winsock quirk). 200ms granularity is invisible to
    // the async insert pipeline.
    const int POLL_US = 200000;   // 200ms

    while (_recvRunning.load()) {
        // Wait until the socket is readable (or the poll interval elapses).
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(_sock, &rfds);
        struct timeval tv;
        tv.tv_sec  = 0;
        tv.tv_usec = POLL_US;
        int sel = ::select((int)_sock + 1, &rfds, nullptr, nullptr, &tv);
        if (sel < 0) {
            // Socket closed by close() or transient error — stop if asked to.
            if (!_recvRunning.load()) break;
            continue;
        }
        if (sel == 0) continue;                    // timeout → re-check flag
        if (!FD_ISSET(_sock, &rfds)) continue;

        STxHead head;
        memset(&head, 0, sizeof(head));
        int n = TcpClient::recv(_sock, &head, (int)sizeof(STxHead), MSG_WAITALL);
        if (n != (int)sizeof(STxHead)) break;     // connection closed or error

        int code = ntohl(head.rtCode);
        uint8_t type = head.msgType;
        uint64_t batchId = head.batchId;           // echoed batch id (host order)
        int contLen = ntohl(head.msgLen) - (int)sizeof(STxHead);
        std::vector<uint8_t> cont;
        if (contLen > 0) {
            cont.resize(contLen);
            n = TcpClient::recv(_sock, cont.data(), contLen, MSG_WAITALL);
            if (n != contLen) break;
        }

        std::lock_guard<std::mutex> lk(_respMutex);

        if (_syncWaiting) {
            // Response for the pending synchronous request.
            _syncCode = code;
            _syncType = type;
            _syncCont = std::move(cont);
            _syncReady = true;
        } else {
            // Async INSERT response. Correlate by the batch id echoed by the
            // server (batchId) when present; otherwise fall back to submission
            // order (responses are in-order today). Results are stored in a
            // map keyed by batch id — a batch id is a correlation handle, not
            // a vector subscript, so it is never used as an array index.
            uint64_t bid = (batchId != 0) ? batchId : (_receivedCount.load() + 1);
            _receivedCount.store(_receivedCount.load() + 1);
            AsyncResult& r = _asyncResults[bid];
            r.ready = true;
            r.code  = code;
            if (code == 0 && cont.size() >= (size_t)(sizeof(SSubmitRspMsg) - 1)) {
                const auto* rsp = (const SSubmitRspMsg*)cont.data();
                r.submitted = (int)ntohl(rsp->numOfRows);
                r.affected  = (int)ntohl(rsp->affectedRows);
                r.errors    = (int)ntohl(rsp->errorRows);
            }
        }
        _respCond.notify_all();
    }

    // Receiver stopped (connection closed or error) — wake up any waiters.
    {
        std::lock_guard<std::mutex> lk(_respMutex);
        if (_syncWaiting && !_syncReady) { _syncCode = -1; _syncReady = true; }
    }
    _respCond.notify_all();
    _recvRunning.store(false);
}

int64_t EtDBConnection::executeInsertAsync(const uint8_t* submitData, int submitLen, int dbId) {
    if (!TcpClient::valid(_sock)) return -1;
    if (!ensureAsyncRecv()) return -1;

    // Backpressure: bound the number of not-yet-consumed results. If the user
    // never drains via getAsyncResult(), block here once the pending result
    // set reaches _maxAsyncPending so memory cannot grow without bound.
    {
        std::unique_lock<std::mutex> lk(_respMutex);
        _respCond.wait(lk, [&]{
            return (_receivedCount.load() - _consumedCount.load()) < (uint64_t)_maxAsyncPending
                || !_recvRunning.load();
        });
        if (!_recvRunning.load()) return -1;   // connection closed while waiting
    }

    uint64_t batchId = _nextBatchId.fetch_add(1);
    _sentAsyncCount.fetch_add(1);

    int contentLen = (int)sizeof(SMsgHead) + submitLen;
    int totalLen = (int)sizeof(STxHead) + contentLen;

    // Build + send atomically under _ioMutex, reusing _sendBuf: the hot async
    // path allocates hundreds of KB per batch, so avoid a per-batch heap
    // allocation + zero-init. _sendBuf is only touched while _ioMutex is held,
    // so concurrent async submits cannot clobber each other.
    std::lock_guard<std::mutex> io(_ioMutex);
    _sendBuf.resize(totalLen);
    uint8_t* msgBuf = _sendBuf.data();
    memset(msgBuf, 0, (size_t)totalLen);

    SMsgHead* head = (SMsgHead*)(msgBuf + sizeof(STxHead));
    head->dbId    = htonl(dbId);
    head->contLen = htonl(submitLen);

    memcpy(msgBuf + sizeof(STxHead) + sizeof(SMsgHead),
           submitData, submitLen);

    STxHead* rpc = (STxHead*)msgBuf;
    rpc->version = 0x01;
    rpc->msgType = Proto::MSG_SUBMIT;
    rpc->msgLen  = htonl(totalLen);
    rpc->batchId = batchId;   // batch id for correlation (echoed by server)
    strncpy(rpc->user, _user.c_str(), ETDB_USER_LEN - 1);

    if (TcpClient::sendAll(_sock, msgBuf, totalLen) != totalLen) {
        _sentAsyncCount.fetch_sub(1);
        return -1;
    }
    return (int64_t)batchId;
}

bool EtDBConnection::waitAsyncResult(uint64_t batchId, int timeoutMs) {
    if (batchId == 0) return false;
    std::unique_lock<std::mutex> lk(_respMutex);
    auto pred = [&]{
        auto it = _asyncResults.find(batchId);
        return (it != _asyncResults.end() && it->second.ready)
            || !_recvRunning.load();
    };
    if (timeoutMs <= 0) _respCond.wait(lk, pred);
    else _respCond.wait_for(lk, std::chrono::milliseconds(timeoutMs), pred);
    auto it = _asyncResults.find(batchId);
    return it != _asyncResults.end() && it->second.ready;
}

bool EtDBConnection::pollAsyncResult(uint64_t batchId) const {
    std::lock_guard<std::mutex> lk(_respMutex);
    auto it = _asyncResults.find(batchId);
    return it != _asyncResults.end() && it->second.ready;
}

EtDBConnection::AsyncResult EtDBConnection::peekAsyncResult(uint64_t batchId) const {
    std::lock_guard<std::mutex> lk(_respMutex);
    auto it = _asyncResults.find(batchId);
    if (it != _asyncResults.end() && it->second.ready)
        return it->second;
    return AsyncResult();
}

// Consume the result: returns a copy and REMOVES the entry so the pending
// result set stays bounded (frees one backpressure slot).
EtDBConnection::AsyncResult EtDBConnection::getAsyncResult(uint64_t batchId) {
    std::lock_guard<std::mutex> lk(_respMutex);
    auto it = _asyncResults.find(batchId);
    if (it != _asyncResults.end() && it->second.ready) {
        AsyncResult r = it->second;
        _asyncResults.erase(it);
        _consumedCount.fetch_add(1);
        _respCond.notify_all();   // wake a blocked executeInsertAsync (backpressure)
        return r;
    }
    return AsyncResult();
}

void EtDBConnection::setMaxAsyncPending(int n) {
    std::lock_guard<std::mutex> lk(_respMutex);
    _maxAsyncPending = (n > 0) ? n : 512;
    _respCond.notify_all();
}

bool EtDBConnection::waitAllAsync(int timeoutMs) {
    std::unique_lock<std::mutex> lk(_respMutex);
    uint64_t target = _sentAsyncCount.load();
    auto pred = [&]{ return _receivedCount.load() >= target || !_recvRunning.load(); };
    if (timeoutMs <= 0) _respCond.wait(lk, pred);
    else _respCond.wait_for(lk, std::chrono::milliseconds(timeoutMs), pred);
    return _receivedCount.load() >= target;
}

uint64_t EtDBConnection::asyncSentCount() const { return _sentAsyncCount.load(); }
uint64_t EtDBConnection::asyncReceivedCount() const { return _receivedCount.load(); }
uint64_t EtDBConnection::asyncPendingCount() const { return _receivedCount.load() - _consumedCount.load(); }

// --- Table name→UID cache ---
uint64_t EtDBConnection::lookupTableUid(const std::string& tableName) {
    auto it = _tableUidCache.find(tableName);
    if (it != _tableUidCache.end()) return it->second;
    return 0;  // not found
}

void EtDBConnection::cacheTableUid(const std::string& tableName, uint64_t uid) {
    _tableUidCache[tableName] = uid;
}

uint64_t EtDBConnection::resolveTableUidFromServer(const std::string& dbName,
                                                   const std::string& tableName) {
    if (!TcpClient::valid(_sock)) return 0;
    // Build STableMetaReq
    STableMetaReq req;
    memset(&req, 0, sizeof(req));
    int dnl = std::min((int)dbName.size(), 63);
    req.dbNameLen = htonl(dnl);
    memcpy(req.dbName, dbName.c_str(), dnl);
    int tnl = std::min((int)tableName.size(), 255);
    req.tableNameLen = htonl(tnl);
    memcpy(req.tableName, tableName.c_str(), tnl);
    req.needColumns = 1;

    // Build full message with STxHead
    int totalLen = (int)sizeof(STxHead) + (int)sizeof(STableMetaReq);
    std::vector<uint8_t> fullMsg(totalLen, 0);
    STxHead* head = (STxHead*)fullMsg.data();
    head->version = 0x01;
    head->msgType = ETDB::Proto::MSG_CM_TABLE_META;  // CM_TABLE_META
    head->msgLen  = htonl(totalLen);
    strncpy(head->user, _user.c_str(), ETDB_USER_LEN - 1);
    memcpy(fullMsg.data() + sizeof(STxHead), &req, sizeof(req));

    // Send and receive
    std::vector<uint8_t> rspCont;
    int rspCode = -1;
    uint8_t rspType = 0;
    if (!sendRecv(fullMsg.data(), totalLen, rspCont, rspCode, rspType))
        return 0;
    if (rspCode != 0 || rspCont.empty()) return 0;

    // Parse STableMetaRsp — extract UID of the first table entry
    constexpr int kEntryFixed = (int)sizeof(STableMetaEntry) - 256;
    if (rspCont.size() < (size_t)(STableMetaRsp::HEADER_SIZE + kEntryFixed + 1)) return 0;
    const STableMetaRsp* rsp = (const STableMetaRsp*)rspCont.data();
    int numTables = ntohl(rsp->numOfTables);
    if (numTables <= 0) return 0;

    const STableMetaEntry* entry = (const STableMetaEntry*)(rspCont.data() + STableMetaRsp::HEADER_SIZE);
    uint64_t uid = be64toh(entry->uid);
    if (uid > 0) {
        cacheTableUid(tableName, uid);
        // Also cache db-qualified name
        cacheTableUid(dbName + "." + tableName, uid);
    }
    return uid;
}

// ============================================================================
// SQL INSERT value scanner + fast span converters
//
// The SQL-based INSERT packer in EtDBConnection::query() streams rows: every
// "(...)" group is structurally identical (same column count/types — only the
// values differ), so the first row fixes the schema and each row is parsed
// into (ptr,len) spans over the SQL string and serialized immediately into the
// wire buffer. No row is materialized as std::string on the hot path.
// ============================================================================
namespace {

// 10^k for k in [0,15]; each entry is exactly representable in double.
const double kPow10[16] = {
    1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,
    1e8,  1e9,  1e10, 1e11, 1e12, 1e13, 1e14, 1e15
};

// ---- fast integer parsing on non-NUL-terminated spans ----
// Semantics match strtol/strtoll (base 10): skip leading whitespace, optional
// sign, consume decimal digits, stop at the first non-digit (trailing junk is
// ignored). They saturate instead of overflowing and return 0 with no digits.

// strtoll(str, NULL, 10) equivalent.
inline int64_t fastAtoi64(const char* s, size_t len) {
    size_t i = 0;
    while (i < len && isspace((unsigned char)s[i])) ++i;
    bool neg = false;
    if (i < len && (s[i] == '-' || s[i] == '+')) { neg = (s[i] == '-'); ++i; }
    const uint64_t lim = neg ? (uint64_t)0x8000000000000000ULL   // |INT64_MIN|
                            : (uint64_t)0x7FFFFFFFFFFFFFFFULL;   // INT64_MAX
    uint64_t acc = 0;
    bool any = false;
    for (; i < len; ++i) {
        const uint64_t d = (uint64_t)(s[i] - '0');
        if (d > 9) break;
        any = true;
        if (acc > (lim - d) / 10) acc = lim;   // saturate
        else acc = acc * 10 + d;
    }
    if (!any) return 0;
    if (neg) {
        // acc == 2^63 casts to INT64_MIN on two's-complement targets.
        if (acc == (uint64_t)0x8000000000000000ULL) return (int64_t)acc;
        return -(int64_t)acc;
    }
    return (int64_t)acc;
}

// strtoull(str, NULL, 10) equivalent ('-' wraps around, like strtoul).
inline uint64_t fastAtou64(const char* s, size_t len) {
    size_t i = 0;
    while (i < len && isspace((unsigned char)s[i])) ++i;
    bool neg = false;
    if (i < len && (s[i] == '-' || s[i] == '+')) { neg = (s[i] == '-'); ++i; }
    const uint64_t lim = ~(uint64_t)0;         // UINT64_MAX
    uint64_t acc = 0;
    bool any = false;
    for (; i < len; ++i) {
        const uint64_t d = (uint64_t)(s[i] - '0');
        if (d > 9) break;
        any = true;
        if (acc > (lim - d) / 10) acc = lim;
        else acc = acc * 10 + d;
    }
    if (!any) return 0;
    return neg ? ((uint64_t)0 - acc) : acc;
}

// Timestamp column: keeps the historical guard — only [0-9-] is accepted in the
// whole token (anything else yields 0) — then parses like strtoll.
inline int64_t fastTsParse(const char* s, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        const char c = s[i];
        if (!((c >= '0' && c <= '9') || c == '-')) return 0;
    }
    return fastAtoi64(s, len);
}

// Plain-decimal fast parse that is correctly rounded like strtod when the
// mantissa has <=15 significant digits and the power-of-ten scale fits in
// [-15,15]: both operands are then exact in double, so the single multiply /
// divide is correctly rounded. Returns false for the forms libc must handle
// (hex floats, inf/nan, >15 significant digits, huge exponents); the caller
// falls back to strtod/strtof on a NUL-terminated scratch copy.
inline bool fastParseDecimal(const char* s, size_t len, double& out) {
    size_t i = 0;
    while (i < len && isspace((unsigned char)s[i])) ++i;
    bool neg = false;
    if (i < len && (s[i] == '-' || s[i] == '+')) { neg = (s[i] == '-'); ++i; }

    if (i < len && s[i] == '0' && i + 1 < len && (s[i + 1] == 'x' || s[i + 1] == 'X'))
        return false;                          // hex float -> libc
    if (i < len && (s[i] == 'i' || s[i] == 'I' || s[i] == 'n' || s[i] == 'N'))
        return false;                          // inf / nan -> libc

    uint64_t m = 0;
    int nd = 0;                                // significant digits
    int scale = 0;                             // -#fractional digits
    bool any = false;
    bool mantOver = false;                     // mantissa ran out of uint64 headroom

    while (i < len && s[i] >= '0' && s[i] <= '9') {
        any = true;
        if (!mantOver) {
            const uint64_t d = (uint64_t)(s[i] - '0');
            if (m > (~(uint64_t)0 - d) / 10) mantOver = true;
            else {
                const bool wasZero = (m == 0);
                m = m * 10 + d;
                if (!(wasZero && d == 0)) ++nd; // leading zeros are not significant
            }
        }
        ++i;
    }
    if (i < len && s[i] == '.') {
        ++i;
        while (i < len && s[i] >= '0' && s[i] <= '9') {
            any = true;
            if (!mantOver) {
                const uint64_t d = (uint64_t)(s[i] - '0');
                if (m > (~(uint64_t)0 - d) / 10) mantOver = true;
                else {
                    const bool wasZero = (m == 0);
                    m = m * 10 + d;
                    if (!(wasZero && d == 0)) ++nd;
                }
            }
            --scale;
            ++i;
        }
    }
    if (!any) { out = 0.0; return true; }      // no number -> 0 (strtod agrees)

    int e10 = 0;
    if (i < len && (s[i] == 'e' || s[i] == 'E')) {
        ++i;
        bool eneg = false;
        if (i < len && (s[i] == '-' || s[i] == '+')) { eneg = (s[i] == '-'); ++i; }
        bool eany = false;
        int ed = 0;
        while (i < len && s[i] >= '0' && s[i] <= '9') {
            eany = true;
            if (ed < 100000) ed = ed * 10 + (s[i] - '0');
            ++i;
        }
        if (eany) e10 = eneg ? -ed : ed;
        // "1e" / "1e+" without digits: libc stops before 'e' -> keep e10 = 0
    }
    const long long combined = (long long)scale + (long long)e10;
    if (mantOver || nd > 15 || combined < -15 || combined > 15) return false;

    double d = (double)m;                      // m < 10^15 < 2^50 -> exact
    if (combined > 0)      d *= kPow10[(int)combined];
    else if (combined < 0) d /= kPow10[(int)-combined];
    out = neg ? -d : d;
    return true;
}

inline double parseDoubleSpan(const char* s, size_t len, std::string& scratch) {
    double d;
    if (fastParseDecimal(s, len, d)) return d;
    scratch.assign(s, len);                    // strtod needs a NUL terminator
    return strtod(scratch.c_str(), nullptr);
}

inline float parseFloatSpan(const char* s, size_t len, std::string& scratch) {
    double d;
    if (fastParseDecimal(s, len, d)) return (float)d;   // fl32(fl64(x)) == fl32(x)
    scratch.assign(s, len);
    return strtof(scratch.c_str(), nullptr);
}

// Original quote-aware per-value builder. Only used for non-trivial quoting
// (doubled quotes, junk after a closing quote, quote inside a bare token); it
// exactly mirrors the legacy allRows state machine.
// On entry *p is at the value start; consumes through the terminating ',' or
// ')' and returns that separator, or 0 at EOF (partial value is discarded).
inline char scanInsertValueGeneric(const char* s, size_t n, size_t& p,
                                   std::string& out) {
    out.clear();
    bool inQ = false;
    char qc = 0;
    while (p < n) {
        const char c = s[p++];
        if (inQ) { if (c == qc) inQ = false; else out += c; }
        else if (c == '\'' || c == '"') { inQ = true; qc = c; }
        else if (c == ',' || c == ')') {
            while (!out.empty() && isspace((unsigned char)out.back())) out.pop_back();
            size_t b = 0;
            while (b < out.size() && isspace((unsigned char)out[b])) ++b;
            if (b) out.erase(0, b);
            return c;
        } else out += c;
    }
    return 0;
}

// Advance the row-group scanner past one value. On entry *p points at the
// first char of the value (right after '(' or ','). Returns the separator that
// ended it (',' -> more values, ')' -> row end) or 0 when input ends before a
// separator (the partial value is discarded, matching the legacy parser).
// vptr/vlen receive a trimmed span into `s` (zero copy) — or into `scratch`
// when quotes make the content non-contiguous.
inline char scanInsertValue(const char* s, size_t n, size_t& p,
                            const char*& vptr, size_t& vlen,
                            std::string& scratch) {
    while (p < n && isspace((unsigned char)s[p])) ++p;
    const size_t vbeg = p;                     // first content char
    if (p >= n) return 0;                      // EOF right after separator

    // --- whole-value quoted literal: 'content' / "content" ----
    if (s[p] == '\'' || s[p] == '"') {
        const char q = s[p];
        const size_t cs = ++p;                 // content start (past open quote)
        while (p < n && s[p] != q) ++p;
        if (p >= n) { p = n; return 0; }       // unterminated quote -> discard
        const size_t ce = p;                   // content end (at close quote)
        size_t a = p + 1;
        while (a < n && isspace((unsigned char)s[a])) ++a;
        if (a < n && (s[a] == ',' || s[a] == ')')) {
            const char sep = s[a];
            // Clean quoted value: the content is contiguous; trim its ends the
            // way the legacy parser trimmed the whole token.
            vptr = s + cs;
            vlen = ce - cs;
            while (vlen && isspace((unsigned char)vptr[0])) { ++vptr; --vlen; }
            while (vlen && isspace((unsigned char)vptr[vlen - 1])) --vlen;
            p = a + 1;
            return sep;
        }
        // Complex quoting: replicate the legacy state machine into scratch.
        p = vbeg;
        return scanInsertValueGeneric(s, n, p, scratch);
    }

    // --- bare (unquoted) run: stops at ',' or ')' or a quote ----
    size_t e = p;
    for (; e < n; ++e) {
        const char c = s[e];
        if (c == ',' || c == ')') break;
        if (c == '\'' || c == '"') {          // quote mid-token -> generic path
            p = vbeg;
            return scanInsertValueGeneric(s, n, p, scratch);
        }
    }
    if (e >= n) { p = n; return 0; }           // EOF: partial value discarded
    const char sep = s[e];
    vptr = s + vbeg;
    vlen = e - vbeg;
    while (vlen && isspace((unsigned char)vptr[vlen - 1])) --vlen;
    p = e + 1;
    return sep;
}

// Serialize one INSERT value span into slot `dst` (cb bytes) for column type
// ct. Column 0 is always the timestamp (8 bytes, big-endian). Uses the fast
// span converters above; scratch is only touched by rare libc float fallbacks.
inline void storeInsertValue(int vi, ColType ct, const char* vp, size_t vl,
                             uint8_t* dst, int cb, std::string& scratch) {
    if (vi == 0) {
        // First column is always timestamp (8 bytes, big-endian)
        const int64_t ts = fastTsParse(vp, vl);
        const uint64_t be = htobe64((uint64_t)ts);
        memcpy(dst, &be, 8);
        return;
    }
    switch (ct) {
        case ColType::INT: {
            int32_t v = (int32_t)fastAtoi64(vp, vl);
            v = htonl(v);
            memcpy(dst, &v, 4);
            break;
        }
        case ColType::UINT: {
            uint32_t v = (uint32_t)fastAtou64(vp, vl);
            v = htonl(v);
            memcpy(dst, &v, 4);
            break;
        }
        case ColType::FLOAT: {
            const float f = parseFloatSpan(vp, vl, scratch);
            uint32_t b;
            memcpy(&b, &f, 4);
            b = htonl(b);
            memcpy(dst, &b, 4);
            break;
        }
        case ColType::DOUBLE: {
            const double d = parseDoubleSpan(vp, vl, scratch);
            uint64_t b;
            memcpy(&b, &d, 8);
            b = htobe64(b);
            memcpy(dst, &b, 8);
            break;
        }
        case ColType::BIGINT: {
            const int64_t v = fastAtoi64(vp, vl);
            const uint64_t b = htobe64((uint64_t)v);
            memcpy(dst, &b, 8);
            break;
        }
        case ColType::UBIGINT: {
            const uint64_t v = fastAtou64(vp, vl);
            const uint64_t b = htobe64(v);
            memcpy(dst, &b, 8);
            break;
        }
        case ColType::SMALLINT: {
            int16_t v = (int16_t)fastAtoi64(vp, vl);
            v = htons(v);
            memcpy(dst, &v, 2);
            break;
        }
        case ColType::USMALLINT: {
            uint16_t v = (uint16_t)fastAtou64(vp, vl);
            v = htons(v);
            memcpy(dst, &v, 2);
            break;
        }
        case ColType::TINYINT: {
            const int8_t v = (int8_t)fastAtoi64(vp, vl);
            memcpy(dst, &v, 1);
            break;
        }
        case ColType::UTINYINT: {
            const uint8_t v = (uint8_t)fastAtou64(vp, vl);
            memcpy(dst, &v, 1);
            break;
        }
        case ColType::BOOL: {
            const int8_t v = (int8_t)fastAtoi64(vp, vl);
            memcpy(dst, &v, 1);
            break;
        }
        case ColType::BINARY:
        case ColType::NCHAR: {
            // Fixed-size slot: write string bytes, zero-pad the rest.
            memset(dst, 0, (size_t)cb);
            size_t slen = vl;
            if (slen > (size_t)cb) slen = (size_t)cb;  // truncate
            if (slen) memcpy(dst, vp, slen);
            break;
        }
        default: {
            // Fallback: treat as double (8 bytes)
            const double d = parseDoubleSpan(vp, vl, scratch);
            uint64_t b;
            memcpy(&b, &d, 8);
            b = htobe64(b);
            memcpy(dst, &b, 8);
            break;
        }
    }
}

} // namespace

EtDBResult EtDBConnection::query(const std::string& sql, int dbId,
                                 EtDBMetaCache* metaCache) {
    EtDBResult result;
    int effdbId = (dbId > 0) ? dbId : _currentdbId;
    _lastdbId = effdbId;
    
    int byteCounts = sql.size();
    if (byteCounts > ETDB::Client::MAX_SQL_LEN) {
        result.setError("SQL statement too long");
        return result;
    }

    std::string upper = sql;
    for (auto& c : upper) c = (char)toupper((unsigned char)c);
    size_t pp = 0;
    while (pp < upper.size() && isspace((unsigned char)upper[pp])) ++pp;

    uint8_t msgBuf[ETDB::Client::MAX_SQL_LEN];
    int msgLen = 0;
    uint8_t msgType;
    bool expectResultSet = false;  // true → parse SQueryRsp; false → expect empty DDL response

    // === INSERT: pack binary SSubmitReq ===
    if (upper.find("INSERT ", pp) == pp) {
        msgType = Proto::MSG_SUBMIT;
        size_t p = 0;
        // skip "INSERT INTO"
        while (p < sql.size() && isalpha((unsigned char)sql[p])) ++p;
        while (p < sql.size() && isspace((unsigned char)sql[p])) ++p;
        while (p < sql.size() && isalpha((unsigned char)sql[p])) ++p;
        while (p < sql.size() && isspace((unsigned char)sql[p])) ++p;
        // extract table name
        std::string tn;
        while (p < sql.size() && !isspace((unsigned char)sql[p]) && sql[p] != '(') tn += sql[p++];

        // Parse db.table format
        std::string dbName = _db;
        std::string tblName = tn;
        auto dot = tn.find('.');
        if (dot != std::string::npos) {
            dbName = tn.substr(0, dot);
            tblName = tn.substr(dot + 1);
        }
        // Require a database context
        if (dbName.empty()) {
            result.setError("No database selected. Use USE <db> first or specify db.table");
            return result;
        }

        // Fetch table meta from shared cache (fetches from server if not cached)
        const TableMeta* tblMeta = nullptr;
        if (metaCache) {
            tblMeta = metaCache->getTableMeta(dbName, tblName);
            if (tblMeta)
                tDebug(", query: using cached table meta for %s, %u", tblMeta->name.c_str(), tblMeta->uid);
        }
        uint64_t tblUid = tblMeta ? tblMeta->uid : lookupTableUid(tn);
        if (tblUid == 0 && tblMeta) tblUid = tblMeta->uid;
        // Last resort: direct server meta query
        if (tblUid == 0) {
            tblUid = resolveTableUidFromServer(dbName, tblName);
        }
        // Still no uid — table doesn't exist or is inaccessible
        if (tblUid == 0) {
            result.setError("Table not found: " + tn);
            return result;
        }

        // Per-column wire slot sizes + types from meta (index 0 = timestamp).
        // Used through raw pointers with a hoisted count so the per-value hot
        // loop never re-checks vector size / bounds.
        std::vector<int> colSizeBuf;
        std::vector<ColType> colTypeBuf;
        if (tblMeta && !tblMeta->columns.empty()) {
            colSizeBuf.reserve(tblMeta->columns.size());
            colTypeBuf.reserve(tblMeta->columns.size());
            for (const auto& col : tblMeta->columns) {
                int cb = 8;  // default
                switch (col.type) {
                    case ColType::TIMESTAMP: case ColType::BIGINT: case ColType::UBIGINT:
                    case ColType::DOUBLE: cb = 8; break;
                    case ColType::INT: case ColType::UINT: case ColType::FLOAT: cb = 4; break;
                    case ColType::SMALLINT: case ColType::USMALLINT: cb = 2; break;
                    case ColType::TINYINT: case ColType::UTINYINT: case ColType::BOOL: cb = 1; break;
                    case ColType::BINARY: case ColType::NCHAR:
                        cb = (col.bytes > 0) ? col.bytes : 8; break;  // declared-size slot
                    default: cb = 8; break;
                }
                colSizeBuf.push_back(cb);
                colTypeBuf.push_back(col.type);
            }
        }
        const int nMetaCols = (int)colSizeBuf.size();
        const int* colSizeP = colSizeBuf.data();
        const ColType* colTypeP = colTypeBuf.data();

        // Find the VALUES row groups: (v1,v2,...) (v3,v4,...) ...
        while (p < sql.size() && sql[p] != '(') ++p;
        if (p >= sql.size()) { result.setError("Parse error: no VALUES"); return result; }

        // ---- Streaming packer (see the span converters above) ----
        // All "(...)" groups share one schema (column count/types — only the
        // values differ), so the first row fixes the layout and every row is
        // scanned and serialized straight into the wire buffer as it goes:
        // no allRows storage, no per-value std::string on the hot path.
        int blkHdr = (int)(sizeof(SDataBlock) - sizeof(char));
        int msgHdr = (int)(sizeof(SSubmitReq) - sizeof(char));
        int headerLen = (int)sizeof(SMsgHead) + (int)sizeof(STxHead);
        int dataStartOff = headerLen + msgHdr + blkHdr;
        int maxData = (int)sizeof(msgBuf) - dataStartOff;
        if (maxData < 8) { result.setError("INSERT message buffer too small"); return result; }
        char* dataStart = (char*)msgBuf + dataStartOff;

        const char* s = sql.c_str();
        const size_t n = sql.size();
        size_t g = p;                        // scanner position (p points at '(')
        int totalColsPerRow = 0;
        int rowBytes = 0;
        int numRows = 0;
        std::string scratch;                 // rare-path fallback buffer (reused)
        scratch.reserve(64);

        while (g < n && s[g] == '(') {
            ++g;                             // skip '('
            char* rowBase = dataStart + (size_t)numRows * (size_t)rowBytes;
            int colCount = 0;
            int rowOff = 0;
            char sep = 0;
            while (g < n) {
                const char* vp = nullptr;
                size_t vl = 0;
                sep = scanInsertValue(s, n, g, vp, vl, scratch);
                if (sep == 0) break;         // EOF mid-row: keep flushed values
                const int vi = colCount;
                const int cb = (vi < nMetaCols) ? colSizeP[vi] : 8;
                if (rowOff + cb > maxData - (int)(rowBase - dataStart)) {
                    result.setError("INSERT too large for one message");
                    return result;
                }
                const ColType ct = (vi < nMetaCols) ? colTypeP[vi] : ColType::BIGINT;
                storeInsertValue(vi, ct, vp, vl, (uint8_t*)rowBase + rowOff, cb, scratch);
                rowOff += cb;
                ++colCount;
                if (sep == ')') break;       // row closed
            }
            if (totalColsPerRow == 0) {
                // First row fixes the per-row width (all rows share it).
                totalColsPerRow = colCount;
                if (totalColsPerRow > 0) {
                    rowBytes = 0;
                    for (int vi2 = 0; vi2 < totalColsPerRow; ++vi2)
                        rowBytes += (vi2 < nMetaCols) ? colSizeP[vi2] : 8;
                    if (rowBytes == 0) rowBytes = 8 + (totalColsPerRow - 1) * 8;
                }
            } else if (colCount != totalColsPerRow) {
                result.setError("Parse error: inconsistent column count in VALUES");
                return result;
            }
            if (colCount == 0) break;        // empty trailing group at EOF -> ignore
            ++numRows;
            // Skip whitespace and comma between groups
            while (g < n && (isspace((unsigned char)s[g]) || s[g] == ',')) ++g;
        }
        if (numRows == 0) { result.setError("Parse error: no values"); return result; }

        int totalRowData = numRows * rowBytes;
        int blkSize = blkHdr + totalRowData;
        int payloadLen = msgHdr + blkSize;

        // Fill the SSubmitReq / SDataBlock headers (row data was written above)
        uint8_t* payload = msgBuf + headerLen;  // construct payload directly in msgBuf to avoid extra copy
        memset(payload, 0, (size_t)dataStartOff - (size_t)headerLen);
        SSubmitReq* sm = (SSubmitReq*)payload;
        sm->length = htonl(payloadLen);
        sm->numOfBlocks = htons(1); // single block for all rows
        SDataBlock* blk = (SDataBlock*)(payload + msgHdr);
        blk->uid = htobe64(tblUid);
        blk->sversion = htonl(tblMeta ? tblMeta->sversion : 1);
        blk->suid = 0;
        blk->dataLen = htonl(totalRowData);
        blk->schemaLen = 0;
        blk->numOfRows = htons((int16_t)numRows);
        // INSERT routes to the TABLE's dbnode (dbId from its db), not the
        // session's current dbId — INSERT INTO other_db.tbl must land in the
        // owning dbnode even if a different db is currently selected.
        int routedbId = (tblMeta && tblMeta->dbId > 0) ? (int)tblMeta->dbId : effdbId;
        _lastdbId = routedbId;
        msgLen = etdbBuildFullMsg(msgBuf, sizeof(msgBuf), routedbId, payload, payloadLen, msgType, _user.c_str());
        if (msgLen < 0) { result.setError("Build INSERT msg failed"); return result; }
        expectResultSet = true;  // INSERT response: SQueryRsp with "affected_rows"
    }
    // === SELECT: SQL text → server parses ===
    else if (upper.find("SELECT ", pp) == pp) {
        msgType = Proto::MSG_QUERY;
        msgLen = etdbBuildSqlMsg(msgBuf, sizeof(msgBuf), effdbId, sql, msgType, _user.c_str());
        if (msgLen < 0) { result.setError("Build QUERY msg failed"); return result; }
        expectResultSet = true;
    }
    // === SHOW, DESCRIBE: SQL text → server parses, returns SQueryRsp ===
    else if (upper.find("SHOW ", pp) == pp || upper.find("DESCRIBE ", pp) == pp
             || upper.find("DESC ", pp) == pp) {
        msgType = Proto::MSG_CM_SHOW;
        msgLen = etdbBuildSqlMsg(msgBuf, sizeof(msgBuf), effdbId, sql, msgType, _user.c_str());
        if (msgLen < 0) { result.setError("Build SHOW msg failed"); return result; }
        expectResultSet = true;
    }
    // === DDL (CREATE/DROP/ALTER/USE): SQL text → server parses, response empty ===
    else {
        if (upper.find("CREATE DATABASE", pp) == pp || upper.find("CREATE DB", pp) == pp)
            msgType = Proto::MSG_CM_CREATE_DB;
        else if (upper.find("DROP DATABASE", pp) == pp || upper.find("DROP DB", pp) == pp)
            msgType = Proto::MSG_CM_DROP_DB;
        else if (upper.find("USE ", pp) == pp)
            msgType = Proto::MSG_CM_USE_DB;
        else if (upper.find("CREATE TABLE", pp) == pp || upper.find("CREATE STABLE", pp) == pp)
            msgType = Proto::MSG_CM_CREATE_TABLE;
        else if (upper.find("DROP TABLE", pp) == pp || upper.find("DROP STABLE", pp) == pp)
            msgType = Proto::MSG_CM_DROP_TABLE;
        else if (upper.find("ALTER ", pp) == pp)
            msgType = Proto::MSG_CM_ALTER_TABLE;
        else if (upper.find("CREATE USER", pp) == pp)
            msgType = Proto::MSG_CM_CREATE_USER;
        else if (upper.find("DROP USER", pp) == pp)
            msgType = Proto::MSG_CM_DROP_USER;
        else if (upper.find("ALTER USER", pp) == pp)
            msgType = Proto::MSG_CM_ALTER_USER;
        else if (upper.find("GRANT ", pp) == pp)
            msgType = Proto::MSG_CM_ALTER_USER;   // reuse: GRANT is a DDL write
        else if (upper.find("REVOKE ", pp) == pp)
            msgType = Proto::MSG_CM_ALTER_USER;   // reuse: REVOKE is a DDL write
        else {
            result.setError("Syntax error: unrecognized SQL command near '" +
                            upper.substr(pp, std::min(size_t(20), upper.size() - pp)) + "'");
            return result;
        }
        msgLen = etdbBuildSqlMsg(msgBuf, sizeof(msgBuf), effdbId, sql, msgType, _user.c_str());
        if (msgLen < 0) { result.setError("Build DDL msg failed"); return result; }
    }

    // ---- Send and receive ----
    bool qon = qtimeEnabled();
    if (qon) g_qperf.enabled = true;
    int64_t tsQ = qon ? qnowUs() : 0;
    std::vector<uint8_t> rspCont;
    int rspCode;
    uint8_t rspType;
    if (!sendRecv(msgBuf, msgLen, rspCont, rspCode, rspType)) {
        result.setError("Network send/recv failed");
        return result;
    }
    if (qon) g_qperf.queryRttUs += qnowUs() - tsQ;
    if (rspCode != 0) {
        result.setError("Server returned error code: " + std::to_string(rspCode));
        return result;
    }

    // ---- Parse response ----
    if (!expectResultSet && rspCont.size() <= sizeof(SMsgHead)) {
        // Update _db on successful USE command (parse from original SQL, not uppercased)
        if (msgType == Proto::MSG_CM_USE_DB) {
            const char* sp = sql.c_str();
            while (*sp && isspace((unsigned char)*sp)) ++sp;
            if (strncasecmp(sp, "use ", 4) == 0) {
                sp += 4;
                while (*sp && isspace((unsigned char)*sp)) ++sp;
                std::string newDb;
                while (*sp && !isspace((unsigned char)*sp) && *sp != ';') newDb += *sp++;
                if (!newDb.empty()) {
                    _db = newDb;
                    tDebug("Switched to database '%s'", _db.c_str());
                }
            }
        }
        return result;
    }
    if (expectResultSet) {
        if (msgType == Proto::MSG_SUBMIT) {
            // INSERT response: SSubmitRspMsg format
            if (!result.deserializeSubmitRsp(rspCont.data(), (int)rspCont.size())) {
                result.setError("Failed to deserialize submit response");
            }
        } else {
            int64_t tsD = qon ? qnowUs() : 0;
            if (!result.deserialize(rspCont.data(), (int)rspCont.size())) {
                result.setError("Failed to deserialize response");
            }
            if (qon) g_qperf.queryDeserUs += qnowUs() - tsD;
        }
        // Bind connection for streaming FETCH (streamQId() != 0 → fetchRow works)
        result.setStreamConn(this);
    }
    return result;
}

bool EtDBConnection::fetchStreamRows(int64_t qId, int batchSize, EtDBResult& out) {
    if (!TcpClient::valid(_sock) || qId == 0) return false;

    // Build FETCH request: STxHead  + SMsgHead + SFetchReq
    int contentLen = sizeof(SMsgHead) + sizeof(SFetchReq);
    int totalLen = (int)sizeof(STxHead) + contentLen;
    uint8_t msgBuf[1024*2];
    memset(msgBuf, 0, sizeof(msgBuf));

    SMsgHead* head = (SMsgHead*)(msgBuf + sizeof(STxHead) );
    head->dbId    = htonl(_lastdbId);
    head->contLen = htonl((int)sizeof(SFetchReq));

    SFetchReq* req = (SFetchReq*)(msgBuf + sizeof(STxHead) + sizeof(SMsgHead));
    req->qId       = htonl((int32_t)qId);
    req->batchSize = htonl(batchSize);

    STxHead* rpc = (STxHead*)msgBuf;
    rpc->version = 0x01;
    rpc->msgType = Proto::MSG_FETCH;
    rpc->msgLen  = htonl(totalLen);
    strncpy(rpc->user, _user.c_str(), ETDB_USER_LEN - 1);

    bool qon = qtimeEnabled();
    if (qon) g_qperf.enabled = true;
    int64_t tsF = qon ? qnowUs() : 0;
    std::vector<uint8_t> rspCont;
    int rspCode; uint8_t rspType;
    if (!sendRecv(msgBuf, totalLen, rspCont, rspCode, rspType)) return false;
    if (qon) g_qperf.fetchRttUs += qnowUs() - tsF;
    if (rspCode != 0) return false;

    int64_t tsD = qon ? qnowUs() : 0;
    if (!out.deserialize(rspCont.data(), (int)rspCont.size())) return false;
    if (qon) { g_qperf.fetchDeserUs += qnowUs() - tsD; g_qperf.fetchBatches++; }
    return true;
}

int EtDBConnection::executeInsert(const uint8_t* submitData, int submitLen, int dbId,
                                  std::vector<uint8_t>& rspCont) {
    if (!TcpClient::valid(_sock)) return -1;

    int contentLen = (int)sizeof(SMsgHead) + submitLen;
    int totalLen = (int)sizeof(STxHead) + contentLen;
    std::vector<uint8_t> msgBuf(totalLen, 0);

    SMsgHead* head = (SMsgHead*)(msgBuf.data() + sizeof(STxHead));
    head->dbId    = htonl(dbId);
    head->contLen = htonl(submitLen);

    // Copy submit data
    memcpy(msgBuf.data() + sizeof(STxHead)+ sizeof(SMsgHead),
           submitData, submitLen);

    // STxHead
    STxHead* rpc = (STxHead*)msgBuf.data();
    rpc->version = 0x01;
    rpc->msgType = Proto::MSG_SUBMIT;
    rpc->msgLen  = htonl(totalLen);
    strncpy(rpc->user, _user.c_str(), ETDB_USER_LEN - 1);

    // Send
    int rspCode;
    uint8_t rspType;
    if (!sendRecv(msgBuf.data(), totalLen, rspCont, rspCode, rspType))
        return -1;

    // Parse SSubmitRspMsg from response body to get actual affected rows
    if (rspCode == 0 && !rspCont.empty()) {
        int minSize = (int)(sizeof(SSubmitRspMsg) - 1);
        if ((int)rspCont.size() >= minSize) {
            const auto* rsp = (const SSubmitRspMsg*)rspCont.data();
            int affectedRows = (int)ntohl(rsp->affectedRows);
            int errorRows    = (int)ntohl(rsp->errorRows);
            if (errorRows > 0) return -errorRows;  // negative = error count
            return (affectedRows > 0) ? affectedRows : rspCode;
        }
    }
    return rspCode;
}

// ============================================================================
// EtDBMetaCache::fetchTableMeta — Real implementation
// (needs EtDBConnection::sendRecv, so it lives in the cpp)
// ============================================================================
TableMeta* EtDBMetaCache::fetchTableMeta(const std::string& dbName,
                                         const std::string& tableName) {
    if (!_conn || !_conn->isConnected()) {
        return nullptr;
    }

    // Build STableMetaReq
    STableMetaReq req;
    memset(&req, 0, sizeof(req));
    int dnl = std::min((int)dbName.size(), 63);
    req.dbNameLen = htonl(dnl);
    memcpy(req.dbName, dbName.c_str(), dnl);
    int tnl = std::min((int)tableName.size(), 255);
    req.tableNameLen = htonl(tnl);
    memcpy(req.tableName, tableName.c_str(), tnl);
    req.needColumns = 1;

    int totalLen = (int)sizeof(STxHead) + (int)sizeof(STableMetaReq);
    std::vector<uint8_t> fullMsg(totalLen, 0);
    STxHead* head = (STxHead*)fullMsg.data();
    head->version = 0x01;
    head->msgType = ETDB::Proto::MSG_CM_TABLE_META;
    head->msgLen  = htonl(totalLen);
    strncpy(head->user, _conn->userName().c_str(), ETDB_USER_LEN - 1);
    memcpy(fullMsg.data() + sizeof(STxHead), &req, sizeof(req));

    std::vector<uint8_t> rspCont;
    int rspCode = -1;
    uint8_t rspType = 0;
    if (!_conn->sendRecv(fullMsg.data(), totalLen, rspCont, rspCode, rspType))
        return nullptr;
    if (rspCode != 0 || rspCont.empty()) return nullptr;

    std::vector<TableMeta> metas;
    if (parseMetaRsp(rspCont.data(), (int32_t)rspCont.size(), metas) != 0)
        return nullptr;
    if (metas.empty()) return nullptr;

    // Fix up dbName and re-cache with correct key
    for (auto& m : metas) {
        m.dbName = dbName;
        putCached(dbName, m.name, m);
    }

    return const_cast<TableMeta*>(getCached(dbName, tableName));
}

// ============================================================================
// EtDBStmt Implementation
// ============================================================================

EtDBStmt::EtDBStmt(EtDBConnection* conn, EtDBMetaCache* cache)
    : _conn(conn), _metaCache(cache), _state(INIT) {}

EtDBStmt::~EtDBStmt() { close(); }

bool EtDBStmt::prepare(const std::string& sql) {
    if (!_conn || !_conn->isConnected()) return false;

    // Parse INSERT SQL to extract table name and column count
    const char* s = sql.c_str();
    while (*s && isspace(*s)) ++s;
    if (strncasecmp(s, "insert", 6) != 0) return false;
    s += 6;
    while (*s && isspace(*s)) ++s;
    if (strncasecmp(s, "into", 4) != 0) return false;
    s += 4;
    while (*s && isspace(*s)) ++s;

    // Extract table name
    _tableName.clear();
    while (*s && !isspace(*s) && *s != '(' && *s != ';')
        _tableName += *s++;
    if (_tableName.empty()) return false;

    // Find "VALUES" and count '?' placeholders
    const char* vals = strcasestr(s, "values");
    if (!vals) return false;
    vals += 6;

    // Count '?' between VALUES(...)
    _numCols = 0;
    const char* p = vals;
    while (*p && *p != ')') {
        if (*p == '?') _numCols++;
        ++p;
    }
    if (_numCols <= 0) return false;

    // Parse "db.table" format
    std::string dbName = _conn->dbName();  // use connection's current db
    std::string tblName = _tableName;
    size_t dotPos = _tableName.find('.');
    if (dotPos != std::string::npos) {
        dbName  = _tableName.substr(0, dotPos);
        tblName = _tableName.substr(dotPos + 1);
    }

    // Try to fetch table meta from server (via meta cache)
    if (_metaCache) {
        _tableMeta = _metaCache->getTableMeta(dbName, tblName);
    }
    if (!_tableMeta) {
        uint64_t cachedUid = _conn->lookupTableUid(_tableName);
        if (cachedUid > 0) {
            _tableMeta = _metaCache ? _metaCache->getTableMeta(dbName, tblName) : nullptr;
        }
    }
    if (!_tableMeta) {
        uint64_t uid = _conn->resolveTableUidFromServer(dbName, tblName);
        if (uid > 0) {
            _tableUid = uid;
            _targetdbId = 1;
            _tableTid = 0;
            _tableMeta = _metaCache ? _metaCache->getTableMeta(dbName, tblName) : nullptr;
        }
    }

    if (_tableMeta) {
        _targetdbId = _tableMeta->dbId;
        _tableUid   = _tableMeta->uid;
        _tableTid   = _tableMeta->tid;

        // Use actual column types from meta
        _colTypes.resize(_numCols);
        for (int i = 0; i < _numCols && i < (int)_tableMeta->columns.size(); ++i) {
            switch (_tableMeta->columns[i].type) {
                case ColType::TIMESTAMP: _colTypes[i] = TYPE_TIMESTAMP; break;
                case ColType::BOOL:      _colTypes[i] = TYPE_BOOL;      break;
                case ColType::TINYINT:   _colTypes[i] = TYPE_TINYINT;   break;
                case ColType::SMALLINT:  _colTypes[i] = TYPE_SMALLINT;  break;
                case ColType::INT:       _colTypes[i] = TYPE_INT;       break;
                case ColType::BIGINT:    _colTypes[i] = TYPE_BIGINT;    break;
                case ColType::FLOAT:     _colTypes[i] = TYPE_FLOAT;     break;
                case ColType::DOUBLE:    _colTypes[i] = TYPE_DOUBLE;    break;
                case ColType::BINARY:    _colTypes[i] = TYPE_BINARY;    break;
                case ColType::NCHAR:     _colTypes[i] = TYPE_NCHAR;     break;
                case ColType::UTINYINT:  _colTypes[i] = TYPE_TINYINT;   break;
                case ColType::USMALLINT: _colTypes[i] = TYPE_SMALLINT;  break;
                case ColType::UINT:      _colTypes[i] = TYPE_INT;       break;
                case ColType::UBIGINT:   _colTypes[i] = TYPE_BIGINT;    break;
                default:                 _colTypes[i] = TYPE_BIGINT;    break;
            }
        }
        for (int i = (int)_tableMeta->columns.size(); i < _numCols; ++i)
            _colTypes[i] = TYPE_BIGINT;
    } else if (_tableUid > 0) {
        // UID resolved via fallback but no full meta — use default column types
        _colTypes.resize(_numCols);
        for (int i = 0; i < _numCols; ++i) {
            _colTypes[i] = TYPE_TIMESTAMP;
        }
        if (_numCols >= 3) {
            _colTypes[0] = TYPE_TIMESTAMP;
            _colTypes[1] = TYPE_FLOAT;
            _colTypes[2] = TYPE_FLOAT;
        }
    } else {
        LOG_ERROR << "prepare: failed to resolve table UID for " << dbName << "." << tblName;
        _state = INIT;
        return false;
    }

    // Compute row bytes (host byte order) + per-column slot sizes
    _rowBytes = 0;
    _colBytes.clear();
    for (int i = 0; i < _numCols; ++i) {
        int cb = 4;  // default
        switch (_colTypes[i]) {
            case TYPE_TIMESTAMP: case TYPE_BIGINT: case TYPE_DOUBLE: cb = 8; break;
            case TYPE_INT: case TYPE_FLOAT: cb = 4; break;
            case TYPE_SMALLINT: cb = 2; break;
            case TYPE_TINYINT: case TYPE_BOOL: cb = 1; break;
            case TYPE_BINARY: case TYPE_NCHAR:
                if (_tableMeta && i < (int)_tableMeta->columns.size()
                    && _tableMeta->columns[i].bytes > 0)
                    cb = _tableMeta->columns[i].bytes;
                else
                    cb = 8;
                break;
            default: cb = 4; break;
        }
        _colBytes.push_back(cb);
        _rowBytes += cb;
    }

    _rowBuf.resize(_rowBytes);
    _state = PREPARED;
    return true;
}

bool EtDBStmt::bindParam(BindParam* binds, int numBinds) {
    if (_state != PREPARED && _state != BINDING && _state != BATCHED) return false;
    if (numBinds != _numCols) return false;

    memset(_rowBuf.data(), 0, _rowBytes);
    int offset = 0;

    for (int i = 0; i < numBinds; ++i) {
        BindParam& b = binds[i];
        if (b.isNull && *b.isNull) {
            // NULL: write zeros
            int cb = 0;
            switch (_colTypes[i]) {
                case TYPE_TIMESTAMP: case TYPE_BIGINT: case TYPE_DOUBLE: cb = 8; break;
                case TYPE_INT: case TYPE_FLOAT: cb = 4; break;
                case TYPE_SMALLINT: cb = 2; break;
                case TYPE_TINYINT: case TYPE_BOOL: cb = 1; break;
                default: cb = 4; break;
            }
            offset += cb;
            continue;
        }

        switch (_colTypes[i]) {
            case TYPE_TIMESTAMP:
            case TYPE_BIGINT: {
                if (offset + 8 <= _rowBytes) {
                    int64_t v = *(int64_t*)b.buffer;
                    memcpy(_rowBuf.data() + offset, &v, 8);
                    offset += 8;
                }
                break;
            }
            case TYPE_DOUBLE: {
                if (offset + 8 <= _rowBytes) {
                    double v = *(double*)b.buffer;
                    memcpy(_rowBuf.data() + offset, &v, 8);
                    offset += 8;
                }
                break;
            }
            case TYPE_INT: {
                if (offset + 4 <= _rowBytes) {
                    int32_t v = *(int32_t*)b.buffer;
                    memcpy(_rowBuf.data() + offset, &v, 4);
                    offset += 4;
                }
                break;
            }
            case TYPE_FLOAT: {
                if (offset + 4 <= _rowBytes) {
                    float v = *(float*)b.buffer;
                    memcpy(_rowBuf.data() + offset, &v, 4);
                    offset += 4;
                }
                break;
            }
            case TYPE_SMALLINT: {
                if (offset + 2 <= _rowBytes) {
                    int16_t v = *(int16_t*)b.buffer;
                    memcpy(_rowBuf.data() + offset, &v, 2);
                    offset += 2;
                }
                break;
            }
            case TYPE_TINYINT:
            case TYPE_BOOL: {
                if (offset + 1 <= _rowBytes) {
                    int8_t v = *(int8_t*)b.buffer;
                    memcpy(_rowBuf.data() + offset, &v, 1);
                    offset += 1;
                }
                break;
            }
            case TYPE_BINARY:
            case TYPE_NCHAR: {
                // Fixed-size slot: copy up to `slot` bytes, zero-pad the rest.
                int len = b.lengthPtr ? (int)*b.lengthPtr : (int)b.length;
                if (len < 0) len = 0;
                int slot = (i < (int)_colBytes.size()) ? _colBytes[i] : 8;
                if (offset + slot > _rowBytes) break;
                memset(_rowBuf.data() + offset, 0, (size_t)slot);
                if (len > slot) len = slot;
                if (b.buffer && len > 0)
                    memcpy(_rowBuf.data() + offset, b.buffer, (size_t)len);
                offset += slot;
                break;
            }
            default:
                break;
        }
    }

    _state = BINDING;
    return true;
}

bool EtDBStmt::bindParamBatch(MultiBind* binds, int numBinds) {
    if (_state != PREPARED && _state != BATCHED) return false;
    if (numBinds != _numCols) return false;

    int numRows = binds[0].numRows;
    int rowBytes = _rowBytes;

    // Build SDataBlock + row data in network byte order
    int blkHdrSize = (int)(sizeof(SDataBlock) - 1);
    int batchSize = blkHdrSize + numRows * rowBytes;
    _batchData.assign(batchSize, 0);

    SDataBlock* blk = (SDataBlock*)_batchData.data();
    blk->sversion = htonl(_tableMeta ? _tableMeta->sversion : 1);
    blk->suid     = 0;
    blk->uid      = htobe64(_tableUid);
    blk->dataLen  = htonl(numRows * rowBytes);
    blk->schemaLen = 0;
    blk->numOfRows = htons((int16_t)numRows);

    char* rows = blk->data;
    for (int ri = 0; ri < numRows; ++ri) {
        int rowOff = ri * rowBytes;
        for (int ci = 0; ci < numBinds; ++ci) {
            MultiBind& b = binds[ci];
            int cb = (ci < (int)_colBytes.size()) ? _colBytes[ci] : 4;

            if (b.nullFlags && b.nullFlags[ri]) {
                memset(rows + rowOff, 0, cb);
                rowOff += cb;
                continue;
            }

            char* src = (char*)b.buffer + ri * (int)b.stride;
            switch (_colTypes[ci]) {
                case TYPE_TIMESTAMP: case TYPE_BIGINT: {
                    int64_t v;
                    memcpy(&v, src, 8);
                    v = htobe64(v);
                    memcpy(rows + rowOff, &v, 8);
                    break;
                }
                case TYPE_DOUBLE: {
                    double dv;
                    memcpy(&dv, src, 8);
                    uint64_t nv = htobe64(*(uint64_t*)&dv);
                    memcpy(rows + rowOff, &nv, 8);
                    break;
                }
                case TYPE_INT: {
                    int32_t v;
                    memcpy(&v, src, 4);
                    v = htonl(v);
                    memcpy(rows + rowOff, &v, 4);
                    break;
                }
                case TYPE_FLOAT: {
                    float fv;
                    memcpy(&fv, src, 4);
                    uint32_t nv = htonl(*(uint32_t*)&fv);
                    memcpy(rows + rowOff, &nv, 4);
                    break;
                }
                case TYPE_SMALLINT: {
                    int16_t v;
                    memcpy(&v, src, 2);
                    v = htons(v);
                    memcpy(rows + rowOff, &v, 2);
                    break;
                }
                case TYPE_TINYINT: case TYPE_BOOL: {
                    memcpy(rows + rowOff, src, 1);
                    break;
                }
                default:
                    memcpy(rows + rowOff, src, cb);
                    break;
            }
            rowOff += cb;
        }
    }

    _batchRows = numRows;
    _state = BATCHED;
    return true;
}

bool EtDBStmt::addBatch() {
    if (_state != BINDING) return false;

    int rowBytes = _rowBytes;
    int blkHdrSize = (int)(sizeof(SDataBlock) - 1);
    bool firstRow = _batchData.empty();

    if (firstRow) {
        _batchData.resize(blkHdrSize);
        memset(_batchData.data(), 0, blkHdrSize);
    }
    int oldSize = (int)_batchData.size();
    _batchData.resize(oldSize + rowBytes);

    char* dst = (char*)_batchData.data() + oldSize;
    int srcOff = 0;
    for (int ci = 0; ci < _numCols; ++ci) {
        int cb = (ci < (int)_colBytes.size()) ? _colBytes[ci] : 4;
        if (srcOff + cb > _rowBytes) break;

        switch (_colTypes[ci]) {
            case TYPE_TIMESTAMP: case TYPE_BIGINT: {
                int64_t v;
                memcpy(&v, _rowBuf.data() + srcOff, 8);
                v = htobe64(v);
                memcpy(dst + srcOff, &v, 8);
                break;
            }
            case TYPE_DOUBLE: {
                double dv;
                memcpy(&dv, _rowBuf.data() + srcOff, 8);
                uint64_t nv = htobe64(*(uint64_t*)&dv);
                memcpy(dst + srcOff, &nv, 8);
                break;
            }
            case TYPE_INT: {
                int32_t v;
                memcpy(&v, _rowBuf.data() + srcOff, 4);
                v = htonl(v);
                memcpy(dst + srcOff, &v, 4);
                break;
            }
            case TYPE_FLOAT: {
                float fv;
                memcpy(&fv, _rowBuf.data() + srcOff, 4);
                uint32_t nv = htonl(*(uint32_t*)&fv);
                memcpy(dst + srcOff, &nv, 4);
                break;
            }
            case TYPE_SMALLINT: {
                int16_t v;
                memcpy(&v, _rowBuf.data() + srcOff, 2);
                v = htons(v);
                memcpy(dst + srcOff, &v, 2);
                break;
            }
            case TYPE_TINYINT: case TYPE_BOOL:
                memcpy(dst + srcOff, _rowBuf.data() + srcOff, 1);
                break;
            default:
                memcpy(dst + srcOff, _rowBuf.data() + srcOff, cb);
                break;
        }
        srcOff += cb;
    }

    _batchRows++;
    _state = BATCHED;
    return true;
}

int EtDBStmt::execute(int dbId) {
    if (_state != BATCHED || _batchData.empty()) return -1;
    if (!_conn || !_conn->isConnected()) return -1;

    if (_tableUid == 0) {
        LOG_ERROR << "execute: table UID not resolved, call prepare() first";
        return -1;
    }

    int32_t  actualdbId = (_targetdbId > 0) ? _targetdbId : dbId;
    uint64_t actualUid  = _tableUid;
    int32_t  sversion   = _tableMeta ? _tableMeta->sversion : 1;

    // Update SDataBlock header with meta-derived uid and sversion
    SDataBlock* blk = (SDataBlock*)_batchData.data();
    blk->sversion = htonl(sversion);
    blk->uid      = htobe64(actualUid);
    blk->dataLen  = htonl(_batchRows * _rowBytes);
    blk->numOfRows = htons((int16_t)_batchRows);
    LOG_DEBUG << ", executing batch of " << _batchRows << " rows for table UID " << actualUid
              << " (dbId=" << actualdbId << ", sversion=" << sversion << ")";

    // Build SSubmitReq wrapper
    int blkSize = (int)_batchData.size();
    int msgSize = (int)(sizeof(SSubmitReq) - 1) + blkSize;
    std::vector<uint8_t> submitBuf(msgSize);
    SSubmitReq* sm = (SSubmitReq*)submitBuf.data();
    sm->length      = htonl(msgSize);
    sm->numOfBlocks = htons(1); //single table insert with one block
    memcpy(submitBuf.data() + sizeof(SSubmitReq) - 1, _batchData.data(), blkSize);

    // Send to server and receive response with body
    std::vector<uint8_t> rspCont;
    int code = _conn->executeInsert(submitBuf.data(), msgSize, actualdbId, rspCont);

    // Parse SSubmitRspMsg from response body
    if ((int)rspCont.size() >= (int)(sizeof(SSubmitRspMsg) - 1)) {
        const auto* rsp = (const SSubmitRspMsg*)rspCont.data();
        int submitted = (int)ntohl(rsp->numOfRows);
        int affected  = (int)ntohl(rsp->affectedRows);
        int errors    = (int)ntohl(rsp->errorRows);

        _submittedRows += submitted;
        _affectedRows  += affected;
        _errorRows     += errors;

        // Parse error entries
        _errorEntries.clear();
        if (errors > 0) {
            const auto* errEntry = (const SSubmitErrEntry*)rsp->errorData;
            for (int ei = 0; ei < errors; ++ei) {
                RowError re;
                re.rowIndex  = (int)ntohl(errEntry[ei].rowIndex);
                re.errorCode = (int)ntohl(errEntry[ei].errorCode);
                _errorEntries.push_back(re);
            }
        }
    } else if (code == 0) {
        // No detailed response body — assume all succeeded
        _affectedRows  += _batchRows;
        _submittedRows += _batchRows;
    }

    int rowsThisBatch = _batchRows;
    (void)rowsThisBatch;
    // Reset batch (keep accumulated stats)
    _batchData.clear();
    _batchRows = 0;
    _state = PREPARED;
    return (code == 0) ? _affectedRows : code;
}

int64_t EtDBStmt::executeAsync(int dbId) {
    if (_state != BATCHED || _batchData.empty()) return -1;
    if (!_conn || !_conn->isConnected()) return -1;
    if (_tableUid == 0) {
        LOG_ERROR << "executeAsync: table UID not resolved, call prepare() first";
        return -1;
    }

    int32_t  actualdbId = (_targetdbId > 0) ? _targetdbId : dbId;
    uint64_t actualUid  = _tableUid;
    int32_t  sversion   = _tableMeta ? _tableMeta->sversion : 1;

    // Update SDataBlock header with meta-derived uid and sversion
    SDataBlock* blk = (SDataBlock*)_batchData.data();
    blk->sversion = htonl(sversion);
    blk->uid      = htobe64(actualUid);
    blk->dataLen  = htonl(_batchRows * _rowBytes);
    blk->numOfRows = htons((int16_t)_batchRows);
    LOG_DEBUG << ", async submit batch of " << _batchRows << " rows for table UID " << actualUid
              << " (dbId=" << actualdbId << ", sversion=" << sversion << ")";

    // Build SSubmitReq wrapper
    int blkSize = (int)_batchData.size();
    int msgSize = (int)(sizeof(SSubmitReq) - 1) + blkSize;
    std::vector<uint8_t> submitBuf(msgSize);
    SSubmitReq* sm = (SSubmitReq*)submitBuf.data();
    sm->length      = htonl(msgSize);
    sm->numOfBlocks = htons(1);
    memcpy(submitBuf.data() + sizeof(SSubmitReq) - 1, _batchData.data(), blkSize);

    // Send without waiting; the response is collected into the result vector
    // and can be retrieved with the returned batch id via waitAsyncResult().
    int64_t batchId = _conn->executeInsertAsync(submitBuf.data(), msgSize, actualdbId);
    if (batchId <= 0) return -1;

    // Reset batch (keep accumulated stats for sync execute())
    _batchData.clear();
    _batchRows = 0;
    _state = PREPARED;
    return batchId;
}

void EtDBStmt::close() {
    _batchData.clear();
    _rowBuf.clear();
    _numCols = 0;
    _rowBytes = 0;
    _batchRows = 0;
    _affectedRows = 0;
    _submittedRows = 0;
    _errorRows = 0;
    _errorEntries.clear();
    _state = INIT;
}

int EtDBStmt::affectedRows() const { return _affectedRows; }
int EtDBStmt::submittedRows() const { return _submittedRows; }
int EtDBStmt::errorRows() const { return _errorRows; }
const std::vector<EtDBStmt::RowError>& EtDBStmt::errorEntries() const { return _errorEntries; }

// ============================================================================
// EtDBClient Implementation
// ============================================================================

EtDBClient::EtDBClient() : _metaCache(&_conn) {}
EtDBClient::~EtDBClient() { close(); }

void EtDBClient::init() {
    // Placeholder for library-level init (Windows 下 WSAStartup 在 connect 时调用)
    TcpClient::init();
}

bool EtDBClient::connect(const std::string& host, uint16_t port,
                         const char* user, const char* password,
                         const char* db) {
    return _conn.connect(host, port, user, password, db);
}

// 提取 SQL 中表引用的 db 前缀（"FROM db.table" / "INTO db.table" /
// "TABLE db.table" 等）。返回小写 db 名；无前缀或非表语句返回空。
// 用于跨库查询路由：SELECT ... FROM other_db.tbl 必须落到 other_db 的 dbnode。
static std::string extractTableDbPrefix(const std::string& sql) {
    static const char* kMarkers[] = { "FROM ", "INTO ", "TABLE ", "UPDATE ", "JOIN " };
    size_t best = std::string::npos;
    for (const char* m : kMarkers) {
        size_t pos = 0;
        const size_t ml = strlen(m);
        while ((pos = sql.find(m, pos)) != std::string::npos) {
            // 前一个字符不能是标识符字符（避免命中列名/子串如 XFROM）
            if (pos > 0) {
                char prev = sql[pos - 1];
                if (isalnum((unsigned char)prev) || prev == '_') { pos += ml; continue; }
            }
            size_t t = pos + ml;
            while (t < sql.size() && isspace((unsigned char)sql[t])) ++t;
            if (t < sql.size()) {
                char c = sql[t];
                if (isalnum((unsigned char)c) || c == '_' || c == '`' || c == '\'') {
                    if (best == std::string::npos) best = t;
                }
            }
            pos += ml;
        }
    }
    if (best == std::string::npos) return "";
    std::string tok;
    char q = sql[best];
    if (q == '`' || q == '\'') {
        size_t i = best + 1;
        while (i < sql.size() && sql[i] != q) tok += sql[i++];
    } else {
        size_t i = best;
        while (i < sql.size() && (isalnum((unsigned char)sql[i]) || sql[i] == '_' || sql[i] == '.'))
            tok += sql[i++];
    }
    auto dot = tok.find('.');
    if (dot == std::string::npos) return "";   // 无 db 前缀 → 用当前 USE 库
    std::string db = tok.substr(0, dot);
    for (auto& ch : db) ch = (char)tolower((unsigned char)ch);
    return db;
}

EtDBResult EtDBClient::query(const std::string& sql, int dbId) {
    int eff = dbId;
    if (eff <= 0) {
        // 目标库：SQL 的 db.table 前缀优先（跨库查询），否则当前 USE 库。
        std::string db = extractTableDbPrefix(sql);
        if (db.empty()) db = _conn.dbName();
        for (auto& ch : db) ch = (char)tolower((unsigned char)ch);

        int32_t v = 0;
        auto it = _dbIdCache.find(db);
        if (it != _dbIdCache.end()) v = it->second;
        if (v <= 0) {
            // 未缓存或之前解析失败（如建表前库无表）→ 重新解析；成功才缓存。
            v = resolveDBdbId(db);
            if (v > 0) _dbIdCache[db] = v;
        }
        eff = (v > 0) ? (int)v : 1;
    }
    return _conn.query(sql, eff, &_metaCache);
}

int32_t EtDBClient::resolveDBdbId(const std::string& dbName) {
    if (dbName.empty()) return 0;
    // db 名不区分大小写（服务端按小写存储）
    std::string db = dbName;
    for (auto& ch : db) ch = (char)tolower((unsigned char)ch);

    // Build STableMetaReq with empty table name to get DB-level info
    STableMetaReq req;
    memset(&req, 0, sizeof(req));
    int dnl = std::min((int)db.size(), 63);
    req.dbNameLen = htonl(dnl);
    memcpy(req.dbName, db.c_str(), dnl);
    req.needColumns = 0;

    int totalLen = (int)sizeof(STxHead) + (int)sizeof(STableMetaReq);
    std::vector<uint8_t> fullMsg(totalLen, 0);
    STxHead* head = (STxHead*)fullMsg.data();
    head->version = 0x01;
    head->msgType = ETDB::Proto::MSG_CM_TABLE_META;  // CM_TABLE_META=87
    head->msgLen  = htonl(totalLen);
    strncpy(head->user, _conn.userName().c_str(), ETDB_USER_LEN - 1);
    memcpy(fullMsg.data() + sizeof(STxHead), &req, sizeof(req));

    std::vector<uint8_t> rspCont;
    int rspCode = -1;
    uint8_t rspType = 0;
    if (!_conn.sendRecv(fullMsg.data(), totalLen, rspCont, rspCode, rspType))
        return 0;
    if (rspCode != 0 || rspCont.empty()) return 0;

    // Parse STableMetaRsp — extract dbId from first table entry
    constexpr int kEntryFixed = (int)sizeof(STableMetaEntry) - 256;
    if (rspCont.size() < (size_t)(STableMetaRsp::HEADER_SIZE + kEntryFixed + 1)) return 0;
    const STableMetaRsp* rsp = (const STableMetaRsp*)rspCont.data();
    int numTables = ntohl(rsp->numOfTables);
    if (numTables <= 0) return 0;  // no tables → cannot determine db dbId yet

    // Read dbId from the first table entry
    const uint8_t* p = rspCont.data() + STableMetaRsp::HEADER_SIZE;
    for (int i = 0; i < numTables; ++i) {
        const STableMetaEntry* entry = (const STableMetaEntry*)p;
        int32_t dbId = ntohl(entry->dbId);
        int nameLen = entry->nameLen > 0 ? entry->nameLen : 0;
        if (dbId > 0) return dbId;
        p += kEntryFixed + nameLen;
    }
    return 0;
}

// ============================================================================
// EtDBClient Meta Operations
// ============================================================================
TableMeta* EtDBClient::fetchTableMeta(const std::string& dbName,
                                      const std::string& tableName) {
    // Build STableMetaReq
    STableMetaReq req;
    memset(&req, 0, sizeof(req));
    int dnl = std::min((int)dbName.size(), 63);
    req.dbNameLen = htonl(dnl);
    memcpy(req.dbName, dbName.c_str(), dnl);
    int tnl = std::min((int)tableName.size(), 255);
    req.tableNameLen = htonl(tnl);
    memcpy(req.tableName, tableName.c_str(), tnl);
    req.needColumns = 1;

    // Build STxHead + STableMetaReq
    int fullLen = (int)sizeof(STxHead) + (int)sizeof(STableMetaReq);
    std::vector<uint8_t> fullMsg(fullLen, 0);
    STxHead* head = (STxHead*)fullMsg.data();
    head->version = 0x01;
    head->msgType = ETDB::Proto::MSG_CM_TABLE_META;  // CM_TABLE_META
    head->msgLen  = htonl(fullLen);
    strncpy(head->user, _conn.userName().c_str(), ETDB_USER_LEN - 1);
    memcpy(fullMsg.data() + sizeof(STxHead), &req, sizeof(req));

    // Send and receive
    std::vector<uint8_t> rspCont;
    int rspCode = -1;
    uint8_t rspType = 0;
    if (!_conn.sendRecv(fullMsg.data(), fullLen, rspCont, rspCode, rspType))
        return nullptr;
    if (rspCode != 0 || rspCont.empty()) return nullptr;

    // Parse response into cache
    std::vector<TableMeta> metas;
    if (_metaCache.parseMetaRsp(rspCont.data(), (int32_t)rspCont.size(), metas) != 0)
        return nullptr;
    if (metas.empty()) return nullptr;

    // Fix up dbName (not sent in wire protocol, set from request context)
    for (auto& m : metas) {
        m.dbName = dbName;
        _metaCache.putCached(dbName, m.name, m);
    }

    auto* cached = _metaCache.getCached(dbName, tableName);
    return cached ? const_cast<TableMeta*>(cached) : nullptr;
}

TableMeta* EtDBClient::getTableMeta(const std::string& dbName,
                                    const std::string& tableName) {
    auto* cached = _metaCache.getCached(dbName, tableName);
    if (cached) return const_cast<TableMeta*>(cached);
    return fetchTableMeta(dbName, tableName);
}

int EtDBClient::fetchAllTableMeta(const std::string& dbName) {
    TableMeta* meta = fetchTableMeta(dbName, "");
    return meta ? 0 : -1;
}

void EtDBClient::invalidateMeta(const std::string& dbName,
                                const std::string& tableName) {
    _metaCache.invalidate(dbName, tableName);
}

EtDBStmt* EtDBClient::createStmt() {
    return new EtDBStmt(&_conn, &_metaCache);
}

// ============================================================================
// EtDBClient — Async insert result access (taos-style)
// ============================================================================
bool EtDBClient::waitAsyncResult(int64_t batchId, int timeoutMs) {
    return _conn.waitAsyncResult((uint64_t)batchId, timeoutMs);
}

bool EtDBClient::pollAsyncResult(int64_t batchId) const {
    return _conn.pollAsyncResult((uint64_t)batchId);
}

EtDBConnection::AsyncResult EtDBClient::getAsyncResult(int64_t batchId) {
    return _conn.getAsyncResult((uint64_t)batchId);
}

EtDBConnection::AsyncResult EtDBClient::peekAsyncResult(int64_t batchId) const {
    return _conn.peekAsyncResult((uint64_t)batchId);
}

bool EtDBClient::waitAllAsync(int timeoutMs) {
    return _conn.waitAllAsync(timeoutMs);
}

void EtDBClient::setMaxAsyncPending(int n) { _conn.setMaxAsyncPending(n); }

int64_t EtDBClient::asyncSentCount() const { return (int64_t)_conn.asyncSentCount(); }
int64_t EtDBClient::asyncReceivedCount() const { return (int64_t)_conn.asyncReceivedCount(); }
int64_t EtDBClient::asyncPendingCount() const { return (int64_t)_conn.asyncPendingCount(); }

void EtDBClient::close() {
    _conn.close();
}

bool EtDBClient::isConnected() const {
    return _conn.isConnected();
}

} // namespace Client
} // namespace ETDB
