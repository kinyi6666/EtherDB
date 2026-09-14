# EtherDB Client SDK

面向数据库使用者的客户端开发包。只发布 **C/C++ 接口头文件 + 预编译库**，
不依赖服务器任何源码，可独立在 Windows / Linux 编译运行。

## 构建 SDK（新增独立构建，不影响原 shell 调试构建）

- Windows（Visual Studio 2022）：
  ```bat
  scripts\build_sdk.bat              :: 静态库
  scripts\build_sdk.bat --shared     :: 共享库 (dll)
  ```
- Linux：
  ```sh
  ./scripts/build_sdk.sh
  ```

构建产物位于 **`src/bin/sdk/`**：

```
src/bin/sdk/
├── include/            C/C++ 接口头文件
│   ├── etdb.h              C 接口（extern "C"，纯 C 可直接调用）
│   ├── EtDBClient.h        C++ 接口（OOP 客户端）
│   ├── EtDBClientProtocol.h  wire 协议定义（一般无需直接引用）
│   ├── EtDBMeta.h          表元数据缓存（C++）
│   ├── EtDBLog.h           轻量日志
│   └── tcpClient.h         跨平台 socket 封装
├── lib/                库文件
│   ├── etherdb_client.lib      (Windows 静态) / libetherdb_client.a (Linux)
│   └── etherdb_client.dll      (Windows 共享，可选)
└── README.md
```

> 原 `src/client/CMakeLists.txt`（交互式 shell）保持源码直接编译，便于调试；
> 本 SDK 库由新增的 `src/client/sdk/CMakeLists.txt` 独立构建。

## 快速开始（C 接口）

```c
#include "etdb.h"
#include <stdio.h>

int main(void) {
    etdb_init();
    ETDB_CONN* conn = etdb_connect("127.0.0.1", 7040, "root", "etherdbdata", "test");
    if (!conn) { printf("connect failed\n"); return 1; }

    ETDB_RESULT* res = etdb_query(conn, "SELECT * FROM sensor");
    int cols = etdb_column_count(res);
    int rows = etdb_row_count(res);
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++)
            printf("%s ", etdb_get_value(res, r, c));
        printf("\n");
    }
    etdb_free_result(res);

    // 参数绑定插入
    ETDB_STMT* stmt = etdb_stmt_init(conn);
    etdb_stmt_prepare(stmt, "INSERT INTO sensor VALUES(?, ?, ?)");
    int64_t ts = 1716364800000LL; float t = 25.5f, p = 1013.2f;
    etdb_stmt_bind_param(stmt, 0, ETDB_TYPE_TIMESTAMP, &ts, sizeof(ts));
    etdb_stmt_bind_param(stmt, 1, ETDB_TYPE_FLOAT, &t, sizeof(t));
    etdb_stmt_bind_param(stmt, 2, ETDB_TYPE_FLOAT, &p, sizeof(p));
    etdb_stmt_add_batch(stmt);
    etdb_stmt_execute(stmt);
    etdb_stmt_close(stmt);

    etdb_close(conn);
    etdb_cleanup();
    return 0;
}
```

## 快速开始（C++ 接口）

```cpp
#include "EtDBClient.h"
#include <cstdio>

int main() {
    ETDB::Client::EtDBClient client;
    if (!client.connect("127.0.0.1", 7000, "root", "etherdbdata", "test")) {
        printf("connect failed: %s\n", client.lastError().c_str());
        return 1;
    }
    auto result = client.query("SELECT * FROM sensor");
    result.print();
    client.close();
    return 0;
}
```

## 链接方式

- Windows（MSVC，库以 /MD 动态 CRT 构建，需保持一致）：
  ```
  cl /MD app.c /I<sdk>\include <sdk>\lib\etherdb_client.lib ws2_32.lib
  ```
- Linux：
  ```sh
  gcc app.c -I<sdk>/include -L<sdk>/lib -letherdb_client -lpthread -o app
  ```
  （共享库时：`export LD_LIBRARY_PATH=<sdk>/lib`，或链接 `-Wl,-rpath,<sdk>/lib`）

> 说明：库主体为 C++ 实现，但 `etdb.h` 是纯 C 接口，C 程序可直接调用；
> 链接时无需服务器源码或其它第三方库。
