
**国产 ARM 服务器（飞腾 D2000）上跑时序数据库：210 万行/秒写入，同机对比 TDengine 实测**
---

## 二、TL;DR：先看结果

**测试机：飞腾 D2000/8（8 核 ARM aarch64，2.3 GHz）+ 31 GB 内存 + 麒麟 Linux。** 客户端与服务端同机（loopback）。EtherDB 与 TDengine **同一台机器、同一份数据模式、同一条 SQL**。

| 测试项 | 规模 | EtherDB | TDengine 2.1.7.2 |
|---|---|---|---|
| 批量写入（10 列定长，列绑定） | 1 万行/批 × 100 轮 = **102 万行** | 平均 **2,146,648 行/秒**，峰值 2,341,920 | 平均 **830,951 行/秒**，峰值 863,260 |
| SQL 文本批量写入（1000 行/批） | 100 轮 = **10 万行** | 平均 **541,549 行/秒** | （未测，两边接口路径不同） |
| 流式查询拉取（`SELECT ts,col_int … WHERE col_int>5`） | **1002 万行**匹配结果 | 11.26 秒 → 约 **89 万行/秒** | **2.12 秒** → 约 **474 万行/秒** |
| 分页 `LIMIT 10 OFFSET 500000` | 同为 **1002 万行**表 | **0.82 ms**（全程平坦） | 133.41 ms（随 OFFSET 线性膨胀） |
| `COUNT(*)` | 1002 万行表 | **0.34 ms** | 122.19 ms |
| 功能回归 `full_test` | 41 项断言 | **41 PASS / 0 FAIL** | — |
| 运行时内存 | 分别持有 6124 万 / 1002 万行 | RSS **233 MB** | taosd RSS **314 MB** |
| 服务端二进制 | — | **1.07 MB**（单文件，零外部数据库依赖） | 2.79 MB（另有 **10.03 MB** 客户端动态库） |
| 同规模数据落盘（1002 万行） | — | **22 MB**（数据文件 6.7 MB） | **39 MB**（数据文件 7.4 MB） |

**先划重点，再展开：**

- **写入、分页、聚合、磁盘、二进制体积：EtherDB 赢。** 写入快约 2.6 倍；`OFFSET` 由于是索引跳转，**从 0 加到 50 万耗时纹丝不动**（0.76~0.87 ms），TDengine 同用例要 133 ms（相差约 160 倍）；`COUNT(*)` 差约 360 倍。
- **全量取数（fetch 全部行）：TDengine 赢得很干脆。** 1002 万行两列，TDengine 2.12 秒拉完，EtherDB 要 11.26 秒。**这一项 EtherDB 输了约 5 倍**，原因我在第七节里拆开讲（既有客户端 API 语义差异，也有服务端批处理差距），不找借口。
- 两边都还是「一个人的项目 vs 一家公司 20 年的积累」——**这份对比的意义是把差距量化，不是宣布胜利。**

---

## 三、测试环境（先摆环境，再谈跑分）

### 3.1 硬件

| 项目 | 配置 |
|---|---|
| CPU | 飞腾 Phytium D2000/8（8 核 ARM aarch64，8C/8T，2.3 GHz，无超线程） |
| 内存 | 31 GB（服务器是国产化整机，内存比板子本身宽裕） |
| 操作系统 | 麒麟 KylinOS（Linux 5.4.18 aarch64） |
| 网络 | 客户端与服务端**同一台机器**（loopback） |
| 磁盘 | 本机 SSD（数据量小，非瓶颈；未单独标注型号） |

### 3.2 软件与配置

| 项目 | 说明 |
|---|---|
| EtherDB 服务端 | `etherdb_dserver`，Release 构建（g++ `-O2`，aarch64） |
| EtherDB 服务端线程配置 | `numCores = 4`，`threadsPerCore = 1`，`queryCoreRatio = 0.5`（`/etc/etherdb/etherdb.cfg`） |
| EtherDB 客户端 | 官方 C++ SDK（`EtDBClient.cpp`）与 C API（`etdb.h`），同机编译 |
| TDengine | **2.1.7.2**（本机实际部署版本，默认配置，`taosd` 监听 6030） |
| TDengine 客户端 | 官方 `libtaos.so`（10 MB 动态库）+ 官方 `taos_perf_query` 取数方式 |
| 数据精度 | EtherDB 库 `PRECISION us`；TDengine 库 `PRECISION 'us'` |
| 表结构 | `ts TIMESTAMP, col_int INT, col_bigint BIGINT, col_float FLOAT, col_double DOUBLE, col_smallint SMALLINT, col_tinyint TINYINT, col_bool BOOL, col_extra1 BIGINT, col_extra2 DOUBLE`（定长合计 52 字节/行） |
| 数据生成 | `col_ts = baseTs + offset + i`（微秒递增），`col_int = offset + i`（严格递增），其余列按固定公式生成——**两边使用完全相同的数据生成公式** |

