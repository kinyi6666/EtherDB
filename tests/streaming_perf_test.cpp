/*
 * streaming_perf_test.cpp — EtherDB 查询性能测试
 *
 * 对标 TDengine 的 taos_perf_query.cpp：
 *   taos_perf_query:  SELECT ts,col_int FROM perf_data WHERE col_int>5
 *                     （taos_fetch_row 逐行拉取，测量 query+全量拉取总耗时）
 *   本测试:          SELECT ts,col_int FROM perf_data_2 WHERE col_int>5
 *                     （EtDBClient 流式 fetchRow 拉取，同样测量总耗时）
 *
 * 编译:
 *   g++ -std=c++17 -O2 -w -I<src> -I<deps/cJson> \
 *       -o streaming_perf_test streaming_perf_test.cpp \
 *       ../client/EtDBClient.cpp ../client/tcpClient.cpp -lpthread -ldl
 *
 * 运行:
 *   ./streaming_perf_test [port] [table] [db]
 *     port  = EtherDB 服务端端口, 默认 7040
 *     table = 表名 (可用 db.table), 默认 perf_data_2
 *     db    = USE 的库, 默认 perftest20
 *
 * 说明:
 *   - 为了与 taos_perf_query 完全对齐, t0 在 query 之前, t1 在全部行取完之后,
 *     elapsed = t1 - t0 (含网络往返 + 服务端执行 + 客户端反序列化 + 逐行复制)。
 *   - 额外输出客户端各阶段耗时分解, 用于定位瓶颈:
 *       query 内部: 建消息+发送+收响应+反序列化第一批
 *       fetch 批次: 每批 FETCH 的 发送+收+反序列化 (含 RTT)
 *       fetchRow:   从内存批次复制一行到调用方 (纯 CPU)
 */

#include <client/EtDBClient.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <chrono>
#include <string>
#include <vector>

using namespace ETDB::Client;
using Clock = std::chrono::system_clock;
using Us    = std::chrono::microseconds;

static int64_t nowUs() {
    return std::chrono::duration_cast<Us>(Clock::now().time_since_epoch()).count();
}

int main(int argc, char* argv[]) {
    uint16_t port  = (argc > 1) ? (uint16_t)atoi(argv[1]) : 7040;
    std::string table = (argc > 2) ? argv[2] : "perf_data";
    std::string db    = (argc > 3) ? argv[3] : "perftest20";

    printf("-----------------------------------------------------\n");
    printf("  EtherDB query performance test (streaming_perf_test)\n");
    printf("  服务端: 127.0.0.1:%d  表: %s.%s\n", port, db.c_str(), table.c_str());
    printf("-----------------------------------------------------\n\n");

    EtDBClient c;
    if (!c.connect("127.0.0.1", port, "root", "etherdbdata", "")) {
        printf("  [FATAL] 无法连接 EtherDB (127.0.0.1:%d)\n", port);
        return 1;
    }
    printf("  [OK] 已连接到 %d\n", port);

    auto r = c.query("USE " + db);
    if (!r.success()) { printf("  [FATAL] USE %s failed: %s\n", db.c_str(), r.error().c_str()); c.close(); return 1; }
    printf("  [OK] USE %s\n", db.c_str());

    std::string sql = "SELECT ts,col_int FROM " + db + "." + table + " WHERE col_int>5";
    printf("  查询: %s\n\n", sql.c_str());

    // 基准行数 (COUNT, 走块索引 0 数据 I/O)
    auto cr = c.query("SELECT COUNT(*) FROM " + db + "." + table + " WHERE col_int>5");
    int64_t expected = (cr.rowCount() > 0) ? cr.get(0, 0).iVal : -1;
    printf("  [INFO] 预期匹配行数: %lld\n\n", (long long)expected);

    // ── 主测试: 与 taos_perf_query 对齐, 测量 query + 全量拉取总耗时 ──
    int64_t t0 = nowUs();
    auto res = c.query(sql);
    if (!res.success() && !res.error().empty()) {
        printf("  [FATAL] 查询失败: %s\n", res.error().c_str());
        c.close();
        return 1;
    }

    int64_t tQuery = nowUs();   // query() 返回 (第一批已到达并反序列化)

    int64_t streamTotal = 0;
    std::vector<Value> row;
    while (res.fetchRow(row)) {
        ++streamTotal;
    }
    int64_t t1 = nowUs();

    int64_t elapsedUs = t1 - t0;

    printf("query() 首批耗时 : %8.2f ms (网络RTT + 服务端执行 + 首批发序列化)\n", (tQuery - t0) / 1000.0);
    printf("流式拉取总耗时   : %8.2f ms (后续 FETCH 批次 + fetchRow 逐行复制)\n", (t1 - tQuery) / 1000.0);
    printf("总耗时           : %8.2f ms , total records %lld\n",
           elapsedUs / 1000.0, (long long)streamTotal);
    printf("(taos_perf_query 输出格式: query time %.2f ms , total records %lld)\n",
           elapsedUs / 1000.0, (long long)streamTotal);

    if (expected >= 0 && streamTotal != expected) {
        printf("  [FAIL] 行数不一致: stream=%lld != count=%lld\n",
               (long long)streamTotal, (long long)expected);
    } else {
        printf("  [OK] 行数一致: %lld\n", (long long)streamTotal);
    }

    c.close();
    printf("\n  测试完成!\n");
    return 0;
}
