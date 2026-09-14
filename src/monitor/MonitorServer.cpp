// Copyright (c) 2026 Liu jinwei <kinyi6666@gmail.com>
//
// Use of this software is governed by the Business Source License 1.1
// included in the file LICENSE.
//
// Change Date: 2030-10-12
//
// Change License: Apache License, Version 2.0

/*
 * EtherDB Monitor - HTTP service and background collector implementation.
 *
 * Reuses the net/http module (HttpServer + HttpServlet) for the control
 * channel, and a dedicated std::thread for system metric sampling.
 */

#include "MonitorServer.h"

#include "Metrics.h"
#include "MetricsSystem.h"
#include "SlowLog.h"

#include <net/EventLoop.h>
#include <net/EventLoopThread.h>
#include <net/InetAddress.h>
#include <net/http/HttpRequest.h>
#include <net/http/HttpResponse.h>
#include <net/http/HttpServer.h>
#include <net/http/HttpServlet.h>

#include <base/CountDownLatch.h>
#include <base/Logging.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

namespace EtherDB {
namespace Monitor {

// ===========================================================================
// Small JSON helpers
// ===========================================================================
namespace {

std::string jsonEscape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if ((unsigned char)c < 0x20) {
                    char b[8];
                    snprintf(b, sizeof(b), "\\u%04x", (unsigned char)c);
                    o += b;
                } else {
                    o += c;
                }
        }
    }
    return o;
}

std::string fmtNum(double v) {
    char b[64];
    if (v == (double)(long long)v) {
        snprintf(b, sizeof(b), "%lld", (long long)v);
    } else {
        snprintf(b, sizeof(b), "%.6g", v);
    }
    return std::string(b);
}

std::string runHttpVersion() {
    // Keep the HTTP control channel version-independent; the DB version is
    // reported through /api/v1/status.
    return "0.1.0";
}

}  // namespace