**关于 TDengine 版本的说明**：本机部署的是 2.1.7.2（2020 年版本）。我知道 3.x 快很多，但**没有部署在本机的东西我不测、不引用**。这份对比只对「本机现在能跑的 TDengine」有效。

**说明**：单机自测，客户端和服务端共享同一块 CPU、内存和磁盘，所以这个成绩是「保守值」。两边都是同样的单机条件，对比是公平的。

---

## 四、实测数据

### 4.1 写入：列绑定批量插入，100 万行

测法：真正的**列绑定**（columnar binding）——把整列数据一次性绑到预编译语句上，一次 `execute` 提交一整批 10000 行，预热 2 轮后连跑 100 轮。

```
=== 4. Statistics Summary ===
| Test rounds:          100                           |
| Rows per batch:      10000                           |
| Total rows inserted: 1000000                         |
| Throughput (rows/s):                                |
|   Average:              2146648                     |
|   Min:                  1508751                     |
|   Max:                  2341920                     |
|   Std dev:               133884                     |
| Throughput (MB/s):                                  |
|   Average:               111.63                     |
| Average latency/batch:       4.68 ms                |
```

逐轮曲线（节选，行/秒）：

| 轮次 | 吞吐 | 轮次 | 吞吐 | 轮次 | 吞吐 |
|---|---|---|---|---|---|
| 1 | 2,181,501 | 20 | 2,174,386 | 77 | 2,243,158 |
| 4 | 2,226,676 | 33 | 2,194,426 | 88 | 2,076,412 |
| 8 | 2,069,536 | 50 | 1,921,968 | 99 | 2,262,443 |

**关于抖动**：`Std dev` 13.4 万（占均值 6%），最低 150.9 万、最高 234.2 万。相比上一轮在 15W 低压笔记本（i7-8550U）上测得的 132 万行/秒均值、65 万标准差，**D2000 上不仅更快，而且曲线平得多**——服务器级的散热和供电让节流基本消失。偶尔掉到 150 万的那几轮，是后台提交线程刷盘与写入线程抢 CPU 造成的，不是数据库在抽风。

再给一个更有说服力的数字：**2 轮预热 + 1000 轮正式（共 1002 万行，全新建表）**

```
| Test rounds:         1000                           |
| Total rows inserted: 10000000                        |
| Throughput (rows/s):                                |
|   Average:              2105169                     |
|   Min:                   550358                     |
|   Max:                  2341372                     |
|   Std dev:               180457                     |
| Average latency/batch:       4.82 ms                |
```

**连续压 1002 万行，平均仍保持 210.5 万行/秒**（中位数 214.5 万）——没有随数据量增长而衰减。最低那一轮 55 万是提交线程做了一次大 flush，整场就一次。

**数据一致性**：插入结束后 `SELECT COUNT(*)` 逐项校验通过：

```
=== 5. Data Validation ===
  SELECT COUNT(*): 10020000 rows
  [OK] data integrity verified 10020000 - 10020000
```


### 4.2 SQL 文本批量写入：客户端解析路径

除了列绑定，另一个真实场景是**直接发 SQL 文本**（MySQL 客户端习惯）：客户端把 1000 行拼成一条 `INSERT INTO ... VALUES (...),(...);` 整串发过去，由服务端解析。每批 1000 行、跑 100 轮：

| 指标 | 数值 |
|---|---|
| 平均吞吐 | **541,549 行/秒** |
| 峰值 / 最低 | 585,138 / 428,449 行/秒 |
| 平均每批延迟 | 1.85 ms |
| 平均带宽 | 28.16 MB/s |

比列绑定（214 万行/秒）慢约 4 倍，**这很正常**：SQL 文本路径每行要过一遍字符串拼接（客户端）和词法/语法解析（服务端），绑定路径则是紧凑二进制直发。两条路径都保留、都诚实报数——用哪种取决于你怎么写客户端代码。顺带说一句，这个路径此前专门做过一轮流式解析优化（零拷贝切分 SQL 子串 + 自定义快速数值转换），比优化前快了约 2.1 倍。

