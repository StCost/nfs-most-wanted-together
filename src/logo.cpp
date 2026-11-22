#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include "logo.h"
#include "ddsutil.h"
#include "log.h"
#include "mem.h"

namespace {
    // speed.exe v1.3
    constexpr uintptr_t kGetTextureInfo = 0x503400;       // TextureInfo* GetTextureInfo(hash, returnDefault, includeUnloaded)
    constexpr uintptr_t kDevice = 0x982BDC;               // IDirect3DDevice9*
    constexpr uint32_t  MW_LOGO = 0x0CD55E13;             // bStringHash("MW_LOGO")
    // TextureInfo: +0x00 platform info (+0x18 IDirect3DTexture9*), +0x24 name hash
    typedef void*(__cdecl* GetTextureInfo_t)(uint32_t, int, int);

    enum State { S_CHECK, S_WAIT, S_ON, S_OFF };
    State    g_state = S_CHECK;
    std::vector<uint8_t> g_file;
    DdsUtil::Info g_info;
    void*    g_ours = nullptr;                            // our IDirect3DTexture9
    uint8_t* g_plat = nullptr;                            // the TextureInfo's platform info we put it in
    DWORD    g_last = 0;

    template <class F> F Slot(void* com, int i) { return (F)(*(void***)com)[i]; }

    void* MakeTexture() {
        void* dev = nullptr;
        if (!Mem::Read(kDevice, dev) || !dev) return nullptr;
        typedef HRESULT(__stdcall* Create_t)(void*, UINT, UINT, UINT, DWORD, UINT, UINT, void**, void*);
        typedef HRESULT(__stdcall* Lock_t)(void*, UINT, void*, const RECT*, DWORD);
        typedef HRESULT(__stdcall* Unlock_t)(void*, UINT);
        typedef ULONG(__stdcall* Release_t)(void*);
        void* tex = nullptr;
        if (FAILED(Slot<Create_t>(dev, 23)(dev, g_info.width, g_info.height, g_info.mips, 0, g_info.format, 1 /*MANAGED*/, &tex, nullptr)) || !tex)
            return nullptr;
        const uint8_t* src = g_file.data() + g_info.dataOffset;
        for (uint32_t l = 0; l < g_info.mips; ++l) {
            uint32_t w = DdsUtil::Mip(g_info.width, l), h = DdsUtil::Mip(g_info.height, l);
            uint32_t rowBytes = DdsUtil::RowBytes(g_info.format, w), rows = DdsUtil::Rows(g_info.format, h);
            struct { INT pitch; void* bits; } lr = {};
            if (FAILED(Slot<Lock_t>(tex, 19)(tex, l, &lr, nullptr, 0)) || !lr.bits) { Slot<Release_t>(tex, 2)(tex); return nullptr; }
            for (uint32_t r = 0; r < rows; ++r) memcpy((uint8_t*)lr.bits + (size_t)r * lr.pitch, src + (size_t)r * rowBytes, rowBytes);
            Slot<Unlock_t>(tex, 20)(tex, l);
            src += (size_t)rowBytes * rows;
        }
        return tex;
    }

    // put ours in (again, if the game made its texture anew); false = not loaded yet
    bool Swap() {
        uint8_t* ti = (uint8_t*)((GetTextureInfo_t)kGetTextureInfo)(MW_LOGO, 0, 0);
        if (!ti || !Mem::IsReadable(ti, 0x28) || *(uint32_t*)(ti + 0x24) != MW_LOGO) return false;
        uint8_t* plat = *(uint8_t**)ti;
        if (!plat || !Mem::IsReadable(plat, 0x1C) || !*(void**)(plat + 0x18)) return false;
        if (*(void**)(plat + 0x18) == g_ours) return true;
        if (!g_ours && !(g_ours = MakeTexture())) { LOG("Logo: Direct3D wouldn't make the texture - the game's logo stays"); g_state = S_OFF; return false; }
        // the game releases what it finds there when it lets the texture go: a reference of its own
        typedef ULONG(__stdcall* AddRef_t)(void*);
        Slot<AddRef_t>(g_ours, 1)(g_ours);
        *(void**)(plat + 0x18) = g_ours;            // (the game's own texture is just left alone)
        if (plat != g_plat) LOG("Logo: MWTogether-logo.dds in use (%ux%u)", g_info.width, g_info.height);
        g_plat = plat;
        return true;
    }

    void TickImpl(const char* dir) {
        if (g_state == S_CHECK) {
            g_state = S_OFF;
            std::string path = std::string(dir) + "MWTogether-logo.dds";
            FILE* f = nullptr;
            if (fopen_s(&f, path.c_str(), "rb") != 0 || !f) return;     // no file: the game's logo
            fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
            if (n > 0 && n <= 32 * 1024 * 1024) { g_file.resize((size_t)n); if (fread(g_file.data(), 1, (size_t)n, f) != (size_t)n) g_file.clear(); }
            fclose(f);
            std::string err;
            if (!DdsUtil::Parse(g_file.data(), g_file.size(), g_info, err)) { LOG("Logo: MWTogether-logo.dds not used: %s", err.c_str()); g_file.clear(); return; }
            if (g_info.width != g_info.height * 4) LOG("Logo: MWTogether-logo.dds is %ux%u - the game's logo is 4:1 (512x128), it will look stretched", g_info.width, g_info.height);
            g_state = S_WAIT;
        }
        DWORD now = GetTickCount();
        if (now - g_last < 1000) return;
        g_last = now;
        if (Swap() && g_state == S_WAIT) { g_state = S_ON; g_file.clear(); g_file.shrink_to_fit(); }
    }
}

void Logo::Tick(const char* dir) {
    if (g_state == S_OFF) return;
    __try { TickImpl(dir); }
    __except (EXCEPTION_EXECUTE_HANDLER) { g_state = S_OFF; LOG("Logo: exception - the game's logo stays"); }
}
