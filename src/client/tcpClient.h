// Copyright (c) 2026 Liu jinwei <kinyi6666@gmail.com>
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/*
 * tcpClient.h — 跨平台 TCP Socket 封装（Windows / Linux）
 *
 * 统一两套系统需要的 socket 函数，供 EtDBClient 使用：
 *   - 创建 socket / 连接 / 发送 / 接收 / 关闭
 *   - TCP_NODELAY
 *   - 主机名或点分 IP 解析（getaddrinfo）
 *   - 字节序工具（Windows 无 htobe64/be64toh，这里统一提供）
 *
 * Windows 编译需链接 ws2_32（CMake 中已处理）。
 */

#ifndef ETHERDB_TCP_CLIENT_H
#define ETHERDB_TCP_CLIENT_H

#include <cstdint>
#include <cstring>
#include <cstdio>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdlib.h>   // _byteswap_uint64
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>   // select() / fd_set (recvLoop readability poll)
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>      // getaddrinfo / freeaddrinfo
#include <strings.h>    // strncasecmp / strcasestr
#include <unistd.h>
#include <cerrno>
#include <endian.h>     // htobe64 / be64toh (glibc)
#endif

// ============================================================================
// 字节序工具
//   Linux 下 htobe64/be64toh 由 <endian.h> 提供；
//   Windows 下没有这两个宏，用 _byteswap_uint64 等价实现（x86/x64 均为小端）。
// ============================================================================
#ifdef _WIN32
static inline uint64_t etdb_bswap64(uint64_t x) { return _byteswap_uint64(x); }
#ifndef htobe64
#define htobe64(x) etdb_bswap64((uint64_t)(x))
#endif
#ifndef be64toh
#define be64toh(x) etdb_bswap64((uint64_t)(x))
#endif
#endif

// ============================================================================
// MSVC 缺失的 POSIX 兼容函数
// ============================================================================
#ifdef _MSC_VER
static inline int etdbStrncasecmp(const char* a, const char* b, size_t n) {
    return _strnicmp(a, b, n);
}
static inline const char* etdbStrcasestr(const char* haystack, const char* needle) {
    if (!haystack || !needle) return nullptr;
    size_t nl = strlen(needle);
    if (nl == 0) return haystack;
    for (; *haystack; ++haystack) {
        if (_strnicmp(haystack, needle, nl) == 0) return haystack;
    }
    return nullptr;
}
#ifndef strncasecmp
#define strncasecmp etdbStrncasecmp
#endif
#ifndef strcasestr
#define strcasestr etdbStrcasestr
#endif
#endif // _MSC_VER

namespace ETDB {
namespace Client {

// ============================================================================
// Socket 句柄类型（Windows: SOCKET；Linux: int）
// ============================================================================
#ifdef _WIN32
typedef SOCKET TcpSocket;
#else
typedef int TcpSocket;
#endif

// 无效 socket 值（Windows 下 SOCKET 为无符号，-1 即 INVALID_SOCKET）
const TcpSocket INVALID_TCP_SOCKET = (TcpSocket)-1;

// ============================================================================
// TcpClient — 跨平台 TCP socket 封装
// ============================================================================
class TcpClient {
public:
    // 初始化（Windows 下 WSAStartup；Linux 下为空操作）。可重复调用。
    static bool init();
    // 清理（Windows 下 WSACleanup）
    static void cleanup();

    // 创建 TCP socket，失败返回 INVALID_TCP_SOCKET
    static TcpSocket create();

    // 设置 TCP_NODELAY
    static bool setNoDelay(TcpSocket s);

    // 连接 host:port（host 支持点分 IP 或主机名）
    static bool connect(TcpSocket s, const char* host, uint16_t port);

    // 发送全部数据（循环直至发完），返回已发送字节数
    static int sendAll(TcpSocket s, const void* buf, int len);

    // 接收数据（flags 透传，如 MSG_WAITALL），返回接收字节数或 -1
    static int recv(TcpSocket s, void* buf, int len, int flags = 0);

    // 关闭 socket
    static void close(TcpSocket s);

    // 半关闭 socket（how: 0=SHUT_RD, 1=SHUT_WR, 2=SHUT_RDWR）。
    // 用于解除阻塞在 recv() 上的接收线程。
    static void shutdown(TcpSocket s, int how);

    // socket 是否有效
    static bool valid(TcpSocket s) { return s != INVALID_TCP_SOCKET; }
};

} // namespace Client
} // namespace ETDB

#endif // ETHERDB_TCP_CLIENT_H