### 4.3 流式查询：1002 万行怎么拉

这个测试**刻意对齐了 TDengine 官方示例的取数方式**：计时从 `query` 之前开始，到所有行被取完为止，中间包含网络往返、服务端执行、客户端反序列化和逐行复制——也就是说，这是**端到端**的取数耗时，不是服务端内部耗时。

```sql
SELECT ts,col_int FROM perftest20.perf_data_3 WHERE col_int>5
```

表里 1002 万行，命中 10,019,994 行（不是抽样，是全量拉完）：

```
query() 首批耗时 :   141.98 ms (网络RTT + 服务端执行 + 首批发序列化)
流式拉取总耗时   : 11120.40 ms (后续 FETCH 批次 + fetchRow 逐行复制)
总耗时           : 11262.38 ms , total records 10019994
  [OK] 行数一致: 10019994
```

**11.26 秒拉完 1002 万行 → 约 89 万行/秒**（两列，逐行回调，单线程客户端）。C API 的 `etdb_fetch_block`（批量接口）实测 11.55 秒、`etdb_fetch_row`（逐行接口）12.59 秒，三种用法基本一个量级。

值得单独说的是那 141.98 ms 的「首批耗时」：它不是等整个结果集算完才返回，而是服务端分块产出、客户端收到第一批就能开始消费——**首字节延迟和总吞吐是两件独立的事**。

> ⚠️ **预告**：同一张表、同一条 SQL，TDengine 2.1.7.2 只用 **2.12 秒**。这一项 EtherDB 输了约 5 倍。原因拆解见第七节——我不打算藏这个数字。

### 4.4 分页与聚合：OFFSET 越大越慢？这里不是

**1002 万行表**（与 TDengine 对比的同一张表），`LIMIT 10` 配不同 `OFFSET`，每项跑 3 次取平均：

| 测试 | 平均耗时 | 行数 |
|---|---|---|
| OFFSET 0 | 0.87 ms | 10 |
| OFFSET 1000 | 0.79 ms | 10 |
| OFFSET 10000 | 0.82 ms | 10 |
| OFFSET 100000 | 0.76 ms | 10 |
| OFFSET 300000 | 0.82 ms | 10 |
| OFFSET 500000 | 0.82 ms | 10 |
| COUNT(*) | 0.34 ms | 1 |
| COUNT(*) WHERE col_int < 5 | 4.10 ms | 1 |
| SELECT ... WHERE col_int < 5 LIMIT 10 | 8.91 ms | 5 |

换一张更大的表（**3118 万行**）再跑一遍同组用例，结果依然是平的：

| 测试 | 平均耗时（3118 万行表） |
|---|---|
| OFFSET 0 → 500000 全程 | 1.37 ~ 1.68 ms（无衰减） |
| COUNT(*) | 0.63 ms |
| COUNT(*) WHERE col_int < 5 | 15.63 ms |
| SELECT ... WHERE col_int < 5 LIMIT 10 | 18.02 ms |

**OFFSET 从 0 加到 50 万，耗时基本是平的，表大了 3 倍也一样。** 原因：时间戳本身就是天然索引，块级 SMA 又能把不相关的块整块剪掉，不需要「先扫过前 N 行再丢掉」。对比之下，同样的用例在 TDengine 2.1.7.2 上 `OFFSET 500000` 要 133 ms——**差约 160 倍**（数据见第七节）。

### 4.5 功能回归：41 项断言全绿 + 类型用例 23 项

性能再猛，SQL 是错的也没用。**整套代码在 D2000 上从源码重新编译（aarch64），`full_test` 覆盖 DDL/DML/查询/聚合/排序/C API 的完整生命周期：**

- `CREATE / DROP DATABASE`（含 `KEEP`、`REPLICA`）、`CREATE / DROP TABLE`（含 `IF NOT EXISTS` 幂等）
- `SHOW DATABASES`、`SHOW TABLES`、`DESCRIBE`、`USE`
- `INSERT ... VALUES` 单行写入并回查
- `SELECT *`、`WHERE` 过滤、`COUNT / AVG / MAX / MIN` 聚合、`ORDER BY ... DESC LIMIT`
- C API（`etdb.h`）查询与 prepared statement 写入

```
║   PASSED: 41                               ║
║   FAILED: 0                                ║
║   TOTAL:  41                               ║
```

