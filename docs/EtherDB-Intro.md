**Title**: I benchmarked my 1 MB time-series database against TDengine 2.1 and 3.4 on a Phytium D2000 (ARM) — here's where each one wins

---


I've been building a time-series database from scratch in C++17 as a side project. No JVM, no third-party storage engine, no external database deps — the server is a single executable.

Numbers first. Test machine is a **Phytium D2000** (8-core ARM aarch64 / 31 GB RAM / Kylin Linux). This box already ran **TDengine 2.1.7.2**, and I've now added the **latest 3.4.2.8 (official Docker image, default settings)** — so this round has the same-machine, same-dataset, same-SQL comparison for **both generations**:

> **Positioning first**: EtherDB isn't fighting TDengine for the platform market — it targets **edge / single-node workloads with event-driven burst writes** (think radar sampling: quiet most of the time, then a flood of data), and it doesn't do large clusters. The same-box comparison below is for **calibration**, not an arena fight.

| What | EtherDB | **TDengine 3.4.2.8 (latest)** | TDengine 2.1.7.2 (reference) |
|---|---|---|---|
| Batch insert, 10 cols, 10k rows/batch | **2.15M rows/s** avg | **1.38M rows/s** avg | 0.83M rows/s avg |
| Concurrent writes, 8 procs (own tables) | **6.18M rows/s** aggregate (24.2M rows in 9.5 s) | 4.58–4.79M rows/s (stalls; 29–35 s) | — |
| SQL-text insert, 1k rows/batch | 0.54M rows/s avg | — (not tested) | — |
| Streaming fetch, 10.02M rows | **~1.55 s** (~6.47M rows/s, after query-path optimization) | **~2.06 s** (~4.86M rows/s) | 2.1 s (~4.7M rows/s) |
| `LIMIT 10 OFFSET 500000` | **0.82 ms** (flat) | 22.35 ms (grows with offset) | 133.41 ms (linear) |
| `COUNT(*)` on 10M-row table | **0.34 ms** | 39.14 ms | 122.19 ms |
| Integration test suite | **41/41 assertions pass** | — | — |
| On-disk size, same 10.02M rows | **22 MB** | 12.1 MB data (WAL 497 MB, 1h retention) | 39 MB |
| Server RSS / size | 233 MB (holding 61.2M rows) / **1.07 MB** | 422 MB (taosd) / 805 MB image + 105 MB client libs | 314 MB / 2.79 MB (+10 MB client lib) |

A few design decisions behind those numbers:

**Writes never do random disk IO.** Rows land in a skip-list MemTable, then a commit queue, and background threads flush to disk with mem/imem double buffering — queries keep reading while commits happen. WAL covers crash replay. 10M rows in a row, 2.1M rows/s sustained, no decay.

**Two-stage compression.** Stage one is type-specialized (integers get delta + zigzag + Simple8B-style bit packing), stage two is optional LZ4. The 10.02M rows occupy a 5.9 MB data file on disk.

**Per-block SMA.** Each block carries precomputed min/max/sum, so `WHERE` filters can discard whole blocks and pagination jumps via the block index — that's why `OFFSET` costs the same at 0 and at 500k (0.8 ms).

**SQL is MySQL/TDengine-flavored** — `WHERE`, `GROUP BY`, `HAVING`, `ORDER BY`, `LIMIT/OFFSET`, `DISTINCT` plus aggregates. The client SDK is a C API + a C++ API, headers plus a 224 KB static lib, no server sources needed.

**Now the parts that aren't flattering:**

- **Full-fetch (my biggest weakness before) is now a win**: after shipping a zero-copy columnar cursor (raw column pointers, no per-value materialization, slimmer transfer format), 10.02M rows fetch in **~1.55 s** (was 11.3 s — a 7.2x speedup; ~6.47M rows/s), ~1.3x faster than TDengine 3.4's row interface (~2.06 s). One caveat: 2.1.7.2's block interface (1.21 s) still beats me by ~28%.
- **I also tested the latest TDengine (3.4.2.8, default Docker settings) this time**: I win writes (~1.6x), pagination (~27x) and `COUNT(*)` (~115x); but 3.4's **full-scan filtering got 3–4x slower than 2.1.7.2** (759 vs 196 ms). Reporting it as-is, no cherry-picking. The 2.1.7.2 column stays as the generational reference.
- **Concurrency scaling**: 8 writer processes (one table each) → **6.18M rows/s** aggregate and the box at 79% avg / 87% peak CPU; TDengine 3.4.2.8 reached 4.58–4.79M but hit second-long stalls (29–35 s for the same 24.2M rows vs 9.5 s).
- Synthetic monotonic data, identical generator formulas on both sides — friendly to compression and aggregation in both engines.

**Who it's for**: people who want to read the whole source, edge/embedded deployments, single-node workloads that take **event-driven burst writes** (think radar sampling) with range/pagination/aggregate queries.
**Who it's not for**: production SLA, distributed strongly-consistent clusters, plug-and-play BI connectors.

BSL 1.1, converting to Apache 2.0 in 2030. All benchmark programs (including the TDengine comparison tool) live in `tests/`, so you can reproduce the numbers yourself.

**Tear apart my index design if you want — verifiable criticism is more useful to me than praise.** Contact: kinyi6666@gmail.com