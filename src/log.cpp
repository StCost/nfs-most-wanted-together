#include "log.h"
#include "logutil.h"
#include <share.h>
#include <string>
#include <vector>

namespace {
    FILE* g_file = nullptr;
    CRITICAL_SECTION g_cs;
    bool g_init = false;

    std::string DirOf(const char* path) {
        std::string s = path ? path : "";
        size_t slash = s.find_last_of("\\/");
        if (slash == std::string::npos) return "";
        return s.substr(0, slash + 1);
    }
    bool FileExists(const char* path) {
        DWORD a = GetFileAttributesA(path);
        return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
    }
    uint64_t FileSizeOf(const char* path) {
        WIN32_FILE_ATTRIBUTE_DATA d{};
        if (!GetFileAttributesExA(path, GetFileExInfoStandard, &d)) return 0;
        return ((uint64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow;
    }
}

void Log::RotateAndPrune(const char* currentPath) {
    if (!currentPath || !currentPath[0]) return;
    std::string dir = DirOf(currentPath);
    if (FileExists(currentPath) && FileSizeOf(currentPath) > 0) {
        FILETIME ftWrite{};
        HANDLE h = CreateFileA(currentPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        SYSTEMTIME utc{}, local{};
        bool gotTime = false;
        if (h != INVALID_HANDLE_VALUE) {
            FILETIME c{}, a{};
            if (GetFileTime(h, &c, &a, &ftWrite) && FileTimeToSystemTime(&ftWrite, &utc) &&
                SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local))
                gotTime = true;
            CloseHandle(h);
        }
        if (!gotTime) GetLocalTime(&local);
        std::string name = LogUtil::SessionLogName(local.wYear, local.wMonth, local.wDay,
                                                   local.wHour, local.wMinute, local.wSecond);
        std::string dest = dir + name;
        // same-second relaunch: pick the next free second
        for (int i = 0; i < 30 && FileExists(dest.c_str()); ++i) {
            unsigned sec = ((unsigned)local.wSecond + (unsigned)i + 1u) % 60u;
            unsigned min = (unsigned)local.wMinute + (((unsigned)local.wSecond + (unsigned)i + 1u) / 60u);
            unsigned hour = (unsigned)local.wHour + min / 60u;
            name = LogUtil::SessionLogName(local.wYear, local.wMonth, local.wDay,
                                           (int)(hour % 24u), (int)(min % 60u), (int)sec);
            dest = dir + name;
        }
        if (!FileExists(dest.c_str()))
            MoveFileA(currentPath, dest.c_str());
    }

    std::vector<LogUtil::SessionFile> found;
    WIN32_FIND_DATAA fd{};
    HANDLE f = FindFirstFileA((dir + "MWTogether-*.log").c_str(), &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            if (!LogUtil::IsSessionLogName(fd.cFileName)) continue;
            LogUtil::SessionFile s;
            s.name = fd.cFileName;
            s.size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            if (!LogUtil::ParseSessionLogName(fd.cFileName, &s.year, &s.month, &s.day, &s.hour, &s.minute, &s.second))
                continue;
            found.push_back(s);
        } while (FindNextFileA(f, &fd));
        FindClose(f);
    }
    for (const std::string& drop : LogUtil::SessionLogsToDelete(found, LogUtil::kKeepSessions, LogUtil::kMaxArchiveBytes))
        DeleteFileA((dir + drop).c_str());
}

void Log::Init(const char* path) {
    if (g_init) return;
    InitializeCriticalSection(&g_cs);
    // keep the previous session: _fsopen(..., "w") would otherwise erase it
    RotateAndPrune(path);
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
