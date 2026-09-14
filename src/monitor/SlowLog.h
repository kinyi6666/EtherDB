// Copyright (c) 2026 Liu jinwei <kinyi6666@gmail.com>
//
// Use of this software is governed by the Business Source License 1.1
// included in the file LICENSE.
//
// Change Date: 2030-10-12
//
// Change License: Apache License, Version 2.0

/*
 * EtherDB Monitor - bounded slow-query log (ring buffer).
 *
 * The slow path (a query exceeding the latency threshold) appends an entry
 * here; the /api/v1/slowlog endpoint reads a snapshot. This is intentionally
 * decoupled from the hot path: normal queries never touch the mutex.
 */

#ifndef ETHERDB_MONITOR_SLOWLOG_H
#define ETHERDB_MONITOR_SLOWLOG_H

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace EtherDB {
namespace Monitor {

struct SlowQueryEntry {
    uint64_t    tsMs        = 0;    // wall-clock time (ms since epoch)
    double      durationSec = 0.0;  // execution time
    int32_t     code        = 0;    // response code
    uint8_t     msgType     = 0;
    uint64_t    rows        = 0;
    std::string user;
    std::string sql;

    SlowQueryEntry() {}
};

class SlowLog {
public:
    static SlowLog& instance() {
        static SlowLog l;
        return l;
    }

    void setCapacity(size_t n) {
        std::lock_guard<std::mutex> lk(_mu);
        if (n == 0) n = 1;
        _buf.assign(n, SlowQueryEntry());
        _cap = n;
        _head = 0;
        _count = 0;
    }

    size_t capacity() const {
        std::lock_guard<std::mutex> lk(_mu);
        return _cap;
    }

    void add(const SlowQueryEntry& e) {
        std::lock_guard<std::mutex> lk(_mu);
        if (_cap == 0) return;
        _buf[_head] = e;
        _head = (_head + 1) % _cap;
        if (_count < _cap) ++_count;
    }

    // Most-recent-first snapshot, at most maxN entries.
    std::vector<SlowQueryEntry> snapshot(size_t maxN) const {
        std::lock_guard<std::mutex> lk(_mu);
        std::vector<SlowQueryEntry> out;
        size_t n = _count < maxN ? _count : maxN;
        out.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            // _head-1 is the newest entry
            size_t idx = (_head + _cap - 1 - i) % _cap;
            out.push_back(_buf[idx]);
        }
        return out;
    }

    size_t size() const {
        std::lock_guard<std::mutex> lk(_mu);
        return _count;
    }

private:
    SlowLog() : _cap(0), _head(0), _count(0) {}
    SlowLog(const SlowLog&);
    SlowLog& operator=(const SlowLog&);

    mutable std::mutex _mu;
    std::vector<SlowQueryEntry> _buf;
    size_t _cap;
    size_t _head;
    size_t _count;
};

}  // namespace Monitor
}  // namespace EtherDB

#endif  // ETHERDB_MONITOR_SLOWLOG_H
