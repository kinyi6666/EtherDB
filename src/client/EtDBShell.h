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
 * EtDBShell — Interactive SQL Shell (taosc equivalent)
 *
 * Object-oriented design with clear separation of concerns:
 *   EtDBShell       — Main REPL controller, connection lifecycle
 *   EtDBConnection  — TCP connection management (from EtDBClient.h)
 *   EtDBResult      — Query result wrapper (from EtDBClient.h)
 *   ShellFormatter  — Result formatting (table, vertical, CSV modes)
 *   ShellParser     — Command line parser (SQL vs meta-commands)
 *
 * Supports GNU readline for line editing and history (if available).
 *
 * Usage:
 *   ./etherdb -h 127.0.0.1 -p 7000
 *
 * SQL syntax: commands end with semicolon (;), press Enter to execute.
 * Multi-line: if no semicolon, continuation prompt '... >' appears.
 */

#ifndef ETHERDB_SHELL_H
#define ETHERDB_SHELL_H

#include "EtDBClient.h"
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <sstream>
#include <iomanip>
#include <iostream>
#include <fstream>
#include <ctime>
#include <chrono>
#include <cstring>
#include <cerrno>

// ---- 跨平台终端支持 ----
#ifdef _WIN32
#include <conio.h>    // _kbhit / _getch
#include <io.h>       // _isatty / _fileno
#include <windows.h>  // Sleep
#define STDIN_FILENO 0
#define isatty(fd) _isatty(fd)
#define fileno(fp) _fileno(fp)
#else
#include <termios.h>
#include <unistd.h>
#endif

// localtime_r 的跨平台封装
static inline struct tm* etdbLocalTime(const time_t* t, struct tm* out) {
#ifdef _WIN32
    localtime_s(out, t);
#else
    localtime_r(t, out);
#endif
    return out;
}

// Readline support (optional, falls back to std::getline)
#ifdef ETHERDB_USE_READLINE
#include <readline/readline.h>
#include <readline/history.h>
#endif

namespace ETDB {
namespace Shell {

using Client::EtDBClient;
using Client::EtDBConnection;
using Client::EtDBResult;
using Client::EtDBStmt;
using Client::Value;

// Trim leading/trailing ASCII whitespace from a string
inline std::string trimWs(const std::string& in) {
    size_t a = in.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = in.find_last_not_of(" \t\r\n");
    return in.substr(a, b - a + 1);
}

// Case-insensitive check that `s` starts with the whole word `kw` (i.e. `kw`
// is followed by whitespace or end-of-string). Used to recognize client
// commands such as `source <file>`.
inline bool startsWithKeyword(const std::string& s, const char* kw) {
    size_t len = strlen(kw);
    if (s.size() < len) return false;
    if (strncasecmp(s.c_str(), kw, len) != 0) return false;
    if (s.size() > len && !isspace((unsigned char)s[len])) return false;
    return true;
}

// ============================================================================
// ShellConfig — Connection and display configuration
// ============================================================================
struct ShellConfig {
    std::string host     = "127.0.0.1";
    uint16_t    port     = 7000;
    std::string user     = "root";
    std::string password = "etherdbdata";
    bool        timing   = false;       // show query execution time
    bool        prompt   = true;        // show prompt
    bool        header   = true;        // show column headers
};

// ============================================================================
// Output format enumeration
// ============================================================================
enum class OutputFormat {
    TABLE,      // aligned table with borders
    VERTICAL,   // key: value per row
    CSV,        // comma-separated values
};

// ============================================================================
// 时间戳显示：把 ts 值转成 "YYYY-MM-DD HH:MM:SS[.fff]" 可读格式
// prec: 0=毫秒, 1=微秒, 2=纳秒（来自数据库 precision）
// ============================================================================
inline std::string formatTimestampDisplay(int64_t v, uint8_t prec) {
    int64_t div;
    int     digits;
    switch (prec) {
        case 2: div = 1000000000LL; digits = 9; break;
        case 1: div = 1000000LL;    digits = 6; break;
        default: div = 1000;        digits = 3; break;
    }
    int64_t secs = v / div;
    int64_t frac = v % div;
    if (frac < 0) { frac += div; secs -= 1; }  // 负时间戳（1970 之前）
    time_t t = (time_t)secs;
    struct tm tmv;
    etdbLocalTime(&t, &tmv);
    char buf[96];
    char fmt[64];
    snprintf(fmt, sizeof(fmt), "%%04d-%%02d-%%02d %%02d:%%02d:%%02d.%%0%dd", digits);
    snprintf(buf, sizeof(buf), fmt,
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec, (int)frac);
    return std::string(buf);
}

// 结果第一列为 ts（时间戳主键，约定在第一列）时才显示为时间
inline bool isTsDisplayColumn(const EtDBResult& result, int c) {
    if (c != 0) return false;
    const auto& names = result.columnNames();
    return !names.empty() && names[0] == "ts";
}

// 结果单元格显示值：第一列 ts 转成可读时间，其余用 Value::toString()
inline std::string displayResultValue(const EtDBResult& result, int c,
                                      const Value& v, uint8_t precision) {
    if (v.isNull()) return "NULL";
    if (isTsDisplayColumn(result, c)) return formatTimestampDisplay(v.iVal, precision);
    return v.toString();
}

// ============================================================================
// ShellFormatter — Formats query results for display
// ============================================================================
class ShellFormatter {
public:
    ShellFormatter() = default;

    void setFormat(OutputFormat fmt) { _format = fmt; }
    OutputFormat format() const { return _format; }

    // Format and print a query result (precision: db ts precision 0=ms,1=us,2=ns)
    void print(const EtDBResult& result, uint8_t precision = 0);

    // Print an error message
    void printError(const std::string& msg);

    // Print a status message (affected rows, etc.)
    void printStatus(const std::string& msg);

private:
    void printTable(const EtDBResult& result, uint8_t precision);
    void printVertical(const EtDBResult& result, uint8_t precision);
    void printCSV(const EtDBResult& result, uint8_t precision);

    // Compute column widths for table formatting
    std::vector<int> computeWidths(const EtDBResult& result, int maxWidth, uint8_t precision) const;

