// Included from game.cpp (after stream.inl).
// ---------------------------------------------------------------------------
// Fast start
//  [Start] SkipIntro: the boot flow (BootFlowManager, *0x91CF48) takes its screens from static tables of
//    string pointers (0x8F3C58..0x8F3CD0). Pointing the EA logo / PSA / attract movie / "Press START"
//    entries at "" makes the boot go MC_Bootup -> MC_Main (profile autoload) -> MainMenu.
//  [Start] DirectFreeRoam: once the profile is loaded and MainMenu is up, do what Career > Resume
//    Career > Resume Free Roam > Yes do: CareerSettings::ResumeCareer (0x57F570, this = profile+0xA8),
//    GManager+0x110 = 1 (start at the safe house), the career-intro check, 0x56C5B0 (start career free
//    roam), FEGameMode |= 1.
//  [Start] SkipLeaveSafeHouseDialog: CResumeFreeRoam::React (vtable slot 0x89C42C, 0x51FF10) asks
//    "leave the safe house?"; instead send its Yes message (0xD05FC3A3) straight back to the screen.
// ---------------------------------------------------------------------------
namespace {
    using ReactFn = int(__thiscall*)(void*, const char*, uint32_t, void*, uint32_t, uint32_t);
    ReactFn g_origResumeFreeRoamReact = nullptr;
    int __fastcall hk_ResumeFreeRoamReact(void* self, void*, const char* pkg, uint32_t msg, void* obj, uint32_t p1, uint32_t p2) {
        if (msg == 0x0C407210) {   // the option was activated: answer the dialog's Yes ourselves
            void* fe = nullptr;
            if (Mem::Read(0x91CADC, fe) && fe) {
                ((void(__thiscall*)(void*, uint32_t, const char*, uint32_t))0x516C50)(fe, 0xD05FC3A3, pkg, 0xFF);
                LOG("Start: Resume Free Roam without the leave-safe-house dialog");
                return 1;
            }
        }
        return g_origResumeFreeRoamReact(self, pkg, msg, obj, p1, p2);
    }

    bool RaceDone(const char* id) {
        void* db = nullptr;
        if (!Mem::Read(0x91E004, db) || !db) return false;
        uint32_t h = ((uint32_t(__cdecl*)(const char*))0x4519D0)(id);
        void* p = ((void*(__thiscall*)(void*, uint32_t))0x5FB710)(db, h);
        if (!p) return false;
        uint32_t eh = ((uint32_t(__thiscall*)(void*))0x5FAB40)(p);
        return ((bool(__thiscall*)(void*, uint32_t, int))0x5DCA70)(db, eh, 2);
    }

    // "Press accept" on a front-end screen: a real key when the game is focused, otherwise the FE
    // message the pad/keyboard would produce (PAD_ACCEPT 0x406415E3 via cFEng::QueueGameMessage
    // 0x516C50), so the fast start also works with the game in the background.
    bool PressAccept(const char* pkg) {
        if (Overlay::GameWindow() == GetForegroundWindow()) return Input::Tap(0x1C);
        void* fe = nullptr;
        if (!Mem::Read(0x91CADC, fe) || !fe) return false;
        ((void(__thiscall*)(void*, uint32_t, const char*, uint32_t))0x516C50)(fe, 0x406415E3, pkg, 0xFF);
        return true;
    }

    enum class Boot { Waiting, Settling, Done } g_boot = Boot::Waiting;
    DWORD g_bootReadyAt = 0;

    // returns a message for the log
    const char* DirectFreeRoamNow() {
        uint8_t* fedb = nullptr;
        Mem::Read(0x91CF90, fedb);
        uint8_t* up = nullptr;
        if (!fedb || !Mem::Read((uintptr_t)fedb + 0x10, up) || !up) return "no profile";
        if (!(up[0xAC] & 1)) return "career not started in this profile";
        bool prologue = up[0xB0] == 0x10;
        bool special = !((*(uint32_t*)(up + 0xAC) >> 14) & 1) && RaceDone("1.2.3");
        ((void(__thiscall*)(void*))0x57F570)(up + 0xA8);                  // ResumeCareer
        if (prologue || special) return "Resume Career started loading by itself";
        uint8_t* gm = nullptr;
        if (Mem::Read(0x91E00C, gm) && gm) gm[0x110] = 1;                 // start from the safe house
        uint8_t debugFlag = 0; Mem::Read(0x926126, debugFlag);
        if (!debugFlag && !RaceDone("16.2.1")) {                          // same as the safe house Yes handler
            void* db = nullptr; Mem::Read(0x91E004, db);
            const char* name = ((const char*(__thiscall*)(void*))0x600C30)(db);
            if (name) {
                uint32_t h = ((uint32_t(__cdecl*)(const char*))0x4519D0)(name);
                void* p = ((void*(__thiscall*)(void*, uint32_t))0x5FB710)(db, h);
                if (p) {
                    void* c = ((void*(__thiscall*)(void*, void*))0x603440)(db, p);
                    ((void(__thiscall*)(void*, void*, int))0x600AB0)(db, c, 2);
                    ((void(__thiscall*)(void*, void*))0x5E39E0)(db, c);
                }
            }
        }
        ((void(__cdecl*)())0x56C5B0)();                                   // start career free roam
        *(uint32_t*)(fedb + 0x12C) |= 1;
        return "loading career free roam";
    }
}

