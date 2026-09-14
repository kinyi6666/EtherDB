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
 * EtDBMeta.h — Client-Side Table Meta Cache
 *
 * Provides a local cache for table metadata fetched from the server.
 * The cache is used by EtDBStmt to properly pack INSERT data with
 * correct uid, tid, dbId, and column type information.
 *
 * Flow:
 *   1. Client connects → optionally pre-fetches all table meta
 *   2. EtDBStmt::prepare() → checks cache → fetches if missing
 *   3. INSERT data packing uses meta: uid, tid, column types
 *   4. Cache can be invalidated/refreshed on schema version change
 *
 * Usage:
 *   EtDBMetaCache cache(conn);
 *   auto* meta = cache.getTableMeta("test", "sensor");
 *   if (meta) {
 *       printf("uid=%lu tid=%d dbId=%d\n", meta->uid, meta->tid, meta->dbId);
 *       for (auto& col : meta->columns)
 *           printf("  col[%d] %s type=%d\n", col.colId, col.name.c_str(), (int)col.type);
 *   }
 */

#ifndef ETHERDB_CLIENT_META_H
#define ETHERDB_CLIENT_META_H

#include "EtDBClientProtocol.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <cstring>

namespace ETDB {
namespace Client {

// ============================================================================
// In-memory table meta (client-side representation)
// ============================================================================
struct TableMeta {
    int32_t  dbId      = 0;
    uint64_t uid       = 0;
    int32_t  tid       = 0;
    int32_t  sversion  = 0;
    int32_t  tversion  = 0;
    TableType tableType = TableType::NORMAL;
    uint8_t  precision = 0;
    std::string name;
    std::string dbName;

    struct ColumnInfo {
        int16_t  colId;
        ColType  type;
        int16_t  bytes;
        std::string name;
    };
    std::vector<ColumnInfo> columns;
    std::vector<ColumnInfo> tags;
    int totalCols() const { return (int)columns.size() + (int)tags.size(); }

    // Look up column by name (case-insensitive in practice)
    const ColumnInfo* findColumn(const std::string& colName) const {
        for (auto& c : columns) if (c.name == colName) return &c;
        for (auto& t : tags)    if (t.name == colName) return &t;
        return nullptr;
    }

    // Build key for cache
    static std::string makeKey(const std::string& db, const std::string& table) {
        return db + "." + table;
    }
};

// ============================================================================
// EtDBMetaCache — Thread-safe client-side meta cache
// ============================================================================
class EtDBMetaCache {
public:
    explicit EtDBMetaCache(class EtDBConnection* conn) : _conn(conn) {}

    // Fetch table meta from server (sends CM_TABLE_META, parses response)
    // Returns nullptr on failure
    TableMeta* fetchTableMeta(const std::string& dbName, const std::string& tableName);

    // Get cached meta, or fetch if not present
    TableMeta* getTableMeta(const std::string& dbName, const std::string& tableName);

    // Pre-fetch all tables in a database
    int fetchAllTables(const std::string& dbName);

    // Invalidate cache for a specific table
    void invalidate(const std::string& dbName, const std::string& tableName);

    // Clear entire cache
    void clear();

    // Check if a table is cached (returns pointer to cached meta, or nullptr)
    const TableMeta* getCached(const std::string& dbName, const std::string& tableName) const;
    bool isCached(const std::string& dbName, const std::string& tableName) const {
        return getCached(dbName, tableName) != nullptr;
    }

    // Insert/update a table meta into the cache
    void putCached(const std::string& dbName, const std::string& tableName, const TableMeta& meta);