    OutputFormat _format = OutputFormat::TABLE;
};

// ============================================================================
// ShellParser — Parses command line input
// ============================================================================
class ShellParser {
public:
    enum class CmdType {
        SQL,            // regular SQL statement
        META_QUIT,      // \q
        META_HELP,      // \?
        META_DESCRIBE,  // \d [table]
        META_LIST_DB,   // \l
        META_CLEAR,     // \c
        META_FORMAT,    // \o [table|csv|vertical]
        META_TIMING,    // \timing
        META_HEADER,    // \header
        META_SOURCE,    // source <file.sql>  /  \. <file.sql>
        EMPTY,          // blank line
        UNKNOWN,
    };

    struct ParsedCommand {
        CmdType     type = CmdType::EMPTY;
        std::string sql;        // for SQL type
        std::string arg;        // for meta-command arguments
    };

    // Parse a line of input
    ParsedCommand parse(const std::string& line);

    // Check if a string is a meta-command
    static bool isMetaCommand(const std::string& line);
};

// ============================================================================
// LineReader — Terminal line reader with history and arrow key support
// ============================================================================
class LineReader {
public:
    LineReader(const std::string& prompt);
    ~LineReader() = default;

    std::string readLine();
    void        addHistory(const std::string& line);
    void        setPrompt(const std::string& prompt) { _prompt = prompt; }
    bool        isInteractive() const { return _interactive; }

    // Read a complete SQL command (accumulate until ';' or meta-command)
    std::string readCommand();

private:
    std::string _prompt;
    bool        _interactive;
    std::vector<std::string> _history;
};

// ============================================================================
// EtDBShell — Main interactive shell controller
// ============================================================================
class EtDBShell {
public:
    EtDBShell(const ShellConfig& cfg);
    ~EtDBShell();

    // Non-copyable
    EtDBShell(const EtDBShell&) = delete;
    EtDBShell& operator=(const EtDBShell&) = delete;

    // Initialize: connect to server
    bool init();

    // Run the main REPL loop
    int  run();

    // Shutdown: disconnect
    void shutdown();

    // Accessors
    bool isConnected() const { return _client.isConnected(); }
    ShellConfig& config() { return _config; }

private:
    // Execute a SQL statement and display results
    bool executeSQL(const std::string& sql);

    // Execute a meta-command
    bool executeMeta(const ShellParser::ParsedCommand& cmd);

    // Execute an INSERT statement (detects and uses stmt if possible)
    bool executeInsert(const std::string& sql);

    // Execute one already-extracted statement text (SQL or `source <file>`),
    // dispatching to executeSQL()/sourceFile(). Shared by the REPL and runScript().
    bool executeStatement(const std::string& text);

    // Run all SQL statements from a stream (stdin or an open file). Handles
    // multi-line statements, comments (--, #, /* */) and ';' terminators.
    // Returns the number of failed statements (0 = all OK).
    int  runScript(std::istream& in, const std::string& name);

    // Execute a .sql file (the `source <path>` command). Returns true on success.
    bool sourceFile(const std::string& path);

    // Display help
    void showHelp();

    // Look up the timestamp precision of a database (0=ms, 1=us, 2=ns)
    // via SHOW DATABASES. Falls back to ms (0) on any failure.
    uint8_t lookupPrecision(const std::string& db);

    // Display welcome banner
    void showBanner();

    // Get current time string
    static std::string nowStr();

    // Try to parse result as DDL response (text message)
    bool tryDisplayDDLResult(const EtDBResult& result);

