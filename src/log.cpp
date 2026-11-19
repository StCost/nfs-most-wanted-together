#include "log.h"
#include <share.h>

namespace {
    FILE* g_file = nullptr;
    CRITICAL_SECTION g_cs;
    bool g_init = false;
}

void Log::Init(const char* path) {
    if (g_init) return;
    InitializeCriticalSection(&g_cs);
    // _SH_DENYWR: other programs may read the log while the game runs
    g_file = _fsopen(path, "w", _SH_DENYWR);
    g_init = true;
}

void Log::Write(const char* fmt, ...) {
    if (!g_init || !g_file) return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    SYSTEMTIME st;
    GetLocalTime(&st);
    EnterCriticalSection(&g_cs);
    fprintf(g_file, "[%02d:%02d:%02d.%03d] %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, buf);
    fflush(g_file);
    LeaveCriticalSection(&g_cs);
}
