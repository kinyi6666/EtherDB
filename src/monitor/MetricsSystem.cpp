// Copyright (c) 2026 Liu jinwei <kinyi6666@gmail.com>
//
// Use of this software is governed by the Business Source License 1.1
// included in the file LICENSE.
//
// Change Date: 2030-10-12
//
// Change License: Apache License, Version 2.0

/*
 * EtherDB Monitor - platform system statistics.
 */

#include "MetricsSystem.h"

#include <cstdio>
#include <cstring>
#include <mutex>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/statvfs.h>
#include <unistd.h>
#endif

namespace EtherDB {
namespace Monitor {

namespace {

// ---------------------------------------------------------------------------
// CPU sampling baseline (needs two samples to compute a ratio).
// ---------------------------------------------------------------------------
struct CpuSample {
    bool     valid = false;
    uint64_t idle  = 0;
    uint64_t total = 0;
};

CpuSample g_prevCpu;
std::mutex g_cpuMutex;

#ifdef _WIN32
inline uint64_t fileTimeToU64(const FILETIME& ft) {
    ULARGE_INTEGER u;
    u.LowPart  = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return u.QuadPart;
}

void readCpu(CpuSample& s) {
    FILETIME idleFt, kernelFt, userFt;
    if (!GetSystemTimes(&idleFt, &kernelFt, &userFt)) {
        s.valid = false;
        return;
    }
    uint64_t idle   = fileTimeToU64(idleFt);
    uint64_t kernel = fileTimeToU64(kernelFt);
    uint64_t user   = fileTimeToU64(userFt);
    s.idle  = idle;
    s.total = kernel + user;  // kernel already contains idle time
    s.valid = true;
}
#else
void readCpu(CpuSample& s) {
    FILE* fp = fopen("/proc/stat", "r");
    if (!fp) {
        s.valid = false;
        return;
    }
    char line[256];
    if (!fgets(line, sizeof(line), fp)) {
        fclose(fp);
        s.valid = false;
        return;
    }
    fclose(fp);
    // "cpu  user nice system idle iowait irq softirq steal guest guest_nice"
    unsigned long long user = 0, nice = 0, system = 0, idle = 0, iowait = 0;
    unsigned long long irq = 0, softirq = 0, steal = 0;
    int n = sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
                   &user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal);
    if (n < 4) {
        s.valid = false;
        return;
    }
    s.idle  = idle + iowait;
    s.total = user + nice + system + idle + iowait + irq + softirq + steal;
    s.valid = true;
}
#endif

// ---------------------------------------------------------------------------
// Memory
// ---------------------------------------------------------------------------
void readMemory(uint64_t& used, uint64_t& total) {
    used = 0;
    total = 0;
#ifdef _WIN32
    MEMORYSTATUSEX ms;
    memset(&ms, 0, sizeof(ms));
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        total = ms.ullTotalPhys;
        uint64_t avail = ms.ullAvailPhys;
        used = (total > avail) ? (total - avail) : 0;
    }
#else
    uint64_t memTotal = 0, memAvail = 0;
    FILE* fp = fopen("/proc/meminfo", "r");
    if (fp) {
        char line[256];
        while (fgets(line, sizeof(line), fp)) {
            unsigned long long kb = 0;
            if (sscanf(line, "MemTotal: %llu kB", &kb) == 1) {
                memTotal = kb * 1024ULL;
            } else if (sscanf(line, "MemAvailable: %llu kB", &kb) == 1) {
                memAvail = kb * 1024ULL;
            }
        }
        fclose(fp);
    }
    total = memTotal;
    used = (memTotal > memAvail) ? (memTotal - memAvail) : 0;
#endif
}

// ---------------------------------------------------------------------------
// Disk
// ---------------------------------------------------------------------------
void readDisk(const char* path, uint64_t& used, uint64_t& total) {
    used = 0;
    total = 0;
    if (!path || !*path) path = ".";
#ifdef _WIN32
    ULARGE_INTEGER freeBytes, totalBytes, totalFree;
    if (GetDiskFreeSpaceExA(path, &freeBytes, &totalBytes, &totalFree)) {
        total = totalBytes.QuadPart;
        used  = total - freeBytes.QuadPart;
    }
#else
    struct statvfs st;
    if (statvfs(path, &st) == 0) {
        uint64_t blockSize = st.f_frsize ? st.f_frsize : st.f_bsize;
        total = (uint64_t)st.f_blocks * blockSize;
        uint64_t avail = (uint64_t)st.f_bavail * blockSize;
        used = (total > avail) ? (total - avail) : 0;
    }
#endif
}

}  // namespace

void collectSystemStats(const char* path, SystemStats& out) {
    // --- CPU ---
    {
        std::lock_guard<std::mutex> lk(g_cpuMutex);
        CpuSample cur;
        readCpu(cur);
        if (cur.valid) {
            if (g_prevCpu.valid && cur.total > g_prevCpu.total) {
                uint64_t dTotal = cur.total - g_prevCpu.total;
                uint64_t dIdle  = (cur.idle >= g_prevCpu.idle)
                                      ? (cur.idle - g_prevCpu.idle) : 0;
                out.cpuRatio = (dTotal > 0)
                    ? (double)(dTotal - dIdle) / (double)dTotal : 0.0;
                if (out.cpuRatio < 0.0) out.cpuRatio = 0.0;
                if (out.cpuRatio > 1.0) out.cpuRatio = 1.0;
            } else {
                out.cpuRatio = 0.0;
            }
            g_prevCpu = cur;
        }
    }

    // --- Memory ---
    readMemory(out.memUsedBytes, out.memTotalBytes);

    // --- Disk ---
    readDisk(path, out.diskUsedBytes, out.diskTotalBytes);
}

}  // namespace Monitor
}  // namespace EtherDB
