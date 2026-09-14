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
 * EtDBClientProtocol.h — 客户端/服务端共享的 wire 协议定义
 *
 * 这是 client 与 server 共用的唯一协议头：客户端自包含（不依赖服务器
 * 源码），服务端各模块（query/EtDBMessage.h、etdb/ETDBStubs.h、
 * rpc/RPCCommon.h、etdb/ETDBMetaMsg.h、query/QueryAst.h、
 * etdb/ETDBMeta.h 的 ColType、query/ProtDef.h）通过包含本头文件获得
 * 全部 wire 结构/常量/序列化函数，不再各自重复定义，保证二进制兼容。
 *
 * client 目录仍可独立在 Windows/Linux 编译；服务器端通过相对路径
 * #include "../client/EtDBClientProtocol.h" 引用本文件。
 */

#ifndef ETHERDB_CLIENT_PROTOCOL_H
#define ETHERDB_CLIENT_PROTOCOL_H

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "tcpClient.h"   // htonl/ntohl/htobe64/be64toh 等字节序工具

namespace ETDB {

// ============================================================================
// 消息类型常量
// ============================================================================
namespace Proto {
constexpr uint8_t MSG_SUBMIT              = 1;
constexpr uint8_t MSG_SUBMIT_RSP          = 2;
constexpr uint8_t MSG_QUERY               = 3;
constexpr uint8_t MSG_QUERY_RSP           = 4;
constexpr uint8_t MSG_FETCH               = 5;
constexpr uint8_t MSG_FETCH_RSP           = 6;
constexpr uint8_t MSG_CM_CONNECT          = 49;
constexpr uint8_t MSG_CM_CONNECT_RSP      = 50;
constexpr uint8_t MSG_CM_CREATE_DB        = 61;
constexpr uint8_t MSG_CM_CREATE_DB_RSP    = 62;
constexpr uint8_t MSG_CM_DROP_DB          = 65;
constexpr uint8_t MSG_CM_DROP_DB_RSP      = 66;
constexpr uint8_t MSG_CM_USE_DB           = 69;
constexpr uint8_t MSG_CM_USE_DB_RSP       = 70;
constexpr uint8_t MSG_CM_CREATE_TABLE     = 75;
constexpr uint8_t MSG_CM_CREATE_TABLE_RSP = 76;
constexpr uint8_t MSG_CM_DROP_TABLE       = 77;
constexpr uint8_t MSG_CM_DROP_TABLE_RSP   = 78;
constexpr uint8_t MSG_CM_ALTER_TABLE      = 79;
constexpr uint8_t MSG_CM_ALTER_TABLE_RSP  = 80;
constexpr uint8_t MSG_CM_TABLE_META       = 87;
constexpr uint8_t MSG_CM_TABLE_META_RSP   = 88;
constexpr uint8_t MSG_CM_SHOW             = 91;
constexpr uint8_t MSG_CM_SHOW_RSP         = 92;
constexpr uint8_t MSG_CM_TABLES_META      = 93;
constexpr uint8_t MSG_CM_TABLES_META_RSP  = 94;
constexpr uint8_t MSG_CM_CREATE_USER      = 95;
constexpr uint8_t MSG_CM_CREATE_USER_RSP  = 96;
constexpr uint8_t MSG_CM_DROP_USER        = 97;
constexpr uint8_t MSG_CM_DROP_USER_RSP    = 98;
constexpr uint8_t MSG_CM_ALTER_USER       = 99;
constexpr uint8_t MSG_CM_ALTER_USER_RSP   = 100;
constexpr uint8_t MSG_CM_HEARTBEAT        = 109;
constexpr uint8_t MSG_CM_HEARTBEAT_RSP    = 110;

constexpr uint8_t MSG_MAX                 = 200;

inline uint8_t rspType(uint8_t req) { return (uint8_t)(req + 1); }
inline bool    isReq(uint8_t t)     { return (t & 1) != 0; }
} // namespace Proto

constexpr int ETDB_USER_LEN = 32;

// ============================================================================
// 列类型
// ============================================================================
enum class ColType : int8_t {
    TIMESTAMP = 0,   // 8 bytes
    BOOL      = 1,   // 1 byte
    TINYINT   = 2,   // 1 byte
    SMALLINT  = 3,   // 2 bytes
    INT       = 4,   // 4 bytes
    BIGINT    = 5,   // 8 bytes
    FLOAT     = 6,   // 4 bytes
    DOUBLE    = 7,   // 8 bytes
    BINARY    = 8,   // fixed-size slot (bytes holds declared size)
    NCHAR     = 9,   // fixed-size slot (bytes holds declared size)
    UTINYINT  = 10,  // 1 byte
    USMALLINT = 11,  // 2 bytes
    UINT      = 12,  // 4 bytes
    UBIGINT   = 13,  // 8 bytes
};

// ============================================================================
// 表类型
// ============================================================================
enum class TableType : uint8_t {
    NORMAL      = 0,
    SUPER_TABLE = 1,
    CHILD_TABLE = 2,
    STREAM      = 3,
};

// ============================================================================
// 值类型
// ============================================================================
namespace Query {

enum class ValType : uint8_t {
    INT    = 0,
    FLOAT  = 1,
    STRING = 2,
    BOOL   = 3,
    NULLVAL = 4,
    UINT64 = 5,   // full unsigned 64-bit
};

struct Value {
    ValType type = ValType::NULLVAL;
    int64_t  iVal = 0;
    uint64_t uVal = 0;   // UINT64
    double   fVal = 0.0;
    std::string sVal;
    bool     bVal = false;

