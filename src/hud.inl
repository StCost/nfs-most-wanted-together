// Included from game.cpp (after worldmap.inl).
// ---------------------------------------------------------------------------
// F1: hide the minimap (with the heat meter) and the speedometer / tachometer cluster; the rest of the
// HUD (the rear-view mirror, race information, notices) stays. [UI] HideHudKey.
//  The HUD's parts are FEng objects of the HUD package (HUD_SingleRace.fng, HUD_Drag.fng ...). Package
//  +0x4C: first top-level object; FEObject +0x04 next sibling, +0x10 name hash (upper-case FE hash
//  0x5AF1C0), +0x18 type (5 = group), +0x1C flags (bit 0 hidden, 0x40 locked, 0x2400000 changed), a
//  group: +0x60 child count, +0x64 first child (the walk of 0x5B1560 / FEngSetVisible 0x514CC0).
//  Hiding a group's own bit doesn't hide what's in it - the renderer looks at each object - and the HUD
//  shows some of its objects again every frame (the speedometer digits: 0x57A540). So F1 hides every
//  object under the chosen groups, and FEngSetVisible (0x514CC0, rewritten here with the same logic)
//  keeps those hidden while F1 is on.
//  What to hide is picked by screen position (see InHideZone): the minimap with its icons and heat meter
//  bottom-left, the gauge cluster bottom-right. Most of these parts have no name the exe looks up.
// ---------------------------------------------------------------------------
#include <unordered_set>
#include <unordered_map>
namespace {
    bool g_hudHidden = false;
    std::unordered_set<void*> g_hudSet;          // every object F1 hid (the chosen groups and all that is in them)
    std::unordered_map<void*, bool> g_hudWasHidden;   // each one's own state before F1 (restored exactly when shown again)
    const char* const kHudPackages[] = { "HUD_SingleRace.fng", "HUD_Drag.fng", "HUD_Player1.fng", "HUD_Drag_Player1.fng" };

    uint32_t ObjFlags(void* o) { return *(uint32_t*)((uint8_t*)o + 0x1C); }
    int      ObjType(void* o) { return *(int*)((uint8_t*)o + 0x18); }
    int CountTree(void* o, int depth = 0) {
        int n = 1;
        if (depth < 12 && ObjType(o) == 5) {
            int c = *(int*)((uint8_t*)o + 0x60); void* ch = *(void**)((uint8_t*)o + 0x64);
            for (int i = 0; i < c && ch && i < 500; ++i, ch = *(void**)((uint8_t*)ch + 4)) n += CountTree(ch, depth + 1);
        }
        return n;
    }
    void CollectTree(void* o, int depth = 0) {
        g_hudSet.insert(o);
        if (!g_hudWasHidden.count(o)) g_hudWasHidden[o] = (ObjFlags(o) & 1) != 0;
        if (depth < 12 && ObjType(o) == 5) {
            int c = *(int*)((uint8_t*)o + 0x60); void* ch = *(void**)((uint8_t*)o + 0x64);
            for (int i = 0; i < c && ch && i < 500; ++i, ch = *(void**)((uint8_t*)ch + 4)) CollectTree(ch, depth + 1);
        }
    }
    void HideObj(void* o) { if (!(ObjFlags(o) & 1)) *(uint32_t*)((uint8_t*)o + 0x1C) |= 0x2400001; }
    void ShowObj(void* o) { uint32_t& f = *(uint32_t*)((uint8_t*)o + 0x1C); if (!(f & 0x40)) f = (f & ~1u) | 0x2400000; }