// ===========================================================================
// Servlets
// ===========================================================================
namespace {

class MetricsServlet : public Net::HttpServlet {
public:
    void get(const Net::HttpRequestPtr&, const Net::HttpResponsePtr& response) {
        response->setStatus(200);
        response->setContentType("text/plain; version=0.0.4; charset=utf-8");
        response->setHeader("Cache-Control", "no-cache");
        response->setBody(Registry::instance().renderPrometheus());
        response->flush();
    }
};

class HealthServlet : public Net::HttpServlet {
public:
    void get(const Net::HttpRequestPtr&, const Net::HttpResponsePtr& response) {
        response->setStatus(200);
        response->setContentType("application/json; charset=utf-8");
        response->setBody("{\"status\":\"ok\"}");
        response->flush();
    }
};

class MetricsListServlet : public Net::HttpServlet {
public:
    void get(const Net::HttpRequestPtr&, const Net::HttpResponsePtr& response) {
        response->setStatus(200);
        response->setContentType("application/json; charset=utf-8");
        response->setBody(Registry::instance().renderMetricNamesJson());
        response->flush();
    }
};

class StatusServlet : public Net::HttpServlet {
public:
    void get(const Net::HttpRequestPtr&, const Net::HttpResponsePtr& response) {
        Registry& reg = Registry::instance();
        const MonitorConfig& cfg = MonitorAgent::instance().config();

        uint64_t queriesTotal = Registry::familySum(metrics::queries());
        uint64_t errorsTotal  = Registry::familySum(metrics::queryErrors());

        std::ostringstream os;
        os << "{";
        os << "\"version\":\"" << jsonEscape(cfg.version.empty() ? runHttpVersion()
                                                                 : cfg.version) << "\",";
        os << "\"status\":\"" << (MonitorAgent::instance().running() ? "running"
                                                                     : "stopped") << "\",";
        os << "\"uptime_seconds\":" << fmtNum(reg.uptimeSeconds()) << ",";
        os << "\"connections\":" << fmtNum(metrics::connectionsCurrent().value()) << ",";
        os << "\"connections_max\":" << fmtNum(metrics::connectionsMax().value()) << ",";
        os << "\"connections_total\":" << metrics::connectionsTotal().value() << ",";
        os << "\"connections_rejected_total\":" << metrics::connectionsRejected().value() << ",";
        os << "\"qps\":" << fmtNum(metrics::qps().value()) << ",";
        os << "\"queries_total\":" << queriesTotal << ",";
        os << "\"errors_total\":" << errorsTotal << ",";
        os << "\"slow_queries_total\":" << metrics::slowQueries().value() << ",";
        os << "\"slow_queries_recent\":" << SlowLog::instance().size() << ",";
        os << "\"cpu_usage_ratio\":" << fmtNum(metrics::cpuUsage().value()) << ",";
        os << "\"memory_used_bytes\":" << fmtNum(metrics::memoryUsed().value()) << ",";
        os << "\"memory_total_bytes\":" << fmtNum(metrics::memoryTotal().value()) << ",";
        os << "\"disk_used_bytes\":" << fmtNum(metrics::diskUsed().value()) << ",";
        os << "\"disk_total_bytes\":" << fmtNum(metrics::diskTotal().value()) << ",";
        os << "\"disk_used_ratio\":" << fmtNum(metrics::diskUsedRatio().value());
        os << "}";

        response->setStatus(200);
        response->setContentType("application/json; charset=utf-8");
        response->setBody(os.str());
        response->flush();
    }
};

class SlowlogServlet : public Net::HttpServlet {
public:
    void get(const Net::HttpRequestPtr& request, const Net::HttpResponsePtr& response) {
        size_t n = SlowLog::instance().capacity();
        std::string nStr = request->getParam("n");
        if (!nStr.empty()) {
            int v = atoi(nStr.c_str());
            if (v > 0) n = (size_t)v;
        }

        std::vector<SlowQueryEntry> entries = SlowLog::instance().snapshot(n);
        std::ostringstream os;
        os << "[";
        for (size_t i = 0; i < entries.size(); ++i) {
            const SlowQueryEntry& e = entries[i];
            if (i) os << ",";
            os << "{";
            os << "\"ts_ms\":" << e.tsMs << ",";
            os << "\"duration_seconds\":" << fmtNum(e.durationSec) << ",";
            os << "\"code\":" << e.code << ",";
            os << "\"msg_type\":" << (int)e.msgType << ",";
            os << "\"rows\":" << e.rows << ",";
            os << "\"user\":\"" << jsonEscape(e.user) << "\",";
            os << "\"sql\":\"" << jsonEscape(e.sql) << "\"";
            os << "}";
        }
        os << "]";

        response->setStatus(200);
        response->setContentType("application/json; charset=utf-8");
        response->setBody(os.str());
        response->flush();
    }
};

Net::HttpServletPtr createMetricsServlet() { return Net::HttpServletPtr(new MetricsServlet()); }
Net::HttpServletPtr createStatusServlet()  { return Net::HttpServletPtr(new StatusServlet()); }
Net::HttpServletPtr createHealthServlet()  { return Net::HttpServletPtr(new HealthServlet()); }
Net::HttpServletPtr createMetricsListServlet() { return Net::HttpServletPtr(new MetricsListServlet()); }
Net::HttpServletPtr createSlowlogServlet() { return Net::HttpServletPtr(new SlowlogServlet()); }

}  // namespace

// ===========================================================================
// MonitorHttpServer - owns the loop thread + net::HttpServer
// ===========================================================================
class MonitorHttpServer {
public:
    MonitorHttpServer() : _port(0), _loop(0) {}
    ~MonitorHttpServer() { stop(); }

    bool start(const MonitorConfig& cfg) {
        _port = cfg.port;

        _loopThread.reset(new Net::EventLoopThread(
            Net::EventLoopThread::ThreadInitCallback(), "monitor_http"));
        _loop = _loopThread->startLoop();
        if (!_loop) return false;

        Net::InetAddress addr(cfg.port);
        _http.reset(new Net::HttpServer(_loop, addr, "monitor_http"));
        _http->setThreadNum(cfg.threads > 0 ? cfg.threads : 1);
        _http->setTimeout(30);

        // Register the monitor endpoints before the server starts accepting.
        // The servlet registry is process-global and thread-safe.
        Net::HttpServlet::insert("/metrics", &createMetricsServlet);
        Net::HttpServlet::insert("/api/v1/status", &createStatusServlet);
        Net::HttpServlet::insert("/api/v1/health", &createHealthServlet);
        Net::HttpServlet::insert("/api/v1/metrics/list", &createMetricsListServlet);
        Net::HttpServlet::insert("/api/v1/slowlog", &createSlowlogServlet);

        // HttpServer::start() schedules a timer via EventLoop::runEvery(), which
        // asserts it runs on the loop thread — post it there.
        Net::EventLoop* loop = _loop;
        Net::HttpServer* http = _http.get();
        loop->runInLoop([loop, http]() { (void)loop; http->start(); });
        return true;
    }