    Value() = default;
    explicit Value(int64_t v)           : type(ValType::INT), iVal(v) {}
    explicit Value(double v)            : type(ValType::FLOAT), fVal(v) {}
    explicit Value(const std::string& v): type(ValType::STRING), sVal(v) {}
    explicit Value(bool v)              : type(ValType::BOOL), bVal(v) {}

    static Value fromUInt64(uint64_t v) {
        Value r;
        r.type = ValType::UINT64;
        r.uVal = v;
        return r;
    }

    std::string toString() const {
        switch (type) {
            case ValType::INT:    return std::to_string(iVal);
            case ValType::UINT64: return std::to_string(uVal);
            case ValType::FLOAT:  return std::to_string(fVal);
            case ValType::STRING: return "'" + sVal + "'";
            case ValType::BOOL:   return bVal ? "true" : "false";
            case ValType::NULLVAL: return "NULL";
        }
        return "?";
    }

    bool isNull() const { return type == ValType::NULLVAL; }

    // True for numeric value types (comparable across INT/UINT64/FLOAT/BOOL)
    static bool isNumeric(ValType t) {
        return t == ValType::INT || t == ValType::UINT64
            || t == ValType::FLOAT || t == ValType::BOOL;
    }

    // Cross-type numeric comparison. Handles UINT64 vs signed-int correctly
    // (a uint64 > INT64_MAX compares greater than any positive int64).
    static int cmpNumeric(const Value& a, const Value& b) {
        bool aU = (a.type == ValType::UINT64);
        bool bU = (b.type == ValType::UINT64);

        // Any float involved → compare via long double
        if (a.type == ValType::FLOAT || b.type == ValType::FLOAT) {
            long double da = aU ? (long double)a.uVal
                : (a.type == ValType::FLOAT) ? (long double)a.fVal
                : (a.type == ValType::BOOL)  ? (a.bVal ? 1.0L : 0.0L)
                : (long double)a.iVal;
            long double db = bU ? (long double)b.uVal
                : (b.type == ValType::FLOAT) ? (long double)b.fVal
                : (b.type == ValType::BOOL)  ? (b.bVal ? 1.0L : 0.0L)
                : (long double)b.iVal;
            return da < db ? -1 : (da > db ? 1 : 0);
        }

        int64_t ai = (a.type == ValType::BOOL) ? (a.bVal ? 1 : 0) : a.iVal;
        int64_t bi = (b.type == ValType::BOOL) ? (b.bVal ? 1 : 0) : b.iVal;

        if (aU && bU) return a.uVal < b.uVal ? -1 : (a.uVal > b.uVal ? 1 : 0);
        if (aU) {  // uint64 vs signed-int
            if (bi < 0) return 1;
            uint64_t ub = (uint64_t)bi;
            return a.uVal < ub ? -1 : (a.uVal > ub ? 1 : 0);
        }
        if (bU) {  // signed-int vs uint64
            if (ai < 0) return -1;
            uint64_t ua = (uint64_t)ai;
            return ua < b.uVal ? -1 : (ua > b.uVal ? 1 : 0);
        }
        return ai < bi ? -1 : (ai > bi ? 1 : 0);
    }