    ShellConfig    _config;
    EtDBClient     _client;
    ShellFormatter _formatter;
    LineReader     _reader;
    bool           _running = false;
    std::string    _currentDB;     // session state
    uint8_t        _precision = 0; // current db ts precision (0=ms, 1=us, 2=ns)
    int32_t        _currentdbId = 1;  // dbId for the current database (for query routing)
};

// ============================================================================
// ShellFormatter Implementation
// ============================================================================

inline void ShellFormatter::print(const EtDBResult& result, uint8_t precision) {
    switch (_format) {
        case OutputFormat::TABLE:    printTable(result, precision);    break;
        case OutputFormat::VERTICAL: printVertical(result, precision); break;
        case OutputFormat::CSV:      printCSV(result, precision);      break;
    }
}

inline void ShellFormatter::printError(const std::string& msg) {
    printf("ERROR: %s\n", msg.c_str());
}

inline void ShellFormatter::printStatus(const std::string& msg) {
    printf("%s\n", msg.c_str());
}

inline std::vector<int> ShellFormatter::computeWidths(const EtDBResult& result,
                                                       int maxWidth, uint8_t precision) const {
    int nCols = result.colCount();
    std::vector<int> widths(nCols, 0);

    // Column name widths
    for (int c = 0; c < nCols; ++c) {
        int w = (int)result.columnNames()[c].size();
        if (w > widths[c]) widths[c] = w;
    }

    // Data widths
    for (int r = 0; r < result.rowCount(); ++r) {
        for (int c = 0; c < nCols; ++c) {
            Value v = result.get(r, c);
            std::string s = displayResultValue(result, c, v, precision);
            int w = (int)s.size();
            if (w > widths[c]) widths[c] = w;
        }
    }

    // Clamp — ts 列的时间串较长（YYYY-MM-DD HH:MM:SS.ffffff 最多 29 字符）
    for (int c = 0; c < nCols; ++c) {
        int cap = isTsDisplayColumn(result, c) ? 40 : maxWidth;
        if (widths[c] > cap) widths[c] = cap;
    }
    return widths;
}

inline void ShellFormatter::printTable(const EtDBResult& result, uint8_t precision) {
    int nCols = result.colCount();
    int nRows = result.rowCount();

    if (nCols == 0) {
        printf("(empty result set)\n");
        return;
    }

    auto widths = computeWidths(result, 20, precision);
    const auto& names = result.columnNames();

    // Top border
    printf("+");
    for (int c = 0; c < nCols; ++c) {
        for (int i = 0; i < widths[c] + 2; ++i) printf("-");
        printf("+");
    }
    printf("\n");

    // Header
    printf("|");
    for (int c = 0; c < nCols; ++c) {
        printf(" %-*s |", widths[c], names[c].c_str());
    }
    printf("\n");

    // Separator
    printf("+");
    for (int c = 0; c < nCols; ++c) {
        for (int i = 0; i < widths[c] + 2; ++i) printf("-");
        printf("+");
    }
    printf("\n");

    // Rows
    for (int r = 0; r < nRows; ++r) {
        printf("|");
        for (int c = 0; c < nCols; ++c) {
            Value v = result.get(r, c);
            std::string s = displayResultValue(result, c, v, precision);
            if ((int)s.size() > widths[c])
                s = s.substr(0, widths[c] - 1) + "~";
            printf(" %-*s |", widths[c], s.c_str());
        }
        printf("\n");
    }

    // Bottom border
    printf("+");
    for (int c = 0; c < nCols; ++c) {
        for (int i = 0; i < widths[c] + 2; ++i) printf("-");
        printf("+");
    }
    printf("\n");

    printf("(%d row%s)\n", nRows, nRows == 1 ? "" : "s");
}

inline void ShellFormatter::printVertical(const EtDBResult& result, uint8_t precision) {
    int nCols = result.colCount();
    int nRows = result.rowCount();
    const auto& names = result.columnNames();

    for (int r = 0; r < nRows; ++r) {
        printf("--- Row %d ---\n", r + 1);
        for (int c = 0; c < nCols; ++c) {
            Value v = result.get(r, c);
            std::string s = displayResultValue(result, c, v, precision);
            printf("  %-20s : %s\n", names[c].c_str(), s.c_str());
        }
    }
    printf("(%d row%s)\n", nRows, nRows == 1 ? "" : "s");
}

inline void ShellFormatter::printCSV(const EtDBResult& result, uint8_t precision) {
    int nCols = result.colCount();
    const auto& names = result.columnNames();

    // Header
    for (int c = 0; c < nCols; ++c) {
        if (c) printf(",");
        printf("\"%s\"", names[c].c_str());
    }
    printf("\n");

    // Rows
    for (int r = 0; r < result.rowCount(); ++r) {
        for (int c = 0; c < nCols; ++c) {
            if (c) printf(",");
            Value v = result.get(r, c);
            printf("\"%s\"", displayResultValue(result, c, v, precision).c_str());
        }
        printf("\n");
    }
}

// ============================================================================
// ShellParser Implementation
// ============================================================================

inline bool ShellParser::isMetaCommand(const std::string& line) {
    if (line.empty()) return false;
    return line[0] == '\\';
}

inline ShellParser::ParsedCommand ShellParser::parse(const std::string& line) {
    ParsedCommand cmd;

    // Trim leading whitespace
    size_t start = 0;
    while (start < line.size() && isspace(line[start])) ++start;
    std::string trimmed = line.substr(start);

    if (trimmed.empty()) {
        cmd.type = CmdType::EMPTY;
        return cmd;
    }

    // Meta commands start with backslash
    if (trimmed[0] == '\\') {
        std::string meta = trimmed.substr(1);
        // Split at first space
        size_t sp = meta.find(' ');
        std::string cmdName = meta.substr(0, sp);
        std::string cmdArg = (sp != std::string::npos) ? meta.substr(sp + 1) : "";

        // Trim arg
        while (!cmdArg.empty() && isspace(cmdArg.front())) cmdArg.erase(0, 1);
        while (!cmdArg.empty() && isspace(cmdArg.back())) cmdArg.pop_back();

        if (cmdName == "q" || cmdName == "quit" || cmdName == "exit" || cmdName == "Q") {
            cmd.type = CmdType::META_QUIT;
        } else if (cmdName == "?" || cmdName == "h" || cmdName == "help") {
            cmd.type = CmdType::META_HELP;
        } else if (cmdName == "d" || cmdName == "describe") {
            cmd.type = CmdType::META_DESCRIBE;
            cmd.arg = cmdArg;
        } else if (cmdName == "l" || cmdName == "list") {
            cmd.type = CmdType::META_LIST_DB;
        } else if (cmdName == "c" || cmdName == "clear") {
            cmd.type = CmdType::META_CLEAR;
        } else if (cmdName == "o" || cmdName == "output") {
            cmd.type = CmdType::META_FORMAT;
            cmd.arg = cmdArg;
        } else if (cmdName == "timing") {
            cmd.type = CmdType::META_TIMING;
        } else if (cmdName == "header") {
            cmd.type = CmdType::META_HEADER;
        } else if (cmdName == "source") {
            cmd.type = CmdType::META_SOURCE;
            cmd.arg = cmdArg;
        } else if (cmdName == ".") {
            // MySQL-style alias: \. <file>  ==  source <file>
            cmd.type = CmdType::META_SOURCE;
            cmd.arg = cmdArg;
        } else {
            cmd.type = CmdType::UNKNOWN;
        }
    } else {
        // MySQL-style client command: source <file.sql>
        if (startsWithKeyword(trimmed, "source")) {
            std::string arg = trimWs(trimmed.substr(6));   // strlen("source")
            // Allow optional surrounding quotes around the path
            if (arg.size() >= 2 &&
                ((arg.front() == '"' && arg.back() == '"') ||
                 (arg.front() == '\'' && arg.back() == '\''))) {
                arg = arg.substr(1, arg.size() - 2);
            }
            cmd.type = CmdType::META_SOURCE;
            cmd.arg = arg;
            return cmd;
        }

        // SQL statement
        cmd.type = CmdType::SQL;
        cmd.sql = trimmed;
        // Remove trailing semicolon
        if (!cmd.sql.empty() && cmd.sql.back() == ';')
            cmd.sql.pop_back();
    }

    return cmd;
}

// ============================================================================
// LineReader Implementation (raw terminal with escape handling)
// ============================================================================

inline LineReader::LineReader(const std::string& prompt)
    : _prompt(prompt), _interactive(isatty(fileno(stdin))) {}

inline std::string LineReader::readLine() {
    if (!_interactive) {
        std::string line;
        if (!std::getline(std::cin, line)) return "";
        while (!line.empty() && isspace(line.back())) line.pop_back();
        return line;
    }

    printf("%s", _prompt.c_str());
    fflush(stdout);

    std::string line;
    std::string savedLine;   // saved pre-history line (restored on down-past-end)
    int cursor = 0, histIdx = -1;

#ifdef _WIN32
    // Windows：_getch() 直接进入原始模式（无回显），无需 termios
#else
    struct termios oldt, newt;
    tcgetattr(STDIN_FILENO, &oldt);
    newt = oldt;
    newt.c_lflag &= ~(ICANON | ECHO);
    // VMIN=0, VTIME=1: read() returns available bytes immediately (0.1s max wait)
    // This is critical for escape sequence handling: ESC arrives first, then
    // subsequent read() picks up [A/B/C/D immediately since they're in the buffer.
    newt.c_cc[VMIN]  = 0;
    newt.c_cc[VTIME] = 1;
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);
#endif

