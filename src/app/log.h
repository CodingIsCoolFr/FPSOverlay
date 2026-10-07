// Small thread-safe file logger. One line per call, timestamped, UTF-8.
#pragma once

#include <string>

namespace logx {

// Opens (and rotates) the log file. Safe to call once at startup; calls before Init are dropped.
void Init(const std::wstring& path);
void Shutdown();

void Info(const char* fmt, ...);
void Warn(const char* fmt, ...);
void Error(const char* fmt, ...);

} // namespace logx