    // Compare two values
    int compare(const Value& other) const {
        if (isNumeric(type) && isNumeric(other.type))
            return cmpNumeric(*this, other);
        if (type != other.type) return (int)type - (int)other.type;
        switch (type) {
            case ValType::INT:    return iVal < other.iVal ? -1 : (iVal > other.iVal ? 1 : 0);
            case ValType::UINT64: return uVal < other.uVal ? -1 : (uVal > other.uVal ? 1 : 0);
            case ValType::FLOAT:  return fVal < other.fVal ? -1 : (fVal > other.fVal ? 1 : 0);
            case ValType::STRING: return sVal.compare(other.sVal);
            case ValType::BOOL:   return bVal == other.bVal ? 0 : (bVal ? 1 : -1);
            case ValType::NULLVAL: return 0;
        }
        return 0;
    }

    bool operator==(const Value& other) const { return compare(other) == 0; }
    bool operator!=(const Value& other) const { return compare(other) != 0; }
    bool operator<(const Value& other)  const { return compare(other) < 0; }
    bool operator<=(const Value& other) const { return compare(other) <= 0; }
    bool operator>(const Value& other)  const { return compare(other) > 0; }
    bool operator>=(const Value& other) const { return compare(other) >= 0; }
};

} // namespace Query

// ============================================================================
// Wire 结构
// ============================================================================
#pragma pack(push, 1)

// Transaction 消息头
struct STxHead {
    uint8_t  version;
    uint8_t  msgType;
    uint8_t  msgVer;
    int32_t  msgLen;
    int32_t  rtCode;
    uint8_t  security;
    uint64_t batchId;
    char     user[ETDB_USER_LEN];  
    uint8_t  content[0];
    uint8_t  getVersion()  const { return version & 0x0F; }
    uint8_t  getComp()     const { return (version >> 4) & 0x0F; }
    uint8_t  getSpi() const { return security >> 5; } // 
    uint8_t  getEncrypt() const { return security >> 2 & 7; } // 
    void     setVersion(uint8_t v) { version = (version & 0xF0) | (v & 0x0F); }
    void     setComp(uint8_t c)    { version = (version & 0x0F) | ((c & 0x0F) << 4); }
    bool     isRequest()   const { return msgType & 1; }
    bool     isResponse()  const { return !(msgType & 1); }
};

// 路由头
struct SMsgExtInfo { int32_t ipAddr; uint16_t port; };
struct SMsgHead { int32_t dbId; int32_t contLen; };

// SQL 请求
struct SSimpleMsg { int32_t sqlLen; char sql[0]; };
typedef SSimpleMsg SQueryReq;

// 查询响应
struct SQueryRsp {
    int32_t numCols;
    int32_t numRows;
    int32_t dataLen;
    char    data[0];
};

// 流式元数据（查询响应 data[] 前缀）
constexpr uint32_t ETDB_STREAM_MAGIC = 0x5354524D;  // "STRM"
constexpr int64_t  ETDB_STREAM_BATCH_BYTES = 1024 * 1024 * 2;  // 每批字节预算

struct SStreamMeta {
    uint32_t magic;     // ETDB_STREAM_MAGIC
    int64_t  qId;       // streaming query id (0 = one-shot)
    int64_t  totalRows; // total matching rows
    uint8_t  isEnd;     // 1 = final batch
    uint8_t  pad[3];
};

// FETCH 请求
struct SFetchReq {
    int32_t qId;        // streaming query id
    int32_t batchSize;  // requested batch size in BYTES
};

// INSERT 提交（对应服务器 etdb/ETDBStubs.h，网络字节序字段）
struct SSubmitReq {
    int32_t  length;      // total msg length incl header (network byte order)
    int16_t  numOfBlocks; // number of SDataBlock following
    uint8_t  padding[2];
    char     data[1];     // dummy for offset calc
};

struct SDataBlock {
    int32_t  sversion;    // schema version
    int32_t  suid;        // schema uid (super table uid)
    uint64_t uid;         // table uid
    int32_t  dataLen;     // total row data length
    int32_t  schemaLen;   // schema length (unused)
    int16_t  numOfRows;   // number of rows in this block
    uint8_t  pad2[2];     // alignment
    char     data[1];     // row data follows
};

struct SSubmitErrEntry {
    int32_t  rowIndex;    // 0-based row index in the batch
    int32_t  errorCode;   // error code
};

struct SSubmitRspMsg {
    int32_t  numOfRows;      // total rows submitted
    int32_t  affectedRows;   // successfully inserted rows
    int32_t  errorRows;      // number of failed rows
    char     errorData[1];   // flexible array marker (errorRows * SSubmitErrEntry)
};

// 列元数据
struct SColumnMeta {
    int16_t  colId;
    uint8_t  type;          // ColType
    int16_t  bytes;
    int16_t  nameLen;
    char     name[64];

