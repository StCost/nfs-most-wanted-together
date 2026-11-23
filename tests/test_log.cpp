// Session log rotation and the "Peer started event" rate limit (src/logutil.h).
#include "mwt_test.h"
#include "../src/logutil.h"
#include "../src/log.h"
#include <windows.h>
#include <string>

using namespace LogUtil;

TEST(log_session_name_round_trips) {
    std::string n = SessionLogName(2026, 11, 23, 2, 5, 31);
    CHECK_STR(n.c_str(), "MWTogether-20261123-020531.log");
    int y=0, mo=0, d=0, h=0, mi=0, s=0;
    CHECK(ParseSessionLogName(n.c_str(), &y, &mo, &d, &h, &mi, &s));
    CHECK_EQ(y, 2026); CHECK_EQ(mo, 11); CHECK_EQ(d, 23);
    CHECK_EQ(h, 2); CHECK_EQ(mi, 5); CHECK_EQ(s, 31);
    CHECK(IsSessionLogName(n.c_str()));
    CHECK(!IsSessionLogName("MWTogether.log"));
    CHECK(!IsSessionLogName("MWTogether-last.log"));
    CHECK(!IsSessionLogName("MWTogether-20261123.log"));
    CHECK(!IsSessionLogName("MWTogether-20261123-020531.log.bak"));
    CHECK(!IsSessionLogName("notes-20261123-020531.log"));
}

static SessionFile F(const char* name, uint64_t size) {
    SessionFile s;
    s.name = name; s.size = size;
    CHECK(ParseSessionLogName(name, &s.year, &s.month, &s.day, &s.hour, &s.minute, &s.second));
    return s;
}

TEST(log_keeps_the_last_five_sessions) {
    std::vector<SessionFile> v = {
        F("MWTogether-20261121-100000.log", 100),
        F("MWTogether-20261122-100000.log", 100),
        F("MWTogether-20261123-010000.log", 100),
        F("MWTogether-20261123-020000.log", 100),
        F("MWTogether-20261123-030000.log", 100),
        F("MWTogether-20261120-090000.log", 100),
    };
    auto drop = SessionLogsToDelete(v, kKeepSessions, kMaxArchiveBytes);
    REQUIRE(drop.size() == 1);
    CHECK_STR(drop[0].c_str(), "MWTogether-20261120-090000.log");
    CHECK(SessionLogsToDelete({ v[0], v[1], v[2] }, kKeepSessions, kMaxArchiveBytes).empty());
}

TEST(log_archive_size_cap_drops_oldest_keeps_newest) {
    // 3 logs, cap 250: drop the two oldest so the newest 200-byte file remains (under cap if possible,
    // but a single oversized newest file is still kept).
    std::vector<SessionFile> v = {
        F("MWTogether-20261121-100000.log", 200),
        F("MWTogether-20261122-100000.log", 200),
        F("MWTogether-20261123-020000.log", 200),
    };
    auto drop = SessionLogsToDelete(v, kKeepSessions, 250);
    REQUIRE(drop.size() == 2);
    CHECK_STR(drop[0].c_str(), "MWTogether-20261121-100000.log");
    CHECK_STR(drop[1].c_str(), "MWTogether-20261122-100000.log");

    std::vector<SessionFile> big = { F("MWTogether-20261123-020000.log", 20ull * 1024ull * 1024ull) };
    CHECK(SessionLogsToDelete(big, kKeepSessions, kMaxArchiveBytes).empty());
}

TEST(log_peer_started_event_is_not_repeated_every_invite) {
    const uint32_t ev = 0xAABBCCDDu;
    CHECK(ShouldLogPeerEvent(ev, 0, 0));
    CHECK(!ShouldLogPeerEvent(ev, ev, 0));
    CHECK(!ShouldLogPeerEvent(ev, ev, 1500));
    CHECK(!ShouldLogPeerEvent(ev, ev, 4500));          // three invites 1.5 s apart
    CHECK(!ShouldLogPeerEvent(ev, ev, 29999));
    CHECK(ShouldLogPeerEvent(ev, ev, 30000));
    CHECK(ShouldLogPeerEvent(0x11111111u, ev, 0));     // a different event
}


TEST(log_rotate_keeps_previous_session_and_prunes) {
    char tmp[MAX_PATH] = "";
    GetTempPathA(MAX_PATH, tmp);
    char dir[MAX_PATH];
    snprintf(dir, sizeof(dir), "%smwt-log-%u\\", tmp, (unsigned)GetTickCount());
    CreateDirectoryA(dir, nullptr);
    auto put = [&](const char* name, const char* body) {
        std::string path = std::string(dir) + name;
        FILE* f = nullptr;
        REQUIRE(fopen_s(&f, path.c_str(), "wb") == 0 && f);
        fputs(body, f);
        fclose(f);
    };
    auto exists = [&](const char* name) {
        std::string path = std::string(dir) + name;
        return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    };
    auto wipe = [&]() {
        WIN32_FIND_DATAA fd{};
        HANDLE h = FindFirstFileA((std::string(dir) + "*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (fd.cFileName[0] == '.' ) continue;
                DeleteFileA((std::string(dir) + fd.cFileName).c_str());
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
        RemoveDirectoryA(dir);
    };

    put("MWTogether.log", "previous session\n");
    put("MWTogether-20261118-100000.log", "old1\n");
    put("MWTogether-20261119-100000.log", "old2\n");
    put("MWTogether-20261120-100000.log", "old3\n");
    put("MWTogether-20261121-100000.log", "old4\n");
    put("MWTogether-20261122-100000.log", "old5\n");
    put("MWTogether-20261123-010000.log", "old6\n");
    put("MWTogether-last.log", "legacy last log, leave it\n");

    std::string current = std::string(dir) + "MWTogether.log";
    Log::RotateAndPrune(current.c_str());

    CHECK(!exists("MWTogether.log"));                  // moved aside, not overwritten
    CHECK(exists("MWTogether-last.log"));              // old single-backup name is left alone
    CHECK(!exists("MWTogether-20261118-100000.log"));  // oldest of the 6+new dropped
    // newest of the pre-existing six, plus the just-archived current session, stay in the last 5
    CHECK(exists("MWTogether-20261123-010000.log"));

    int sessions = 0;
    WIN32_FIND_DATAA fd{};
    HANDLE h = FindFirstFileA((std::string(dir) + "MWTogether-*.log").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (LogUtil::IsSessionLogName(fd.cFileName)) ++sessions;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    CHECK_EQ(sessions, kKeepSessions);
    // the current session was archived under a timestamped name
    bool haveArchivedCurrent = false;
    h = FindFirstFileA((std::string(dir) + "MWTogether-*.log").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!LogUtil::IsSessionLogName(fd.cFileName)) continue;
            FILE* f = nullptr;
            if (fopen_s(&f, (std::string(dir) + fd.cFileName).c_str(), "rb") == 0 && f) {
                char buf[64] = "";
                fgets(buf, sizeof(buf), f);
                fclose(f);
                if (strstr(buf, "previous session")) haveArchivedCurrent = true;
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    CHECK(haveArchivedCurrent);
    wipe();
}