    auto redraw = [&]() {
        printf("\r\033[K%s%s", _prompt.c_str(), line.c_str());
        int off = (int)(_prompt.size() + cursor);
        printf("\r\033[%dC", off);
        fflush(stdout);
    };

    // 读一个原始字节；无数据/超时返回 -1
    auto rawRead = [&]() -> int {
#ifdef _WIN32
        if (!_kbhit()) return -1;
        return _getch();
#else
        char c;
        ssize_t nr = read(STDIN_FILENO, &c, 1);
        if (nr <= 0) return -1;
        return (unsigned char)c;
#endif
    };

    // 历史/光标导航（Windows 与 Linux 共用）
    auto histBack = [&]() {
        if (!_history.empty()) {
            if (histIdx == -1) {
                savedLine = line;
                histIdx = (int)_history.size() - 1;
            } else if (histIdx > 0) {
                histIdx--;
            }
            if (histIdx >= 0) {
                line = _history[histIdx];
                cursor = (int)line.size();
                redraw();
            }
        }
    };
    auto histFwd = [&]() {
        if (histIdx >= 0 && histIdx < (int)_history.size() - 1) {
            histIdx++;
            line = _history[histIdx];
            cursor = (int)line.size();
            redraw();
        } else if (histIdx == (int)_history.size() - 1) {
            // Past the newest entry — restore saved line
            histIdx = -1;
            line = savedLine;
            savedLine.clear();
            cursor = (int)line.size();
            redraw();
        }
    };
    auto curRight = [&]() { if (cursor < (int)line.size()) { cursor++; redraw(); } };
    auto curLeft  = [&]() { if (cursor > 0) { cursor--; redraw(); } };

