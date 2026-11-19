#pragma once
#include <windows.h>
#include <cstdio>
#include <cstdarg>

// Simple thread-safe file logger: scripts\CoopMod.log (truncated each launch)
namespace Log {
    void Init(const char* path);
    void Write(const char* fmt, ...);
}

#define LOG(...) Log::Write(__VA_ARGS__)