    // Parse STableMetaRsp binary into TableMeta objects
    int parseMetaRsp(const uint8_t* buf, int32_t bufLen,
                     std::vector<TableMeta>& out);

private:
    EtDBConnection* _conn;
    std::unordered_map<std::string, TableMeta> _cache;
    mutable std::mutex _mutex;
};

// ============================================================================
// Implementation
// ============================================================================

// fetchTableMeta 实现位于 EtDBClient.cpp（需要 EtDBConnection::sendRecv，
// 即完整的 EtDBConnection 定义）；此处仅声明。

inline TableMeta* EtDBMetaCache::getTableMeta(const std::string& dbName,
                                                const std::string& tableName) {
    std::string key = TableMeta::makeKey(dbName, tableName);
    {
        std::lock_guard<std::mutex> lk(_mutex);
        auto it = _cache.find(key);
        if (it != _cache.end()) return &it->second;
    }
    return fetchTableMeta(dbName, tableName);
}

inline int EtDBMetaCache::fetchAllTables(const std::string& dbName) {
    // Fetch all tables by requesting with empty table name
    TableMeta* meta = fetchTableMeta(dbName, "");
    return meta ? 0 : -1;
}

inline void EtDBMetaCache::invalidate(const std::string& dbName,
                                       const std::string& tableName) {
    std::lock_guard<std::mutex> lk(_mutex);
    _cache.erase(TableMeta::makeKey(dbName, tableName));
}

inline void EtDBMetaCache::clear() {
    std::lock_guard<std::mutex> lk(_mutex);
    _cache.clear();
}

inline const TableMeta* EtDBMetaCache::getCached(const std::string& dbName,
                                                   const std::string& tableName) const {
    std::lock_guard<std::mutex> lk(_mutex);
    auto it = _cache.find(TableMeta::makeKey(dbName, tableName));
    return (it != _cache.end()) ? &it->second : nullptr;
}

inline void EtDBMetaCache::putCached(const std::string& dbName,
                                      const std::string& tableName,
                                      const TableMeta& meta) {
    std::lock_guard<std::mutex> lk(_mutex);
    _cache[TableMeta::makeKey(dbName, tableName)] = meta;
}

inline int EtDBMetaCache::parseMetaRsp(const uint8_t* buf, int32_t bufLen,
                                        std::vector<TableMeta>& out) {
    if (!buf || bufLen < STableMetaRsp::HEADER_SIZE) return -1;

    const STableMetaRsp* rsp = (const STableMetaRsp*)buf;
    int32_t numTables = ntohl(rsp->numOfTables);
    if (numTables <= 0 || numTables > 1024) return -1;

    const char* cur = (const char*)buf + STableMetaRsp::HEADER_SIZE;

    for (int32_t i = 0; i < numTables; ++i) {
        // Check minimum room: fixed fields (without name[256]) + 1 byte name
        constexpr int kEntryFixed = (int)sizeof(STableMetaEntry) - 256;
        if (cur + (ptrdiff_t)kEntryFixed + 1 > (const char*)buf + bufLen) break;

        const STableMetaEntry* entry = (const STableMetaEntry*)cur;
        TableMeta meta;
        meta.uid       = be64toh(entry->uid);
        meta.tid       = ntohl(entry->tid);
        meta.dbId      = ntohl(entry->dbId);
        meta.sversion  = ntohl(entry->sversion);
        meta.tversion  = ntohl(entry->tversion);
        meta.tableType = (TableType)entry->tableType;
        meta.precision = entry->precision;

        int32_t nameLen = ntohl(entry->nameLen);
        if (nameLen > 0 && nameLen <= 255)
            meta.name.assign(entry->name, (size_t)nameLen);

        int32_t numCols = ntohl(entry->numOfCols);
        int32_t numTags = ntohl(entry->numOfTags);

        // Advance to columns array
        cur += sizeof(STableMetaEntry) - 256 + nameLen + 1;
        cur = (const char*)((((uintptr_t)cur + 7) / 8) * 8);  // align

        const SColumnMeta* cols = (const SColumnMeta*)cur;
        for (int32_t ci = 0; ci < numCols + numTags; ++ci) {
            TableMeta::ColumnInfo ci2;
            ci2.colId = ntohs(cols[ci].colId);
            ci2.type  = (ColType)cols[ci].type;
            ci2.bytes = ntohs(cols[ci].bytes);
            int16_t cnl = ntohs(cols[ci].nameLen);
            if (cnl > 0 && cnl <= 63)
                ci2.name.assign(cols[ci].name, (size_t)cnl);

            if (ci < numCols)
                meta.columns.push_back(ci2);
            else
                meta.tags.push_back(ci2);
        }
        cur += (numCols + numTags) * (int)sizeof(SColumnMeta);

        // Insert into cache
        {
            std::lock_guard<std::mutex> lk(_mutex);
            _cache[TableMeta::makeKey(meta.dbName, meta.name)] = meta;
        }
        out.push_back(std::move(meta));
    }

    return 0;
}

} // namespace Client
} // namespace ETDB

#endif // ETHERDB_CLIENT_META_H