另一组类型测试（`typed_types_test`）在本机 23 项全过，包括 **`UNSIGNED BIGINT` 最大值 18446744073709551615**、中文 `NCHAR('世界')`、`BINARY` 文本存取与过滤——这些是时序场景里最容易踩坑的边角，ARM 上同样全绿。

---

## 五、为什么能跑到这个数：几个工程决定

### 5.1 写入路径：内存表 + 提交队列 + WAL

```
INSERT → MemTable(skip-list) → commit queue → 后台提交线程
                                    ↓
                            WAL 记录 + 按天分区列式文件
```

- 写入先落**跳表 MemTable**，不在写路径上做任何磁盘随机 IO；
- 提交由**独立后台线程**完成，mem / imem 双缓冲，落盘期间写入不阻塞；
- **WAL** 保证崩溃可重放，并可按 WAL 大小异步触发提交，也支持同步提交模式。

### 5.2 存储格式：列式 + 时间分区 + 两阶段压缩

- 数据**按时间有序、列式**存放于按天分区的文件中，范围扫描和列投影天然高效；
- 压缩分两阶段：**第一阶段**用类型特化算法（布尔位打包、整数 delta + zigzag + Simple8B 风格位打包、浮点、字符串），**第二阶段**可选 **LZ4**；可配置为「关闭 / 单阶段 / 两阶段」；
- 本次测试中，单独建库写入的 **1002 万行数据文件只有 5.9 MB**（外加 SMA 0.8 MB、索引 30 KB。按定长 52 字节/行估算，逻辑原始数据约 521 MB）。

### 5.3 查询加速：块级 SMA 剪枝

每个数据块都带预计算的 **min/max/sum** 聚合（`.sma` 文件）。这让 `SUM / MIN / MAX / COUNT` 有快速路径，也能让 `WHERE` 过滤器**整块丢弃**不匹配的数据块——`COUNT(*)` 只要 0.34 ms（纯读块索引），带列过滤的 `COUNT(*) WHERE col_int < 5` 也只有 4.10 ms（SMA 让大部分块根本不用被解码）。

### 5.4 客户端 SDK：流式 + 背压

C 接口 + 面向对象的 C++ 接口，纯头文件加预编译库，**不需要服务器任何源码**。支持：预编译语句、参数绑定、行式与列式批量写入、**带背压的异步批量**、**流式取数**（逐行 / 逐块）。

### 5.5 架构分层

`dnode / dbnode / vgroup` 三层模型（受 TDengine 启发），自研 RPC 与网络层：Linux 上 epoll，Windows 上 IOCP，线程模型由配置驱动。

---

## 六、资源占用：这部分可能比跑分更离谱

| 指标 | 实测值 |
|---|---|
| 服务端可执行文件 | **1.07 MB**（单文件，aarch64） |
| 交互式 shell 客户端 | 627 KB |
| 客户端静态库 | **224 KB**（头文件 + 静态库即可开发） |
| 持有 6124 万行时进程 RSS | **233 MB**（峰值即 233 MB；持有 5122 万行时测得 172 MB） |
| 全库落盘占用（含索引与 WAL） | **76 MB** |
| ├ perftest20 库（5122 万行） | 55 MB |
| └ diskbench 库（1002 万行，独立数据集） | 22 MB |
| 其中（全库）`.dat` 数据文件 | 31.8 MB |
| ├ `.head` 块索引 | 161 KB |
| ├ `.sma` 聚合 | 4.3 MB |
| └ WAL | 35 MB |

数据库总量（本次测试后）：

| 表 | 行数 |
|---|---|
| `perftest20.perf_data_1` | 31,182,000 |
| `perftest20.perf_data_2` | 10,020,000 |
| `perftest20.perf_data_3` | 10,020,000 |
| `diskbench.perf_data_1` | 10,020,000 |
| **合计** | **61,242,000** |

**6124 万行、233 MB 常驻内存、1 MB 的服务端。** 没有 JVM 预热，没有一堆 `.so`，拷贝一个二进制就能跑——这依然是它最吸引我的地方。

> 关于内存多说一句：RSS 会随测试阶段在 170 ~ 240 MB 之间波动（内存表、查询缓冲、多线程 arena），不是随行数线性增长的「数据常驻」——数据是列式压缩存在文件里的。

内置 HTTP 监控端点，直接吐 Prometheus 格式，不用额外装 agent：

