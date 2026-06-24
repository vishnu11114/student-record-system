// server/logger.h
#pragma once

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>

namespace logger {

enum class Level { Debug, Info, Warn, Error };

inline std::mutex& mutex() {
    static std::mutex m;
    return m;
}

inline Level& minLevel() {
    static Level lvl = Level::Info;
    return lvl;
}

inline const char* levelTag(Level l) {
    switch (l) {
        case Level::Debug: return "DEBUG";
        case Level::Info:  return "INFO ";
        case Level::Warn:  return "WARN ";
        case Level::Error: return "ERROR";
    }
    return "?    ";
}

inline std::string timestamp() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count() % 1000;
    std::tm tm;
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream os;
    os << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << '.'
       << std::setw(3) << std::setfill('0') << ms;
    return os.str();
}

inline void log(Level lvl, const std::string& msg) {
    if (static_cast<int>(lvl) < static_cast<int>(minLevel())) return;
    std::lock_guard<std::mutex> lock(mutex());
    std::cerr << '[' << timestamp() << "] [" << levelTag(lvl) << "] "
              << msg << '\n';
}

inline void debug(const std::string& m) { log(Level::Debug, m); }
inline void info (const std::string& m) { log(Level::Info,  m); }
inline void warn (const std::string& m) { log(Level::Warn,  m); }
inline void error(const std::string& m) { log(Level::Error, m); }

} // namespace logger