    SColumnMeta() { memset(this, 0, sizeof(*this)); }
};

// 表元数据请求
struct STableMetaReq {
    int32_t  dbNameLen;
    char     dbName[64];
    int32_t  tableNameLen;
    char     tableName[256];
    int8_t   needColumns;

    STableMetaReq() { memset(this, 0, sizeof(*this)); }

    // Serialize for wire
    void hton() {
        dbNameLen    = htonl(dbNameLen);
        tableNameLen = htonl(tableNameLen);
    }
    void ntoh() {
        dbNameLen    = ntohl(dbNameLen);
        tableNameLen = ntohl(tableNameLen);
    }
};

// 表元数据响应
struct STableMetaRsp {
    int32_t  numOfTables;
    int32_t  totalLen;

    STableMetaRsp() { memset(this, 0, sizeof(*this)); }

    void hton() {
        numOfTables = htonl(numOfTables);
        totalLen    = htonl(totalLen);
    }
    void ntoh() {
        numOfTables = ntohl(numOfTables);
        totalLen    = ntohl(totalLen);
    }

    static constexpr int HEADER_SIZE = 8;
};

// 单表元数据条目（变长：固定字段 + name + columns）
struct STableMetaEntry {
    uint64_t uid;
    int32_t  tid;
    int32_t  dbId;
    int32_t  sversion;
    int32_t  tversion;
    int32_t  numOfCols;
    int32_t  numOfTags;
    uint8_t  tableType;
    uint8_t  precision;
    int32_t  nameLen;
    char     name[256];

    STableMetaEntry() { memset(this, 0, sizeof(*this)); }

    void hton() {
        uid         = htobe64(uid);
        tid         = htonl(tid);
        dbId        = htonl(dbId);
        sversion    = htonl(sversion);
        tversion    = htonl(tversion);
        numOfCols   = htonl(numOfCols);
        numOfTags   = htonl(numOfTags);
        nameLen     = htonl(nameLen);
    }
    void ntoh() {
        uid         = be64toh(uid);
        tid         = ntohl(tid);
        dbId        = ntohl(dbId);
        sversion    = ntohl(sversion);
        tversion    = ntohl(tversion);
        numOfCols   = ntohl(numOfCols);
        numOfTags   = ntohl(numOfTags);
        nameLen     = ntohl(nameLen);
    }

