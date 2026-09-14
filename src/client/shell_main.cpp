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
 * etherdb — Interactive SQL Shell Entry Point 
 *
 * Connects to etherdb server and provides an interactive SQL terminal.
 *
 * Usage:
 *   ./etherdb -h <host> -p <port> [-u user] [-P password]
 *   ./etherdb -h 127.0.0.1 -p 7000
 *   ./etherdb -h 127.0.0.1 -p 7040    (connect to dserver)
 *
 * Import SQL scripts (CREATE DATABASE / CREATE TABLE ...):
 *   ./etherdb -h 127.0.0.1 -p 7040 -u root -P xxx < schema.sql   # via stdin
 *   (or type `source /path/schema.sql` inside the interactive shell)
 *
 * Build:
 *   g++ -std=c++11 -g -I src -I src/query -I src/etdb -I src/client \
 *       -I src/base -I src/dbnode -I src/wal \
 *       -o build/etherdb src/client/shell_main.cpp src/client/etdb.cpp \
 *       src/base/Logging.cpp src/base/Timestamp.cpp \
 *       src/base/LogStream.cpp src/base/Ascii.cpp -lpthread
 */

#include "EtDBShell.h"
#include "etdb.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <csignal>
#include <string>

#ifdef _WIN32
#include <conio.h>   // _getch
#include <io.h>
#include <windows.h> // SetConsoleMode (ANSI/VT)
#else
#include <termios.h>
#include <unistd.h>
#endif

using namespace ETDB::Shell;

// Signal handler for clean exit
static void sigHandler(int) {
    printf("\n");
}

// 无回显读取密码（Windows 用 _getch，Linux 用 termios 关闭 ECHO）
static std::string readPasswordNoEcho() {
    std::string pw;
#ifdef _WIN32
    while (true) {
        int c = _getch();
        if (c == '\r' || c == '\n') break;
        if (c == 8 || c == 127) {          // Backspace
            if (!pw.empty()) pw.pop_back();
            continue;
        }
        if (c >= 32 && c < 127) pw.push_back((char)c);
    }
#else
    struct termios oldt, newt;
    tcgetattr(STDIN_FILENO, &oldt);
    newt = oldt;
    newt.c_lflag &= ~(ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);
    std::getline(std::cin, pw);
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
#endif
    return pw;
}

// 当 stdin 被重定向（如 `etherdb ... < xx.sql`）时，不能从 stdin 读密码
// （会吞掉 SQL）。改从控制终端 /dev/tty 读取；无控制终端则返回空串
// （由调用方回退到默认密码）。
static std::string readPasswordFromTty(const char* user) {
#ifdef _WIN32
    return readPasswordNoEcho();
#else
    FILE* tty = fopen("/dev/tty", "r");
    if (!tty) return "";
    struct termios oldt, newt;
    int fd = fileno(tty);
    tcgetattr(fd, &oldt);
    newt = oldt;
    newt.c_lflag &= ~(ECHO);
    tcsetattr(fd, TCSANOW, &newt);

    printf("Password for %s: ", user ? user : "root");
    fflush(stdout);

    char buf[512];
    std::string pw;
    if (fgets(buf, sizeof(buf), tty)) pw = buf;
    tcsetattr(fd, TCSANOW, &oldt);
    fclose(tty);
    while (!pw.empty() && (pw.back() == '\n' || pw.back() == '\r'))
        pw.pop_back();
    return pw;
#endif
}

// Print usage
static void printUsage(const char* prog) {
    printf("EtherDB Interactive Shell v0.1.0\n");
    printf("Usage: %s [options]\n\n", prog);
    printf("Options:\n");
    printf("  -h <host>      Server host (default: 127.0.0.1)\n");
    printf("  -p <port>      Server port (default: 7000 for net_raw_test, 7040 for dserver)\n");
    printf("  -u <user>      Username (default: root)\n");
    printf("  -P <password>  Password (if omitted, will prompt interactively)\n");
    printf("  -e <sql>       Execute SQL and exit (non-interactive)\n");
    printf("  -V             Print version and exit\n");
    printf("  --help         Show this help\n\n");
    printf("Examples:\n");
    printf("  %s -h 127.0.0.1 -p 7040                  # Connect as root, prompt for password\n", prog);
    printf("  %s -h 127.0.0.1 -p 7040 -u bob -P bobpass # Connect as bob with password\n", prog);
    printf("  %s -h 127.0.0.1 -p 7040 -e \"SHOW DATABASES;\"\n", prog);
    printf("  %s -h 127.0.0.1 -p 7040 -u root -P xxx < schema.sql   # Import SQL file from stdin\n", prog);
    printf("  (inside the shell, run schema.sql with:  source /path/schema.sql)\n");
}