```
$ curl http://127.0.0.1:9187/api/v1/health
{"status":"ok"}

$ curl http://127.0.0.1:9187/metrics
# HELP db_connections_total Total number of accepted client connections
db_connections_total 43
# HELP db_slow_queries_total Total number of queries slower than the threshold
db_slow_queries_total 0
...
```

另有 `/api/v1/status`、`/api/v1/metrics/list`、`/api/v1/slowlog`。

---

## 七、和 TDengine 比怎么样？——这一轮终于比上了

上一版文章的遗憾是「TDengine 在 Windows 上没部署成功，所以没比」。这次的测试机（麒麟 Linux / ARM）上**本来就跑着 TDengine 2.1.7.2**，条件齐了，直接同机对比。

- **同一台机器**：飞腾 D2000，客户端/服务端全在 loopback；
- **同一份数据**：两边用同一套数据生成公式（微秒递增时间戳 + 严格递增 `col_int`），10 列定长 52 字节/行；
- **同一条 SQL**：`SELECT ts,col_int FROM perf_data WHERE col_int>5`，全量拉完，两边行数一致（10,019,994）；
- **同一测法**：写入都是预编译语句**列绑定**，10000 行/批；查询都是**官方客户端**端到端计时（t0 在 query 之前，t1 在最后一行取完之后）；
- TDengine 版本是本机实际部署的 **2.1.7.2**（2020 年发布，默认配置）。

### 7.1 写入：EtherDB 快约 2.6 倍

| 测试 | EtherDB | TDengine 2.1.7.2 |
|---|---|---|
| 列绑定写入（1 万行/批） | 平均 **2,146,648 行/秒**（2+100 轮） | 平均 **830,951 行/秒**（2+50 轮） |
| 长跑（2+1000 轮，1002 万行） | 平均 **2,105,169 行/秒**（新表） | 平均 **816,893 行/秒**（新表） |
| 单批延迟 | 4.7 ~ 4.8 ms | 12.0 ~ 12.8 ms |
| 波动（标准差/均值） | 6% ~ 9% | 3% ~ 18% |

两边长跑 1000 轮的平均值都只比短跑略低，说明**双方都没有明显的写入衰减**；差距（约 2.6 倍）主要来自写入路径设计——EtherDB 的写路径只进内存表和 WAL，落盘全交给后台线程，客户端 `execute` 的等待时间明显更短。

### 7.2 取数：TDengine 快 5 ~ 9 倍（这一项 EtherDB 输了）

| 取数方式（1002 万行 × 2 列） | EtherDB | TDengine 2.1.7.2 |
|---|---|---|
| 逐行接口（`fetchRow` / `taos_fetch_row`） | 11.26 s（C++）/ 12.59 s（C） | **2.12 s** |
| 批量接口（`etdb_fetch_block` / `taos_fetch_block`） | 11.55 s | **1.21 s** |
| 折算吞吐 | 约 80 ~ 89 万行/秒 | **474 万 / 826 万行/秒** |

**这个差距是真实存在的，不找角度修饰。** 拆开看，差距主要来自两处：

1. **客户端 API 语义不同（大头）**：`taos_fetch_row/block` 返回的是**指向客户端内部块缓冲的指针**，遍历时不发生数据拷贝；EtherDB 的 `fetchRow` 是把每个值**物化成类型化对象**再交给调用方（`std::vector<Value>`），逐值构造/拷贝的成本在 1000 万行规模上非常可观。`etdb_fetch_block` 虽然减少了往返次数，但依然会在内存中物化整块。
2. **服务端每批「解码 + 序列化」路径**：EtherDB 的 FETCH 批按字节预算打包（本次约 130 KB/批、77 个往返），每次都要走一遍块解码与序列化；TDengine 的块协议按固定行块（约 3300 行/块、3022 块）流水线推进。（注：EtherDB 服务端日志显示每 5.8 万行解码+序列化约 35 ms，服务端不是主要瓶颈。）

**已经写进 To-Do 的优化方向**：给 SDK 增加「零拷贝块游标」（返回块内指针 + 类型步长，像 taos 那样直接遍历），预计能吃掉差距的一大半。做完会更新本文。

### 7.3 分页 / 聚合：EtherDB 快 160 ~ 360 倍（TDengine 这一项很吃亏）

同样 1002 万行表、同一批用例（各跑 3 次取平均，C API 客户端内计时）：

