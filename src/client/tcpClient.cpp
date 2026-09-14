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
 * tcpClient.cpp — TcpClient 跨平台实现
 */

#include "tcpClient.h"

namespace ETDB {
namespace Client {

bool TcpClient::init() {
#ifdef _WIN32
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
#else
    return true;
#endif
}

void TcpClient::cleanup() {
#ifdef _WIN32
    WSACleanup();
#endif
}

TcpSocket TcpClient::create() {
#ifdef _WIN32
    return ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#else
    return ::socket(AF_INET, SOCK_STREAM, 0);
#endif
}

bool TcpClient::setNoDelay(TcpSocket s) {
    if (!valid(s)) return false;
    int nodelay = 1;
    return ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY,
                        (const char*)&nodelay, sizeof(nodelay)) == 0;
}

bool TcpClient::connect(TcpSocket s, const char* host, uint16_t port) {
    if (!valid(s) || !host) return false;

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    char portStr[16];
    snprintf(portStr, sizeof(portStr), "%u", (unsigned)port);

    struct addrinfo* res = nullptr;
    if (getaddrinfo(host, portStr, &hints, &res) != 0) return false;

    bool ok = false;
    for (struct addrinfo* ai = res; ai; ai = ai->ai_next) {
        if (::connect(s, ai->ai_addr, (int)ai->ai_addrlen) == 0) {
            ok = true;
            break;
        }
    }
    freeaddrinfo(res);
    return ok;
}

int TcpClient::sendAll(TcpSocket s, const void* buf, int len) {
    if (!valid(s) || !buf || len <= 0) return 0;
    const char* p = (const char*)buf;
    int sent = 0;
    while (sent < len) {
        int n = (int)::send(s, p + sent, len - sent, 0);
        if (n <= 0) break;   // 出错或对端关闭
        sent += n;
    }
    return sent;
}

int TcpClient::recv(TcpSocket s, void* buf, int len, int flags) {
    if (!valid(s) || !buf || len <= 0) return -1;
    return (int)::recv(s, (char*)buf, len, flags);
}

void TcpClient::close(TcpSocket s) {
    if (!valid(s)) return;
#ifdef _WIN32
    ::closesocket(s);
#else
    ::close(s);
#endif
}

void TcpClient::shutdown(TcpSocket s, int how) {
    if (!valid(s)) return;
    ::shutdown(s, how);
}

} // namespace Client
} // namespace ETDB
