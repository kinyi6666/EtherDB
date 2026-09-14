/*
 * sdk_example.c — EtherDB Client SDK 使用示例（C 接口）
 *
 * 编译（Windows / MSVC，库以 /MD 动态 CRT 构建，链接需一致）：
 *   cl /MD sdk_example.c /I<path>\sdk\include <path>\sdk\lib\etherdb_client.lib ws2_32.lib
 *
 * 编译（Linux / gcc）：
 *   gcc sdk_example.c -I<path>/sdk/include -L<path>/sdk/lib \
 *       -letherdb_client -lpthread -o sdk_example
 *
 * 运行前需确保 etherdb 服务端已启动；未启动时 connect 会返回 NULL（正常现象）。
 */
#include "etdb.h"
#include <stdio.h>

int main(void) {
    etdb_init();

    /* 1. 连接服务端 */
    ETDB_CONN* conn = etdb_connect("127.0.0.1", 7000, "root", "etherdbdata", "test");
    if (!conn) {
        printf("connect failed (server not running?)\n");
        etdb_cleanup();
        return 1;
    }
    printf("connected\n");

    /* 2. 查询 */
    ETDB_RESULT* res = etdb_query(conn, "SELECT * FROM sensor");
    if (res) {
        int cols = etdb_column_count(res);
        int rows = etdb_row_count(res);
        printf("result: %d cols, %d rows\n", cols, rows);
        for (int r = 0; r < rows; r++) {
            for (int c = 0; c < cols; c++)
                printf("%s ", etdb_get_value(res, r, c));
            printf("\n");
        }
        etdb_free_result(res);
    }

    /* 3. 参数绑定插入 */
    ETDB_STMT* stmt = etdb_stmt_init(conn);
    if (etdb_stmt_prepare(stmt, "INSERT INTO sensor VALUES(?, ?, ?)") == 0) {
        int64_t ts = 1716364800000LL;
        float   t  = 25.5f, p = 1013.2f;
        etdb_stmt_bind_param(stmt, 0, ETDB_TYPE_TIMESTAMP, &ts, sizeof(ts));
        etdb_stmt_bind_param(stmt, 1, ETDB_TYPE_FLOAT, &t, sizeof(t));
        etdb_stmt_bind_param(stmt, 2, ETDB_TYPE_FLOAT, &p, sizeof(p));
        etdb_stmt_add_batch(stmt);
        int rc = etdb_stmt_execute(stmt);
        printf("insert: rc=%d affected=%d\n", rc, etdb_stmt_affected_rows(stmt));
    }
    etdb_stmt_close(stmt);

    etdb_close(conn);
    etdb_cleanup();
    printf("SDK example OK\n");
    return 0;
}
