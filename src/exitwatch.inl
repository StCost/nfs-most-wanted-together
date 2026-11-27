// Included from game.cpp (after boot.inl).
// ---------------------------------------------------------------------------
// Why the game quit (exitutil.h has the game's quit paths). The log said only "Window: closing message
// 0002" (WM_DESTROY) when the game closed by itself - no WM_CLOSE before it, so the game had set its
// own exit flag. Here:
//  - every writer of the exit flag logs "Exit: requested by <site>" with the game flow, the open FE
//    screens and the return addresses on the stack (the writers' 10-byte "mov [0x9257EC], 1" becomes a
//    call to a stub that logs and then does the same write);
//  - the message pump's PeekMessage (IAT 0x89034C) is wrapped: a stale WM_QUIT left on the pump's stack
//    with no message waiting is cleared (the game keeps running) and logged; a real WM_QUIT is logged;
//  - PostQuitMessage / PostThreadMessage / PostMessage (WM_QUIT, WM_CLOSE) calls from the exe and the
//    other plugins in the game folder are logged with the caller (module+offset);
//  - the window hook logs the stack on WM_DESTROY (overlay.cpp calls Game::LogStack).
// ---------------------------------------------------------------------------
#include "exitutil.h"
#include <tlhelp32.h>

namespace {
    bool IsCodeAddress(uintptr_t a) {
        if (a < 0x10000) return false;
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((const void*)a, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT || mbi.Type != MEM_IMAGE) return false;
        if ((uintptr_t)mbi.BaseAddress + 7 > a) return false;            // the call bytes in front must be in the same region
        return (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
    }
    void DescribeAddress(uintptr_t a, char* out, size_t n) {
        HMODULE m = nullptr;
        char path[MAX_PATH] = "";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)a, &m) && m)
            GetModuleFileNameA(m, path, MAX_PATH);
        ExitUtil::FormatAddress(out, n, ExitUtil::BaseName(path), (uintptr_t)m, a, !m || m == GetModuleHandleA(nullptr));
    }
    // up to `max` return addresses above `sp` (values that point right after a call in a loaded image)
    int StackReturns(const uint8_t* sp, uintptr_t* out, int max) {
        int found = 0;
        __try {
            const uint8_t* top = (const uint8_t*)((NT_TIB*)NtCurrentTeb())->StackBase;
            const uint8_t* p = (const uint8_t*)((uintptr_t)sp & ~3u);
            for (int scanned = 0; p + 4 <= top && found < max && scanned < 4096; p += 4, ++scanned) {
                uintptr_t a = *(const uintptr_t*)p;
                if (!IsCodeAddress(a)) continue;
                uint8_t before[7];
                if (!Mem::SafeRead((const void*)(a - 7), before, 7) || !ExitUtil::LooksAfterCall(before)) continue;
                out[found++] = a;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return found;
    }
}

void Game::LogFault(const char* tag, void* ep) {
    __try {
        auto* e = (EXCEPTION_POINTERS*)ep;
        char at[64]; DescribeAddress((uintptr_t)e->ExceptionRecord->ExceptionAddress, at, sizeof(at));
        uintptr_t ret[14];
        int n = StackReturns((const uint8_t*)(uintptr_t)e->ContextRecord->Esp, ret, 14);
        char line[900]; int len = sprintf_s(line, "%s: exception %08X at %s (eax %08X ecx %08X edx %08X esi %08X edi %08X ebp %08X), stack", tag,
            (unsigned)e->ExceptionRecord->ExceptionCode, at, (unsigned)e->ContextRecord->Eax, (unsigned)e->ContextRecord->Ecx,
            (unsigned)e->ContextRecord->Edx, (unsigned)e->ContextRecord->Esi, (unsigned)e->ContextRecord->Edi, (unsigned)e->ContextRecord->Ebp);
        for (int i = 0; i < n && len > 0 && len < (int)sizeof(line) - 40; ++i) {
            char a[64]; DescribeAddress(ret[i], a, sizeof(a));
            len += sprintf_s(line + len, sizeof(line) - len, " %s", a);
        }
        LOG("%s", line);
    } __except (EXCEPTION_EXECUTE_HANDLER) { LOG("%s: exception (no details)", tag); }
}

void Game::LogStack(const char* tag) {
    char here = 0;
    uintptr_t ret[12];
    int n = StackReturns((const uint8_t*)&here, ret, 12);
    char line[700]; int len = sprintf_s(line, "%s: stack", tag ? tag : "Stack");
    for (int i = 0; i < n && len > 0 && len < (int)sizeof(line) - 40; ++i) {
        char a[64]; DescribeAddress(ret[i], a, sizeof(a));
        len += sprintf_s(line + len, sizeof(line) - len, " %s", a);
    }
    if (!n) strcat_s(line, " (no return addresses found)");
    LOG("%s", line);
}

namespace {
    // ---- the exit flag's writers ----
    void __cdecl ExitHit(int site) {
        static int logged = 0;
        if (logged >= 8) return;
        ++logged;
        int count = 0; const ExitUtil::Site* s = ExitUtil::Sites(&count);
        const char* screens = FEScreens::Current();
        LOG("Exit: requested by %s (at %08X, game flow %d, FE screens [%s], connected %d)", ExitUtil::SiteName(site),
            site >= 0 && site < count ? (unsigned)s[site].at : 0u, Game::GameFlowState(), screens ? screens : "",
            (int)Net::Stats().connected);
        Game::LogStack("Exit");
    }
    void __cdecl ExitHitSafe(int site) { __try { ExitHit(site); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

    // pushad; pushfd; push site; call ExitHitSafe; add esp, 4; popfd; popad; mov [0x9257EC], 1; ret
    bool PatchExitWriter(int site, uintptr_t at) {
        const uint8_t* orig = ExitUtil::SetFlagBytes();
        uint8_t have[10];
        if (!Mem::SafeRead((const void*)at, have, 10) || memcmp(have, orig, 10) != 0) return false;
        uint8_t* t = (uint8_t*)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!t) return false;
        int k = 0;
        t[k++] = 0x60; t[k++] = 0x9C; t[k++] = 0x6A; t[k++] = (uint8_t)site;
        t[k++] = 0xE8; *(int32_t*)(t + k) = (int32_t)((uintptr_t)&ExitHitSafe - ((uintptr_t)t + k + 4)); k += 4;
        t[k++] = 0x83; t[k++] = 0xC4; t[k++] = 0x04; t[k++] = 0x9D; t[k++] = 0x61;
        memcpy(t + k, orig, 10); k += 10;
        t[k++] = 0xC3;
        DWORD old = 0;
        if (!VirtualProtect(t, 32, PAGE_EXECUTE_READ, &old)) { VirtualFree(t, 0, MEM_RELEASE); return false; }
        FlushInstructionCache(GetCurrentProcess(), t, 32);
        uint8_t patch[10] = { 0xE8, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90, 0x90 };
        *(int32_t*)(patch + 1) = (int32_t)((uintptr_t)t - (at + 5));
        if (!Mem::PatchBytes(at, orig, patch, 10)) { VirtualFree(t, 0, MEM_RELEASE); return false; }
        FlushInstructionCache(GetCurrentProcess(), (void*)at, 10);
        return true;
    }

    // ---- the message pump's PeekMessage ----
    using PeekMessageA_t = BOOL(WINAPI*)(LPMSG, HWND, UINT, UINT, UINT);
    PeekMessageA_t g_realPeek = nullptr;
    BOOL WINAPI hk_PeekMessageA(LPMSG m, HWND h, UINT lo, UINT hi, UINT flags) {
        BOOL r = g_realPeek(m, h, lo, hi, flags);
        if (m) {
            if (ExitUtil::IsStaleQuit(r != FALSE, m->message, (uintptr_t)_ReturnAddress())) {
                m->message = WM_NULL;          // nothing was waiting: the pump must not read it as a quit
                static int n = 0;
                if (n++ < 4) { LOG("Exit: the message pump read a stale WM_QUIT (no message was waiting) - ignored, the game keeps running"); Game::LogStack("Exit"); }
            } else if (r && m->message == WM_QUIT) {
                static int n = 0;
                if (n++ < 4) LOG("Exit: WM_QUIT in the message queue (exit code %u, %s)", (unsigned)m->wParam, (flags & PM_REMOVE) ? "taken" : "seen");
            }
        }
        return r;
    }

    // ---- who posts a quit ----
    using PostQuit_t = void(WINAPI*)(int);
    using PostThread_t = BOOL(WINAPI*)(DWORD, UINT, WPARAM, LPARAM);
    using PostMsg_t = BOOL(WINAPI*)(HWND, UINT, WPARAM, LPARAM);
    PostQuit_t g_realPostQuit = nullptr;
    PostThread_t g_realPostThreadA = nullptr, g_realPostThreadW = nullptr;
    PostMsg_t g_realPostMsgA = nullptr, g_realPostMsgW = nullptr;
    bool IsQuitMessage(UINT msg, WPARAM w) { return msg == WM_QUIT || msg == WM_CLOSE || (msg == WM_SYSCOMMAND && (w & 0xFFF0) == SC_CLOSE); }
    void LogPost(const char* fn, UINT msg, WPARAM w, void* ret) {
        static int n = 0;
        if (n >= 8) return;
        ++n;
        char a[64]; DescribeAddress((uintptr_t)ret, a, sizeof(a));
        LOG("Exit: %s(%04X, %u) from %s (game flow %d)", fn, msg, (unsigned)w, a, Game::GameFlowState());
    }
    void WINAPI hk_PostQuitMessage(int code) { LogPost("PostQuitMessage", WM_QUIT, (WPARAM)code, _ReturnAddress()); g_realPostQuit(code); }
    BOOL WINAPI hk_PostThreadMessageA(DWORD t, UINT m, WPARAM w, LPARAM l) { if (IsQuitMessage(m, w)) LogPost("PostThreadMessageA", m, w, _ReturnAddress()); return g_realPostThreadA(t, m, w, l); }
    BOOL WINAPI hk_PostThreadMessageW(DWORD t, UINT m, WPARAM w, LPARAM l) { if (IsQuitMessage(m, w)) LogPost("PostThreadMessageW", m, w, _ReturnAddress()); return g_realPostThreadW(t, m, w, l); }
    BOOL WINAPI hk_PostMessageA(HWND h, UINT m, WPARAM w, LPARAM l) { if (IsQuitMessage(m, w)) LogPost("PostMessageA", m, w, _ReturnAddress()); return g_realPostMsgA(h, m, w, l); }
    BOOL WINAPI hk_PostMessageW(HWND h, UINT m, WPARAM w, LPARAM l) { if (IsQuitMessage(m, w)) LogPost("PostMessageW", m, w, _ReturnAddress()); return g_realPostMsgW(h, m, w, l); }

    // the module's import address table: every slot holding `target` now holds `hook`
    int PatchImportSlots(HMODULE mod, void* target, void* hook) {
        int n = 0;
        __try {
            const uint8_t* b = (const uint8_t*)mod;
            const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)b;
            if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
            const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(b + dos->e_lfanew);
            if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
            const IMAGE_DATA_DIRECTORY& d = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IAT];
            if (!d.VirtualAddress || d.Size < sizeof(void*)) return 0;
            void** slots = (void**)(b + d.VirtualAddress);
            for (DWORD i = 0; i < d.Size / sizeof(void*); ++i)
                if (slots[i] == target && Mem::PatchPointer(&slots[i], hook)) ++n;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return n;
    }
    // the exe, and with `plugins` the modules loaded from the game folder (other ASI plugins, wrapper
    // DLLs) - not Windows' own. The module list is not walked from DllMain (the loader lock).
    int HookQuitPosts(bool plugins) {
        HMODULE user32 = GetModuleHandleA("user32.dll");
        if (!user32) return 0;
        struct { const char* name; void* hook; void** real; } fns[] = {
            { "PostQuitMessage",    (void*)&hk_PostQuitMessage,    (void**)&g_realPostQuit },
            { "PostThreadMessageA", (void*)&hk_PostThreadMessageA, (void**)&g_realPostThreadA },
            { "PostThreadMessageW", (void*)&hk_PostThreadMessageW, (void**)&g_realPostThreadW },
            { "PostMessageA",       (void*)&hk_PostMessageA,       (void**)&g_realPostMsgA },
            { "PostMessageW",       (void*)&hk_PostMessageW,       (void**)&g_realPostMsgW },
        };
        for (auto& f : fns) if (!*f.real) *f.real = (void*)GetProcAddress(user32, f.name);
        char exe[MAX_PATH] = "";
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        size_t dirLen = ExitUtil::BaseName(exe) - exe;
        HMODULE self = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&HookQuitPosts, &self);
        int total = 0;
        HMODULE exeMod = GetModuleHandleA(nullptr);
        for (auto& f : fns) if (*f.real) total += PatchImportSlots(exeMod, *f.real, f.hook);
        if (!plugins) return total;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
        if (snap == INVALID_HANDLE_VALUE) return 0;
        MODULEENTRY32 me; me.dwSize = sizeof(me);
        for (BOOL ok = Module32First(snap, &me); ok; ok = Module32Next(snap, &me)) {
            if (me.hModule == self || me.hModule == exeMod || _strnicmp(me.szExePath, exe, dirLen) != 0) continue;   // ours, or not from the game folder
            for (auto& f : fns) if (*f.real) total += PatchImportSlots(me.hModule, *f.real, f.hook);
        }
        CloseHandle(snap);
        return total;
    }
    bool g_exitWatchPump = false;
}

