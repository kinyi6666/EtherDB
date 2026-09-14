# EtherDB

***840KB Bin,8MB RAM, 1.8M writes/sec, 1.5M rows/sec fetch on ARM D2000**

**EtherDB** is a high-performance, SQL-driven **time-series database (TSDB)** written from scratch in **C++17**.

It ingests, stores, and queries massive streams of timestamped data at high speed, using familiar
SQL instead of proprietary query languages — while keeping the codebase small, transparent, and free
of third-party database engines.

- 📄 License: **BSL 1.1** (source-available)
- 🧩 Server: Linux · 💻 Client shell & SDK: Linux / Windows
- 📦 Zero database dependencies (bundled LZ4 only)

---

## ✨ Why EtherDB

| | |
|---|---|
| **SQL-first, zero learning cost** | MySQL / TDengine-style SQL. No special query language — go from zero to querying in minutes. |
| **Built for time-series data** | Data is stored **time-ordered and column-oriented** in day-partitioned files, so range scans, projections, and aggregations stay fast on billions of rows. |
| **High-performance storage pipeline** | Skip-list **MemTable** → lock-free commit queue → background commit threads flush to append-only disk files. Queries keep reading while commits happen (mem/imem double buffering). |
| **Aggregation acceleration (SMA)** | Each data block carries precomputed **min/max/sum aggregates** (`.sma`), giving `SUM` / `MIN` / `MAX` / `COUNT` fast paths and letting the query engine **prune whole blocks** from `WHERE` filters. |
| **Multi-stage columnar compression** | Stage 1 = type-specialized algorithms (boolean bit-packing, integer **delta + zigzag + Simple8B-style bit packing**, float/double, strings); Stage 2 = optional **LZ4**. Configurable off / 1-stage / 2-stage. |
| **Crash-safe by design** | **Write-Ahead Log (WAL)** with crash replay; async commits triggered by WAL size, plus a synchronous commit mode. |
| **Rich C/C++ client SDK** | Pure C API + object-oriented C++ API shipped as headers + prebuilt library: prepared statements, parameter binding, row & columnar **batch insert**, **async batch with backpressure**, **streaming fetch** (row / block). |
| **Interactive shell** | Console client with readline editing, meta-commands (`\?`, `\q`, `\d`, `\l`, `\c`) and table / CSV output. |
| **Clean layered architecture** | dnode / dbnode / vgroup model (TDengine-inspired), custom RPC + network layer (epoll on Linux, IOCP on Windows), config-driven threading. |

---

## 🏗 Architecture

```
┌──────────────┐      ┌──────────────┐
│  etherdb     │      │  C / C++     │   clients
│  shell       │      │  SDK (lib)   │
└──────┬───────┘      └──────┬───────┘
       │      SQL over TCP   │
       └──────────┬──────────┘
                  ▼
        ┌────────────────────┐
        │   DServer (server)   │   epoll/IOCP, worker pools, RPC
        └─────────┬──────────┘
        ┌─────────┴──────────┐
        ▼                    ▼
  dbNode (dbId=1)       dbNode (dbId=2)
        │                    │
        ▼                    ▼
   ETDB storage repo   ETDB storage repo
```

**Write path:** `INSERT` → MemTable (skip-list) → commit queue → background commit
(swap mem → imem) → WAL + append to time-partitioned columnar files.

**Query path:** SQL → recursive-descent parser → planner/executor → storage reader with
**timestamp-range pushdown**, **column projection**, block-level pruning, and k-way streaming merge.

Each data file set is stored per dbnode as:
`v<dbId>f<fileId>.data` (compressed columnar blocks), `.head` (block index / metadata), `.sma` (per-block aggregates), plus a `WAL`.

---

## 🚀 Quick Start (Linux)

### Prerequisites

- Linux, a C++17 compiler (GCC ≥ 8), CMake ≥ 3.10, `pthread`.

### 1. Build the server

```bash
# base library  → src/bin/libbase.so
cd src/base
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j

# dnode server → src/bin/etherdb_server
cd ../dnode
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
```

