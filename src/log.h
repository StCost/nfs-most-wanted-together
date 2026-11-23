#pragma once
#include <windows.h>
#include <cstdio>
#include <cstdarg>

// Thread-safe file logger: scripts\MWTogether.log for the current session. A previous
// MWTogether.log is renamed to MWTogether-YYYYMMDD-HHMMSS.log (last ~5 sessions, size-capped).
namespace Log {
    void Init(const char* path);
    void Write(const char* fmt, ...);
    // Move an existing current log aside and drop old archives. Safe to call without Init (tests).
    void RotateAndPrune(const char* currentPath);
}

#define LOG(...) Log::Write(__VA_ARGS__)
