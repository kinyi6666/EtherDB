
**Title**: I benchmarked my 1 MB time-series database against TDengine on a Phytium D2000 (ARM) — here's where each one wins

---

I've been building a time-series database from scratch in C++17 as a side project. No JVM, no third-party storage engine, no external database deps — the server is a single executable.

Numbers first. Test machine is a **Phytium D2000** (8-core ARM aarch64 / 31 GB RAM / Kylin Linux). Better: **TDengine 2.1.7.2 is installed and running on the same box**, so this round finally has the same-machine, same-dataset, same-SQL comparison I owed from my last post:

| What | EtherDB | TDengine 2.1.7.2 (same box) |
|---|---|---|
| Batch insert, 10 cols, 10k rows/batch | **2.15M rows/s** avg, 2.34M peak | 0.83M rows/s avg |
| SQL-text insert, 1k rows/batch | 0.54M rows/s avg | — (not tested) |
| Streaming fetch, 10.02M rows | 11.3 s (~0.89M rows/s) | **2.1 s** (~4.7M rows/s) |
| `LIMIT 10 OFFSET 500000` | **0.82 ms** (flat) | 133.41 ms (linear) |
| `COUNT(*)` on 10M-row table | **0.34 ms** | 122.19 ms |
| Integration test suite | **41/41 assertions pass** | — |
| On-disk size, same 10.02M rows | **22 MB** | 39 MB |
| Server RSS / binary | 233 MB (holding 61.2M rows) / **1.07 MB** | 314 MB / 2.79 MB (+10 MB client lib) |

A few design decisions behind those numbers:

**Writes never do random disk IO.** Rows land in a skip-list MemTable, then a commit queue, and background threads flush to disk with mem/imem double buffering — queries keep reading while commits happen. WAL covers crash replay. 10M rows in a row, 2.1M rows/s sustained, no decay.

**Two-stage compression.** Stage one is type-specialized (integers get delta + zigzag + Simple8B-style bit packing), stage two is optional LZ4. The 10.02M rows occupy a 5.9 MB data file on disk.

**Per-block SMA.** Each block carries precomputed min/max/sum, so `WHERE` filters can discard whole blocks and pagination jumps via the block index — that's why `OFFSET` costs the same at 0 and at 500k (0.8 ms).

**SQL is MySQL/TDengine-flavored** — `WHERE`, `GROUP BY`, `HAVING`, `ORDER BY`, `LIMIT/OFFSET`, `DISTINCT` plus aggregates. The client SDK is a C API + a C++ API, headers plus a 224 KB static lib, no server sources needed.

**Now the parts that aren't flattering:**

- **I lose the full-fetch benchmark to TDengine by ~5x** (11.3 s vs 2.1 s for 10.02M rows). Main reason: client API semantics — `taos_fetch_row` hands you pointers into the client's block buffer (zero copy), while my `fetchRow` materializes every value into a typed object. A zero-copy block cursor is on my To-Do list.
- **The TDengine here is v2.1.7.2** (a 2020 release running with default settings on this box) — the pagination/COUNT numbers especially do not represent TDengine 3.x.
- Synthetic monotonic data, identical generator formulas on both sides — friendly to compression and aggregation in both engines.

**Who it's for**: people who want to read the whole source, edge/embedded deployments, write-heavy single-node workloads with range/pagination/aggregate queries.
**Who it's not for**: high-speed bulk export of huge result sets, production SLA, distributed strongly-consistent clusters, plug-and-play BI connectors. This is one person's project, not a company's product.

BSL 1.1, converting to Apache 2.0 in 2030. All benchmark programs (including the TDengine comparison tool) live in `tests/`, so you can reproduce the numbers yourself.

**Tear apart my index design if you want — verifiable criticism is more useful to me than praise.** Contact: kinyi6666@gmail.com

---