    bool done = false;
    while (!done) {
        int c = rawRead();
        if (c < 0) {
#ifdef _WIN32
            Sleep(10);  // 避免空转
#endif
            continue;
        }

        if (c == '\n' || c == '\r') {
            printf("\n"); done = true;
        } else if (c == 127 || c == '\b') {
            if (cursor > 0) { line.erase(--cursor, 1); redraw(); }
        }
#ifdef _WIN32
        else if (c == 0 || c == 0xE0) {
            // Windows 扩展键：0xE0/0x00 前缀 + 虚拟扫描码
            int ext = _getch();
            switch (ext) {
                case 72: histBack(); break;                                 // Up
                case 80: histFwd(); break;                                  // Down
                case 77: curRight(); break;                                 // Right
                case 75: curLeft(); break;                                  // Left
                case 71: cursor = 0; redraw(); break;                       // Home
                case 79: cursor = (int)line.size(); redraw(); break;        // End
                case 83: if (cursor < (int)line.size()) { line.erase(cursor, 1); redraw(); } break; // Del
                default: break;
            }
        }
#else
        else if (c == '\033') {
            // ESC received — read continuation bytes.
            // With VMIN=0,VTIME=1, read() returns whatever is buffered immediately.
            // Terminal escape sequences (\033[A etc.) arrive atomically in the
            // kernel tty buffer, so the continuation bytes are already available.
            char s[8] = {0};
            int n = (int)read(STDIN_FILENO, s, sizeof(s) - 1);
            if (n < 0) n = 0;

            // CSI sequences: \033[ ...  (e.g., \033[A = UP, \033[B = DOWN)
            if (n >= 1 && s[0] == '[') {
                if (n >= 2 && s[1] == 'A') histBack();                     // UP
                else if (n >= 2 && s[1] == 'B') histFwd();                 // DOWN
                else if (n >= 2 && s[1] == 'C') curRight();                // RIGHT
                else if (n >= 2 && s[1] == 'D') curLeft();                 // LEFT
                else if (n >= 2 && s[1] == 'H') { cursor = 0; redraw(); }  // HOME  (\033[H)
                else if (n >= 2 && s[1] == 'F') { cursor = (int)line.size(); redraw(); } // END (\033[F)
                else if (n >= 3 && s[1] == '3' && s[2] == '~') {           // DELETE key  (\033[3~)
                    if (cursor < (int)line.size()) { line.erase(cursor, 1); redraw(); }
                }
            }
            // SS3 sequences: \033O ... (application-mode arrow keys, e.g., \033OA = UP)
            else if (n >= 1 && s[0] == 'O') {
                if (n >= 2 && s[1] == 'A') histBack();
                else if (n >= 2 && s[1] == 'B') histFwd();
                else if (n >= 2 && s[1] == 'C') curRight();
                else if (n >= 2 && s[1] == 'D') curLeft();
            }
        }
#endif
        else if (c == 1) {   // Ctrl-A → HOME
            cursor = 0; redraw();
        } else if (c == 5) {   // Ctrl-E → END
            cursor = (int)line.size(); redraw();
        } else if (c == 11) {  // Ctrl-K → kill to end of line
            line.erase(cursor); redraw();
        } else if (c == 23) {  // Ctrl-W → delete word backward
            while (cursor > 0 && line[cursor-1] == ' ') { line.erase(--cursor, 1); }
            while (cursor > 0 && line[cursor-1] != ' ') { line.erase(--cursor, 1); }
            redraw();
        } else if (c >= 32 && c < 127) {
            line.insert(cursor++, 1, (char)c);
            printf("%c", c);
            if (cursor < (int)line.size()) {
                printf("%s\033[%dD", line.substr(cursor).c_str(), (int)(line.size()-cursor));
            }
            fflush(stdout);
        }
    }

#ifndef _WIN32
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
#endif
    while (!line.empty() && isspace(line.back())) line.pop_back();
    if (!line.empty()) addHistory(line);
    return line;
}

inline void LineReader::addHistory(const std::string& line) {
    if (!line.empty()) {
        // Don't add duplicates of the most recent entry
        if (!_history.empty() && _history.back() == line) return;
        _history.push_back(line);
        if (_history.size() > 1000) _history.erase(_history.begin());
    }
}

inline std::string LineReader::readCommand() {
    std::string fullCmd = readLine();
    if (fullCmd.empty()) return "";
    if (!fullCmd.empty() && fullCmd[0] == '\\') return fullCmd;  // meta-command
    if (!fullCmd.empty() && fullCmd.back() == ';') return fullCmd;  // has terminator

    // `source <file>` is a standalone client command (mysql style) that needs
    // no trailing ';' — return it immediately instead of waiting for more input.
    {
        std::string t = fullCmd;
        size_t k = t.find_first_not_of(" \t");
        if (k != std::string::npos) t = t.substr(k);
        else t.clear();
        if (startsWithKeyword(t, "source")) return fullCmd;
    }
    // Multi-line continuation — use simple getline for continuation to avoid raw mode issues
    std::string saved = _prompt;
    bool inContinuation = true;
    while (inContinuation) {
        if (_interactive) {
            printf("    -> ");
            fflush(stdout);
        }
        std::string cont;
        if (!std::getline(std::cin, cont)) break;
        while (!cont.empty() && isspace(cont.back())) cont.pop_back();

        if (cont.empty()) break;
        if (!cont.empty() && cont[0] == '\\') { fullCmd = cont; break; }
        fullCmd += " " + cont;
        if (!cont.empty() && cont.back() == ';') break;  // keep semicolons
        // Non-interactive mode: stop after first continuation line if no semicolon
        if (!_interactive) break;
    }
    _prompt = saved;
    // Add multi-line commands to history as well
    if (!fullCmd.empty() && fullCmd[0] != '\\') addHistory(fullCmd);
    return fullCmd;
}

// ============================================================================
// EtDBShell Implementation
// ============================================================================

inline EtDBShell::EtDBShell(const ShellConfig& cfg)
    : _config(cfg)
    , _reader("etherdb > ") {}

inline EtDBShell::~EtDBShell() {
    shutdown();
}

// 查询 SHOW DATABASES 解析当前数据库的时间精度（0=ms, 1=us, 2=ns）
// 库名比较大小写不敏感（shell 的 USE 解析结果是全大写）
inline uint8_t EtDBShell::lookupPrecision(const std::string& db) {
    if (db.empty()) return 0;
    std::string dbl = db;
    for (auto& c : dbl) c = (char)tolower((unsigned char)c);
    auto r = _client.query("SHOW DATABASES");
    for (int row = 0; row < r.rowCount(); ++row) {
        if (r.colCount() < 5) continue;
        // Zero-copy access to string columns (SHOW results are NCHAR columns)
        std::string name(r.stringValue(row, 0));
        for (auto& c : name) c = (char)tolower((unsigned char)c);
        if (name == dbl) {
            std::string_view prec = r.stringValue(row, 4);
            if (prec == "ns") return 2;
            if (prec == "us") return 1;
            return 0;
        }
    }
    return 0;
}

inline bool EtDBShell::init() {
    printf("Connecting to %s:%d as %s...\n",
           _config.host.c_str(), _config.port, _config.user.c_str());

    if (!_client.connect(_config.host, _config.port,
                          _config.user.c_str(), _config.password.c_str(), "")) {
        printf("ERROR: Failed to connect to %s:%d\n",
               _config.host.c_str(), _config.port);
        return false;
    }
    return true;
}

inline void EtDBShell::shutdown() {
    _running = false;
    _client.close();
}

inline void EtDBShell::showBanner() {


    printf("\n");
    printf("  +------------------------------------------+\n");
    printf("  |   EtherDB Interactive Shell v0.1.0       |\n");
    printf("  |   Connected to %-25s |\n", 
       (_config.host + ":" + std::to_string(_config.port)).c_str());
    printf("  +------------------------------------------+\n");
    printf("  Type \\? for help, \\q to quit.\n\n");
}

inline void EtDBShell::showHelp() {
    printf("\n");
    printf("  Meta-commands:\n");
    printf("    \\?            Show this help\n");
    printf("    \\q            Quit\n");
    printf("    \\d [table]    Describe table\n");
    printf("    \\l            List databases (SHOW DATABASES)\n");
    printf("    \\c            Clear screen\n");
    printf("    source <file> Run a .sql script (alias: \\\\source / \\\\. <file>)\n");
    printf("\n");
    printf("  Batch mode:\n");
    printf("    etherdb -h <host> -p <port> -u root -P xxx < schema.sql\n");
    printf("\n");
    printf("  DDL Commands:\n");
    printf("    CREATE DATABASE <name> [KEEP 3650] [REPLICA 1];\n");
    printf("    DROP DATABASE <name>;\n");
    printf("    CREATE TABLE <name> (ts TIMESTAMP, col1 INT, col2 FLOAT);\n");
    printf("    DROP TABLE <name>;\n");
    printf("    SHOW DATABASES;\n");
    printf("    SHOW TABLES;\n");
    printf("    USE <db_name>;\n");
    printf("\n");
    printf("  DML Commands:\n");
    printf("    SELECT * FROM test;\n");
    printf("    SELECT * FROM test WHERE temperature > 25.0;\n");
    printf("    SELECT COUNT(*), AVG(temperature) FROM test;\n");
    printf("    SELECT * FROM test ORDER BY temperature DESC LIMIT 5;\n");
    printf("    INSERT INTO test VALUES(1716364800000, 25.5, 1013.2);\n");
    printf("\n");
}

inline std::string EtDBShell::nowStr() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", localtime(&t));
    return buf;
}

