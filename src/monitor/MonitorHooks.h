// Copyright (c) 2026 Liu jinwei <kinyi6666@gmail.com>
//
// Use of this software is governed by the Business Source License 1.1
// included in the file LICENSE.
//
// Change Date: 2030-10-12
//
// Change License: Apache License, Version 2.0

/*
 * EtherDB Monitor - instrumentation helpers used by the request path.
 *
 * All functions here are inline and only perform atomic operations plus (for
 * slow queries) an occasional mutex-protected append. Include this from the
 * gateway transport / dserver workers where requests are observed.
 */

#ifndef ETHERDB_MONITOR_HOOKS_H
#define ETHERDB_MONITOR_HOOKS_H

#include <monitor/Metrics.h>
#include <monitor/SlowLog.h>
#include <client/EtDBClientProtocol.h>

#include <cctype>
#include <chrono>
#include <cstring>
#include <ctime>

namespace EtherDB {
namespace Monitor {

// ---------------------------------------------------------------------------
// Which request messages are counted as "queries".
//   Control messages (connect / heartbeat) are intentionally excluded so QPS
//   reflects real workload.
// ---------------------------------------------------------------------------
inline bool isCountedRequest(uint8_t mt) {
    using namespace ETDB::Proto;
    switch (mt) {
        case MSG_SUBMIT:
        case MSG_QUERY:
        case MSG_FETCH:
        case MSG_CM_CREATE_DB:
        case MSG_CM_DROP_DB:
        case MSG_CM_USE_DB:
        case MSG_CM_CREATE_TABLE:
        case MSG_CM_DROP_TABLE:
        case MSG_CM_ALTER_TABLE:
        case MSG_CM_SHOW:
        case MSG_CM_TABLE_META:
        case MSG_CM_TABLES_META:
        case MSG_CM_CREATE_USER:
        case MSG_CM_DROP_USER:
        case MSG_CM_ALTER_USER:
            return true;
        default:
            return false;
    }
}

// ---------------------------------------------------------------------------
// Best-effort SQL keyword sniff. The payload may start with a small binary
// prefix (e.g. SSimpleMsg::sqlLen), so skip up to a few non-letter bytes
// before reading the leading keyword.
// ---------------------------------------------------------------------------
inline qtype::Type sniffSql(const char* p, int len) {
    if (!p || len <= 0) return qtype::Other;

    int i = 0;
    int skipped = 0;
    while (i < len && skipped < 24) {
        char c = p[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) break;
        ++i;
        ++skipped;
    }

    char kw[12];
    int n = 0;
    while (i < len && n < 11) {
        char c = p[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) break;
        kw[n++] = (char)std::toupper((unsigned char)c);
        ++i;
    }
    kw[n] = '\0';
    if (n == 0) return qtype::Other;

    if (strcmp(kw, "SELECT") == 0) return qtype::Select;
    if (strcmp(kw, "INSERT") == 0) return qtype::Insert;
    if (strcmp(kw, "CREATE") == 0) return qtype::Ddl;
    if (strcmp(kw, "DROP") == 0)   return qtype::Ddl;
    if (strcmp(kw, "ALTER") == 0)  return qtype::Ddl;
    if (strcmp(kw, "TRUNCATE") == 0) return qtype::Ddl;
    if (strcmp(kw, "SHOW") == 0)   return qtype::Show;
    if (strcmp(kw, "USE") == 0)    return qtype::Use;
    if (strcmp(kw, "FETCH") == 0)  return qtype::Fetch;
    return qtype::Other;
}

// ---------------------------------------------------------------------------
// Classify a request message into a db_queries_total{type=...} index.
// `body` may be null (caller has no payload) — classification then falls back
// to the message type alone.
// ---------------------------------------------------------------------------
inline qtype::Type classifyRequest(uint8_t mt, const char* body, int len) {
    using namespace ETDB::Proto;
    switch (mt) {
        case MSG_SUBMIT:  return qtype::Insert;
        case MSG_FETCH:   return qtype::Fetch;
        case MSG_QUERY:   return sniffSql(body, len);
        case MSG_CM_SHOW: return qtype::Show;
        case MSG_CM_USE_DB: return qtype::Use;
        case MSG_CM_TABLE_META:
        case MSG_CM_TABLES_META: return qtype::Meta;
        case MSG_CM_CREATE_DB:
        case MSG_CM_DROP_DB:
        case MSG_CM_CREATE_TABLE:
        case MSG_CM_DROP_TABLE:
        case MSG_CM_ALTER_TABLE:
        case MSG_CM_CREATE_USER:
        case MSG_CM_DROP_USER:
        case MSG_CM_ALTER_USER:
            return qtype::Ddl;
        default:
            return qtype::Other;
    }
}

// ---------------------------------------------------------------------------
// Map a response code to a db_query_errors_total{error_type=...} index.
// Only meaningful when code != 0.
//
// NOTE: the protocol uses -1 as a generic failure code, so it must NOT be
// treated as "auth" here — auth failures are recorded explicitly via
// recordAuthError() by the shell server.
// ---------------------------------------------------------------------------
inline qerr::Type classifyError(int32_t code) {
    if (code == -300) return qerr::NotFound;  // invalid group / dbnode
    if (code == -200) return qerr::Timeout;   // app not ready yet
    if (code == -1)   return qerr::Invalid;   // generic bad/invalid request
    return qerr::Internal;
}

// ---------------------------------------------------------------------------
// Payload pointer for an RPC content block: skips [STxHead][SMsgHead].
// Returns null when there is no payload.
// ---------------------------------------------------------------------------
inline const char* payloadPtr(const void* pCont, int contLen, int& outLen) {
    const int off = (int)(sizeof(ETDB::STxHead) + sizeof(ETDB::SMsgHead));
    if (!pCont || contLen <= off) {
        outLen = 0;
        return 0;
    }
    outLen = contLen - off;
    return static_cast<const char*>(pCont) + off;
}

// ---------------------------------------------------------------------------
// Recording helpers
// ---------------------------------------------------------------------------
inline void recordRequest(uint8_t mt, const char* body, int len) {
    metrics::queries().at(classifyRequest(mt, body, len)).inc();
}

inline void recordError(int32_t code) {
    if (code == 0) return;
    metrics::queryErrors().at(classifyError(code)).inc();
}

// Explicit error classification for paths that know the reason directly
// (e.g. authentication failures, which share the generic -1 code).
inline void recordErrorType(qerr::Type t) {
    metrics::queryErrors().at((size_t)t).inc();
}

inline void recordAuthError() {
    recordErrorType(qerr::Auth);
}

inline void recordDuration(double sec) {
    metrics::queryDuration().observe(sec);
}

inline void recordRows(uint64_t rows) {
    if (rows > 0) metrics::queryRows().observe((double)rows);
}

// Record a completed request: latency histogram, error counter and — when the
// latency exceeds the threshold — the slow-query counter and ring buffer.
inline void recordCompletion(double sec, int32_t code) {
    recordDuration(sec);
    recordError(code);
    if (sec >= slowQueryThresholdSec()) {
        metrics::slowQueries().inc();
    }
}

inline uint64_t nowMs() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

// Append to the slow log (only call when the query is actually slow).
inline void recordSlow(uint64_t tsMs, double sec, int32_t code, uint8_t mt,
                       uint64_t rows, const std::string& user,
                       const std::string& sql) {
    SlowQueryEntry e;
    e.tsMs        = tsMs;
    e.durationSec = sec;
    e.code        = code;
    e.msgType     = mt;
    e.rows        = rows;
    e.user        = user;
    e.sql         = sql;
    SlowLog::instance().add(e);
}

// Build a slow-log entry directly from an RPC content block
// ([STxHead][SMsgHead][payload]). `contLen` includes the STxHead.
// Must be called while the buffer is still alive.
inline void recordSlowFromCont(const void* pCont, int contLen, double sec,
                               int32_t code, uint8_t mt, uint64_t rows) {
    std::string user;
    std::string sql;

    if (pCont && contLen >= (int)sizeof(ETDB::STxHead)) {
        const ETDB::STxHead* head = static_cast<const ETDB::STxHead*>(pCont);
        size_t n = 0;
        while (n < sizeof(head->user) && head->user[n] != '\0') ++n;
        user.assign(head->user, n);
    }

    int plen = 0;
    const char* payload = payloadPtr(pCont, contLen, plen);
    if (payload && plen > 0) {
        // Skip a small binary prefix (SSimpleMsg::sqlLen) so the log shows SQL.
        int i = 0;
        int skipped = 0;
        while (i < plen && skipped < 24) {
            char c = payload[i];
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                (c >= '0' && c <= '9')) {
                break;
            }
            ++i;
            ++skipped;
        }
        int maxLen = plen - i;
        if (maxLen > 512) maxLen = 512;
        sql.assign(payload + i, maxLen > 0 ? maxLen : 0);
        for (size_t k = 0; k < sql.size(); ++k) {
            if (sql[k] == '\n' || sql[k] == '\r') sql[k] = ' ';
        }
    }

    recordSlow(nowMs(), sec, code, mt, rows, user, sql);
}

}  // namespace Monitor
}  // namespace EtherDB

#endif  // ETHERDB_MONITOR_HOOKS_H
