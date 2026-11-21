// Included from game.cpp (after worldmap.inl).
// ---------------------------------------------------------------------------
// F1: hide the minimap (with the heat meter) and the speedometer / tachometer cluster; the rest of the
// HUD (the rear-view mirror, race information, notices) stays. [UI] HideHudKey.
//  The HUD's parts are FEng group objects of the HUD package (HUD_SingleRace.fng, HUD_Drag.fng), found
//  by name: the HUD's own code looks them up with the upper-case FE hash 0x5AF1C0 ("SpeedometerGroup",
//  "MINI_MAP", "GaugeCluster"). FEObject +0x1C flags: bit 0 = hidden, 0x2400000 = changed (what
//  FEngSetInvisible 0x514C70 / FEngSetVisible 0x514CC0 write; those also walk a group's children - we
//  flip only the group's own bit, so its children keep whatever the HUD set for them). Said again every
//  frame while hidden: the HUD may show a group again when its state changes.
// ---------------------------------------------------------------------------
namespace {
    bool g_hudHidden = false;
    const char* const kHudPackages[] = { "HUD_SingleRace.fng", "HUD_Drag.fng", "HUD_Player1.fng", "HUD_Drag_Player1.fng" };
    const char* const kHudHide[] = { "MINI_MAP", "SpeedometerGroup", "GaugeCluster" };

    void* HudObject(void* fe, const char* pkgName, const char* name) {
        void* pkg = ((void*(__thiscall*)(void*, const char*))0x516AF0)(fe, pkgName);
        if (!pkg) return nullptr;
        uint32_t h = ((uint32_t(__cdecl*)(const char*))0x5AF1C0)(name);
        return ((void*(__thiscall*)(void*, uint32_t))0x5B8550)(pkg, h);
    }
    // apply the hidden / shown state to the HUD parts; returns how many were found
    int ApplyHud(bool hide, bool log) {
        void* fe = nullptr;
        if (!Mem::Read(0x91CADC, fe) || !fe) return 0;
        int found = 0;
        for (const char* pkg : kHudPackages) {
            if (!((bool(__thiscall*)(void*, const char*))0x516B50)(fe, pkg)) continue;     // pushed now
            for (const char* name : kHudHide) {
                uint8_t* obj = (uint8_t*)HudObject(fe, pkg, name);
                if (!obj || !Mem::IsReadable(obj + 0x1C, 4)) { if (log) LOG("HUD: %s not found in %s", name, pkg); continue; }
                uint32_t& f = *(uint32_t*)(obj + 0x1C);
                if (hide) { if (!(f & 1)) f |= 0x2400001; }
                else if (f & 1) f = (f & ~1u) | 0x2400000;
                ++found;
                if (log) LOG("HUD: %s %s (%s)", hide ? "hid" : "showed", name, pkg);
            }
        }
        return found;
    }
    int ApplyHudSafe(bool hide, bool log) { __try { return ApplyHud(hide, log); } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; } }
}

std::string Game::ToggleHud() {
    if (!g_ok) return "game not recognised";
    g_hudHidden = !g_hudHidden;
    int n = ApplyHudSafe(g_hudHidden, true);
    return std::string(g_hudHidden ? "HUD: minimap and speedometer hidden" : "HUD: minimap and speedometer shown") + " (" + std::to_string(n) + " part(s))";
}
void Game::HudTick() {
    if (!g_ok || !g_hudHidden || GameFlowState() != 6) return;
    ApplyHudSafe(true, false);
}
