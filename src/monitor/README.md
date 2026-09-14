# EtherDB 监控接口设计

> 目标：让 EtherDB 内核以极低开销暴露自身运行状态。
> 本阶段仅通过 HTTP（Prometheus/Grafana 链路）落地；
> 基于查询消息的客户端 API 直连监控**暂缓实现**（见第 6 节）。

本文档描述第一阶段（HTTP）已实现的接口，以及第二阶段（查询消息）的设计。

---

## 1. 设计原则

沿用 `monitor.txt` 的核心结论：

1. **热路径只做原子加**：不做锁、不做格式化、不做 I/O。
2. **只有抓取路径才遍历/序列化**：`/metrics` 由 Prometheus 每 15s 调用一次，频率极低。
3. **标签基数可控**：标签值只能是注册时确定的枚举（select/insert/…），
   不允许动态字符串，避免时间序列爆炸。
4. **监控端口与业务端口分离**：默认 DB 端口 `7040`，监控 HTTP 端口 `9187`，
   互不抢占连接与限流资源。
5. **桶边界按本系统实际延迟分布设定**，不照抄 MySQL。

---

## 2. 总体结构

```
┌──────────────────────────────────────────────────────────────┐
│                        EtherDB 进程                          │
│                                                              │
│  ┌───────────────┐            ┌───────────────────────────┐  │
│  │ 客户端协议服务 │            │  监控 HTTP 服务            │  │
│  │ GW/ShellServer│            │  net::HttpServer :9187     │  │
│  │    :7040      │            │  /metrics /api/v1/*        │  │
│  └──────┬────────┘            └────────────┬──────────────┘  │
│         │ 埋点(原子)                        │ 读取             │
│         ▼                                  ▼                 │
│  ┌───────────────────────────────────────────────────────┐   │
│  │        Metrics Registry（进程内单例，无锁热路径）        │   │
│  │  Counter / Gauge / Histogram / 固定标签 Family         │   │
│  └───────────────────────────────────────────────────────┘   │
│         ▲                                                    │
│         │ 定时采集(5s)                                       │
│  ┌──────┴────────┐                                           │
│  │ 采集线程       │ CPU / 内存 / 磁盘（系统级）                │
│  └───────────────┘                                           │
└──────────────────────────────────────────────────────────────┘
```

代码位置：

| 文件 | 职责 |
| --- | --- |
| `src/monitor/Metrics.h` | 指标类型 + 注册表（header-only，仅依赖 STL，可被 gateway/dserver 共用） |
| `src/monitor/MonitorHooks.h` | 埋点辅助：请求分类、错误分类、慢查询记录 |
| `src/monitor/SlowLog.h` | 慢查询环形缓冲（定长，极少加锁） |
| `src/monitor/MetricsSystem.h/.cpp` | 平台系统采集（CPU/内存/磁盘） |
| `src/monitor/MonitorServer.h/.cpp` | HTTP 服务 + servlet + 后台采集线程 |

---

## 3. 指标类型

| 类型 | 语义 | 热路径开销 |
| --- | --- | --- |
| `Counter` | 只增计数，`atomic<uint64_t>` | 1 次 `fetch_add` |
| `Gauge` | 可增可减 / 可置值，`atomic<double>` | 1 次 store 或 CAS |
| `Histogram` | 固定桶分布（`atomic<uint64_t>[]` + sum 用 CAS） | 3 次原子操作 |
| `CounterFamily` / `GaugeFamily` | 单固定标签（枚举）族，下标定位、无 map 查找 | 1 次 `fetch_add` |

`Histogram` 桶边界（秒）：

```
0.0001, 0.0005, 0.001, 0.005, 0.01, 0.05, 0.1, 0.5, 1.0
```

---

## 4. 第一阶段指标清单

### 4.1 连接

| 指标 | 类型 | 说明 |
| --- | --- | --- |
| `db_connections_current` | gauge | 当前连接数 |
| `db_connections_max` | gauge | 最大连接数（配置 `maxShellConns`） |
| `db_connections_total` | counter | 累计建立连接数 |
| `db_connections_rejected_total` | counter | 累计拒绝连接数 |

> 埋点位置：`gateway/GWTcp.cpp::GWTcpServer::onConnection`（TCP 连接建立/断开）。

### 4.2 查询

| 指标 | 类型 | 标签 | 说明 |
| --- | --- | --- | --- |
| `db_queries_total` | counter | `type = select \| insert \| ddl \| show \| use \| fetch \| meta \| other` | 累计查询数 |
| `db_query_errors_total` | counter | `error_type = invalid \| not_found \| auth \| timeout \| internal` | 累计错误数 |
| `db_slow_queries_total` | counter | — | 累计慢查询数 |
| `db_query_duration_seconds` | histogram | — | 查询延迟分布 |
| `db_query_rows_returned` | histogram | — | 返回行数分布 |
| `db_qps` | gauge | — | 滚动采样 QPS |

> `type` 标签对齐 `query/QueryAst.h::StmtType`：EtherDB 当前仅支持
> SELECT / INSERT / DDL(create/drop/alter) / SHOW / USE，**没有 UPDATE / DELETE**，
> 写路径只有 INSERT。控制类消息（CONNECT / HEARTBEAT）不计入查询。

