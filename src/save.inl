// Included from game.cpp (after nis.inl).
// ---------------------------------------------------------------------------
// Career save kept by the host
//  Every player plays their own career profile. The client's save file (Documents\NFS Most Wanted\
//  <profile>\<profile>, ~64 KB, written by the game's autosave) is uploaded to the host whenever it
//  changes; the host keeps it in scripts\MWTogether-saves\<player>\<profile>.
//  When the client starts the game and connects before its profile is loaded, it asks the host for
//  that copy. If the client's own file is missing or older, the host's copy is written in its place
//  (the old file is kept as <profile>.mwt-backup) and the game then loads it as usual. The file is
//  never edited: the game checks an MD5 inside it.
// ---------------------------------------------------------------------------
#include <shlobj.h>

namespace {
    enum class Restore { Idle, Requested, Done } g_restore = Restore::Idle;
    DWORD    g_restoreAt = 0, g_saveFirstTick = 0;
    uint64_t g_uploadedTime = 0;
    char     g_saveStatus[96] = "";

    bool SafeName(const char* s, char* out, size_t n) {      // file-name safe copy; false if empty
        size_t j = 0;
        for (; *s && j + 1 < n; ++s) {
            unsigned char c = (unsigned char)*s;
            if (isalnum(c) || c == ' ' || c == '-' || c == '_') out[j++] = (char)c;
        }
        while (j && out[j - 1] == ' ') --j;
        out[j] = 0;
        return j > 0;
    }
    std::string DocsSavePath(const char* profile) {
        char docs[MAX_PATH];
        if (FAILED(SHGetFolderPathA(nullptr, CSIDL_PERSONAL, nullptr, SHGFP_TYPE_CURRENT, docs))) return "";
        return std::string(docs) + "\\NFS Most Wanted\\" + profile + "\\" + profile;
    }
    std::string HostDir(const char* owner) { return std::string(g_dataDir) + "MWTogether-saves\\" + owner; }