| 用例 | EtherDB | TDengine 2.1.7.2 |
|---|---|---|
| `LIMIT 10 OFFSET 0` | 0.87 ms | 2.90 ms |
| `LIMIT 10 OFFSET 10000` | 0.82 ms | 4.71 ms |
| `LIMIT 10 OFFSET 100000` | 0.76 ms | 28.02 ms |
| `LIMIT 10 OFFSET 300000` | 0.82 ms | 80.70 ms |
| `LIMIT 10 OFFSET 500000` | **0.82 ms** | **133.41 ms** |
| `COUNT(*)` | **0.34 ms** | **122.19 ms** |
| `COUNT(*) WHERE col_int < 5` | 4.10 ms | 195.67 ms |
| `WHERE col_int < 5 LIMIT 10` | 8.91 ms | 436.09 ms |

- **EtherDB 的 OFFSET 是「索引跳转」**：时间戳本身有序 + 块级 min/max 索引，OFFSET 加多少都不需要扫过前面的行，所以耗时是平的（0.8 ms）。
- **TDengine 2.x 的 OFFSET 是「扫描跳过」**：先读块再丢弃，每加 10 万偏移就多约 25 ms，`OFFSET 500000` 要 133 ms。
- `COUNT(*)` 差距最大：EtherDB 直接读块索引（0 数据 I/O），0.34 ms 完事；TDengine 2.1.7.2 会实打实地扫，122 ms。

**但也要说公道话**：（1）这些是「极快路径」的对决，绝对耗时都不大，业务里更常见的可能是带条件的长时段聚合，两边都要扫数据，差距会小得多；（2）TDengine 3.x 的查询引擎（尤其索引和执行器）比 2.x 换了代，本表不代表它现在的水平。

### 7.4 资源与体积：EtherDB 全面占优

| 项目 | EtherDB | TDengine 2.1.7.2 |
|---|---|---|
| 同规模数据落盘（1002 万行） | **22 MB**（其中数据文件 5.9 MB + SMA 0.8 MB，WAL 15.6 MB） | **39 MB**（其中数据文件 7.3 MB，WAL 33 MB） |
| 服务端进程 RSS | **233 MB**（持有 6124 万行） | **314 MB**（taosd 进程，另承载其他库） |
| 服务端二进制 | **1.07 MB** | 2.79 MB |
| 客户端库 | 静态库 **224 KB** | 动态库 **10.03 MB**（`libtaos.so`） |
| 服务端线程数 | 21 | 73 |

两边数据文件的压缩水平接近（5.9 MB vs 7.3 MB，EtherDB 略小）；差距主要在 **WAL 策略**（EtherDB 本次常驻约 15.6 MB，TDengine 33 MB）和**部署体积**：**1 MB 单文件 vs 13 MB（taosd + libtaos）**。

### 7.5 结论（打平？并没有——各有胜负，看你用在哪儿）

| 维度 | 赢家 | 差距 |
|---|---|---|
| 写入吞吐 | **EtherDB** | ~2.6× |
| 全量取数（fetch all） | **TDengine** | ~5 ~ 9× |
| 分页 / 点查 / COUNT | **EtherDB** | 160 ~ 360×（且不随 OFFSET 衰减） |
| 磁盘占用（同数据） | **EtherDB** | ~1.8×（含 WAL） |
| 部署体积 | **EtherDB** | ~12× |
| 成熟度、生态、分布式 | **TDengine** | 不是一个量级的对手 |

**适用面一句话**：如果你是「写入量大、查询模式以范围/分页/聚合为主、部署资源受限」的场景，EtherDB 的设计是有优势的；如果你需要**大结果集高速导出、成熟生态、分布式与运维体系**，TDengine（尤其新版本）依然是更稳的选择。

**最后照例声明**：TDengine 是本机 2.1.7.2 老版本 + 默认配置，测试数据是合成顺序数据（对两边的压缩和聚合都友好），单机 loopback 环境。**这些数字只对「这台机器、这个版本、这份数据」负责。**

---

## 八、怎么上手

```bash
# 1. 启动服务端
./src/bin/etherdb_dserver -p 7040 -d ./etdb_data

# 2. 连上交互式 shell
./src/bin/etherdb -h 127.0.0.1 -p 7040 -u root -P etherdbdata
```

```sql
CREATE DATABASE testdb;
USE testdb;
CREATE TABLE sensor (ts TIMESTAMP, temperature FLOAT, humidity FLOAT);
INSERT INTO sensor VALUES(1716364800000, 25.5, 60.2);
SELECT * FROM sensor WHERE temperature > 25.0;
SELECT COUNT(*), AVG(temperature), MAX(temperature) FROM sensor;
```

