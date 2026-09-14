/*
 * EtherDB 多线程预绑定批量插入性能测试（每线程独立表，5线程）
 *
 * 每个线程使用独立连接，写入独立表 perf_data_<id>，通过 bindParamBatch 列绑定方式插入。
 * TS 取值 = 系统当前毫秒时间 + 行内偏移，自然单调递增。
 *   - 5 个独立连接
 *   - 每线程独立表（同结构），避免多连接写同表的锁竞争
 *   - 每批 500 行
 *   - 每线程 10 轮
 *   - 总数据量: 5 × 500 × 10 = 25,000 行
 */

#include <client/EtDBClient.h>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <thread>
#include <vector>
#include <string>
#include <cmath>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <sys/time.h>

using namespace ETDB::Client;
using Clock = std::chrono::high_resolution_clock;
using Us    = std::chrono::microseconds;
using Ms    = std::chrono::milliseconds;
using SysClock = std::chrono::system_clock;

constexpr int  NUM_THREADS   = 5;
constexpr int  BATCH_SIZE    = 500;
constexpr int  NUM_COLS      = 10;
constexpr int  ROUNDS        = 10;
constexpr int  WARMUP        = 2;

static int64_t nowUs() { return std::chrono::duration_cast<Us>(Clock::now().time_since_epoch()).count(); }
static int64_t nowMs() { return std::chrono::duration_cast<Ms>(SysClock::now().time_since_epoch()).count(); }

// 简易同步屏障：所有线程 + main 都到达后才继续
class SimpleBarrier {
public:
    explicit SimpleBarrier(int count) : _count(count), _original(count), _generation(0) {}
    void arrive_and_wait() {
        std::unique_lock<std::mutex> lock(_mutex);
        int gen = _generation;
        if (--_count == 0) {
            _generation++;
            _count = _original;
            _cv.notify_all();
        } else {
            _cv.wait(lock, [this, gen] { return gen != _generation; });
        }
    }
private:
    std::mutex _mutex; std::condition_variable _cv;
    int _count, _original, _generation;
};
static SimpleBarrier g_barrier(NUM_THREADS + 1);

// 全局基准时间 + 全局递增计数器，保证所有线程的 ts 全局单调递增
static int64_t              g_base_ts    = nowMs();
static std::atomic<int64_t> g_ts_counter{0};

struct ThreadResult { int id; int64_t rows, us; int errors; };

