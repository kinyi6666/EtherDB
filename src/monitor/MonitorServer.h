// Copyright (c) 2026 Liu jinwei <kinyi6666@gmail.com>
//
// Use of this software is governed by the Business Source License 1.1
// included in the file LICENSE.
//
// Change Date: 2030-10-12
//
// Change License: Apache License, Version 2.0

/*
 * EtherDB Monitor - HTTP monitoring service and background collector.
 *
 * The monitor runs on its own port (default 9187) so it never competes with the
 * client protocol port for connections or rate limits.
 *
 * Endpoints (phase 1):
 *   GET /metrics                Prometheus text exposition
 *   GET /api/v1/status          JSON runtime summary (version/uptime/qps/...)
 *   GET /api/v1/health          {"status":"ok"}
 *   GET /api/v1/metrics/list    JSON array of metric names
 *   GET /api/v1/slowlog         JSON array of recent slow queries
 *
 * A dedicated query message for reading the same snapshot through the client
 * API is intentionally NOT implemented yet — the HTTP endpoints above are the
 * only monitoring channel for now.
 */

#ifndef ETHERDB_MONITOR_SERVER_H
#define ETHERDB_MONITOR_SERVER_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

namespace EtherDB {
namespace Monitor {

struct MonitorConfig {
    bool        enabled         = true;
    uint16_t    port            = 9187;
    int         threads         = 2;
    int         slowThresholdMs = 100;      // query slower than this is "slow"
    int         collectIntervalMs = 5000;   // system metric sampling period
    size_t      slowLogCapacity = 128;      // ring buffer size
    uint32_t    maxConnectionsHint = 0;     // reported as db_connections_max
    std::string dataDir;                    // volume measured for disk usage
    std::string version;                    // reported by /api/v1/status
};

class MonitorHttpServer;  // defined in MonitorServer.cpp

class MonitorAgent {
public:
    static MonitorAgent& instance();

    // Idempotent: safe to call more than once.
    bool start(const MonitorConfig& cfg);
    void stop();

    bool running() const { return _running.load(); }
    const MonitorConfig& config() const { return _cfg; }

    // Rolling QPS sample (updated by the collector thread).
    double currentQps() const;

private:
    MonitorAgent();
    ~MonitorAgent();
    MonitorAgent(const MonitorAgent&);
    MonitorAgent& operator=(const MonitorAgent&);

    void collectLoop();

    MonitorConfig                _cfg;
    std::atomic<bool>            _running;
    std::unique_ptr<MonitorHttpServer> _http;
    std::thread                  _collector;
};

}  // namespace Monitor
}  // namespace EtherDB

#endif  // ETHERDB_MONITOR_SERVER_H