inline bool EtDBShell::executeSQL(const std::string& sql) {
    if (sql.empty()) return true;

    // ── Client-side SQL pre-validation ──
    // Perform basic syntax checks before sending to server.
    // This gives immediate, user-friendly error messages in the shell.
    {
        std::string upper = sql;
        for (auto& c : upper) c = (char)toupper((unsigned char)c);
        size_t p = 0;
        while (p < upper.size() && isspace((unsigned char)upper[p])) ++p;

        // Check for SELECT without WHERE — detect missing WHERE keyword
        if (upper.find("SELECT ", p) == p) {
            // Find FROM position
            size_t fromPos = upper.find(" FROM ");
            if (fromPos != std::string::npos) {
                // Skip past the table name (identifier after FROM)
                size_t afterFrom = fromPos + 6;  // skip " FROM "
                while (afterFrom < upper.size() && isspace((unsigned char)upper[afterFrom])) ++afterFrom;
                // Skip table name (identifier)
                while (afterFrom < upper.size() && (isalnum((unsigned char)upper[afterFrom]) || upper[afterFrom] == '_' || upper[afterFrom] == '.'))
                    ++afterFrom;
                while (afterFrom < upper.size() && isspace((unsigned char)upper[afterFrom])) ++afterFrom;

                // Check what comes after the table name
                if (afterFrom < upper.size() && upper[afterFrom] != ';') {
                    std::string rest = upper.substr(afterFrom);
                    size_t rs = 0;
                    while (rs < rest.size() && isspace((unsigned char)rest[rs])) ++rs;
                    rest = rest.substr(rs);

                    // Extract first token
                    std::string firstToken;
                    size_t tk = 0;
                    while (tk < rest.size() && (isalnum((unsigned char)rest[tk]) || rest[tk] == '_'))
                        firstToken += rest[tk++];

                    // Valid clause keywords after table name
                    bool isValidClause = (firstToken == "WHERE" || firstToken == "GROUP" ||
                                          firstToken == "ORDER" || firstToken == "LIMIT" ||
                                          firstToken == "HAVING");

                    if (!isValidClause) {
                        // Check if this looks like a WHERE clause missing the keyword
                        bool hasComparison = (rest.find_first_of("><=") != std::string::npos);
                        if (!hasComparison) {
                            auto hasWord = [&](const char* w) {
                                size_t pos = rest.find(w);
                                if (pos == std::string::npos) return false;
                                bool left  = (pos == 0 || !isalnum((unsigned char)rest[pos-1]));
                                bool right = (pos + strlen(w) >= rest.size() ||
                                              !isalnum((unsigned char)rest[pos+strlen(w)]));
                                return left && right;
                            };
                            hasComparison = hasWord("AND") || hasWord("OR") || hasWord("NOT");
                        }

                        std::string badToken = firstToken;
                        if (hasComparison) {
                            _formatter.printError(
                                "Syntax error: missing WHERE before '" + badToken +
                                "'. Did you forget the WHERE keyword?\n"
                                "  Example: SELECT ... FROM table WHERE ts >= 100 AND ts < 200");
                        } else {
                            _formatter.printError(
                                "Syntax error: unexpected '" + badToken +
                                "' after table name. Expected WHERE, GROUP, ORDER, LIMIT, HAVING or end of statement.");
                        }
                        return false;
                    }
                }
            }
        }
    }

    // ── Auto-LIMIT 100 for SELECT without explicit LIMIT ──
    // Decode at most 100 rows (no full decode), and separately run COUNT(*)
    // to show the total matching rows on the UI (TDengine-shell style).
    // Skip for aggregate queries (COUNT/SUM/AVG/MIN/MAX) — LIMIT would cap them.
    std::string effectiveSql = sql;
    std::string countSql;    // non-empty → run COUNT(*) for the total
    {
        std::string upper = sql;
        for (auto& c : upper) c = (char)toupper((unsigned char)c);
        size_t p = 0;
        while (p < upper.size() && isspace((unsigned char)upper[p])) ++p;
        if (upper.find("SELECT ", p) == p) {
            bool isAgg = (upper.find("COUNT(") != std::string::npos)
                      || (upper.find("SUM(")   != std::string::npos)
                      || (upper.find("AVG(")   != std::string::npos)
                      || (upper.find("MIN(")   != std::string::npos)
                      || (upper.find("MAX(")   != std::string::npos);
            if (!isAgg && upper.find("LIMIT ") == std::string::npos) {
                effectiveSql = sql + " LIMIT 100";
                // Build "SELECT COUNT(*) FROM <rest>" — keep WHERE/ORDER clauses
                size_t fromIdx = upper.find(" FROM ", p + 7);
                if (fromIdx != std::string::npos) {
                    countSql = "SELECT COUNT(*) " + sql.substr(fromIdx + 1);
                }
            }
        }
    }

    auto start = std::chrono::steady_clock::now();

    // dbId 路由由 EtDBClient 自动按当前 USE 库解析（不再手工传 _currentdbId，
    // 避免 shell 缓存的大写/错误 dbId 路由到错误 dbnode）
    EtDBResult result = _client.query(effectiveSql);

    // Run COUNT(*) first to get the total matching rows (fast estimate path)
    int64_t countTotal = -1;
    if (!countSql.empty()) {
        EtDBResult cnt = _client.query(countSql);
        if (cnt.rowCount() > 0 && cnt.colCount() > 0) {
            countTotal = cnt.get(0, 0).iVal;
        }
    }

    auto end = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    if (!result.success() && !result.error().empty()) {
        _formatter.printError(result.error());
        return false;
    }

    // Empty result = DDL success (binary protocol: no content for CREATE/DROP/USE/ALTER)
    if (result.colCount() == 0) {
        // Check if this was an INSERT with submit errors
        if (result.submitErrors() > 0) {
            // INSERT result status: single NCHAR string column (columnar storage, zero-copy access)
            _formatter.printStatus(result.rowCount() == 0
                                       ? std::string("OK")
                                       : std::string(result.stringValue(0, 0)));
            printf("  Error details:\n");
            for (const auto& se : result.submitErrorEntries()) {
                printf("    row[%d]: error code %d\n", se.rowIndex, se.errorCode);
            }
        }
        // Track current DB for USE command
        std::string upper = sql;
        for (auto& c : upper) c = (char)toupper((unsigned char)c);
        size_t p = 0;
        while (p < upper.size() && isspace(upper[p])) ++p;
        if (upper.find("USE ", p) == p) {
            // 从原始 SQL 提取 db 名（保留原始大小写——服务端 db 名区分大小写）
            std::string rawDb;
            {
                const char* sp = sql.c_str();
                while (*sp && isspace((unsigned char)*sp)) ++sp;
                if (strncasecmp(sp, "use ", 4) == 0) {
                    sp += 4;
                    while (*sp && isspace((unsigned char)*sp)) ++sp;
                    while (*sp && !isspace((unsigned char)*sp) && *sp != ';') rawDb += *sp++;
                }
            }
            if (!rawDb.empty()) _currentDB = rawDb;
            // 解析 dbId（仅用于显示；实际路由由 EtDBClient 自动按当前库解析）
            _currentdbId = _client.resolveDBdbId(_currentDB);
            // Track ts precision so the ts column renders as readable time
            _precision = lookupPrecision(_currentDB);
            std::string status = "OK: Using database '" + _currentDB + "'";
                               //+ " (dbId=" + std::to_string(_currentdbId)
                               //+ " precision=" + std::to_string((int)_precision) + ")";
            _formatter.printStatus(status);
        } else {
            // Successful DDL (CREATE/DROP/ALTER/GRANT/REVOKE/TRUNCATE) is
            // acknowledged by the server with an empty reply. Show a clear
            // "Query OK" instead of a misleading error message — this is what
            // users expect when sourcing schema .sql files.
            std::string rest = trimWs(upper.substr(p));
            bool isDDL = startsWithKeyword(rest, "CREATE")
                      || startsWithKeyword(rest, "DROP")
                      || startsWithKeyword(rest, "ALTER")
                      || startsWithKeyword(rest, "GRANT")
                      || startsWithKeyword(rest, "REVOKE")
                      || startsWithKeyword(rest, "TRUNCATE")
                      || startsWithKeyword(rest, "COMMENT");
            if (isDDL)
                _formatter.printStatus("Query OK");
            else
                _formatter.printStatus("Unknown database or syntax error.");
        }
    } else {
        _formatter.print(result, _precision);
        // Show total matching rows when auto-limited: only `limit` rows decoded,
        // the total comes from a fast COUNT(*) estimate (no full decode).
        if (!countSql.empty() && countTotal >= 0 && countTotal > result.rowCount()) {
            printf("  --- Total %lld records, only showing first %d (use LIMIT to see more) ---\n",
                   (long long)countTotal, result.rowCount());
        } else if (effectiveSql != sql && result.rowCount() >= 100) {
            printf("  --- Showing up to %d rows (auto-limited, use explicit LIMIT for more) ---\n",
                   result.rowCount());
        }
        if (result.submitErrors() > 0) {
            printf("  --- Error details ---\n");
            for (const auto& se : result.submitErrorEntries()) {
                printf("  row[%d]: error code %d\n", se.rowIndex, se.errorCode);
            }
        }
    }

    if (_config.timing) {
        printf("  (query took %lld ms)\n", (long long)elapsed);
    }

    // Update prompt with current db
    std::string prompt = _currentDB.empty() ? "etherdb" : _currentDB;
    prompt += " > ";
    _reader.setPrompt(prompt);

    return true;
}