static ThreadResult worker(int id, uint16_t port) {
    ThreadResult r{id, 0, 0, 0};
    std::string tableName = "perf_data_" + std::to_string(id);

    printf("  [线程 %d] 连接中...\n", id); fflush(stdout);
    EtDBClient client;
    if (!client.connect("127.0.0.1", port)) {
        printf("  [线程 %d] 连接失败!\n", id); fflush(stdout);
        r.errors++; 
        g_barrier.arrive_and_wait(); 
        return r;
    }

    // 设置 socket 超时（30秒），防止因服务端不响应导致线程永久阻塞
    {
        int fd = client.connection()->socketFd();
        struct timeval tv;
        tv.tv_sec = 30;
        tv.tv_usec = 0;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }

    printf("  [线程 %d] 切换数据库...\n", id); fflush(stdout);
    if (!client.query("USE mtperf").error().empty()) {
        printf("  [线程 %d] USE mtperf 失败!\n", id); fflush(stdout);
        r.errors++; 
        g_barrier.arrive_and_wait(); 
        return r;
    }

    // 每个线程独立 prepare 到自己的表，无需互斥锁
    printf("  [线程 %d] prepare 语句...\n", id); fflush(stdout);
    EtDBStmt* stmt = client.createStmt();
    std::string insertSql = "INSERT INTO " + tableName + " VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
    if (!stmt || !stmt->prepare(insertSql)) {
        printf("  [线程 %d] prepare 失败!\n", id); fflush(stdout);
        r.errors++; delete stmt;
        g_barrier.arrive_and_wait();
        return r;
    }

    std::vector<int64_t> c0(BATCH_SIZE), c2(BATCH_SIZE), c8(BATCH_SIZE);
    std::vector<int32_t> c1(BATCH_SIZE);
    std::vector<float>   c3(BATCH_SIZE);
    std::vector<double>  c4(BATCH_SIZE), c9(BATCH_SIZE);
    std::vector<int16_t> c5(BATCH_SIZE);
    std::vector<int8_t>  c6(BATCH_SIZE), c7(BATCH_SIZE);

    EtDBStmt::MultiBind mb[NUM_COLS];
    auto setup = [&]() {
        memset(mb, 0, sizeof(mb));
        for (int i = 0; i < NUM_COLS; ++i) mb[i].numRows = BATCH_SIZE;
        mb[0].type = EtDBStmt::TYPE_TIMESTAMP; mb[0].buffer = c0.data(); mb[0].stride = sizeof(int64_t);
        mb[1].type = EtDBStmt::TYPE_INT;       mb[1].buffer = c1.data(); mb[1].stride = sizeof(int32_t);
        mb[2].type = EtDBStmt::TYPE_BIGINT;    mb[2].buffer = c2.data(); mb[2].stride = sizeof(int64_t);
        mb[3].type = EtDBStmt::TYPE_FLOAT;     mb[3].buffer = c3.data(); mb[3].stride = sizeof(float);
        mb[4].type = EtDBStmt::TYPE_DOUBLE;    mb[4].buffer = c4.data(); mb[4].stride = sizeof(double);
        mb[5].type = EtDBStmt::TYPE_SMALLINT;  mb[5].buffer = c5.data(); mb[5].stride = sizeof(int16_t);
        mb[6].type = EtDBStmt::TYPE_TINYINT;   mb[6].buffer = c6.data(); mb[6].stride = sizeof(int8_t);
        mb[7].type = EtDBStmt::TYPE_BOOL;      mb[7].buffer = c7.data(); mb[7].stride = sizeof(int8_t);
        mb[8].type = EtDBStmt::TYPE_BIGINT;    mb[8].buffer = c8.data(); mb[8].stride = sizeof(int64_t);
        mb[9].type = EtDBStmt::TYPE_DOUBLE;    mb[9].buffer = c9.data(); mb[9].stride = sizeof(double);
    };

    // 预热（使用全局递增 TS）
    for (int w = 0; w < WARMUP; ++w) {
        for (int i = 0; i < BATCH_SIZE; ++i) {
            int64_t ts = g_base_ts + g_ts_counter.fetch_add(1, std::memory_order_relaxed);
            c0[i]=ts; c1[i]=(int32_t)i; c2[i]=ts; c3[i]=20.f+i*0.5f;
            c4[i]=100.0+i*1.5; c5[i]=(int16_t)(i%32767); c6[i]=(int8_t)(i%127);
            c7[i]=(int8_t)(i%2); c8[i]=ts*7+i*13; c9[i]=500.0+i*0.75;
        }
        setup();
        if (!stmt->bindParamBatch(mb, NUM_COLS)) {
            printf("  [线程 %d] 预热轮%d bindParamBatch 失败!\n", id, w+1); fflush(stdout);
            r.errors++; delete stmt;
            g_barrier.arrive_and_wait();
            return r;
        }
        printf("  [线程 %d] 预热轮%d execute...\n", id, w+1); fflush(stdout);
        int aff = stmt->execute();
        if (aff < 0) {
            printf("  [线程 %d] 预热轮%d execute 失败 (返回%d)!\n", id, w+1, aff); fflush(stdout);
            // 打印服务端返回的错误详情
            const auto& errEntries = stmt->errorEntries();
            int showCnt = std::min((int)errEntries.size(), 10);
            for (int ei = 0; ei < showCnt; ++ei) {
                printf("    row %d: errorCode=%d\n", errEntries[ei].rowIndex, errEntries[ei].errorCode);
            }
            if ((int)errEntries.size() > 10)
                printf("    ... 共 %d 条错误\n", (int)errEntries.size());
            printf("  提交:%d 成功:%d 失败:%d\n", stmt->submittedRows(), stmt->affectedRows(), stmt->errorRows());
            r.errors++; delete stmt;
            g_barrier.arrive_and_wait();
            return r;
        }
    }

    printf("  [线程 %d] 预热完成，等待同步...\n", id); 
    fflush(stdout);
    g_barrier.arrive_and_wait();

    // 正式测试 —— 使用全局递增 TS，保证所有线程的 ts 全局单调递增
    printf("  [线程 %d] 开始正式测试...\n", id); 
    fflush(stdout);
    auto t0 = nowUs();
    for (int round = 0; round < ROUNDS; ++round) {
        for (int i = 0; i < BATCH_SIZE; ++i) {
            int64_t ts = g_base_ts + g_ts_counter.fetch_add(1, std::memory_order_relaxed);
            c0[i]=ts; c1[i]=(int32_t)(round*BATCH_SIZE+i);
            c2[i]=round*BATCH_SIZE+i; c3[i]=20.f+(round*BATCH_SIZE+i)*0.5f;
            c4[i]=100.0+(round*BATCH_SIZE+i)*1.5;
            c5[i]=(int16_t)((round*BATCH_SIZE+i)%32767);
            c6[i]=(int8_t)((round*BATCH_SIZE+i)%127);
            c7[i]=(int8_t)((round*BATCH_SIZE+i)%2);
            c8[i]=(round*BATCH_SIZE+i)*777; c9[i]=500.0+(round*BATCH_SIZE+i)*0.75;
        }
        setup();
        if (!stmt->bindParamBatch(mb, NUM_COLS)) { r.errors++; continue; }
        int aff = stmt->execute();
        if (aff < 0) {
            printf("  [线程 %d] 第%d轮 execute 失败 (返回%d)!\n", id, round+1, aff); fflush(stdout);
            const auto& errEntries = stmt->errorEntries();
            int showCnt = std::min((int)errEntries.size(), 5);
            for (int ei = 0; ei < showCnt; ++ei) {
                printf("    row %d: errorCode=%d\n", errEntries[ei].rowIndex, errEntries[ei].errorCode);
            }
            if ((int)errEntries.size() > 5)
                printf("    ... 共 %d 条错误\n", (int)errEntries.size());
            r.errors++; continue;
        }
        r.rows += aff;
    }
    auto t1 = nowUs(); r.us = t1 - t0;
    printf("  [线程 %d] 完成: %lld 行, %.2f ms\n", id, (long long)r.rows, r.us/1000.0); fflush(stdout);
    delete stmt; 
    //client.close(); 
    return r;
}

