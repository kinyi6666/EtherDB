// Copyright (c) 2026 Liu jinwei <kinyi6666@gmail.com>
//
// Use of this software is governed by the Business Source License 1.1
// included in the file LICENSE.
//
// Change Date: 2030-10-12
//
// Change License: Apache License, Version 2.0

/*
 * EtherDB Monitor - lightweight, lock-free metrics registry.
 *
 * Design goals (see monitor.txt):
 *   - Hot path only does atomic increments: no locks, no formatting, no I/O.
 *   - Only the scrape/serialization path walks the registry.
 *   - Label values are fixed enums decided at registration time, so the number
 *     of time series can never explode.
 *
 * This header is header-only (all functions are implicitly inline) and only
 * depends on the C++11 standard library, so it can be included from the
 * gateway transport layer, the dserver, and the monitor HTTP service without
 * introducing a link dependency.
 */

#ifndef ETHERDB_MONITOR_METRICS_H
#define ETHERDB_MONITOR_METRICS_H

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace EtherDB {
namespace Monitor {

// ===========================================================================
// atomic<double> helpers
//   std::atomic<double>::fetch_add only exists in C++20; use a CAS loop.
//   Gauges/histogram sums are updated rarely (system collectors, per query),
//   so a CAS loop is perfectly adequate.
// ===========================================================================
inline void atomicAddDouble(std::atomic<double>& a, double v) {
    double old = a.load(std::memory_order_relaxed);
    double want = 0.0;
    do {
        want = old + v;
    } while (!a.compare_exchange_weak(old, want,
                                     std::memory_order_relaxed,
                                     std::memory_order_relaxed));
}

// ===========================================================================
// Counter — monotonically increasing, atomic only.
// ===========================================================================
class Counter {
public:
    Counter() : _v(0) {}

    void inc(uint64_t n = 1) { _v.fetch_add(n, std::memory_order_relaxed); }
    uint64_t value() const { return _v.load(std::memory_order_relaxed); }
    void reset() { _v.store(0, std::memory_order_relaxed); }

private:
    std::atomic<uint64_t> _v;
};

// ===========================================================================
// Gauge — can go up and down. Stored as double so ratios (CPU usage) and
// connection counts share one type.
// ===========================================================================
class Gauge {
public:
    Gauge() : _v(0.0) {}

    void set(double v) { _v.store(v, std::memory_order_relaxed); }
    void inc(double v = 1.0) { atomicAddDouble(_v, v); }
    void dec(double v = 1.0) { atomicAddDouble(_v, -v); }
    double value() const { return _v.load(std::memory_order_relaxed); }

private:
    std::atomic<double> _v;
};

// ===========================================================================
// Histogram — fixed bucket bounds decided at registration.
//   observe() is lock-free; bucket lookup is a small linear scan (<= 16 buckets).
// ===========================================================================
class Histogram {
public:
    Histogram() : _nbuckets(0), _sum(0.0), _count(0) {}

    void init(const std::vector<double>& bounds) {
        _bounds = bounds;
        _nbuckets = bounds.size() + 1;  // last bucket is +Inf
        _buckets.reset(new std::atomic<uint64_t>[_nbuckets]);
        for (size_t i = 0; i < _nbuckets; ++i) {
            _buckets[i].store(0, std::memory_order_relaxed);
        }
        _sum.store(0.0, std::memory_order_relaxed);
        _count.store(0, std::memory_order_relaxed);
    }

    void observe(double v) {
        if (_nbuckets == 0) return;
        _buckets[bucketIndex(v)].fetch_add(1, std::memory_order_relaxed);
        atomicAddDouble(_sum, v);
        _count.fetch_add(1, std::memory_order_relaxed);
    }

    size_t bucketIndex(double v) const {
        for (size_t i = 0; i < _bounds.size(); ++i) {
            if (v <= _bounds[i]) return i;
        }
        return _bounds.size();
    }

    const std::vector<double>& bounds() const { return _bounds; }
    size_t bucketCount() const { return _nbuckets; }
    uint64_t bucket(size_t i) const {
        return (i < _nbuckets) ? _buckets[i].load(std::memory_order_relaxed) : 0;
    }
    double sum() const { return _sum.load(std::memory_order_relaxed); }
    uint64_t count() const { return _count.load(std::memory_order_relaxed); }

private:
    std::vector<double> _bounds;
    size_t _nbuckets;
    std::unique_ptr<std::atomic<uint64_t>[]> _buckets;
    std::atomic<double> _sum;
    std::atomic<uint64_t> _count;
};

// ===========================================================================
// CounterFamily / GaugeFamily — a metric with one fixed-value label.
//   Label values are enumerated at registration time, so the hot path is a
//   plain array index with no map lookup and no lock.
// ===========================================================================
class CounterFamily {
public:
    CounterFamily() {}

    void init(const std::string& labelName, const std::vector<std::string>& values) {
        _labelName = labelName;
        _values = values;
        _counters.reset(new Counter[values.empty() ? 1 : values.size()]);
    }

    size_t size() const { return _values.size(); }
    Counter& at(size_t i) { return _counters[i]; }
    const Counter& at(size_t i) const { return _counters[i]; }

    size_t indexOf(const std::string& v) const {
        for (size_t i = 0; i < _values.size(); ++i) {
            if (_values[i] == v) return i;
        }
        return _values.size();  // not found
    }

    const std::string& labelName() const { return _labelName; }
    const std::vector<std::string>& values() const { return _values; }

private:
    std::string _labelName;
    std::vector<std::string> _values;
    std::unique_ptr<Counter[]> _counters;
};

class GaugeFamily {
public:
    GaugeFamily() {}

    void init(const std::string& labelName, const std::vector<std::string>& values) {
        _labelName = labelName;
        _values = values;
        _gauges.reset(new Gauge[values.empty() ? 1 : values.size()]);
    }

    size_t size() const { return _values.size(); }
    Gauge& at(size_t i) { return _gauges[i]; }
    const Gauge& at(size_t i) const { return _gauges[i]; }

    const std::string& labelName() const { return _labelName; }
    const std::vector<std::string>& values() const { return _values; }

private:
    std::string _labelName;
    std::vector<std::string> _values;
    std::unique_ptr<Gauge[]> _gauges;
};

// ===========================================================================
// Registry — owns every metric and renders the Prometheus text format.
//   Registration and rendering take a lock; the hot path never touches it.
// ===========================================================================
class Registry {
public:
    static Registry& instance() {
        static Registry r;
        return r;
    }

    // ---- Registration (call once, typically at startup) ----
    Counter& addCounter(const std::string& name, const std::string& help) {
        std::lock_guard<std::mutex> lk(_mu);
        std::map<std::string, std::unique_ptr<Counter> >::iterator it = _counters.find(name);
        if (it != _counters.end()) return *it->second;
        _counters[name].reset(new Counter());
        _help[name] = help;
        return *_counters[name];
    }

    Gauge& addGauge(const std::string& name, const std::string& help) {
        std::lock_guard<std::mutex> lk(_mu);
        std::map<std::string, std::unique_ptr<Gauge> >::iterator it = _gauges.find(name);
        if (it != _gauges.end()) return *it->second;
        _gauges[name].reset(new Gauge());
        _help[name] = help;
        return *_gauges[name];
    }

    Histogram& addHistogram(const std::string& name, const std::string& help,
                            const std::vector<double>& bounds) {
        std::lock_guard<std::mutex> lk(_mu);
        std::map<std::string, std::unique_ptr<Histogram> >::iterator it = _histograms.find(name);
        if (it != _histograms.end()) return *it->second;
        _histograms[name].reset(new Histogram());
        _histograms[name]->init(bounds);
        _help[name] = help;
        return *_histograms[name];
    }

    CounterFamily& addCounterFamily(const std::string& name, const std::string& help,
                                    const std::string& labelName,
                                    const std::vector<std::string>& values) {
        std::lock_guard<std::mutex> lk(_mu);
        std::map<std::string, std::unique_ptr<CounterFamily> >::iterator it =
            _counterFamilies.find(name);
        if (it != _counterFamilies.end()) return *it->second;
        _counterFamilies[name].reset(new CounterFamily());
        _counterFamilies[name]->init(labelName, values);
        _help[name] = help;
        return *_counterFamilies[name];
    }

    GaugeFamily& addGaugeFamily(const std::string& name, const std::string& help,
                                const std::string& labelName,
                                const std::vector<std::string>& values) {
        std::lock_guard<std::mutex> lk(_mu);
        std::map<std::string, std::unique_ptr<GaugeFamily> >::iterator it =
            _gaugeFamilies.find(name);
        if (it != _gaugeFamilies.end()) return *it->second;
        _gaugeFamilies[name].reset(new GaugeFamily());
        _gaugeFamilies[name]->init(labelName, values);
        _help[name] = help;
        return *_gaugeFamilies[name];
    }

    // ---- Serialization (scrape path, low frequency) ----
    std::string renderPrometheus() const {
        std::lock_guard<std::mutex> lk(_mu);
        std::ostringstream os;

        for (std::map<std::string, std::unique_ptr<Counter> >::const_iterator it =
                 _counters.begin(); it != _counters.end(); ++it) {
            header(os, it->first, "counter");
            os << it->first << " " << it->second->value() << "\n";
        }
        for (std::map<std::string, std::unique_ptr<Gauge> >::const_iterator it =
                 _gauges.begin(); it != _gauges.end(); ++it) {
            header(os, it->first, "gauge");
            os << it->first << " " << fmt(it->second->value()) << "\n";
        }
        for (std::map<std::string, std::unique_ptr<CounterFamily> >::const_iterator it =
                 _counterFamilies.begin(); it != _counterFamilies.end(); ++it) {
            header(os, it->first, "counter");
            const CounterFamily& f = *it->second;
            for (size_t i = 0; i < f.size(); ++i) {
                os << it->first << "{" << f.labelName() << "=\"" << f.values()[i]
                   << "\"} " << f.at(i).value() << "\n";
            }
        }
        for (std::map<std::string, std::unique_ptr<GaugeFamily> >::const_iterator it =
                 _gaugeFamilies.begin(); it != _gaugeFamilies.end(); ++it) {
            header(os, it->first, "gauge");
            const GaugeFamily& f = *it->second;
            for (size_t i = 0; i < f.size(); ++i) {
                os << it->first << "{" << f.labelName() << "=\"" << f.values()[i]
                   << "\"} " << fmt(f.at(i).value()) << "\n";
            }
        }
        for (std::map<std::string, std::unique_ptr<Histogram> >::const_iterator it =
                 _histograms.begin(); it != _histograms.end(); ++it) {
            header(os, it->first, "histogram");
            const Histogram& h = *it->second;
            for (size_t i = 0; i < h.bounds().size(); ++i) {
                os << it->first << "_bucket{le=\"" << fmt(h.bounds()[i]) << "\"} "
                   << h.bucket(i) << "\n";
            }
            os << it->first << "_bucket{le=\"+Inf\"} " << h.bucket(h.bounds().size())
               << "\n";
            os << it->first << "_sum " << fmt(h.sum()) << "\n";
            os << it->first << "_count " << h.count() << "\n";
        }
        return os.str();
    }

    // Flat JSON snapshot: {"name": value, ...} plus label-qualified names.
    std::string renderJson() const {
        std::lock_guard<std::mutex> lk(_mu);
        std::ostringstream os;
        os << "{";
        bool first = true;

        for (std::map<std::string, std::unique_ptr<Counter> >::const_iterator it =
                 _counters.begin(); it != _counters.end(); ++it) {
            sep(os, first);
            os << "\"" << it->first << "\":" << it->second->value();
        }
        for (std::map<std::string, std::unique_ptr<Gauge> >::const_iterator it =
                 _gauges.begin(); it != _gauges.end(); ++it) {
            sep(os, first);
            os << "\"" << it->first << "\":" << fmt(it->second->value());
        }
        for (std::map<std::string, std::unique_ptr<CounterFamily> >::const_iterator it =
                 _counterFamilies.begin(); it != _counterFamilies.end(); ++it) {
            const CounterFamily& f = *it->second;
            for (size_t i = 0; i < f.size(); ++i) {
                sep(os, first);
                os << "\"" << it->first << "{" << f.labelName() << "=\\\""
                   << f.values()[i] << "\\\"}\":" << f.at(i).value();
            }
        }
        for (std::map<std::string, std::unique_ptr<GaugeFamily> >::const_iterator it =
                 _gaugeFamilies.begin(); it != _gaugeFamilies.end(); ++it) {
            const GaugeFamily& f = *it->second;
            for (size_t i = 0; i < f.size(); ++i) {
                sep(os, first);
                os << "\"" << it->first << "{" << f.labelName() << "=\\\""
                   << f.values()[i] << "\\\"}\":" << fmt(f.at(i).value());
            }
        }
        for (std::map<std::string, std::unique_ptr<Histogram> >::const_iterator it =
                 _histograms.begin(); it != _histograms.end(); ++it) {
            sep(os, first);
            os << "\"" << it->first << "_count\":" << it->second->count() << ",";
            os << "\"" << it->first << "_sum\":" << fmt(it->second->sum());
        }
        os << "}";
        return os.str();
    }

    // JSON array of metric names (for Grafana template variables).
    std::string renderMetricNamesJson() const {
        std::lock_guard<std::mutex> lk(_mu);
        std::ostringstream os;
        os << "[";
        bool first = true;
        appendName(os, first, _counters);
        appendName(os, first, _gauges);
        appendName(os, first, _histograms);
        appendName(os, first, _counterFamilies);
        appendName(os, first, _gaugeFamilies);
        os << "]";
        return os.str();
    }

    double uptimeSeconds() const {
        return std::chrono::duration<double>(
                   std::chrono::steady_clock::now() - _start).count();
    }

    // Convenience: sum of a counter family (used for QPS sampling).
    static uint64_t familySum(const CounterFamily& f) {
        uint64_t total = 0;
        for (size_t i = 0; i < f.size(); ++i) total += f.at(i).value();
        return total;
    }

private:
    Registry() : _start(std::chrono::steady_clock::now()) {}
    Registry(const Registry&);
    Registry& operator=(const Registry&);

    void header(std::ostringstream& os, const std::string& name,
                const char* type) const {
        std::map<std::string, std::string>::const_iterator it = _help.find(name);
        if (it != _help.end() && !it->second.empty()) {
            os << "# HELP " << name << " " << it->second << "\n";
        }
        os << "# TYPE " << name << " " << type << "\n";
    }

    static void sep(std::ostringstream& os, bool& first) {
        if (!first) os << ",";
        first = false;
    }

    template <typename MapT>
    static void appendName(std::ostringstream& os, bool& first, const MapT& m) {
        for (typename MapT::const_iterator it = m.begin(); it != m.end(); ++it) {
            sep(os, first);
            os << "\"" << it->first << "\"";
        }
    }

    // Compact double formatting: integers without a decimal point, otherwise
    // up to 6 significant digits (good enough for Prometheus text output).
    static std::string fmt(double v) {
        std::ostringstream os;
        if (std::fabs(v - std::floor(v)) < 1e-9 && std::fabs(v) < 1e15) {
            os << (long long)v;
        } else {
            os.precision(6);
            os << v;
        }
        return os.str();
    }

    mutable std::mutex _mu;
    std::map<std::string, std::string> _help;
    std::map<std::string, std::unique_ptr<Counter> > _counters;
    std::map<std::string, std::unique_ptr<Gauge> > _gauges;
    std::map<std::string, std::unique_ptr<Histogram> > _histograms;
    std::map<std::string, std::unique_ptr<CounterFamily> > _counterFamilies;
    std::map<std::string, std::unique_ptr<GaugeFamily> > _gaugeFamilies;
    std::chrono::steady_clock::time_point _start;
};

// ===========================================================================
// Query / error type enumerations.
//   These indices are the label order used by the metric families below and
//   must stay in sync with the label vectors.
//
//   The query labels mirror the statements actually supported by the server
//   (see query/QueryAst.h StmtType): there is no UPDATE / DELETE — the write
//   path is INSERT-only, and metadata changes are DDL (create/drop).
// ===========================================================================
namespace qtype {
enum Type { Select = 0, Insert, Ddl, Show, Use, Fetch, Meta, Other, Count };
}

namespace qerr {
enum Type { Invalid = 0, NotFound, Auth, Timeout, Internal, Count };
}

// Global slow-query threshold (seconds). Set by MonitorAgent::start().
inline double& slowQueryThresholdSec() {
    static double v = 0.1;
    return v;
}

// ===========================================================================
// Concrete metric set.
//   Each accessor registers its metric on first use and caches the reference
//   in a function-local static (thread-safe magic static). Callers may cache
//   the returned reference at startup to keep the hot path to a single atomic.
// ===========================================================================
namespace metrics {

inline const std::vector<std::string>& queryTypeLabels() {
    static const std::vector<std::string> v = {
        "select", "insert", "ddl", "show", "use", "fetch", "meta", "other"};
    return v;
}

inline const std::vector<std::string>& errorTypeLabels() {
    static const std::vector<std::string> v = {
        "invalid", "not_found", "auth", "timeout", "internal"};
    return v;
}

// ---- Connections ----
inline Counter& connectionsTotal() {
    static Counter& c = Registry::instance().addCounter(
        "db_connections_total", "Total number of accepted client connections");
    return c;
}
inline Counter& connectionsRejected() {
    static Counter& c = Registry::instance().addCounter(
        "db_connections_rejected_total",
        "Total number of client connections rejected (auth or capacity)");
    return c;
}
inline Gauge& connectionsCurrent() {
    static Gauge& g = Registry::instance().addGauge(
        "db_connections_current", "Current number of open client connections");
    return g;
}
inline Gauge& connectionsMax() {
    static Gauge& g = Registry::instance().addGauge(
        "db_connections_max", "Configured maximum number of client connections");
    return g;
}

// ---- Queries ----
inline CounterFamily& queries() {
    static CounterFamily& f = Registry::instance().addCounterFamily(
        "db_queries_total", "Total number of processed queries, by statement type",
        "type", queryTypeLabels());
    return f;
}
inline CounterFamily& queryErrors() {
    static CounterFamily& f = Registry::instance().addCounterFamily(
        "db_query_errors_total", "Total number of failed queries, by error type",
        "error_type", errorTypeLabels());
    return f;
}
inline Counter& slowQueries() {
    static Counter& c = Registry::instance().addCounter(
        "db_slow_queries_total", "Total number of queries slower than the threshold");
    return c;
}

// ---- Latency / rows ----
inline Histogram& queryDuration() {
    static Histogram& h = Registry::instance().addHistogram(
        "db_query_duration_seconds", "Query latency distribution in seconds",
        std::vector<double>{0.0001, 0.0005, 0.001, 0.005, 0.01, 0.05, 0.1, 0.5, 1.0});
    return h;
}
inline Histogram& queryRows() {
    static Histogram& h = Registry::instance().addHistogram(
        "db_query_rows_returned", "Number of rows returned per query",
        std::vector<double>{1, 10, 100, 1000, 10000, 100000});
    return h;
}

// ---- Server / process ----
inline Gauge& uptime() {
    static Gauge& g = Registry::instance().addGauge(
        "db_uptime_seconds", "Server uptime in seconds");
    return g;
}
inline Gauge& qps() {
    static Gauge& g = Registry::instance().addGauge(
        "db_qps", "Queries per second (rolling sample)");
    return g;
}
inline Gauge& cpuUsage() {
    static Gauge& g = Registry::instance().addGauge(
        "db_cpu_usage_ratio", "Process CPU usage ratio (0..1)");
    return g;
}
inline Gauge& memoryUsed() {
    static Gauge& g = Registry::instance().addGauge(
        "db_memory_used_bytes", "Resident memory used by the server process");
    return g;
}
inline Gauge& memoryTotal() {
    static Gauge& g = Registry::instance().addGauge(
        "db_memory_total_bytes", "Total physical memory available");
    return g;
}
inline Gauge& diskUsed() {
    static Gauge& g = Registry::instance().addGauge(
        "db_disk_used_bytes", "Disk bytes used in the data directory");
    return g;
}
inline Gauge& diskTotal() {
    static Gauge& g = Registry::instance().addGauge(
        "db_disk_total_bytes", "Total disk bytes in the data directory volume");
    return g;
}
inline Gauge& diskUsedRatio() {
    static Gauge& g = Registry::instance().addGauge(
        "db_disk_used_ratio", "Disk usage ratio of the data volume (0..1)");
    return g;
}

// Eagerly register every metric so that scrapes expose a stable set of time
// series even before the first query arrives. Call once at startup.
inline void registerAll() {
    connectionsTotal();
    connectionsRejected();
    connectionsCurrent();
    connectionsMax();
    queries();
    queryErrors();
    slowQueries();
    queryDuration();
    queryRows();
    uptime();
    qps();
    cpuUsage();
    memoryUsed();
    memoryTotal();
    diskUsed();
    diskTotal();
    diskUsedRatio();
}

}  // namespace metrics
}  // namespace Monitor
}  // namespace EtherDB

#endif  // ETHERDB_MONITOR_METRICS_H
