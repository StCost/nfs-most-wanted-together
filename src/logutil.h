#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

// Session log archives and log-line rate limits (src/log.cpp, the PKT_EVENT log in net.cpp).
namespace LogUtil {
    constexpr int      kKeepSessions    = 5;
    constexpr uint64_t kMaxArchiveBytes = 8ull * 1024ull * 1024ull;   // all archived sessions together
    constexpr unsigned kPeerEventLogWindowMs = 30000;

    // MWTogether-YYYYMMDD-HHMMSS.log (local time of the previous session).
    inline std::string SessionLogName(int year, int month, int day, int hour, int minute, int second) {
        char b[48];
        snprintf(b, sizeof(b), "MWTogether-%04d%02d%02d-%02d%02d%02d.log",
                 year, month, day, hour, minute, second);
        return b;
    }

    inline bool ParseSessionLogName(const char* name, int* year, int* month, int* day,
                                    int* hour, int* minute, int* second) {
        if (!name) return false;
        int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
        char extra = 0;
        if (sscanf(name, "MWTogether-%4d%2d%2d-%2d%2d%2d.log%c", &y, &mo, &d, &h, &mi, &s, &extra) != 6)
            return false;
        if (y < 2000 || y > 2100 || mo < 1 || mo > 12 || d < 1 || d > 31) return false;
        if (h < 0 || h > 23 || mi < 0 || mi > 59 || s < 0 || s > 59) return false;
        if (year) *year = y;
        if (month) *month = mo;
        if (day) *day = d;
        if (hour) *hour = h;
        if (minute) *minute = mi;
        if (second) *second = s;
        return true;
    }

    inline bool IsSessionLogName(const char* name) {
        return ParseSessionLogName(name, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    }

    struct SessionFile {
        std::string name;
        uint64_t    size = 0;
        int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    };

    // Oldest first. Names to delete so at most `keep` remain and their total size is <= maxBytes.
    // The newest archive is always kept, even if it alone is over the cap (so the last session is never dropped).
    inline std::vector<std::string> SessionLogsToDelete(std::vector<SessionFile> files, int keep, uint64_t maxBytes) {
        std::vector<std::string> drop;
        if (files.empty()) return drop;
        std::sort(files.begin(), files.end(), [](const SessionFile& a, const SessionFile& b) {
            if (a.year != b.year) return a.year < b.year;
            if (a.month != b.month) return a.month < b.month;
            if (a.day != b.day) return a.day < b.day;
            if (a.hour != b.hour) return a.hour < b.hour;
            if (a.minute != b.minute) return a.minute < b.minute;
            if (a.second != b.second) return a.second < b.second;
            return a.name < b.name;
        });
        auto total = [&]() {
            uint64_t n = 0;
            for (const auto& f : files) n += f.size;
            return n;
        };
        if (keep < 1) keep = 1;
        while (files.size() > 1 && ((int)files.size() > keep || total() > maxBytes)) {
            drop.push_back(files.front().name);
            files.erase(files.begin());
        }
        return drop;
    }

    // The peer repeats a race invitation for a few seconds (UDP). Log the start once per event,
    // not on every copy (otherwise "Peer started event" appears several times every 1.5 s).
    inline bool ShouldLogPeerEvent(uint32_t eventHash, uint32_t lastLoggedHash, unsigned elapsedMs,
                                   unsigned windowMs = kPeerEventLogWindowMs) {
        if (eventHash != lastLoggedHash) return true;
        return elapsedMs >= windowMs;
    }
}
