#pragma once
// Pure helpers of the career save sync (save.inl). Nothing here touches the game or the socket, so the
// test suite (tests\) covers all of it without the game.
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <share.h>
#include <string>
#include <vector>

namespace SaveUtil {
    // ---- names from the network ---------------------------------------------------------------
    // Player and profile names become folder and file names, so only ASCII letters, digits, space,
    // '-' and '_' survive: no dots, slashes, colons or wildcards, whatever the peer sent.
    inline bool NameChar(unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '-' || c == '_';
    }
    // names Windows treats as devices in any folder (CON, NUL, COM1 ...)
    inline bool ReservedName(const char* s) {
        static const char* const dev[] = { "CON", "PRN", "AUX", "NUL" };
        for (const char* d : dev) if (!_stricmp(s, d)) return true;
        return (!_strnicmp(s, "COM", 3) || !_strnicmp(s, "LPT", 3)) && s[3] >= '0' && s[3] <= '9' && !s[4];
    }
    // File-name safe copy of `s` (other characters are dropped, as are spaces at the end).
    // false if `s` is null, nothing is left or the result is a device name; `out` is then empty.
    inline bool SafeName(const char* s, char* out, size_t n) {
        if (!out || !n) return false;
        size_t j = 0;
        if (s) for (; *s && j + 1 < n; ++s) if (NameChar((unsigned char)*s)) out[j++] = *s;
        while (j && out[j - 1] == ' ') --j;
        out[j] = 0;
        if (j && ReservedName(out)) { out[0] = 0; return false; }
        return j > 0;
    }
    // true if `s` is already a safe name (SafeName would not change it)
    inline bool PlainName(const char* s) {
        char t[64];
        return s && strlen(s) < sizeof(t) && SafeName(s, t, sizeof(t)) && !strcmp(s, t);
    }
    // Do two player names mean the same folder on the host? (The host answers with the safe form of
    // the name, so "J.D." asks and "JD" comes back.)
    inline bool SameOwner(const char* a, const char* b) {
        char x[64], y[64];
        return SafeName(a, x, sizeof(x)) && SafeName(b, y, sizeof(y)) && !_stricmp(x, y);
    }

    // ---- which copy wins ----------------------------------------------------------------------
    // Both times are the owner's own file times (the host stores the time it was sent), so the two
    // PCs' clocks are never compared. The host's copy replaces ours only if it is strictly newer;
    // 0 = no file / no time.
    inline bool HostCopyIsNewer(uint64_t mine, uint64_t host) { return host != 0 && host > mine; }

    // ---- files --------------------------------------------------------------------------------
    inline uint64_t FileTime64(const std::string& path) {       // last write time, 0 if there is no such file
        WIN32_FILE_ATTRIBUTE_DATA a;
        if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &a) || (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return 0;
        return ((uint64_t)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime;
    }
    // whole file, 1..maxSize bytes
    inline bool ReadAll(const std::string& path, std::vector<uint8_t>& out, uint32_t maxSize) {
        out.clear();
        FILE* f = _fsopen(path.c_str(), "rb", _SH_DENYNO);
        if (!f) return false;
        long n = (fseek(f, 0, SEEK_END) == 0) ? ftell(f) : -1;
        bool ok = n > 0 && (unsigned long)n <= maxSize && fseek(f, 0, SEEK_SET) == 0;
        if (ok) { out.resize((size_t)n); ok = fread(out.data(), 1, (size_t)n, f) == (size_t)n; }
        fclose(f);
        if (!ok) out.clear();
        return ok;
    }
    // Writes next to `path` and then renames over it, so a failed or interrupted write never leaves
    // half a file where a good one was. `mtime` (FILETIME as 64 bits, 0 = now) becomes its write time.
    inline bool WriteAllAtomic(const std::string& path, const uint8_t* d, uint32_t n, uint64_t mtime) {
        std::string tmp = path + ".mwt-tmp";
        FILE* f = fopen(tmp.c_str(), "wb");
        if (!f) return false;
        bool ok = fwrite(d, 1, n, f) == n;
        ok = (fclose(f) == 0) && ok;
        if (ok && mtime) {
            HANDLE h = CreateFileA(tmp.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                FILETIME ft{ (DWORD)mtime, (DWORD)(mtime >> 32) };
                SetFileTime(h, nullptr, nullptr, &ft);
                CloseHandle(h);
            }
        }
        ok = ok && MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        if (!ok) DeleteFileA(tmp.c_str());
        return ok;
    }

    // ---- the host's folders from before 1.7.6 ---------------------------------------------------
    // Up to 1.7.5 a player's folder on the host (MWTogether-saves\<player>) was named after their Together
    // nickname; now it is named after their career profile, which is also the name their save file has
    // (save.ini Profile). A player with no folder of the new name keeps using the one old folder that
    // keeps a save of that profile (nothing is moved). A renamed profile is a new player to the host.
    struct HostFolder { std::string folder, profile; };
    inline int LegacyFolderFor(const std::vector<HostFolder>& folders, const char* owner) {
        char want[64];
        if (!SafeName(owner, want, sizeof(want))) return -1;
        int found = -1;
        for (int i = 0; i < (int)folders.size(); ++i) {
            // both plain names (what this mod writes there), the folder not already the new name
            if (!PlainName(folders[i].folder.c_str()) || !_stricmp(folders[i].folder.c_str(), want)) continue;
            if (!PlainName(folders[i].profile.c_str()) || _stricmp(folders[i].profile.c_str(), want)) continue;
            if (found >= 0) return -1;           // two old folders keep that profile: don't guess
            found = i;
        }
        return found;
    }
}
