#pragma once
// Files built into MWTogether.asi as RCDATA resources (src\version.rc), stored as they are: no packing,
// no encryption. The release is the one .asi, so it carries what used to ship next to it:
//   MWT_LOGO         assets\MWTogether-logo.dds   the main menu logo (logo.cpp)
//   MWT_DEFAULT_INI  MWTogether.ini               written to scripts\MWTogether.ini when there is none
//   MWT_LICENSE      LICENSE                      MIT notice, also in the log on the first start
//   MWT_NOTICES      THIRD-PARTY-NOTICES.txt
// Old MWTogether-logo.dds / MWTogether.default.ini files next to the plugin are not read any more (and not deleted).
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ResUtil {
    constexpr const char* kLogo = "MWT_LOGO";
    constexpr const char* kDefaultIni = "MWT_DEFAULT_INI";
    constexpr const char* kLicense = "MWT_LICENSE";
    constexpr const char* kNotices = "MWT_NOTICES";
    constexpr const char* kRepoUrl = "https://github.com/StCost/nfs-most-wanted-together";

    // the module this code is in (the .asi; the test exe in the tests)
    inline HMODULE Self() {
        HMODULE m = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)&Self, &m);
        return m;
    }
    // An embedded file: read-only, valid as long as the module is loaded.
    inline bool Get(const char* name, const uint8_t*& data, size_t& size, HMODULE mod = nullptr) {
        data = nullptr; size = 0;
        if (!mod) mod = Self();
        HRSRC r = mod ? FindResourceA(mod, name, MAKEINTRESOURCEA(10)) : nullptr;   // RT_RCDATA
        HGLOBAL g = r ? LoadResource(mod, r) : nullptr;
        const void* p = g ? LockResource(g) : nullptr;
        DWORD n = r ? SizeofResource(mod, r) : 0;
        if (!p || !n) return false;
        data = (const uint8_t*)p; size = n;
        return true;
    }
    // Writes the file only when nothing is at path (an existing file is never replaced). True when it wrote it.
    inline bool WriteIfMissing(const char* path, const void* data, size_t size) {
        if (!path || !path[0] || !data || !size) return false;
        HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        DWORD wrote = 0;
        bool ok = WriteFile(h, data, (DWORD)size, &wrote, nullptr) && wrote == size;
        CloseHandle(h);
        if (!ok) DeleteFileA(path);   // no half-written settings
        return ok;
    }
    // Text split into lines for the log (CR/LF dropped, empty lines kept as "").
    inline std::vector<std::string> Lines(const uint8_t* data, size_t size) {
        std::vector<std::string> out;
        if (!data || !size) return out;
        std::string cur;
        for (size_t i = 0; i < size; ++i) {
            char c = (char)data[i];
            if (c == '\r') continue;
            if (c == '\n') { out.push_back(cur); cur.clear(); continue; }
            cur += c;
        }
        if (!cur.empty()) out.push_back(cur);
        return out;
    }
}