    uint64_t FileTime64(const std::string& path) {
        WIN32_FILE_ATTRIBUTE_DATA a;
        if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &a) || (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return 0;
        return ((uint64_t)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime;
    }
    bool ReadAll(const std::string& path, std::vector<uint8_t>& out) {
        FILE* f = _fsopen(path.c_str(), "rb", _SH_DENYNO);
        if (!f) return false;
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        bool ok = n > 0 && n <= (long)FILE_MAX;
        if (ok) { out.resize(n); ok = fread(out.data(), 1, n, f) == (size_t)n; }
        fclose(f);
        return ok;
    }
    bool WriteAll(const std::string& path, const uint8_t* d, uint32_t n, uint64_t mtime) {
        FILE* f = fopen(path.c_str(), "wb");
        if (!f) return false;
        bool ok = fwrite(d, 1, n, f) == n;
        fclose(f);
        if (ok && mtime) {
            HANDLE h = CreateFileA(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                FILETIME ft{ (DWORD)mtime, (DWORD)(mtime >> 32) };
                SetFileTime(h, nullptr, nullptr, &ft);
                CloseHandle(h);
            }
        }
        return ok;
    }
    bool ProfileLoaded() { uint8_t* fedb = nullptr; return Mem::Read(0x91CF90, fedb) && fedb && fedb[3]; }
    bool CurrentProfile(char* out, size_t n) {
        uint8_t* fedb = nullptr; uint8_t* up = nullptr; char raw[33] = {};
        if (!Mem::Read(0x91CF90, fedb) || !fedb || !fedb[3] || !Mem::Read((uintptr_t)fedb + 0x10, up) || !up) return false;
        if (!Mem::SafeRead(up, raw, 32)) return false;
        return SafeName(raw, out, n) && !strcmp(raw, out);    // only plain names (no path tricks)
    }

    // host: a client's save arrived
    void HostStore(const char* ownerRaw, const char* profileRaw, const uint8_t* d, uint32_t n, uint64_t mtime) {
        char owner[40], profile[40];
        if (!SafeName(ownerRaw, owner, sizeof(owner)) || !SafeName(profileRaw, profile, sizeof(profile))) return;
        CreateDirectoryA((std::string(g_dataDir) + "MWTogether-saves").c_str(), nullptr);
        std::string dir = HostDir(owner);
        CreateDirectoryA(dir.c_str(), nullptr);
        // one save per player: remember which profile it is
        if (WriteAll(dir + "\\" + profile, d, n, mtime)) {
            WritePrivateProfileStringA("Save", "Profile", profile, (dir + "\\save.ini").c_str());
            LOG("Career: stored %s's save '%s' (%u bytes)", owner, profile, n);
            sprintf_s(g_saveStatus, "keeping %s's save", owner);
        }
    }
    // host: the client asks for its save
    void HostSendBack(const char* ownerRaw) {
        char owner[40], profile[40] = "";
        if (!SafeName(ownerRaw, owner, sizeof(owner))) { Net::SendNoFile(); return; }
        std::string dir = HostDir(owner);
        char raw[40] = "";
        GetPrivateProfileStringA("Save", "Profile", "", raw, sizeof(raw), (dir + "\\save.ini").c_str());
        std::vector<uint8_t> data;
        if (!SafeName(raw, profile, sizeof(profile)) || !ReadAll(dir + "\\" + profile, data)) { Net::SendNoFile(); return; }
        Net::SendFile(owner, profile, data.data(), (uint32_t)data.size(), FileTime64(dir + "\\" + profile));
        LOG("Career: sending %s their save '%s' back", owner, profile);
    }
    // client: the host's copy of our save arrived
    void ClientRestore(const char* ownerRaw, const char* profileRaw, const uint8_t* d, uint32_t n, uint64_t mtime) {
        char profile[40];
        g_restore = Restore::Done;
        if (_stricmp(ownerRaw, g_cfg.playerName.c_str()) || !SafeName(profileRaw, profile, sizeof(profile))) return;
        if (Game::GameFlowState() != 3 || ProfileLoaded()) { LOG("Career: host's copy arrived after the profile was loaded - not applied"); return; }
        std::string path = DocsSavePath(profile);
        if (path.empty()) return;
        uint64_t mine = FileTime64(path);
        if (mine >= mtime) { LOG("Career: our save '%s' is up to date", profile); return; }
        if (mine) CopyFileA(path.c_str(), (path + ".mwt-backup").c_str(), FALSE);
        else {
            char docs[MAX_PATH];
            SHGetFolderPathA(nullptr, CSIDL_PERSONAL, nullptr, SHGFP_TYPE_CURRENT, docs);
            CreateDirectoryA((std::string(docs) + "\\NFS Most Wanted").c_str(), nullptr);
            CreateDirectoryA((std::string(docs) + "\\NFS Most Wanted\\" + profile).c_str(), nullptr);
        }
        if (WriteAll(path, d, n, mtime)) {
            g_uploadedTime = mtime;
            LOG("Career: restored save '%s' from the host (%s)", profile, mine ? "ours was older; kept as .mwt-backup" : "we had none");
            strcpy_s(g_saveStatus, "save restored from the host");
        }
    }
}

// The fast start waits with "Press START" while the host's copy of our save may still arrive.
static bool SaveSyncHoldsBoot() {
    if (g_cfg.isHost || !g_cfg.saveSync || g_restore == Restore::Done) return false;
    if (!g_saveFirstTick) g_saveFirstTick = GetTickCount();
    if (g_restore == Restore::Requested) return true;
    return GetTickCount() - g_saveFirstTick < 1500;      // give the connection a moment
}

static void SaveTickImpl(bool connected) {
    DWORD now = GetTickCount();
    if (!g_saveFirstTick) g_saveFirstTick = now;
    if (g_cfg.isHost) return;
    // restore: ask once, before our profile is loaded
    if (g_restore == Restore::Idle && connected && Game::GameFlowState() == 3 && !ProfileLoaded()) {
        Net::RequestFile(g_cfg.playerName.c_str());
        g_restore = Restore::Requested; g_restoreAt = now;
        LOG("Career: asking the host for our save");
    }
    if (g_restore == Restore::Requested && now - g_restoreAt > 4000) { g_restore = Restore::Done; LOG("Career: no answer from the host about our save"); }
    if (g_restore == Restore::Idle && ProfileLoaded()) g_restore = Restore::Done;
    // backup: upload our save whenever the game has written it
    static DWORD nextCheck = 0;
    if (!connected || (int)(now - nextCheck) < 0 || Net::SendingFile()) return;
    nextCheck = now + 5000;
    char profile[40];
    if (!CurrentProfile(profile, sizeof(profile))) return;
    std::string path = DocsSavePath(profile);
    uint64_t t = FileTime64(path);
    if (!t || t == g_uploadedTime) return;
    FILETIME ftNow; GetSystemTimeAsFileTime(&ftNow);
    uint64_t n64 = ((uint64_t)ftNow.dwHighDateTime << 32) | ftNow.dwLowDateTime;
    if (n64 - t < 20000000ull) return;                   // written under 2 s ago: let the game finish
    std::vector<uint8_t> data;
    if (ReadAll(path, data) && Net::SendFile(g_cfg.playerName.c_str(), profile, data.data(), (uint32_t)data.size(), t)) {
        g_uploadedTime = t;
        LOG("Career: sending our save '%s' to the host (%u bytes)", profile, (unsigned)data.size());
        strcpy_s(g_saveStatus, "save backed up on the host");
    }
}

void Game::SaveTick(bool connected) {
    if (!g_ok || !g_cfg.saveSync) return;
    SaveTickImpl(connected);
}
const char* Game::SaveStatus() { return g_saveStatus; }

void Game::InstallSaveSync() {
    Net::OnFile = [](const char* owner, const char* profile, const uint8_t* d, uint32_t n, uint64_t mtime) {
        if (!g_cfg.saveSync) return;
        if (g_cfg.isHost) HostStore(owner, profile, d, n, mtime);
        else ClientRestore(owner, profile, d, n, mtime);
    };
    Net::OnFileRequest = [](const char* owner) { if (g_cfg.isHost && g_cfg.saveSync) HostSendBack(owner); else Net::SendNoFile(); };
    Net::OnNoFile = []() { if (g_restore == Restore::Requested) { g_restore = Restore::Done; LOG("Career: the host keeps no save for us yet"); } };
}