    // FEngSetVisible (0x514CC0, cdecl FEObject*), the game's logic, plus: an object F1 hid stays hidden
    void __cdecl hk_FEngSetVisible(void* o) {
        if (!o) return;
        if (g_hudHidden && g_hudSet.count(o)) { HideObj(o); return; }
        uint32_t& f = *(uint32_t*)((uint8_t*)o + 0x1C);
        if (f & 0x40) return;
        f = (f & ~1u) | 0x2400000;
        if (ObjType(o) == 5) {
            int c = *(int*)((uint8_t*)o + 0x60); void* ch = *(void**)((uint8_t*)o + 0x64);
            for (; c > 0 && ch; --c, ch = *(void**)((uint8_t*)ch + 4)) hk_FEngSetVisible(ch);
        }
    }
    bool g_showHooked = false;
    void HookSetVisible() {
        if (g_showHooked) return;
        g_showHooked = true;
        const uint8_t expect[6] = { 0x8B, 0x44, 0x24, 0x04, 0x85, 0xC0 };   // mov eax,[esp+4]; test eax,eax
        uint8_t jmp[6] = { 0xE9, 0, 0, 0, 0, 0x90 };
        int32_t rel = (int32_t)((uintptr_t)&hk_FEngSetVisible - (0x514CC0 + 5));
        memcpy(jmp + 1, &rel, 4);
        bool ok = Mem::PatchBytes(0x514CC0, expect, jmp, 6);
        LOG("HUD: FEngSetVisible %s", ok ? "hooked" : "NOT hooked (bytes differ) - the speedometer digits may show again");
    }

    // An object's position: FEObject +0x2C -> data block, floats [7] x, [8] y, relative to its parent group;
    // the screen is about 640x480 around (0,0), y down. The minimap sits bottom-left (centre about -220, 140),
    // the gauge cluster bottom-right (centre about 145, 185), the pursuit bar bottom-centre (x -110..110),
    // the mirror top-centre.
    bool ObjPos(void* o, float& x, float& y) {
        const float* d = *(const float**)((uint8_t*)o + 0x2C);
        if ((uintptr_t)d < 0x10000 || !Mem::IsReadable(d, 0x24)) return false;
        x = d[7]; y = d[8];
        return x == x && y == y && fabsf(x) < 5000 && fabsf(y) < 5000;
    }
    bool InHideZone(float x, float y) {
        if (y > 260) return false;       // parked below the screen (race lists that slide in)
        return (x < -150 && y > 50)      // minimap, its icons and the heat meter
            || (x > 120 && y > 0);       // speedometer / tachometer, nitrous, speedbreaker and their icons
    }
    // walk a list: an object in a zone is hidden with all it holds (a big group - the whole HUD or the whole
    // right side - is looked into instead, so the parts outside the zones stay)
    int HideByZone(void* first, float px, float py, int depth) {
        int roots = 0, guard = 0;
        for (void* o = first; o && guard < 500; o = *(void**)((uint8_t*)o + 4), ++guard) {
            float x = 0, y = 0;
            if (!ObjPos(o, x, y)) continue;
            x += px; y += py;
            bool group = ObjType(o) == 5;
            if (InHideZone(x, y) && (!group || CountTree(o) < 60)) { CollectTree(o); ++roots; continue; }
            if (group && depth < 8) roots += HideByZone(*(void**)((uint8_t*)o + 0x64), x, y, depth + 1);
        }
        return roots;
    }

    // build the set of objects to hide from the HUD package(s)
    int BuildHudSet(bool log) {
        g_hudSet.clear();
        void* fe = nullptr;
        if (!Mem::Read(0x91CADC, fe) || !fe) return 0;
        int roots = 0;
        for (const char* pkgName : kHudPackages) {
            void* pkg = ((void*(__thiscall*)(void*, const char*))0x516AF0)(fe, pkgName);
            if (!pkg) continue;
            void* first = *(void**)((uint8_t*)pkg + 0x4C);
            int r = HideByZone(first, 0, 0, 0);
            if (log) LOG("HUD: %s - %d part(s), %d object(s) so far", pkgName, r, (int)g_hudSet.size());
            roots += r;
        }
        return roots;
    }
    int ApplyHud(bool hide, bool log) {
        if (hide) {
            HookSetVisible();
            int roots = BuildHudSet(log);
            for (void* o : g_hudSet) HideObj(o);
            return roots;
        }
        // shown again: the HUD's own updates show what it wants shown (unhide only the groups' own bits here;
        // their parts come back as the HUD sets them, the static ones right away)
        int n = 0;
        for (auto& kv : g_hudWasHidden) if (!kv.second) { ShowObj(kv.first); ++n; }
        g_hudSet.clear(); g_hudWasHidden.clear();
        return n;
    }
    int ApplyHudSafeImpl(bool hide, bool log) { __try { return ApplyHud(hide, log); } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; } }
    int ApplyHudSafe(bool hide, bool log) { int r = ApplyHudSafeImpl(hide, log); if (r < 0) { g_hudSet.clear(); g_hudWasHidden.clear(); } return r; }
}

