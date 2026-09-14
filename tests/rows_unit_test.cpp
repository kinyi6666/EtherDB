// Standalone unit test for the new Rows / mergeSortedRows / RowView logic.
#include "query/StorageReader.h"
#include <cassert>
#include <cstdio>

using namespace ETDB::Query;

int main() {
    // ── Build a 3-column schema: ts, col_int, name(BINARY(8)) ──
    ETDB::TableSchema schema;
    schema.addColumn(0, ETDB::ColType::TIMESTAMP, "ts");
    schema.addColumn(1, ETDB::ColType::INT,       "col_int");
    schema.addColumn(2, ETDB::ColType::BINARY,    "name", 8);
    schema.recalcRowBytes();

    // ── Rows::setupCols (full projection) ──
    Rows r;
    r.setupCols(&schema, nullptr);
    assert(r.colCount == 3);
    assert(r.bytesPerRow() == 8 + 4 + 8);  // ts + int + binary(8)

    // Simulate writing 3 rows manually (row-major):
    // row0: ts=10, col_int=5,  name="ab"
    // row1: ts=20, col_int=7,  name="xyz"
    // row2: ts=30, col_int=99, name="hello"
    r.reserveRows(3);
    int64_t ts[3] = {10, 20, 30};
    int32_t ci[3] = {5, 7, 99};
    char nm[3][8] = {{'a','b',0,0,0,0,0,0},
                     {'x','y','z',0,0,0,0,0},
                     {'h','e','l','l','o',0,0,0}};
    for (int32_t i = 0; i < 3; ++i) {
        int32_t oi = r.numOfRows;
        int64_t rowOff = (int64_t)oi * r.bytesPerRow();
        memcpy(r.data.data() + rowOff + r.colBase[0], &ts[i], 8);
        memcpy(r.data.data() + rowOff + r.colBase[1], &ci[i], 4);
        memcpy(r.data.data() + rowOff + r.colBase[2], nm[i], 8);
        r.numOfRows++;
        if (ts[i] < r.minTs) r.minTs = ts[i];
        if (ts[i] > r.maxTs) r.maxTs = ts[i];
    }

    // ── getValue / tsAt / hasCol / RowView ──
    assert(r.tsAt(0) == 10 && r.tsAt(2) == 30);
    assert(r.getValue(1, 1).iVal == 7);
    assert(r.getValue(0, 1).iVal == 5);
    assert(r.getValue(2, 2).sVal == "hello");
    assert(r.getValue(0, 2).sVal == "ab");
    assert(r.getValue(1, 0).iVal == 20);
    assert(!r.hasCol(5));
    assert(r.hasCol(0) && r.hasCol(2));

    RowView rv;
    rv.rows = &r;
    rv.rowIdx = 2;
    assert(rv.hasCol(1));
    assert(rv.getCol(1).iVal == 99);
    assert(rv.getCol(2).sVal == "hello");
    rv.rowIdx = 0;
    assert(rv.getCol(1).iVal == 5);

    // RowView wrapping a vector<Value> (HAVING path)
    std::vector<Value> outRow = {Value((int64_t)3), Value((double)15.0)};
    RowView rv2;
    rv2.values = &outRow;
    assert(rv2.getCol(0).iVal == 3);
    assert(rv2.getCol(1).fVal == 15.0);
    assert(!rv2.hasCol(2));

    // ── mergeSortedRows: 3 sorted sources with overlapping ts ──
    auto mkSource = [&](const std::vector<int64_t>& tss, const std::vector<int32_t>& cols) {
        Rows s;
        s.setupCols(&schema, nullptr);
        s.reserveRows((int64_t)tss.size());
        for (size_t i = 0; i < tss.size(); ++i) {
            int32_t oi = s.numOfRows;
            int64_t rowOff = (int64_t)oi * s.bytesPerRow();
            memcpy(s.data.data() + rowOff + s.colBase[0], &tss[i], 8);
            memcpy(s.data.data() + rowOff + s.colBase[1], &cols[i], 4);
            // name: leave zeros (empty)
            s.numOfRows++;
        }
        return s;
    };
    // source A: ts 1,3,5   source B: ts 2,4,6   source C: ts 4,7
    std::vector<Rows> sources;
    sources.push_back(mkSource({1, 3, 5}, {10, 30, 50}));
    sources.push_back(mkSource({2, 4, 6}, {20, 40, 60}));
    sources.push_back(mkSource({4, 7},    {40, 70}));

    Rows merged = mergeSortedRows(sources, -1);
    assert(merged.numOfRows == 8);
    // expected ts order: 1,2,3,4,4,5,6,7
    const int64_t expTs[8] = {1, 2, 3, 4, 4, 5, 6, 7};
    const int64_t expCi[8] = {10, 20, 30, 40, 40, 50, 60, 70};
    for (int i = 0; i < 8; ++i) {
        assert(merged.tsAt(i) == expTs[i]);
        assert(merged.getValue(i, 1).iVal == expCi[i]);
    }
    assert(merged.minTs == 1 && merged.maxTs == 7);

    // merge with LIMIT
    Rows limited = mergeSortedRows(sources, 4);
    assert(limited.numOfRows == 4);
    assert(limited.tsAt(3) == 4);

    // merge single source
    std::vector<Rows> one;
    one.push_back(mkSource({5, 9}, {50, 90}));
    Rows single = mergeSortedRows(one, -1);
    assert(single.numOfRows == 2 && single.tsAt(1) == 9);

    printf("rows_unit_test: ALL PASS\n");
    return 0;
}