**SQL 是 MySQL / TDengine 风格的**，`WHERE / GROUP BY / HAVING / ORDER BY / LIMIT / OFFSET / DISTINCT` 加聚合函数，没有需要现学的新查询语言。

Windows 用户：客户端 shell 与 SDK 都是现成的（服务端也支持 Windows 构建）；本次全量实测在 Linux（ARM）上完成。

---

## 九、适合谁，不适合谁

**适合**：

- 想要**能读懂全部源码**的时序数据库，而不是一个黑盒
- 边缘 / 嵌入式 / 单机场景：资源受限，装不了 JVM 和一堆依赖，拷贝一个二进制就要能用
- 千万级到亿级行的单机时序数据，需要 SQL 但不想要运维复杂度
- 想学存储引擎、压缩算法、LSM / 列存实现的人（代码分层清晰，`src/etdb` 和 `src/query` 值得一读）

**不适合**：

- 需要成熟生态、成熟监控告警体系、大厂背书和 SLA 的生产核心系统
- 多节点分布式强一致集群（目前是单机为主，`dnode / dbnode / vgroup` 分层已经铺好，但集群能力还早）
- 指望开箱即用的 BI 连接器和可视化面板

**这是一个人的项目，不是一家公司的产品。** 用之前请自己评估，别把它当生产级方案。

---

## 十、开源信息

- **许可证**：BSL 1.1（源码可见），2030-10-12 自动转为 **Apache License 2.0**；变更日期前的商业使用可另行洽谈授权
- **语言**：C++17，从零实现，无第三方数据库引擎依赖（仅内置 LZ4）
- **平台**：服务端 Linux / Windows；客户端 shell 与 SDK 支持 Linux / Windows（本次全量实测为 Linux ARM）
- **联系**：kinyi6666@gmail.com

如果这个项目对你有帮助，欢迎 star、提 issue、或者直接来喷我的索引设计——**能被验证的批评比夸奖有用得多。**

---

## 附录：原始测试输出摘录

### A. 批量写入（`perf_insert_test`，2+100 轮）

```
  +-----------------------------------------------------+
  | Pre-bound (column binding) Insert Performance       |
  | Test Results                                        |
  +-----------------------------------------------------+
  | Test rounds:          100                           |
  | Rows per batch:      10000                           |
  | Total rows inserted: 1000000                         |
  +-----------------------------------------------------+
  | Throughput (rows/s):                                |
  |   Average:              2146648                     |
  |   Min:                  1508751                     |
  |   Max:                  2341920                     |
  |   Std dev:               133884                     |
  +-----------------------------------------------------+
  | Throughput (MB/s):                                  |
  |   Average:               111.63                     |
  |   Min:                    78.46                     |
  |   Max:                   121.78                     |
  |   Std dev:                 6.96                     |
  +-----------------------------------------------------+
  | Average latency/batch:       4.68 ms                |
  +-----------------------------------------------------+

=== 5. Data Validation ===
  SELECT COUNT(*): 31080000 rows
  [WARN] row count mismatch 31080000 - 1020000
```

> 注：`[WARN] row count mismatch` 是测试脚本自身的口径问题——它只统计正式轮次（100×10000），而预热 2 轮（2×10000）也会落库，表从 30,060,000 增至 31,080,000，**增量恰好 1,020,000，数据本身一致**。

### B. 批量写入长跑（`perf_insert_test 7040 1000`，2+1000 轮，新表）

```
  | Test rounds:         1000                           |
  | Rows per batch:      10000                           |
  | Total rows inserted: 10000000                        |
  | Throughput (rows/s):                                |
  |   Average:              2105169                     |
  |   Min:                   550358                     |
  |   Max:                  2341372                     |
  |   Std dev:               180457                     |
  | Average latency/batch:       4.82 ms                |

=== 5. Data Validation ===
  SELECT COUNT(*): 10020000 rows
  [OK] data integrity verified 10020000 - 10020000
```

### C. 流式查询（`streaming_perf_test`，对齐 `taos_perf_query`）