// test command "hudscan": every object of the pushed HUD package - depth, name hash, type, flags, child
// count - with the object's first 0x80 bytes and 0x60 bytes behind each pointer-like field there, to find
// where an object keeps its screen position (to pick the minimap's and the gauges' parts by place)
namespace {
    void ScanObj(void* o, int depth, int& n) {
        if (n > 400) return;
        ++n;
        const uint32_t* w = (const uint32_t*)o;
        char line[1400]; int L = sprintf_s(line, "HUDSCAN d%d %p hash %08X type %d flags %08X kids %d |", depth, o, w[4], (int)w[6], w[7], ObjType(o) == 5 ? (int)w[0x60 / 4] : 0);
        for (int i = 0; i < 32 && L < 1300; ++i) L += sprintf_s(line + L, sizeof(line) - L, " %08X", w[i]);
        LOG("%s", line);
        for (int i = 8; i < 24; ++i) {
            const uint8_t* p = (const uint8_t*)(uintptr_t)w[i];
            if ((uintptr_t)p < 0x01000000 || !Mem::IsReadable(p, 0x60)) continue;
            const float* f = (const float*)p; int k = sprintf_s(line, "HUDSCAN    +%02X -> %p:", i * 4, p);
            for (int j = 0; j < 24 && k < 1300; ++j) k += sprintf_s(line + k, sizeof(line) - k, " %.2f", f[j]);
            LOG("%s", line);
        }
        if (ObjType(o) == 5 && depth < 8) {
            int c = (int)w[0x60 / 4]; void* ch = *(void**)((uint8_t*)o + 0x64);
            for (int i = 0; i < c && ch && i < 300; ++i, ch = *(void**)((uint8_t*)ch + 4)) ScanObj(ch, depth + 1, n);
        }
    }
    int HudScanImpl() {
        void* fe = nullptr; int n = 0;
        if (!Mem::Read(0x91CADC, fe) || !fe) return -1;
        for (const char* pkgName : kHudPackages) {
            if (!((bool(__thiscall*)(void*, const char*))0x516B50)(fe, pkgName)) continue;
            void* pkg = ((void*(__thiscall*)(void*, const char*))0x516AF0)(fe, pkgName);
            if (!pkg) continue;
            LOG("HUDSCAN package %s %p", pkgName, pkg);
            int guard = 0;
            for (void* o = *(void**)((uint8_t*)pkg + 0x4C); o && guard < 300; o = *(void**)((uint8_t*)o + 4), ++guard) ScanObj(o, 0, n);
        }
        return n;
    }
    int HudScanSafe() { __try { return HudScanImpl(); } __except (EXCEPTION_EXECUTE_HANDLER) { return -2; } }
}
std::string Game::DebugHudScan() { int n = HudScanSafe(); return "hud scan: " + std::to_string(n) + " object(s) in the log"; }

std::string Game::ToggleHud() {
    if (!g_ok) return "game not recognised";
    g_hudHidden = !g_hudHidden;
    int n = ApplyHudSafe(g_hudHidden, true);
    return std::string(g_hudHidden ? "HUD: minimap and speedometer hidden" : "HUD: minimap and speedometer shown") + " (" + std::to_string(n) + ")";
}
// while hidden: a new HUD package (an event loaded) gets the same treatment, once a second
void Game::HudTick() {
    if (!g_ok || !g_hudHidden || GameFlowState() != 6) return;
    static DWORD next = 0;
    if ((int)(GetTickCount() - next) < 0) return;
    next = GetTickCount() + 1000;
    ApplyHudSafe(true, false);
}