inline bool EtDBShell::executeInsert(const std::string& sql) {
    // Use unified query path which handles INSERT with meta-aware binary packing
    return executeSQL(sql);
}

// Execute a single statement extracted from a script/stdin. `source` lines are
// handled recursively; other client meta-commands are ignored inside scripts.
inline bool EtDBShell::executeStatement(const std::string& text) {
    ShellParser parser;
    auto cmd = parser.parse(text);
    switch (cmd.type) {
        case ShellParser::CmdType::SQL:
            return executeSQL(cmd.sql);
        case ShellParser::CmdType::META_SOURCE:
            return sourceFile(cmd.arg);
        default:
            // \q, \timing, ... have no meaning inside a script — ignore quietly.
            return true;
    }
}

// Run SQL statements from a stream (stdin or an open file).
//
// Line-oriented reader that understands:
//   * a whole-line `source <file>` client command (no ';' required)
//   * statement terminators (';'), even several on one line
//   * line comments (-- and #) and /* ... */ block comments (across lines)
//   * single/double-quoted strings (a ';' or comment marker inside a string
//     literal does not terminate the statement)
// Multi-line SQL statements are accumulated until the terminating ';'.
// Returns the number of statements that failed.
inline int EtDBShell::runScript(std::istream& in, const std::string& name) {
    std::string stmt;        // pending SQL statement (across lines)
    int count = 0, errors = 0;
    bool inBlockCmt = false; // /* ... */ open across lines

    auto flushStmt = [&]() {
        std::string s = trimWs(stmt);
        stmt.clear();
        if (s.empty()) return;
        ++count;
        if (!executeStatement(s)) ++errors;
    };

    // Whole-line `source <file>` handling. Returns true if the line was a
    // source command (already executed) and should not be scanned as SQL.
    auto handleSourceLine = [&](const std::string& raw) -> bool {
        if (inBlockCmt) return false;
        std::string t = trimWs(raw);
        if (!startsWithKeyword(t, "source")) return false;
        flushStmt();                                   // nothing normally pending
        std::string arg = trimWs(t.substr(6));         // strlen("source")
        // Drop a trailing ';' and any trailing line comment
        size_t sc = arg.find(';');
        if (sc != std::string::npos) arg = arg.substr(0, sc);
        {
            size_t c2 = arg.find(" --");
            if (c2 != std::string::npos) arg = arg.substr(0, c2);
            else {
                size_t c3 = arg.find(" #");
                if (c3 != std::string::npos) arg = arg.substr(0, c3);
            }
        }
        arg = trimWs(arg);
        // Allow optional surrounding quotes around the path
        if (arg.size() >= 2 &&
            ((arg.front() == '"' && arg.back() == '"') ||
             (arg.front() == '\'' && arg.back() == '\''))) {
            arg = arg.substr(1, arg.size() - 2);
        }
        ++count;
        if (arg.empty()) {
            _formatter.printError("SOURCE requires a file path (source /path/xx.sql)");
            ++errors;
        } else if (!sourceFile(arg)) {
            ++errors;
        }
        return true;
    };

    std::string raw;
    while (std::getline(in, raw)) {
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();

        if (handleSourceLine(raw)) continue;

        // Scan the line into SQL text, honoring quotes/comments. Comment and
        // string state is per-line; only block comments may span lines.
        std::string buf;                 // SQL text contributed by this line
        bool inS = false, inD = false;
        size_t i = 0, n = raw.size();
        while (i < n) {
            char c = raw[i];

            if (inS) {                   // inside '...' string
                if (c == '\'') inS = false;
                buf.push_back(c);
                ++i;
                continue;
            }
            if (inD) {                   // inside "..." string
                if (c == '"') inD = false;
                buf.push_back(c);
                ++i;
                continue;
            }
            if (inBlockCmt) {            // inside /* ... */
                if (c == '*' && i + 1 < n && raw[i + 1] == '/') {
                    inBlockCmt = false;
                    i += 2;
                } else {
                    ++i;
                }
                continue;
            }
            if (c == '\'') { inS = true; buf.push_back(c); ++i; continue; }
            if (c == '"')  { inD = true; buf.push_back(c); ++i; continue; }
            if (c == '/' && i + 1 < n && raw[i + 1] == '*') {
                inBlockCmt = true;
                i += 2;
                continue;
            }
            if (c == '-' && i + 1 < n && raw[i + 1] == '-') break;  // line comment
            if (c == '#') break;                                    // line comment
            if (c == ';') {                 // statement terminator
                if (!trimWs(buf).empty()) {
                    stmt += (stmt.empty() ? "" : " ") + trimWs(buf);
                }
                buf.clear();
                flushStmt();
                ++i;
                continue;
            }
            buf.push_back(c);
            ++i;
        }
        // Remainder of the line continues the statement on the next line
        std::string b = trimWs(buf);
        if (!b.empty()) stmt += (stmt.empty() ? "" : " ") + b;
    }
    flushStmt();   // trailing statement without a terminating ';'

    printf("-- %s: %d statement(s) executed", name.c_str(), count);
    if (errors) printf(", %d error(s)", errors);
    printf("\n");
    return errors;
}