> 埋点位置：
> - `DServers.cpp::ShellServer::onMessage`：请求计数、认证错误。
> - `DServerWorkerPools.cpp::VReadPool::workerLoop` / `VWritePool::workerLoop`：
>   延迟直方图、错误计数、慢查询判定与入环形缓冲。

### 4.3 系统 / 进程

| 指标 | 类型 | 说明 |
| --- | --- | --- |
| `db_uptime_seconds` | gauge | 运行时长 |
| `db_cpu_usage_ratio` | gauge | CPU 使用率（0~1，系统级） |
| `db_memory_used_bytes` / `db_memory_total_bytes` | gauge | 物理内存（已用/总量） |
| `db_disk_used_bytes` / `db_disk_total_bytes` / `db_disk_used_ratio` | gauge | 数据目录所在卷的磁盘使用 |

> 采集：`MonitorAgent::collectLoop()` 每 `collectIntervalMs`（默认 5s）采样一次。
> CPU 使用率需要两次采样求差，首次调用返回 0。

---

## 5. HTTP 接口（已实现）

监控端口默认 `9187`。

| 方法 | 路径 | 返回 | 说明 |
| --- | --- | --- | --- |
| GET | `/metrics` | Prometheus 文本 | 供 Prometheus 抓取 |
| GET | `/api/v1/status` | JSON | 运行状态摘要 |
| GET | `/api/v1/health` | JSON | `{"status":"ok"}` |
| GET | `/api/v1/metrics/list` | JSON | 所有指标名（Grafana 变量用） |
| GET | `/api/v1/slowlog?n=20` | JSON | 最近 N 条慢查询 |

`/api/v1/status` 示例：

```json
{
  "version": "0.1.0",
  "status": "running",
  "uptime_seconds": 86400,
  "connections": 12,
  "connections_max": 5000,
  "connections_total": 428,
  "connections_rejected_total": 0,
  "qps": 350.5,
  "queries_total": 1200000,
  "errors_total": 37,
  "slow_queries_total": 3,
  "slow_queries_recent": 3,
  "cpu_usage_ratio": 0.21,
  "memory_used_bytes": 7263776768,
  "memory_total_bytes": 8051179520,
  "disk_used_bytes": 104279027712,
  "disk_total_bytes": 128850063360,
  "disk_used_ratio": 0.809305
}
```

### 配置

`etherdb.cfg`：

```ini
[monitor]
enabled = 1            # 0 = 关闭 HTTP 监控
port = 9187            # 监控 HTTP 端口
slowQueryMs = 100      # 慢查询阈值
```

### Prometheus / Grafana

```yaml
scrape_configs:
  - job_name: etherdb
    scrape_interval: 15s
    static_configs:
      - targets: ["127.0.0.1:9187"]
```

常用查询：

```promql
rate(db_queries_total[1m])                       # 按 type 的 QPS
sum(rate(db_queries_total[1m]))                  # 总 QPS
rate(db_query_errors_total[1m])                  # 错误率
histogram_quantile(0.99, rate(db_query_duration_seconds_bucket[5m]))  # P99 延迟
db_connections_current                           # 当前连接数
rate(db_slow_queries_total[5m])                  # 慢查询速率
```

---

## 6. 后续扩展（暂缓）

**当前不实现**基于 query 语句 / 专用查询消息的监控通道。
本阶段监控只通过 HTTP 暴露（`/metrics` 与 `/api/v1/*`），客户端如需监控数据，
可自行访问该 HTTP 端口。

指标注册表（`Metrics.h`）与 HTTP 渲染逻辑是解耦的，将来若要支持客户端 API
直连监控，只需新增一对消息类型并复用同一份注册表，无需改动现有指标埋点。

---

## 7. 当前实现边界（第一阶段）

- 延迟直方图只覆盖读路径（`VReadPool`）与写路径（`VWritePool`）；
  纯元数据/DDL 路径（`MWriteWorker`/`MReadPool`）已计入 `db_queries_total`，
  但暂未计入延迟直方图。后续按需补齐。
- `db_connections_rejected_total` 已预留，目前仅在认证失败时配合错误计数统计；
  连接容量拒绝（超过 `maxShellConns`）的埋点待补充。
- 存储层指标（读写次数/字节/缓存命中）与事务/锁指标属于第二阶段，尚未实现。
- 系统指标为“整机”口径（CPU/内存），如需“进程”口径可后续替换采集实现。

## 8. 实现顺序（与 `monitor.txt` 对齐）

1. ✅ Registry（Counter/Gauge/Histogram/Family）+ `/metrics` 文本输出。
2. ✅ 连接数、QPS、错误数、慢查询数、运行时长、CPU、磁盘、内存。
3. ✅ `/api/v1/status`、`/api/v1/health`、`/api/v1/metrics/list`、`/api/v1/slowlog`。
4. ⏳ Prometheus + Grafana 面板与告警规则。
5. ⏸ 查询消息（客户端 API 直连监控）—— 暂缓，本阶段不做。
6. ⏳ 存储层 / 事务层指标；必要时引入 per-thread 计数器分片。
