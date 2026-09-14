EtherDB

***840KB Bin,8MB RAM, 1.8M writes/sec, 1.5M rows/sec fetch on ARM D2000**

EtherDB 是一个高性能、SQL 驱动的时序数据库（TSDB），使用 C++17 从零编写。
它以高速摄取、存储和查询海量时间戳数据，使用熟悉的 SQL 而非专有查询语言 —— 同时保持代码库小巧、透明，且不依赖任何第三方数据库引擎。
📄 许可证：BSL 1.1（源码可见）
🧩 服务端：Linux · 💻 客户端 Shell 与 SDK：Linux / Windows
📦 零数据库依赖（仅捆绑 LZ4）

✨ 为什么选择 EtherDB
SQL 优先，零学习成本	类 MySQL / TDengine 的 SQL。无需学习特殊查询语言 —— 几分钟内从零开始查询。
专为时序数据设计	数据按时间有序、列式存储在按天分区的文件中，因此在数十亿行数据上，范围扫描、投影和聚合依然保持快速。
高性能存储流水线	跳表 MemTable → 无锁提交队列 → 后台提交线程刷写到追加写的磁盘文件。查询在提交发生的同时仍可继续读取（mem/imem 双缓冲）。
聚合加速（SMA）	每个数据块携带预计算的 min/max/sum 聚合值（.sma），为 SUM / MIN / MAX / COUNT 提供快速路径，并允许查询引擎根据 WHERE 过滤条件整块裁剪。
多阶段列式压缩	阶段 1 = 类型专用算法（布尔位打包，整数 差分 + zigzag + Simple8B 风格位打包，浮点/双精度，字符串）；阶段 2 = 可选 LZ4。可配置关闭 / 单阶段 / 双阶段。
崩溃安全设计	预写日志（WAL） 及崩溃回放；由 WAL 大小触发的异步提交，以及同步提交模式。
丰富的 C/C++ 客户端 SDK	纯 C API + 面向对象的 C++ API，以头文件 + 预编译库形式提供：预编译语句、参数绑定、行式及列式批量插入、带背压的异步批量、流式获取（行/块）。
交互式 Shell	控制台客户端，支持 readline 编辑、元命令（\?、\q、\d、\l、\c）以及表格 / CSV 输出。
清晰的分层架构	dnode / dbnode / vgroup 模型（受 TDengine 启发），自研 RPC + 网络层（Linux 上 epoll，Windows 上 IOCP），配置驱动线程模型。
🏗 架构
text
┌──────────────┐      ┌──────────────┐
│  etherdb     │      │  C / C++     │   客户端
│  shell       │      │  SDK (lib)   │
└──────┬───────┘      └──────┬───────┘
       │      SQL over TCP   │
       └──────────┬──────────┘
                  ▼
        ┌────────────────────┐
        │   DServer (服务端)   │   epoll/IOCP，工作线程池，RPC
        └─────────┬──────────┘
        ┌─────────┴──────────┐
        ▼                    ▼
  dbNode (dbId=1)       dbNode (dbId=2)
        │                    │
        ▼                    ▼
   ETDB 存储仓库       ETDB 存储仓库
写入路径： INSERT → MemTable（跳表）→ 提交队列 → 后台提交（交换 mem → imem）→ WAL + 追加写入按时间分区的列式文件。
查询路径： SQL → 递归下降解析器 → 计划器/执行器 → 存储读取器，支持时间范围下推、列投影、块级裁剪，以及 K 路流式合并。
每个 dbnode 的数据文件集存储为：
v<dbId>f<fileId>.data（压缩列式块）、.head（块索引/元数据）、.sma（每块聚合值），以及 WAL。

🚀 快速开始（Linux）
前置条件
Linux，C++17 编译器（GCC ≥ 8），CMake ≥ 3.10，pthread。
1. 构建服务端
bash
# 基础库 → src/bin/libbase.so
cd src/base
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
# dnode 服务端 → src/bin/etherdb_server
cd ../dnode
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
2. 启动服务端
bash
./src/bin/etherdb_server -p 7040 -d ./etdb_data
3. 使用交互式 Shell 连接
bash
# 构建 Shell 客户端 → src/bin/etherdb
cd src/client
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j

cd ../..
./src/bin/etherdb -h 127.0.0.1 -p 7040 -u root -P etherdbdata
试一试：

sql
CREATE DATABASE testdb;
USE testdb;
CREATE TABLE sensor (ts TIMESTAMP, temperature FLOAT, humidity FLOAT);
INSERT INTO sensor VALUES(1716364800000, 25.5, 60.2);
INSERT INTO sensor VALUES(1716364860000, 26.1, 59.0);
SELECT * FROM sensor WHERE temperature > 25.0;
SELECT COUNT(*), AVG(temperature), MAX(temperature) FROM sensor;
SELECT * FROM sensor ORDER BY temperature DESC LIMIT 5;
🗄 支持的 SQL 与数据类型
DDL — CREATE / DROP DATABASE（支持 KEEP、REPLICA 参数），CREATE / DROP TABLE，
SHOW DATABASES，SHOW TABLES，DESCRIBE，USE

DML / DQL — INSERT ... VALUES，带 WHERE、ORDER BY、GROUP BY、
HAVING、LIMIT / OFFSET、DISTINCT 的 SELECT，以及聚合函数 COUNT / SUM / AVG / MIN / MAX

列类型 — TIMESTAMP（毫秒/微秒/纳秒精度），BOOL，TINYINT，SMALLINT，
INT，BIGINT（及其无符号变体），FLOAT，DOUBLE，BINARY，NCHAR

🛠 客户端 SDK
纯头文件 + 预编译库，独立运行于 Windows 和 Linux —— 无需服务端源码。

bash
./scripts/build_sdk.sh            # 静态库（libetherdb_client.a）
./scripts/build_sdk.sh --shared   # 共享库（.so）
# Windows：scripts\build_sdk.bat [--shared]
输出位于 src/bin/sdk/（include/ + lib/）。参见 src/client/README.md 中的 C 和 C++ 示例（连接、查询、预编译语句批量插入、流式获取）。

📂 仓库结构
路径	用途
src/base	线程、日志、通用工具
src/net	事件循环、TCP/UDP（epoll / IOCP）
src/gateway	应用层服务端与消息传输
src/dnode	数据节点服务进程、配置、工作线程池
src/dbnode	虚拟节点（每个 vgroup 一个存储仓库）
src/wal	预写日志模块
src/etdb	存储引擎 —— 仓库、memtable、列式文件、压缩、SMA、提交队列
src/query	SQL 解析器、查询引擎与执行器
src/client	Shell 客户端 + C/C++ 客户端 SDK
tests	集成/正确性/性能测试
packaging/cfg	示例服务端配置 etherdb.cfg
服务端行为通过 etherdb.cfg 配置（[server] 端口与线程，[storage] 数据目录，[log] 异步日志，[auth] 认证凭据，……）。

🧪 测试
tests/ 目录包含端到端集成测试（full_test）、正确性验证（verify_where、value_verify 等）和性能测试（perf_insert_test、query_perf_test、streaming_perf_test 等），这些测试针对正在运行的 etherdb_server 执行。

📄 许可证
EtherDB 采用 Business Source License 1.1（BSL 1.1）许可。
在变更日期（2030-10-12）之后，将自动转换为 Apache License, Version 2.0。
详见 LICENSE。

2030 年前的商业/生产使用需商业授权，联系方式：kinyi6666@gmail.com。