### 2. Start the server

```bash
./src/bin/etherdb_server -p 7040 -d ./etdb_data
```
default path:
/var/log/etherdb/etherdb_log
/var/lib/etherdb/etherdb_data
### 3. Connect with the interactive shell

```bash
# build the shell client → src/bin/etherdb
cd src/client
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j

cd ../..
./src/bin/etherdb -h 127.0.0.1 -p 7040 -u root -P etherdbdata
```

Try it:

```sql
CREATE DATABASE testdb;
USE testdb;
CREATE TABLE sensor (ts TIMESTAMP, temperature FLOAT, humidity FLOAT);
INSERT INTO sensor VALUES(1716364800000, 25.5, 60.2);
INSERT INTO sensor VALUES(1716364860000, 26.1, 59.0);
SELECT * FROM sensor WHERE temperature > 25.0;
SELECT COUNT(*), AVG(temperature), MAX(temperature) FROM sensor;
SELECT * FROM sensor ORDER BY temperature DESC LIMIT 5;
```

---

## 🗄 Supported SQL & Types

- **DDL** — `CREATE / DROP DATABASE` (with `KEEP`, `REPLICA`), `CREATE / DROP TABLE`,
  `SHOW DATABASES`, `SHOW TABLES`, `DESCRIBE`, `USE`
- **DML / DQL** — `INSERT ... VALUES`, `SELECT` with `WHERE`, `ORDER BY`, `GROUP BY`,
  `HAVING`, `LIMIT / OFFSET`, `DISTINCT`, and aggregates `COUNT / SUM / AVG / MIN / MAX`
- **Column types** — `TIMESTAMP` (ms / µs / ns precision), `BOOL`, `TINYINT`, `SMALLINT`,
  `INT`, `BIGINT` (+ unsigned variants), `FLOAT`, `DOUBLE`, `BINARY`, `NCHAR`

---

## 🛠 Client SDK

Header-only + prebuilt library, standalone on **Windows and Linux** — no server sources needed.

```bash
./scripts/build_sdk.sh            # static lib (libetherdb_client.a)
./scripts/build_sdk.sh --shared   # shared lib (.so)
# Windows: scripts\build_sdk.bat [--shared]
```

Output lands in `src/bin/sdk/` (`include/` + `lib/`). See `src/client/README.md` for C and C++ examples
(connect, query, prepared-statement batch insert, streaming fetch).

---

## 📂 Repository Layout

| Path | Purpose |
|---|---|
| `src/base` | Threading, logging, common utilities |
| `src/net` | Event loop, TCP/UDP (epoll / IOCP) |
| `src/gateway` | app server & message transport |
| `src/dserver` | Data-node server process, config, worker pools |
| `src/dbnode` | Virtual nodes (one storage repo per vgroup) |
| `src/wal` | Write-ahead log module |
| `src/etdb` | **Storage engine** — repo, memtable, columnar files, compression, SMA, commit queue |
| `src/query` | SQL parser, query engine & executor |
| `src/client` | Shell client + C/C++ client SDK |
| `tests` | Integration / correctness / performance tests |
| `packaging/cfg` | Example server config `etherdb.cfg` |

Server behavior is configured through `etherdb.cfg`
(`[server]` ports & threads, `[storage]` data dir, `[log]` async logging, `[auth]` credentials, …).

---

## 🧪 Testing

The `tests/` folder contains end-to-end integration tests (`full_test`), correctness verifiers
(`verify_where`, `value_verify`, …), and performance tests (`perf_insert_test`, `query_perf_test`,
`streaming_perf_test`, …) that run against a live `etherdb_server`.

---

## 📄 License

EtherDB is licensed under the **Business Source License 1.1 (BSL 1.1)**.
On the Change Date (**2030-10-12**) it automatically converts to the
**Apache License, Version 2.0**.
See [LICENSE](LICENSE).

Commercial licensing is available for production use before the Change Date —
contact kinyi6666@gmail.com.

Copyright © 2026 Liu jinwei <kinyi6666@gmail.com>
