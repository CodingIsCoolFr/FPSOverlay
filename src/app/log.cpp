#include "app/log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace logx {
namespace {

std::mutex g_mutex;
HANDLE     g_file = INVALID_HANDLE_VALUE;

constexpr LONGLONG kRotateBytes = 1024 * 1024;

void Write(const char* level, const char* fmt, va_list ap)
{
    char msg[2048];
    vsnprintf(msg, sizeof(msg), fmt, ap);

    SYSTEMTIME st;
    GetLocalTime(&st);
    char line[2200];
    const int n = snprintf(line, sizeof(line), "%04u-%02u-%02u %02u:%02u:%02u.%03u [%s] %s\r\n",
                           st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                           st.wMilliseconds, level, msg);
    if (n <= 0) return;

    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(g_file, line, (DWORD)strnlen(line, sizeof(line)), &written, nullptr);
}

} // namespace

void Init(const std::wstring& path)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file != INVALID_HANDLE_VALUE) return;

    // Keep one previous log around so a crash report survives the next start.
    WIN32_FILE_ATTRIBUTE_DATA fad = {};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) {
        LARGE_INTEGER sz;
        sz.LowPart = fad.nFileSizeLow;
        sz.HighPart = (LONG)fad.nFileSizeHigh;
        if (sz.QuadPart > kRotateBytes) {
            const std::wstring old = path + L".old";
            MoveFileExW(path.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING);
        }
    }

    g_file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                         OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

void Shutdown()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file != INVALID_HANDLE_VALUE) {
        CloseHandle(g_file);
        g_file = INVALID_HANDLE_VALUE;
    }
}

void Info(const char* fmt, ...)  { va_list ap; va_start(ap, fmt); Write("info", fmt, ap);  va_end(ap); }
void Warn(const char* fmt, ...)  { va_list ap; va_start(ap, fmt); Write("warn", fmt, ap);  va_end(ap); }
void Error(const char* fmt, ...) { va_list ap; va_start(ap, fmt); Write("error", fmt, ap); va_end(ap); }

} // namespace logx