```
  [INFO] 预期匹配行数: 10019994
query() 首批耗时 :   141.98 ms (网络RTT + 服务端执行 + 首批发序列化)
流式拉取总耗时   : 11120.40 ms (后续 FETCH 批次 + fetchRow 逐行复制)
总耗时           : 11262.38 ms , total records 10019994
(taos_perf_query 输出格式: query time 11262.38 ms , total records 10019994)
  [OK] 行数一致: 10019994
```

C API 两种取数方式（`c_streaming_test`，同一张表同一条 SQL）：

```
=== 批量流式取数 (etdb_fetch_block) ===
  [INFO] 共 10019994 行, 77 批, 耗时 11551.11 ms
=== 逐行流式取数 (etdb_fetch_row) ===
  [INFO] 共 10019994 行, 耗时 12592.90 ms
```

### D. 分页与聚合（`query_perf_test`）

3118 万行表（`perf_data_1`）：

```
测试           | 耗时(ms) | 平均(us) | 行数
-----------------+----------+------------+-------
OFFSET 0         |     1.48 |       1482 |     10
OFFSET 1000      |     1.46 |       1459 |     10
OFFSET 100000    |     1.50 |       1501 |     10
OFFSET 300000    |     1.40 |       1398 |     10
OFFSET 500000    |     1.45 |       1447 |     10
COUNT(*)         |     0.62 |        625 |      1
COUNT col_int<5  |    15.63 |      15630 |      1
WHERE col_int<5  |    18.02 |      18023 |     10
```

1002 万行表（`perf_data_3`，与 TDengine 同规模的表）：

```
测试         | 平均(us) | 行数
---------------+------------+-------
OFFSET 0       |        865 |     10
OFFSET 1000    |        791 |     10
OFFSET 10000   |        820 |     10
OFFSET 100000  |        758 |     10
OFFSET 300000  |        820 |     10
OFFSET 500000  |        821 |     10
COUNT(*)       |        344 |      1
COUNT col<5    |       4102 |      1
WHERE col<5    |       8905 |      5
```

### E. 功能回归（`full_test` + `typed_types_test`）

```
║   PASSED: 41                               ║
║   FAILED: 0                                ║
║   TOTAL:  41                               ║
```

```
[PASS] u2=18446744073709551615 (uint64)
[PASS] n1='世界'
[PASS] b1='hello world'
...
=== 全部通过 (0 failures) ===   （typed_types_test，23 项）
```

### F. SQL 文本写入（`perf_insert_test_string`，1000 行/批）

```
  | Test rounds:          100                           |
  | Rows per batch:      1000                           |
  | Total rows inserted: 100000                         |
  | Throughput (rows/s):                                |
  |   Average:               541549                     |
  |   Min:                   428449                     |
  |   Max:                   585138                     |
  |   Std dev:                29339                     |
  | Average throughput (MB/s):  28.16                   |
  | Average latency/batch:       1.85 ms                |
```

> 注：末尾 `[WARN] row count mismatch` 同样是脚本口径问题（只统计正式轮次，预热 2 轮 ×1000 行也落库，增量恰好 102,000，数据一致）。

### G. TDengine 侧原始输出（2.1.7.2）

官方 `taos_perf_insert_test`（2+50 轮）：

```
  │ 测试轮数:         50                               │
  │ 每批行数:       10000                               │
  │ 总插入行数:     500000                             │
  │ Throughput (rows/s):                                │
  │   平均:              830951                           │
  │   最小:              749963                           │
  │   最大:              863260                           │
  │   标准差:             22468                           │
  │ 平均延迟/批:       12.04 ms                          │
=== 5. 数据验证 ===
  SELECT COUNT(*): 520000 rows
  [OK] 数据完整性验证通过 (expected 520000)
```

取数（`taos_bench query` / `taos_bench blockquery`，同一张 1002 万行表、同一条 SQL）：

```
query time  2116.21 ms , total records 10019994
  [OK] row count 10019994 (expected 10019994)

block fetch time  1212.84 ms , total records 10019994 (blocks=3022)
  [OK] row count 10019994 (expected 10019994)
```

分页/聚合（C API 内计时，同表同用例）：

```
test           |         ms |     rows
---------------+------------+---------
OFFSET 0       |       2.90 |       10
OFFSET 1000    |       2.28 |       10
OFFSET 10000   |       4.71 |       10
OFFSET 100000  |      28.02 |       10
OFFSET 300000  |      80.70 |       10
OFFSET 500000  |     133.41 |       10
COUNT(*)       |     122.19 | 10020000
COUNT col<5    |     195.67 |        5
WHERE col<5    |     436.09 |        5
```
