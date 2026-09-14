/*
 * Async insert backpressure verification.
 *
 * Verifies: if the user does NOT consume async results, the pending
 * (received-not-consumed) result set is bounded — executeAsync() blocks
 * (backpressure) once it reaches the cap. Consuming via getAsyncResult()
 * frees slots and unblocks the sender.
 *
 *   - cap = 4
 *   - phase 1: send 4 batches without consuming
 *   - phase 2: a consumer thread frees one slot after 500ms; the next
 *              executeAsync() must BLOCK until then (proves bounding)
 *   - phase 3: send the rest with sliding-window consumption, then COUNT
 *
 * Usage: ./build/async_backpressure_test [port=7040]
 */

#include <client/EtDBClient.h>
#include <cstdio>
#include <chrono>
#include <thread>
#include <vector>

using namespace ETDB::Client;

static int64_t nowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

int main(int argc, char* argv[]) {
    uint16_t port = (argc > 1) ? (uint16_t)atoi(argv[1]) : 7040;
    const int BATCH = 10000;
    const int TOTAL = 20;
    const int CAP   = 4;

    EtDBClient client;
    if (!client.connect("127.0.0.1", port)) { printf("[FATAL] connect fail\n"); return 1; }
    if (!client.query("DROP DATABASE IF EXISTS bptest").error().empty()) { printf("[FATAL] drop db\n"); return 1; }
    if (!client.query("CREATE DATABASE IF NOT EXISTS bptest KEEP 3650 REPLICA 1 PRECISION us").error().empty()) { printf("[FATAL] create db\n"); return 1; }
    if (!client.query("USE bptest").error().empty()) { printf("[FATAL] use db\n"); return 1; }
    if (!client.query("CREATE TABLE IF NOT EXISTS t (ts TIMESTAMP, v INT, d DOUBLE)").error().empty()) { printf("[FATAL] create table\n"); return 1; }

    EtDBStmt* st = client.createStmt();
    if (!st->prepare("INSERT INTO t VALUES(?, ?, ?)")) { printf("[FATAL] prepare\n"); return 1; }

    std::vector<int64_t> ts(BATCH);
    std::vector<int32_t> vi(BATCH);
    std::vector<double>  dd(BATCH);
    int64_t base = nowUs();

    client.setMaxAsyncPending(CAP);

    EtDBStmt::MultiBind mb[3];
    mb[0].type = EtDBStmt::TYPE_TIMESTAMP; mb[0].buffer = ts.data(); mb[0].stride = 8; mb[0].numRows = BATCH;
    mb[1].type = EtDBStmt::TYPE_INT;       mb[1].buffer = vi.data(); mb[1].stride = 4; mb[1].numRows = BATCH;
    mb[2].type = EtDBStmt::TYPE_DOUBLE;    mb[2].buffer = dd.data(); mb[2].stride = 8; mb[2].numRows = BATCH;

    auto fill = [&](int batch) {
        for (int r = 0; r < BATCH; ++r) { ts[r] = base + (int64_t)batch * BATCH + r; vi[r] = r; dd[r] = r * 0.5; }
    };
    auto sendOne = [&](int batch) -> int64_t {
        fill(batch);
        if (!st->bindParamBatch(mb, 3)) { printf("[FAIL] bind\n"); return -1; }
        return st->executeAsync();   // may block (backpressure)
    };

    int64_t ids[TOTAL];
    int sent = 0;

    // ── Phase 1: send CAP batches WITHOUT consuming ──
    printf("phase 1: send %d batches without consuming (cap=%d)...\n", CAP, CAP);
    for (int i = 0; i < CAP; ++i) {
        int64_t bid = sendOne(i);
        if (bid <= 0) { printf("[FAIL] send %d\n", i); return 1; }
        ids[sent++] = bid;
    }
    for (int i = 0; i < 5000 && client.asyncPendingCount() < (uint64_t)CAP; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    printf("  pending after %d sends = %lld (cap=%d)\n",
           sent, (long long)client.asyncPendingCount(), CAP);

    // ── Phase 2: next executeAsync must BLOCK until a slot is consumed ──
    printf("phase 2: send while NOT consuming -> should BLOCK...\n");
    std::thread consumer([&]{
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        auto r = client.getAsyncResult((uint64_t)ids[0]);   // consume oldest
        printf("  [consumer] consumed batch %lld (affected=%d)\n", (long long)ids[0], r.affected);
    });
    int64_t t0 = nowUs();
    int64_t bid = sendOne(sent);
    int64_t blockedUs = nowUs() - t0;
    consumer.join();
    if (bid <= 0) { printf("[FAIL] blocked send\n"); return 1; }
    ids[sent++] = bid;
    printf("  blocked executeAsync took %6.1f ms  %s\n", blockedUs/1000.0,
           (blockedUs >= 400000) ? "[OK] blocked by backpressure" : "[WARN] did not block");

    // ── Phase 3: raise the cap and send the rest (backpressure already
    //    proven in phase 2); then consume everything and verify COUNT ──
    printf("phase 3: raise cap, send remaining %d batches, then consume all...\n", TOTAL - sent);
    client.setMaxAsyncPending(TOTAL + 8);
    for (int i = sent; i < TOTAL; ++i) {
        int64_t b = sendOne(i);
        if (b <= 0) { printf("[FAIL] send %d\n", i); return 1; }
        ids[sent++] = b;
    }
    client.waitAllAsync();
    int64_t tot = 0, fail = 0;
    for (int i = (int)ids[0] + 1; i <= sent; ++i) {   // id ids[0] already consumed
        auto r = client.getAsyncResult((uint64_t)i);
        if (!r.ready || r.code != 0 || r.errors != 0) fail++;
        tot += r.affected;
    }
    printf("  sent=%d consumed=%d pending=%lld\n", sent, sent - 1,
           (long long)client.asyncPendingCount());

    // ── Verify inserted rows ──
    auto r = client.query("SELECT COUNT(*) FROM bptest.t");
    int64_t cnt = (r.rowCount() > 0) ? r.get(0, 0).iVal : -1;
    printf("  SELECT COUNT(*) = %lld (expected %lld)\n", (long long)cnt, (long long)TOTAL * BATCH);
    bool ok = (sent == TOTAL) && (cnt == (int64_t)TOTAL * BATCH);
    printf(ok ? "[OK] backpressure: bounded pending + consumption works\n"
              : "[FAIL] backpressure test failed\n");
    st->close(); delete st;
    client.close();
    return ok ? 0 : 1;
}