void Game::InstallBootPatches() {
    if (g_cfg.skipIntro) {
        int n = 0;
        static const char empty[] = "";
        // (the "Press START" splash stays: pressing START there sets up the active controller; we press it)
        static const char* drop[] = { "LS_EAlogo.fng", "LS_PSA.fng", "MW_LS_AttractFMV.fng" };
        for (uintptr_t a = 0x8F3C58; a < 0x8F3CD0; a += 4) {
            const char* s = nullptr;
            if (!Mem::Read(a, s) || !s || !Mem::IsReadable(s, 4)) continue;
            for (const char* d : drop)
                if (!_stricmp(s, d)) { Mem::PatchPointer((void*)a, (void*)empty); ++n; break; }
        }
        LOG("Start: skip intro screens - %d boot entries removed", n);
    }
    if (g_cfg.skipLeaveSafeHouseDialog) {
        uintptr_t cur = 0;
        if (Mem::Read(0x89C42C, cur) && cur == 0x51FF10) {
            g_origResumeFreeRoamReact = (ReactFn)cur;
            Mem::PatchPointer((void*)0x89C42C, (void*)&hk_ResumeFreeRoamReact);
            LOG("Start: no leave-safe-house dialog");
        } else LOG("Start: Resume Free Roam slot holds %08X - dialog kept", (unsigned)cur);
    }
}

bool Game::DirectStartActive() { return g_cfg.directFreeRoam && g_boot != Boot::Done; }

void Game::BootTick() {
    if (!g_ok || !g_cfg.directFreeRoam || g_boot == Boot::Done) return;
    __try {
        if (GameFlowState() != 3) { if (GameFlowState() == 6) g_boot = Boot::Done; return; }
        uint8_t* fedb = nullptr; void* bfm = (void*)1; void* fe = nullptr;
        Mem::Read(0x91CF90, fedb); Mem::Read(0x91CF48, bfm); Mem::Read(0x91CADC, fe);
        // "Press START" (splash) and "load complete" (MC_Main) wait for a key: press it
        if (fe && bfm && ((bool(__thiscall*)(void*, const char*))0x516B50)(fe, "WS_MW_LS_Splash.fng")) {
            static DWORD since = 0, lastTap = 0;
            if (!since) since = GetTickCount();
            if (GetTickCount() - since > 1500 && GetTickCount() - lastTap > 2500) {
                lastTap = GetTickCount();
                if (PressAccept("WS_MW_LS_Splash.fng")) LOG("Start: pressing START on the title screen");
            }
            return;
        }
        if (fe && bfm && ((bool(__thiscall*)(void*, const char*))0x516B50)(fe, "MC_Main.fng")) {
            static DWORD since = 0, lastTap = 0;
            if (!since) since = GetTickCount();
            if (GetTickCount() - since > 800 && GetTickCount() - lastTap > 2500) {
                lastTap = GetTickCount();
                if (PressAccept("MC_Main.fng")) LOG("Start: confirming the profile load screen");
            }
            return;
        }
        bool ready = fedb && fedb[3] /*profile loaded*/ && !bfm && fe &&
                     ((bool(__thiscall*)(void*, const char*))0x516B50)(fe, "MainMenu.fng");
        if (!ready) { g_boot = Boot::Waiting; return; }
        if (g_boot == Boot::Waiting) { g_boot = Boot::Settling; g_bootReadyAt = GetTickCount(); return; }
        if (GetTickCount() - g_bootReadyAt < 1000) return;   // let the garage finish loading
        g_boot = Boot::Done;
        LOG("Start: direct free roam - %s", DirectFreeRoamNow());
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_boot = Boot::Done; LOG("Start: exception in direct free roam"); }
}

// [UI] SkipPursuitResults: after a pursuit in free roam (escaped / busted) the game shows
// PostRace_Pursuit.fng and waits for a key. Close it by itself (only this player's game).
void Game::PursuitResultTick() {
    if (!g_ok || !g_cfg.skipPursuitResults || GameFlowState() != 6) return;
    static DWORD since = 0, lastTap = 0;
    bool open = FEScreens::IsPushed("PostRace_Pursuit.fng");
    int mode = 0; void* rs = nullptr;
    if (Mem::Read(0x91E000, rs) && rs) Mem::Read((uintptr_t)rs + 0x1960, mode);
    if (!open || mode == 1) { since = 0; return; }        // not open, or it belongs to a race
    if (!since) since = GetTickCount();
    if (GetTickCount() - since > 700 && GetTickCount() - lastTap > 1500) {
        lastTap = GetTickCount();
        if (Input::Tap(0x1C)) LOG("UI: closing the pursuit result screen");
    }
}