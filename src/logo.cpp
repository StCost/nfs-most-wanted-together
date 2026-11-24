#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>
#include "logo.h"
#include "ddsutil.h"
#include "resutil.h"
#include "log.h"
#include "mem.h"

namespace {
    // speed.exe v1.3
    constexpr uintptr_t kGetTextureInfo = 0x503400;       // TextureInfo* GetTextureInfo(hash, returnDefault, includeUnloaded)
    constexpr uintptr_t kDevice = 0x982BDC;               // IDirect3DDevice9*
    constexpr uintptr_t kFEng = 0x91CADC;
    constexpr uintptr_t kFindObject = 0x524850;           // FEngFindObject(pkg, hash)
    constexpr uintptr_t kGetCenter = 0x524EE0;            // FE_Object_GetCenter
    constexpr uintptr_t kSetCenter = 0x525050;            // FE_Object_SetCenter
    constexpr uint32_t  MW_LOGO = 0x0CD55E13;             // bStringHash("MW_LOGO")
    const char* const   kMainMenuFng = (const char*)0x89AE54;
    // TextureInfo: +0x00 platform info (+0x18 IDirect3DTexture9*), +0x24 name hash
    typedef void*(__cdecl* GetTextureInfo_t)(uint32_t, int, int);
    typedef void*(__cdecl* FindObject_t)(const char*, uint32_t);
    typedef void(__cdecl* GetCenter_t)(void*, float*, float*);
    typedef void(__cdecl* SetCenter_t)(void*, float, float);

    enum State { S_CHECK, S_WAIT, S_ON, S_OFF };
    State    g_state = S_CHECK;
    std::vector<uint8_t> g_file;
    DdsUtil::Info g_info;
    uint32_t g_contentH = 0;                              // painted rows; ignores transparent padding
    void*    g_ours = nullptr;                            // our IDirect3DTexture9
    uint8_t* g_plat = nullptr;                            // the TextureInfo's platform info we put it in
    DWORD    g_last = 0;
    bool     g_resizeOk = false;                          // FEng size functions checked out
    bool     g_resizeTried = false;

    template <class F> F Slot(void* com, int i) { return (F)(*(void***)com)[i]; }

