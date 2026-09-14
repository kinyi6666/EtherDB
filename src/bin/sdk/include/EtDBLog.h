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
 * EtDBLog.h — 客户端轻量日志
 *
 * 自包含的最小日志实现，提供与服务器 base/Logging.h 兼容的接口：
 *   EtherDB::Logger::setLogLevel() / logLevel()
 *   LOG_DEBUG / LOG_INFO / LOG_ERROR 流式宏
 *   tDebug(fmt, ...) printf 风格调试宏
 */

#ifndef ETHERDB_CLIENT_LOG_H
#define ETHERDB_CLIENT_LOG_H

#include <cstdio>
#include <cstdarg>
#include <string>
#include <sstream>

namespace EtherDB {

// ============================================================================
// Logger — 极简日志器，输出到 stderr
// ============================================================================
class Logger {
public:
    enum LogLevel {
        LTRACE,
        LDEBUG,
        LINFO,
        LWARN,
        LERROR,
        LFATAL,
        LNUM_LOG_LEVELS,
    };

    Logger(const char* file, int line, LogLevel level = LINFO)
        : _file(file ? file : ""), _line(line), _level(level) {}

    ~Logger() {
        std::string msg = _stream.str();
        if (!msg.empty()) {
            fprintf(stderr, "[%s:%d] %s\n", _file.c_str(), _line, msg.c_str());
        }
    }

    std::ostringstream& stream() { return _stream; }

    // inline 函数内的 static 局部变量在多个 TU 间共享同一实例；
    // 返回引用以便 setLogLevel 直接写入
    static LogLevel& logLevel() { static LogLevel lv = LINFO; return lv; }
    static void setLogLevel(LogLevel lv) { logLevel() = lv; }

private:
    std::string _file;
    int         _line;
    LogLevel    _level;
    std::ostringstream _stream;
};

#define LOG_DEBUG if (EtherDB::Logger::logLevel() <= EtherDB::Logger::LDEBUG) \
  EtherDB::Logger(__FILE__, __LINE__, EtherDB::Logger::LDEBUG).stream()
#define LOG_INFO if (EtherDB::Logger::logLevel() <= EtherDB::Logger::LINFO) \
  EtherDB::Logger(__FILE__, __LINE__, EtherDB::Logger::LINFO).stream()
#define LOG_WARN EtherDB::Logger(__FILE__, __LINE__, EtherDB::Logger::LWARN).stream()
#define LOG_ERROR EtherDB::Logger(__FILE__, __LINE__, EtherDB::Logger::LERROR).stream()

} // namespace EtherDB

namespace ETDB {

// printf 风格格式化辅助（供 tDebug 使用）
inline std::string _etdbFormat(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return std::string(buf);
}

} // namespace ETDB

#define tDebug(fmt, ...) LOG_DEBUG << "ETDB " << ETDB::_etdbFormat(fmt, ##__VA_ARGS__)

#endif // ETHERDB_CLIENT_LOG_H
