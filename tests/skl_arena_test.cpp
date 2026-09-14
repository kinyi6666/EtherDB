// SkipList arena 分配单元测试（etdb/ETDBStubs.h）
//
// 覆盖：键序/载荷完整性、seek/iterFrom、跨多个 arena 块、超大单行、
//       0 长度载荷、反复创建/销毁；Debug 构建下启用 CRT 泄漏检查。
//
// 构建（MSVC，_DEBUG 下含内存泄漏报告）：
//   cl /std:c++17 /O2 /EHsc /MDd /utf-8 /I src /FI src\wincompat\posix_compat.h tests\skl_arena_test.cpp
#include <etdb/ETDBMeta.h>    // colTypeIsUnsigned (used by the file-system stubs below in ETDBStubs.h)
#include <etdb/ETDBStubs.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>

#ifdef _DEBUG
#include <crtdbg.h>
#endif

using namespace ETDB;

struct Row {
    long long ts;
    long long v;
    char      pad[48];
};

static int g_failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); ++g_failures; } } while (0)

int main() {
#ifdef _DEBUG
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif

    // ---- 1) small list: order, payload integrity, seek, iterFrom ----
    {
        SkipList* sl = etSkipListCreate(8);
        for (long long i = 0; i < 1000; ++i) {
            Row r{};
            r.ts = 1000 + i * 5;
            r.v  = i;
            CHECK(etSkipListPut(sl, (TKEY)r.ts, &r, (int)sizeof(Row)) == 0, "put failed");
        }
        CHECK(etSkipListGetCount(sl) == 1000, "count != 1000");

        SkipListIterator* it = etSkipListCreateIter(sl);
        long long expect = 0;
        SkipListNode* n = nullptr;
        int seen = 0;
        while ((n = etSkipListIterGet(it)) != nullptr) {
            Row* pr = (Row*)etSkipListGetNodeData(n);
            CHECK(pr != nullptr, "null payload");
            CHECK(pr->v == expect, "payload order/value broken");
            CHECK(n->key == (TKEY)(1000 + expect * 5), "key order broken");
            ++expect;
            ++seen;
        }
        CHECK(seen == 1000, "iteration count");
        etSkipListDestroyIter(it);

        CHECK(etSkipListSeek(sl, 1000 + 300 * 5) == 300, "seek exact");
        CHECK(etSkipListSeek(sl, 1000 + 300 * 5 + 1) == 301, "seek between keys");
        CHECK(etSkipListSeek(sl, 99999999) == -1, "seek past end");
        CHECK(etSkipListSeek(sl, 0) == 0, "seek before start");

        SkipListIterator* it2 = etSkipListCreateIterFrom(sl, 300);
        n = etSkipListIterGet(it2);
        CHECK(n != nullptr && ((Row*)etSkipListGetNodeData(n))->v == 300, "iterFrom(300)");
        etSkipListDestroyIter(it2);

        etSkipListDestroy(sl);
    }

    // ---- 2) multi-block growth: 300k rows x 64B (~19 MB) ----
    {
        const int N = 300000;
        SkipList* sl = etSkipListCreate(8);
        for (int i = 0; i < N; ++i) {
            Row r{};
            r.ts = 5000 + i;
            r.v  = (long long)i * 3;
            memset(r.pad, (int)(i & 0x7f), sizeof(r.pad));
            if (etSkipListPut(sl, (TKEY)r.ts, &r, (int)sizeof(Row)) != 0) { CHECK(false, "big put failed"); break; }
        }
        CHECK(etSkipListGetCount(sl) == N, "big count");

        int k = 123457;
        CHECK(etSkipListSeek(sl, 5000 + k) == k, "big seek");
        SkipListIterator* it = etSkipListCreateIterFrom(sl, k);
        for (int j = 0; j < 3; ++j) {
            SkipListNode* n = etSkipListIterGet(it);
            CHECK(n != nullptr, "big iter null");
            Row* pr = (Row*)etSkipListGetNodeData(n);
            CHECK(pr->v == (long long)(k + j) * 3, "big payload value");
            CHECK(pr->pad[10] == (char)((k + j) & 0x7f), "big payload pad");
        }
        etSkipListDestroyIter(it);
        etSkipListDestroy(sl);
    }

    // ---- 3) oversized single payload (> max block) + zero-length ----
    {
        SkipList* sl = etSkipListCreate(8);
        std::vector<char> big(300 * 1024, 'B');
        CHECK(etSkipListPut(sl, 1, big.data(), (int)big.size()) == 0, "huge put");
        CHECK(etSkipListPut(sl, 2, nullptr, 0) == 0, "zero put");
        CHECK(etSkipListGetCount(sl) == 2, "huge count");

        SkipListIterator* it = etSkipListCreateIter(sl);
        SkipListNode* n1 = etSkipListIterGet(it);
        char* p = (char*)etSkipListGetNodeData(n1);
        CHECK(p != nullptr && p[0] == 'B' && p[big.size() - 1] == 'B', "huge payload content");
        SkipListNode* n2 = etSkipListIterGet(it);
        CHECK(n2 != nullptr && etSkipListGetNodeData(n2) != nullptr, "zero-length payload non-null");
        CHECK(etSkipListIterGet(it) == nullptr, "huge list ends");
        etSkipListDestroyIter(it);
        etSkipListDestroy(sl);
    }

    // ---- 4) create/destroy churn (catches double-free / leaks / corruption) ----
    {
        for (int rep = 0; rep < 50; ++rep) {
            SkipList* sl = etSkipListCreate(8);
            for (int i = 0; i < 20000; ++i) {
                Row r{};
                r.ts = i;
                r.v  = i;
                etSkipListPut(sl, (TKEY)i, &r, (int)sizeof(Row));
            }
            etSkipListDestroy(sl);
        }
    }

    if (g_failures == 0) printf("ALL SKIPLIST TESTS PASSED\n");
    else                 printf("SKIPLIST TESTS FAILED: %d\n", g_failures);
    return g_failures ? 1 : 0;
}