    // Trailing fully-transparent rows (8px or more) are power-of-two padding, not art.
    // The in-game quad must use only the painted height so the original 512x128 letters stay vanilla size.
    uint32_t ContentHeight() {
        if (g_info.format != DdsUtil::FMT_A8R8G8B8 || !g_info.width || !g_info.height) return g_info.height;
        const size_t need = (size_t)g_info.dataOffset + (size_t)g_info.width * g_info.height * 4;
        if (g_file.size() < need) return g_info.height;
        const uint8_t* p = g_file.data() + g_info.dataOffset;
        int last = -1;
        for (uint32_t y = 0; y < g_info.height; ++y) {
            const uint8_t* row = p + (size_t)y * g_info.width * 4;
            for (uint32_t x = 0; x < g_info.width; ++x) {
                if (row[x * 4 + 3]) { last = (int)y; break; }
            }
        }
        uint32_t painted = last >= 0 ? (uint32_t)last + 1 : g_info.height;
        if (painted > 0 && g_info.height >= painted + 8) return painted;
        return g_info.height;
    }

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
        if (plat != g_plat) LOG("Logo: the built-in Together logo in use (%ux%u)", g_info.width, g_info.height);
        g_plat = plat;
        return true;
    }

    bool CheckResizeFns() {
        // GetCenter: 81 EC 14 08 00 00; SetCenter: 83 EC 10 56; FindObject: first byte 0x8B or 0x55 / 0xA1 typical
        uint8_t gc[3] = {}, sc[3] = {}, fo[2] = {};
        if (!Mem::SafeRead((const void*)kGetCenter, gc, 3) || gc[0] != 0x81 || gc[1] != 0xEC) return false;
        if (!Mem::SafeRead((const void*)kSetCenter, sc, 3) || sc[0] != 0x83 || sc[1] != 0xEC) return false;
        if (!Mem::SafeRead((const void*)kFindObject, fo, 2) || fo[0] == 0xCC) return false;
        return true;
    }

    // Keep the logo's width and top edge; grow the height downward so a taller texture
    // (NFS / Most Wanted / Together) is not squashed and the original lettering stays the same size.
    void FitMenuLogo() {
        if (!g_resizeOk || !g_info.width || !g_contentH) return;
        uint8_t* obj = (uint8_t*)((FindObject_t)kFindObject)(kMainMenuFng, MW_LOGO);
        if (!obj || !Mem::IsReadable(obj, 0x30)) return;
        if (*(int*)(obj + 0x18) != 1) return;          // FE_Image
        uint8_t* node = *(uint8_t**)(obj + 0x2C);
        if (!node || !Mem::IsReadable(node, 0x40)) return;
        float w = *(float*)(node + 0x38), h = *(float*)(node + 0x3C);
        if (!(w > 1.f && w < 2000.f && h > 1.f && h < 2000.f)) return;
        // The vanilla logo texture is 4:1. Keep this object's width and the scale the
        // 4:1 lettering already has, and grow the height for the extra Together line.
        static uint8_t* last = nullptr; static float origH = 0;
        if (obj != last) { last = obj; origH = 0; }
        if (origH <= 0 || h + 0.5f < origH) origH = h;          // fresh object, or the package reset it
        float newH = origH * (float)g_contentH * 4.f / (float)g_info.width;
        if (newH < 1.f || newH > 2000.f) return;
        if (fabsf(h - newH) < 0.5f) return;
        float cx = 0, cy = 0;
        ((GetCenter_t)kGetCenter)(obj, &cx, &cy);
        // FEng: origin at the screen centre, +Y down. Top stays, height grows downward.
        float top = cy - h * 0.5f;
        *(float*)(node + 0x3C) = newH;
        ((SetCenter_t)kSetCenter)(obj, cx, top + newH * 0.5f);
        LOG("Logo: MainMenu logo %g x %g -> %g x %g (top kept)", w, h, w, newH);
    }

    void TickImpl(const char* dir) {
        if (g_state == S_CHECK) {
            g_state = S_OFF;
            (void)dir;   // the logo is built into the plugin; a MWTogether-logo.dds file there is not read
            const uint8_t* data = nullptr; size_t n = 0;
            if (!ResUtil::Get(ResUtil::kLogo, data, n)) { LOG("Logo: the built-in logo is missing - the game's logo stays"); return; }
            g_file.assign(data, data + n);
            std::string err;
            if (!DdsUtil::Parse(g_file.data(), g_file.size(), g_info, err)) { LOG("Logo: the built-in logo not used: %s", err.c_str()); g_file.clear(); return; }
            g_contentH = ContentHeight();
            g_resizeOk = CheckResizeFns();
            if (!g_resizeOk) LOG("Logo: couldn't resize the menu logo object - a non-4:1 file may look stretched");
            else if (g_info.width * 128 != g_contentH * 512)
                LOG("Logo: the built-in logo is %ux%u (content %u) - the menu logo's height will grow to keep the original width",
                    g_info.width, g_info.height, g_contentH);
            g_state = S_WAIT;
        }
        DWORD now = GetTickCount();
        if (now - g_last < 250) return;
        g_last = now;
        if (Swap() && g_state == S_WAIT) { g_state = S_ON; g_file.clear(); g_file.shrink_to_fit(); }
        if (g_state == S_ON) FitMenuLogo();
    }
}

void Logo::Tick(const char* dir) {
    if (g_state == S_OFF) return;
    __try { TickImpl(dir); }
    __except (EXCEPTION_EXECUTE_HANDLER) { g_state = S_OFF; LOG("Logo: exception - the game's logo stays"); }
}