int main(int argc, char* argv[]) {
    ShellConfig config;
    std::string execSql;
    bool showVersion = false;
    bool hasPasswordArg = false;
    //EtherDB::Logger::setLogLevel(EtherDB::Logger::LINFO);

#ifdef _WIN32
    // 启用控制台 ANSI/VT 转义序列（\033[ 光标移动 / 清屏），经典 conhost 下同样生效
    {
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD mode = 0;
        if (hOut != INVALID_HANDLE_VALUE && GetConsoleMode(hOut, &mode))
            SetConsoleMode(hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
#endif

    // Parse command line
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-h") == 0 && i + 1 < argc) {
            config.host = argv[++i];
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            config.port = (uint16_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "-u") == 0 && i + 1 < argc) {
            config.user = argv[++i];
        } else if (strcmp(argv[i], "-P") == 0 && i + 1 < argc) {
            config.password = argv[++i];
            hasPasswordArg = true;
        } else if (strcmp(argv[i], "-e") == 0 && i + 1 < argc) {
            execSql = argv[++i];
        } else if (strcmp(argv[i], "-V") == 0) {
            showVersion = true;
        } else if (strcmp(argv[i], "--help") == 0) {
            printUsage(argv[0]);
            return 0;
        } else {
            printf("Unknown option: %s\n", argv[i]);
            printUsage(argv[0]);
            return 1;
        }
    }

    // If no -P given, prompt for password interactively.
    // When stdin is redirected (e.g. `etherdb ... < xx.sql`), read from the
    // controlling terminal (/dev/tty) so the SQL on stdin is never consumed
    // as the password. Without a controlling terminal, keep the default.
    if (!hasPasswordArg) {
        std::string pw;
        if (isatty(STDIN_FILENO)) {
            printf("Password for %s: ", config.user.c_str());
            fflush(stdout);
            pw = readPasswordNoEcho();
            printf("\n");
        } else {
            pw = readPasswordFromTty(config.user.c_str());
            if (pw.empty()) pw = config.password;   // fall back to default
        }
        while (!pw.empty() && isspace((unsigned char)pw.back()))
            pw.pop_back();
        if (!pw.empty()) config.password = pw;
    }

    if (showVersion) {
        printf("EtherDB Shell v0.1.0\n");
        return 0;
    }

    // Initialize client library
    etdb_init();

    // Create shell
    EtDBShell shell(config);

    // Connect
    if (!shell.init()) {
        etdb_cleanup();
        return 1;
    }

    // Execute SQL directly if -e option
    if (!execSql.empty()) {
        // Non-interactive mode: connect with C++ client, execute, print, exit
        ETDB::Client::EtDBClient client;
        if (!client.connect(config.host, config.port,
                            config.user.c_str(), config.password.c_str(), "")) {
            printf("ERROR: Failed to connect to %s:%d\n",
                   config.host.c_str(), config.port);
            etdb_cleanup();
            return 1;
        }
        auto result = client.query(execSql);
        ETDB::Shell::ShellFormatter fmt;
        if (!result.success()) {
            printf("ERROR: %s\n", result.error().c_str());
        } else if (result.colCount() == 0) {
            // DDL success — no result set
            printf("OK\n");
        } else {
            fmt.print(result);
        }
        client.close();
        etdb_cleanup();
        return 0;
    }

    // Setup signal handlers
    signal(SIGINT, sigHandler);
    signal(SIGTERM, sigHandler);

    // Run interactive shell
    int ret = shell.run();

    shell.shutdown();
    etdb_cleanup();
    return ret;
}