int main(int argc, char* argv[]) {
    uint16_t port = (argc > 1) ? (uint16_t)atoi(argv[1]) : 7040;

    printf("╔══════════════════════════════════════════════════════╗\n");
    printf("║   EtherDB %d连接 预绑定批量插入性能测试              ║\n", NUM_THREADS);
    printf("║   每连接: %d行/批 × %d轮 = %d行   独立表 perf_data_N ║\n", BATCH_SIZE, ROUNDS, BATCH_SIZE*ROUNDS);
    printf("║   总数据量: %d 行                                   ║\n", NUM_THREADS*BATCH_SIZE*ROUNDS);
    printf("╚══════════════════════════════════════════════════════╝\n\n");

    printf("=== 1. 准备（创建 %d 张独立表） ===\n", NUM_THREADS);
    {
        EtDBClient c;
        if (!c.connect("127.0.0.1", port)) { printf("  [FATAL] connect failed\n"); return 1; }
        c.query("DROP DATABASE IF EXISTS mtperf");
        c.query("CREATE DATABASE mtperf");
        c.query("USE mtperf");

        for (int i = 0; i < NUM_THREADS; ++i) {
            std::string tbl = "perf_data_" + std::to_string(i);
            c.query("CREATE TABLE " + tbl + " ("
                    "ts TIMESTAMP, col_int INT, col_bigint BIGINT, col_float FLOAT, "
                    "col_double DOUBLE, col_smallint SMALLINT, col_tinyint TINYINT, "
                    "col_bool BOOL, col_extra1 BIGINT, col_extra2 DOUBLE)");
            printf("  [OK] 表 %s 已创建\n", tbl.c_str());
        }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    printf("\n=== 2. 多线程写入 ===\n");
    fflush(stdout);
    std::vector<std::thread> threads;
    std::vector<ThreadResult> results(NUM_THREADS);
    for (int i = 0; i < NUM_THREADS; ++i)
        threads.emplace_back([i, port, &results]() { results[i] = worker(i, port); });

    // 等待所有线程完成预热，同步开始计时
    printf("  [main] 等待所有线程完成预热...\n"); fflush(stdout);
    g_barrier.arrive_and_wait();

    auto wallStart = nowUs();
    for (auto& t : threads) t.join();
    auto wallEnd = nowUs();
    int64_t wallUs = wallEnd - wallStart;

    printf("\n=== 3. 结果汇总 ===\n");
    printf("  线程 |    行数  |   耗时(ms) |  rows/s  | 错误\n");
    printf("  -----+----------+------------+----------+-----\n");

    int64_t sumRows = 0, sumUs = 0, sumErrs = 0;
    double minRps = 1e18, maxRps = 0, avgRps = 0;
    for (auto& r : results) {
        double rps = r.us > 0 ? (r.rows * 1e6 / r.us) : 0;
        printf("  %4d | %8lld | %10.2f | %8.0f | %4d\n",
               r.id, (long long)r.rows, r.us/1000.0, rps, r.errors);
        sumRows += r.rows; sumUs += r.us; sumErrs += r.errors;
        avgRps += rps;
        if (rps > 0 && rps < minRps) minRps = rps;
        if (rps > maxRps) maxRps = rps;
    }
    avgRps /= NUM_THREADS;

    double wallRps  = wallUs > 0 ? (sumRows * 1e6 / wallUs) : 0;
    double wallMbps = wallUs > 0 ? (sumRows * 52.0 / 1e6) * 1e6 / wallUs : 0;

    printf("\n");
    printf("  ┌─────────────────────────────────────────────────────┐\n");
    printf("  │         %d连接 预绑定批量插入性能测试结果            │\n", NUM_THREADS);
    printf("  ├─────────────────────────────────────────────────────┤\n");
    printf("  │ 总插入行数:       %6lld                            │\n", (long long)sumRows);
    printf("  │ 总错误数:         %6lld                            │\n", (long long)sumErrs);
    printf("  ├─────────────────────────────────────────────────────┤\n");
    printf("  │ 单连接平均:       %8.0f rows/s                     │\n", avgRps);
    printf("  │ 单连接最小:       %8.0f rows/s                     │\n", minRps);
    printf("  │ 单连接最大:       %8.0f rows/s                     │\n", maxRps);
    printf("  ├─────────────────────────────────────────────────────┤\n");
    printf("  │ 总体吞吐(墙上):   %8.0f rows/s                     │\n", wallRps);
    printf("  │ 总体吞吐(MB/s):   %8.2f MB/s                       │\n", wallMbps);
    printf("  │ 墙上时间:         %8.2f ms                         │\n", wallUs/1000.0);
    printf("  └─────────────────────────────────────────────────────┘\n");

    printf("\n=== 4. 数据验证（汇总所有独立表） ===\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(3000));
    {
        EtDBClient c;
        if (c.connect("127.0.0.1", port)) {
            int64_t totalCnt = 0;
            for (int i = 0; i < NUM_THREADS; ++i) {
                std::string sql = "SELECT COUNT(*) FROM mtperf.perf_data_" + std::to_string(i);
                auto r = c.query(sql);
                if (r.rowCount() > 0 && r.colCount() > 0) {
                    int64_t cnt = r.get(0, 0).iVal;
                    printf("  perf_data_%d: %lld 行\n", i, (long long)cnt);
                    totalCnt += cnt;
                }
            }
            printf("  总计: %lld  (预期 %lld)  %s\n",
                   (long long)totalCnt, (long long)sumRows,
                   totalCnt == sumRows ? "[OK]" : "[WARN]");
        }
    }

    printf("\n=== 5. 清理 ===\n");
    {
        EtDBClient c;
        c.connect("127.0.0.1", port);
        c.query("DROP DATABASE IF EXISTS mtperf");
    }
    printf("  测试完成!\n");
    return sumErrs > 0 ? 1 : 0;
}
