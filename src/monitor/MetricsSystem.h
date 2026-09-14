// Copyright (c) 2026 Liu jinwei <kinyi6666@gmail.com>
//
// Use of this software is governed by the Business Source License 1.1
// included in the file LICENSE.
//
// Change Date: 2030-10-12
//
// Change License: Apache License, Version 2.0

#ifndef ETHERDB_MONITOR_SYSTEM_H
#define ETHERDB_MONITOR_SYSTEM_H

#include <cstdint>
#include <string>

namespace EtherDB {
namespace Monitor {

struct SystemStats {
    double   cpuRatio       = 0.0;  // 0..1, system-wide CPU busy ratio
    uint64_t memUsedBytes   = 0;    // total - available
    uint64_t memTotalBytes  = 0;
    uint64_t diskUsedBytes  = 0;
    uint64_t diskTotalBytes = 0;
};

// Collect system-wide CPU / memory / disk statistics.
// `path` selects the volume to measure for disk usage (typically the data
// directory); if empty the current working directory is used.
// The first call only establishes the CPU sampling baseline (ratio stays 0).
void collectSystemStats(const char* path, SystemStats& out);

}  // namespace Monitor
}  // namespace EtherDB

#endif  // ETHERDB_MONITOR_SYSTEM_H