    // Calculate total wire size of this entry (including columns)
    int wireSize() const {
        int colMetaSize = (int)sizeof(SColumnMeta);
        return (int)sizeof(STableMetaEntry) - 256 + nameLen + 1
               + (numOfCols + numOfTags) * colMetaSize;
    }
};

#pragma pack(pop)

// ============================================================================
// 值序列化
// ============================================================================
constexpr uint8_t VAL_NULL   = 0;
constexpr uint8_t VAL_INT    = 1;
constexpr uint8_t VAL_FLOAT  = 2;
constexpr uint8_t VAL_STRING = 3;
constexpr uint8_t VAL_BOOL   = 4;
constexpr uint8_t VAL_UINT64 = 5;

inline int etdbDeserializeValue(const char* buf, Query::Value& v) {
    uint8_t tp = (uint8_t)*buf++;
    int32_t vl; memcpy(&vl, buf, 4); vl = ntohl(vl); buf += 4;
    switch (tp) {
        case VAL_INT:    { int64_t x; memcpy(&x, buf, 8); v = Query::Value((int64_t)be64toh(x)); break; }
        case VAL_UINT64: { uint64_t x; memcpy(&x, buf, 8); v = Query::Value::fromUInt64((uint64_t)be64toh(x)); break; }
        case VAL_FLOAT:  { uint64_t x; memcpy(&x, buf, 8); x = be64toh(x); double d; memcpy(&d, &x, 8); v = Query::Value(d); break; }
        case VAL_STRING: v = Query::Value(std::string(buf, (size_t)vl)); break;
        case VAL_BOOL:   v = Query::Value(buf[0] != 0); break;
        default:         v = Query::Value(); break;
    }
    return 5 + vl;
}

// ============================================================================
// 消息构建辅助（与服务器 query/EtDBMessage.h 一致）
// ============================================================================

// 用 SMsgHead 包装 payload
inline uint8_t* etdbWrap(uint8_t* buf, int bufSize, int dbId, int payloadLen) {
    int totalNeeded = (int)(sizeof(SMsgHead)) + payloadLen;
    if (totalNeeded > bufSize) return nullptr;
    SMsgHead* h = (SMsgHead*)(buf);
    h->dbId    = htonl(dbId);
    h->contLen = htonl(payloadLen);
    return buf + sizeof(SMsgHead);
}

// 完整消息：STxHead + SMsgHead + payload
// 注意：payload 需已写入 buf 中 (STxHead+SMsgHead) 之后的位置。
inline int etdbBuildFullMsg(uint8_t* buf, int bufSize, int dbId,
                            const uint8_t* payload, int payloadLen, uint8_t msgType,
                            const char* user = "root") {
    int headerLen = sizeof(SMsgHead);
    int tl = (int)sizeof(STxHead) + headerLen;
    int msgLen = tl + payloadLen;
    if (msgLen > bufSize) return -1;
    etdbWrap(buf + sizeof(STxHead), bufSize - (int)sizeof(STxHead), dbId, payloadLen);
    STxHead* r = (STxHead*)buf;
    r->version = 0x01;
    r->msgType = msgType;
    r->msgLen  = htonl(msgLen);
    strncpy(r->user, user ? user : "root", ETDB_USER_LEN - 1);
    return msgLen;
}

// SQL 文本消息（SELECT / DDL / SHOW — 服务器解析 SQL）
inline int etdbBuildSqlMsg(uint8_t* buf, int bufSize, int dbId,
                           const std::string& sql, uint8_t msgType,
                           const char* user = "root") {
    int sl = (int)sql.size();
    int pl = (int)sizeof(SSimpleMsg) + sl;
    SSimpleMsg* s = (SSimpleMsg*)(buf + sizeof(STxHead) + sizeof(SMsgHead));
    s->sqlLen = htonl(sl);
    memcpy(s->sql, sql.data(), sl);

    int msgLen = sizeof(SMsgHead) + (int)sizeof(STxHead) + pl;
    if (msgLen > bufSize) return -1;
    etdbWrap(buf + sizeof(STxHead), bufSize - (int)sizeof(STxHead), dbId, pl);

    STxHead* r = (STxHead*)buf;
    r->version = 0x01;
    r->msgType = msgType;
    r->msgLen  = htonl(msgLen);
    strncpy(r->user, user ? user : "root", ETDB_USER_LEN - 1);
    return msgLen;
}

} // namespace ETDB

#endif // ETHERDB_CLIENT_PROTOCOL_H