    void stop() {
        // The HttpServer must be destroyed on its own loop thread; then the
        // loop thread itself can be joined from here.
        if (_http && _loop) {
            Net::HttpServer* http = _http.get();
            CountDownLatch latch(1);
            _loop->runInLoop([http, &latch]() {
                delete http;
                latch.countDown();
            });
            latch.wait();
            _http.release();
        }
        _http.reset();
        _loop = 0;
        _loopThread.reset();
    }

    uint16_t port() const { return _port; }

private:
    uint16_t _port;
    Net::EventLoop* _loop;
    std::unique_ptr<Net::EventLoopThread> _loopThread;
    std::unique_ptr<Net::HttpServer>      _http;
};

// ===========================================================================
// MonitorAgent
// ===========================================================================
MonitorAgent& MonitorAgent::instance() {
    static MonitorAgent agent;
    return agent;
}

MonitorAgent::MonitorAgent() : _running(false) {}

MonitorAgent::~MonitorAgent() {
    stop();
}

double MonitorAgent::currentQps() const {
    return metrics::qps().value();
}

bool MonitorAgent::start(const MonitorConfig& cfg) {
    if (_running.load()) return true;

    _cfg = cfg;

    // Register the full metric set up front so scrapes are stable.
    metrics::registerAll();

    // Publish the slow-query threshold used by the request path.
    double threshold = (cfg.slowThresholdMs > 0)
                           ? (double)cfg.slowThresholdMs / 1000.0 : 0.1;
    slowQueryThresholdSec() = threshold;

    SlowLog::instance().setCapacity(cfg.slowLogCapacity);
    metrics::connectionsMax().set((double)cfg.maxConnectionsHint);
    metrics::uptime().set(Registry::instance().uptimeSeconds());

    if (cfg.enabled) {
        _http.reset(new MonitorHttpServer());
        if (!_http->start(cfg)) {
            LOG_ERROR << "monitor: failed to start HTTP server on port " << cfg.port;
            _http.reset();
        } else {
            LOG_INFO << "monitor: HTTP endpoint listening on port " << _http->port();
        }
    }

    _running.store(true);
    _collector = std::thread(&MonitorAgent::collectLoop, this);
    return true;
}

void MonitorAgent::stop() {
    if (!_running.exchange(false)) {
        // Still tear down a partially started HTTP server, if any.
        if (_http) _http.reset();
        return;
    }

    if (_collector.joinable()) _collector.join();

    if (_http) {
        _http.reset();
        LOG_INFO << "monitor: HTTP endpoint stopped";
    }
}

void MonitorAgent::collectLoop() {
    using namespace std::chrono;

    Registry& reg = Registry::instance();

    uint64_t lastQueries = Registry::familySum(metrics::queries());
    steady_clock::time_point lastTp = steady_clock::now();

    const int intervalMs = _cfg.collectIntervalMs > 100 ? _cfg.collectIntervalMs : 100;

    while (_running.load()) {
        // Sleep in small slices so stop() is responsive.
        int slept = 0;
        while (slept < intervalMs && _running.load()) {
            std::this_thread::sleep_for(milliseconds(100));
            slept += 100;
        }
        if (!_running.load()) break;

        // ---- system metrics (CPU / memory / disk) ----
        SystemStats st;
        collectSystemStats(_cfg.dataDir.c_str(), st);
        metrics::cpuUsage().set(st.cpuRatio);
        metrics::memoryUsed().set((double)st.memUsedBytes);
        metrics::memoryTotal().set((double)st.memTotalBytes);
        metrics::diskUsed().set((double)st.diskUsedBytes);
        metrics::diskTotal().set((double)st.diskTotalBytes);
        metrics::diskUsedRatio().set(
            st.diskTotalBytes > 0
                ? (double)st.diskUsedBytes / (double)st.diskTotalBytes : 0.0);

        // ---- uptime ----
        metrics::uptime().set(reg.uptimeSeconds());

        // ---- QPS (delta of total queries over the sampling window) ----
        uint64_t now = Registry::familySum(metrics::queries());
        steady_clock::time_point tp = steady_clock::now();
        double dt = duration<double>(tp - lastTp).count();
        if (dt > 0.0) {
            uint64_t delta = (now >= lastQueries) ? (now - lastQueries) : 0;
            metrics::qps().set((double)delta / dt);
        }
        lastQueries = now;
        lastTp = tp;
    }
}

}  // namespace Monitor
}  // namespace EtherDB
