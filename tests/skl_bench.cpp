// SkipList 微基准：旧的逐节点实现 vs arena 块式实现
//
// 场景：单线程「插入 262144 行(60B) 后销毁」与双线程「一边插入一边销毁」，
//       用于量化一次 memtable 提交周期内的分配/销毁开销与尾部抖动。
//
// 构建（MSVC）：
//   cl /std:c++17 /O2 /EHsc /MD /utf-8 /I src /FI src\wincompat\posix_compat.h tests\skl_bench.cpp
#include <etdb/ETDBMeta.h>
#include <etdb/ETDBStubs.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <deque>
#include <algorithm>

using namespace ETDB;

static int64_t nowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ============================================================================
// Legacy implementation (the code that was replaced) — kept for comparison
// ============================================================================
namespace legacy {
struct Node { TKEY key; void* data; Node* next; };
using List = std::vector<Node*>;

inline List* create() { return new List(); }
inline void destroy(List* sl) {
    for (auto* n : *sl) { free(n->data); delete n; }
    delete sl;
}
inline int put(List* sl, TKEY key, const void* data, int len) {
    auto* n = new Node();
    n->key  = key;
    n->data = malloc(len);
    memcpy(n->data, data, len);
    if (!sl->empty()) sl->back()->next = n;
    sl->push_back(n);
    return 0;
}
} // namespace legacy

constexpr int    kRowsPerCycle = 262144;   // one memtable commit cycle
constexpr int    kCycles       = 8;
constexpr int    kPayload      = 60;       // 8B key + 52B row (the perf-test schema)

static char g_payload[kPayload];

// ---------------------------------------------------------------------------
// Single-thread: insert kRowsPerCycle rows, then destroy
// ---------------------------------------------------------------------------
template <typename PutFn, typename DestroyFn, typename CreateFn>
static void runSingle(const char* label, CreateFn create, PutFn put, DestroyFn destroy) {
    std::vector<int64_t> insertUs, destroyUs;
    for (int c = 0; c < kCycles; ++c) {
        void* sl = create();
        int64_t t0 = nowUs();
        for (int i = 0; i < kRowsPerCycle; ++i) {
            if (put(sl, (TKEY)i, g_payload, kPayload) != 0) { printf("put fail\n"); return; }
        }
        int64_t t1 = nowUs();
        destroy(sl);
        int64_t t2 = nowUs();
        insertUs.push_back(t1 - t0);
        destroyUs.push_back(t2 - t1);
    }
    auto avg = [](const std::vector<int64_t>& v) {
        int64_t s = 0; for (auto x : v) s += x; return s / (int64_t)v.size();
    };
    printf("  %-28s insert/cycle=%7.2f ms (~%6.0f ns/row)   destroy=%7.2f ms\n",
           label, avg(insertUs) / 1000.0,
           1000.0 * avg(insertUs) / kRowsPerCycle,
           avg(destroyUs) / 1000.0);
}

// ---------------------------------------------------------------------------
// Two threads: producer inserts cycles, consumer destroys finished lists
// (models commit-thread destruction while the write thread keeps inserting)
// ---------------------------------------------------------------------------
template <typename PutFn, typename DestroyFn, typename CreateFn>
static void runContended(const char* label, CreateFn create, PutFn put, DestroyFn destroy) {
    std::mutex mtx;
    std::condition_variable cv;
    std::deque<void*> ready;
    bool done = false;
    std::vector<int64_t> insertUs;

    std::thread consumer([&]() {
        for (;;) {
            void* sl = nullptr;
            {
                std::unique_lock<std::mutex> lk(mtx);
                cv.wait(lk, [&] { return done || !ready.empty(); });
                if (ready.empty() && done) return;
                sl = ready.front();
                ready.pop_front();
            }
            destroy(sl);      // commit thread
        }
    });

    for (int c = 0; c < kCycles; ++c) {
        void* sl = create();
        int64_t t0 = nowUs();
        for (int i = 0; i < kRowsPerCycle; ++i) put(sl, (TKEY)i, g_payload, kPayload);
        int64_t t1 = nowUs();
        insertUs.push_back(t1 - t0);
        {
            std::lock_guard<std::mutex> lk(mtx);
            ready.push_back(sl);
        }
        cv.notify_one();
    }
    {
        std::lock_guard<std::mutex> lk(mtx);
        done = true;
    }
    cv.notify_one();
    consumer.join();

    int64_t sum = 0; for (auto x : insertUs) sum += x;
    printf("  %-28s insert/cycle=%7.2f ms (~%6.0f ns/row)  [worst %7.2f ms]\n",
           label, sum / (double)kCycles / 1000.0,
           1000.0 * (sum / (double)kCycles) / kRowsPerCycle,
           *std::max_element(insertUs.begin(), insertUs.end()) / 1000.0);
}

int main() {
    memset(g_payload, 0x5A, sizeof(g_payload));
    printf("rows/cycle=%d  payload=%dB  cycles=%d\n", kRowsPerCycle, kPayload, kCycles);

    printf("\n[1] single thread: insert then destroy\n");
    runSingle("legacy (new+malloc/node)",
              [] { return (void*)legacy::create(); },
              [](void* sl, TKEY k, const void* d, int n) { return legacy::put((legacy::List*)sl, k, d, n); },
              [](void* sl) { legacy::destroy((legacy::List*)sl); });
    runSingle("arena (block bump alloc)",
              [] { return (void*)etSkipListCreate(8); },
              [](void* sl, TKEY k, const void* d, int n) { return etSkipListPut((SkipList*)sl, k, (void*)d, n); },
              [](void* sl) { etSkipListDestroy((SkipList*)sl); });

    printf("\n[2] producer thread inserts while consumer thread destroys\n");
    runContended("legacy (new+malloc/node)",
                 [] { return (void*)legacy::create(); },
                 [](void* sl, TKEY k, const void* d, int n) { return legacy::put((legacy::List*)sl, k, d, n); },
                 [](void* sl) { legacy::destroy((legacy::List*)sl); });
    runContended("arena (block bump alloc)",
                 [] { return (void*)etSkipListCreate(8); },
                 [](void* sl, TKEY k, const void* d, int n) { return etSkipListPut((SkipList*)sl, k, (void*)d, n); },
                 [](void* sl) { etSkipListDestroy((SkipList*)sl); });

    return 0;
}