// From DllMain (code patches, before the front end) and again at the first frame (the plugins that
// loaded after ours). Each part only once; the bytes / slots are checked first.
void Game::InstallExitWatch(bool plugins) {
    static bool sites = false;
    if (!sites) {
        sites = true;
        int count = 0, ok = 0; const ExitUtil::Site* s = ExitUtil::Sites(&count);
        for (int i = 0; i < count; ++i) if (PatchExitWriter(i, s[i].at)) ++ok;
        LOG("Exit: watching %d of %d exit-flag writers", ok, count);
    }
    if (!g_exitWatchPump) {
        g_exitWatchPump = true;
        // the pump reads PeekMessageA from the IAT each time (mov esi, [0x89034C] at 0x6C21A5)
        static const uint8_t movEsi[6] = { 0x8B, 0x35, 0x4C, 0x03, 0x89, 0x00 };
        uint8_t have[6]; void* cur = nullptr;
        HMODULE user32 = GetModuleHandleA("user32.dll");
        void* real = user32 ? (void*)GetProcAddress(user32, "PeekMessageA") : nullptr;
        if (Mem::SafeRead((const void*)0x6C21A5, have, 6) && !memcmp(have, movEsi, 6) && Mem::Read(0x89034C, cur) && cur && cur != (void*)&hk_PeekMessageA) {
            g_realPeek = (PeekMessageA_t)cur;      // also when another plugin wrapped it first: we call theirs
            Mem::PatchPointer((void*)0x89034C, (void*)&hk_PeekMessageA);
            LOG("Exit: message pump guarded (stale WM_QUIT)%s", cur == real ? "" : " - PeekMessageA was already wrapped by another plugin");
        } else LOG("Exit: message pump not guarded (unexpected bytes)");
    }
    static bool postsLogged = false;
    int posts = HookQuitPosts(plugins);
    if (posts || !postsLogged) { LOG("Exit: watching %d %squit / close post imports", posts, postsLogged ? "more " : ""); postsLogged = true; }
}
