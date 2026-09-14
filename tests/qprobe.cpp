// 查询延迟探针：在插入压测并行运行时采样服务端响应延迟（RTT）
// 用法：qprobe [port=7040] [iters=700]（需已存在 perftest20.perf_data_1）
#include <EtDBClient.h>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <thread>
using namespace ETDB::Client;
static int64_t nowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
int main(int argc, char* argv[]) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    uint16_t port = (argc > 1) ? (uint16_t)atoi(argv[1]) : 7040;
    int iters = (argc > 2) ? atoi(argv[2]) : 700;
    const char* host = "127.0.0.1";
    EtDBClient c;
    if (!c.connect(host, port)) { printf("probe: connect failed\n"); return 1; }
    c.query("USE perftest20");
    for (int i = 0; i < iters; i++) {
        int64_t t0 = nowUs();
        auto r = c.query("SELECT COUNT(*) FROM perf_data_1");
        int64_t t1 = nowUs();
        if (!r.error().empty()) { printf("probe: query error: %s\n", r.error().c_str()); return 2; }
        printf("Q %d %lld %lld\n", i, (long long)t1, (long long)(t1 - t0));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    c.close();
    return 0;
}