// Execute a .sql file via the `source` command. Returns true on success.
inline bool EtDBShell::sourceFile(const std::string& path) {
    if (path.empty()) {
        _formatter.printError("SOURCE requires a file path, e.g. source /path/xx.sql");
        return false;
    }
    std::ifstream in(path.c_str(), std::ios::binary);
    if (!in.is_open()) {
        std::string msg = "Cannot open file: " + path;
        if (errno != 0) { msg += " ("; msg += strerror(errno); msg += ")"; }
        _formatter.printError(msg);
        return false;
    }
    int errs = runScript(in, path);
    in.close();
    return errs == 0;
}

inline bool EtDBShell::executeMeta(const ShellParser::ParsedCommand& cmd) {
    switch (cmd.type) {
        case ShellParser::CmdType::META_QUIT:
            _running = false;
            printf("Bye.\n");
            return true;

        case ShellParser::CmdType::META_HELP:
            showHelp();
            return true;

        case ShellParser::CmdType::META_CLEAR:
            printf("\033[2J\033[H");  // ANSI clear screen
            return true;

        case ShellParser::CmdType::META_FORMAT: {
            std::string fmt = cmd.arg;
            for (auto& c : fmt) c = tolower(c);
            if (fmt == "csv") {
                _formatter.setFormat(OutputFormat::CSV);
                printf("Output format: CSV\n");
            } else if (fmt == "vertical" || fmt == "v") {
                _formatter.setFormat(OutputFormat::VERTICAL);
                printf("Output format: Vertical\n");
            } else {
                _formatter.setFormat(OutputFormat::TABLE);
                printf("Output format: Table\n");
            }
            return true;
        }

        case ShellParser::CmdType::META_TIMING:
            _config.timing = !_config.timing;
            printf("Timing is %s.\n", _config.timing ? "on" : "off");
            return true;

        case ShellParser::CmdType::META_HEADER:
            _config.header = !_config.header;
            printf("Header is %s.\n", _config.header ? "on" : "off");
            return true;

        case ShellParser::CmdType::META_DESCRIBE: {
            std::string tbl = cmd.arg.empty() ? "test" : cmd.arg;
            // Send a DESCRIBE-like query
            executeSQL("SELECT * FROM " + tbl + " LIMIT 0");
            // Also show column info
            executeSQL("SELECT * FROM " + tbl + " LIMIT 1");
            return true;
        }

        case ShellParser::CmdType::META_LIST_DB:
            _formatter.printStatus("(stub: SHOW DATABASES not yet implemented)");
            return true;

        case ShellParser::CmdType::META_SOURCE:
            // `source /path/xx.sql` — run a SQL script file
            sourceFile(cmd.arg);
            return true;

        case ShellParser::CmdType::UNKNOWN:
            printf("Unknown command. Type \\? for help.\n");
            return true;

        default:
            return true;
    }
}

inline int EtDBShell::run() {
    if (!_client.isConnected()) {
        printf("ERROR: Not connected.\n");
        return 1;
    }

    _running = true;

    // Non-interactive: stdin has been redirected from a file/pipe
    // (e.g. `etherdb -u root -P xxx < schema.sql`) → run it as a SQL script.
    if (!_reader.isInteractive()) {
        runScript(std::cin, "<stdin>");
        _running = false;
        return 0;
    }

    showBanner();

    while (_running) {
        std::string line = _reader.readCommand();

        // Empty line / EOF: skip
        if (line.empty()) continue;

        // Split by semicolon for multi-statement lines
        std::vector<std::string> statements;
        size_t start = 0;
        for (size_t i = 0; i < line.size(); ++i) {
            if (line[i] == ';') {
                std::string stmt = line.substr(start, i - start);
                while (!stmt.empty() && isspace(stmt.front())) stmt.erase(0, 1);
                while (!stmt.empty() && isspace(stmt.back())) stmt.pop_back();
                if (!stmt.empty()) statements.push_back(stmt);
                start = i + 1;
            }
        }
        // Remaining after last semicolon
        std::string remaining = line.substr(start);
        while (!remaining.empty() && isspace(remaining.front())) remaining.erase(0, 1);
        while (!remaining.empty() && isspace(remaining.back())) remaining.pop_back();
        if (!remaining.empty()) statements.push_back(remaining);

        // Execute each statement
        for (const auto& stmt : statements) {
            ShellParser parser;
            auto cmd = parser.parse(stmt);
            if (cmd.type == ShellParser::CmdType::SQL) {
                // All SQL (including INSERT) goes through unified query path
                executeSQL(cmd.sql);
            } else {
                executeMeta(cmd);
            }
        }
    }

    return 0;
}

} // namespace Shell
} // namespace ETDB

#endif // ETHERDB_SHELL_H
