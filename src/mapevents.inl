// Included from game.cpp (after coopwin.inl).
// ---------------------------------------------------------------------------
// Map events ([UI] MapEvents): show career events that aren't won yet on the map, minimap and as
// markers in the world, so a co-op pair can work through the whole career.
//   0 = the game's own choice (only events the career script has unlocked)
//   1 = also every not-won event of the current Blacklist rival
//   2 = also every not-won event of all 15 rivals (the map has a few icon slots per race type, so
//       some icons may not fit on the big map; the minimap and world markers show them all)
// Rival (boss) races keep the game's own rule. Nothing is written to the save: only the icons change.
//
//  0x602840 (thiscall GManager) walks the race triggers and decides each race icon. At 0x602914:
//    8B 41 04  C1 E8 03  24 01  88 44 24 13  74 1E  84 DB  75 1A
//    mov eax,[ecx+4] / shr eax,3 / and al,1 / mov [esp+13],al / jz hide / test bl,bl / jnz hide
//    ecx = the race's progress entry ([ecx] = event hash, [ecx+4] flags: 2 won, 8 available),
//    bl = won, ebp = the race trigger (+0xC0 its icon, icon +0x10 position). show 0x602926, hide 0x602940.
// ---------------------------------------------------------------------------
namespace {
    int g_mapMode = 0;
    bool g_mapHooked = false;
    uint8_t g_mapStore = 0;                       // value for the caller's [esp+13] (flag 8 as the game had it)
    struct MapExtra { uint32_t hash; void* trigger; };
    MapExtra g_mapExtra[96]; int g_mapExtraN = 0;

    int BinOfHash(uint32_t hash, bool& boss, const char*& idOut) {
        void* db = nullptr; Mem::Read(0x91E004, db);
        void* parms = db ? ((void*(__thiscall*)(void*, uint32_t))0x5FB710)(db, hash) : nullptr;
        if (!parms) return 0;
        boss = ((bool(__thiscall*)(void*))0x5FBD90)(parms);
        const char* id = EventIdOf(parms);
        idOut = id;
        if (!id || !Mem::IsReadable(id, 3)) return 0;
        int bin = 0;
        for (int i = 0; i < 3 && id[i] >= '0' && id[i] <= '9'; ++i) bin = bin * 10 + (id[i] - '0');
        return bin;
    }
    int CurrentBin() {
        uint8_t* fedb = nullptr; uint8_t* up = nullptr;
        if (!Mem::Read(0x91CF90, fedb) || !fedb || !Mem::Read((uintptr_t)fedb + 0x10, up) || !up) return 0;
        return up[0xB0];
    }
    // Only the closest extra events are shown, up to the big map's icon slots per race type
    // (WorldMapMain: sprint 6, circuit 6, drag 3, knockout 6, tollbooth 3, speedtrap 6), counting the
    // game's own icons of that type. Re-ranked every few seconds while roaming.
    const int kSlots[6] = { 6, 6, 3, 6, 3, 6 };     // by race type 0..5
    struct MapCand { uint32_t hash; void* trigger; int type; float dist; };
    MapCand g_cand[200]; int g_candN = 0; int g_nativeByType[6];
    uint32_t g_allowed[40]; int g_allowedN = 0;      // extra events shown now
    bool g_collecting = false;
    float g_me[2] = { 0, 0 };                        // our position in map space (x, y)

    bool Allowed(uint32_t h) { for (int i = 0; i < g_allowedN; ++i) if (g_allowed[i] == h) return true; return false; }
    int RaceTypeOf(uint32_t hash) {
        void* db = nullptr; Mem::Read(0x91E004, db);
        void* parms = db ? ((void*(__thiscall*)(void*, uint32_t))0x5FB710)(db, hash) : nullptr;
        return parms ? ((int(__thiscall*)(void*))0x5FAA20)(parms) : -1;
    }
    int g_seen = 0, g_seenWon = 0, g_seenOut = 0;      // diagnostics of the last collect pass
    bool MapDecideImpl(uint32_t* entry, int won, void* trigger) {
        uint32_t flags = entry[1];
        if (g_collecting) { ++g_seen; if (won) ++g_seenWon; }
        bool avail = (flags >> 3) & 1;
        g_mapStore = avail ? 1 : 0;
        if (avail && !won) {                      // the game's own choice
            if (g_collecting) { int ty = RaceTypeOf(entry[0]); if (ty >= 0 && ty < 6) ++g_nativeByType[ty]; }
            return true;
        }
        if (!g_mapMode || won) return false;
        bool boss = false; const char* id = nullptr;
        int bin = BinOfHash(entry[0], boss, id);
        if (boss || bin < 1 || bin > 15 || (g_mapMode == 1 && bin != CurrentBin())) { if (g_collecting) ++g_seenOut; return false; }   // career rivals only: no prologue (16), no Challenge Series (19)
        if (g_collecting && g_candN < 200) {
            int ty = RaceTypeOf(entry[0]);
            void* icon = nullptr;
            if (ty >= 0 && ty < 6 && Mem::Read((uintptr_t)trigger + 0xC0, icon) && icon) {
                const float* p = (const float*)((uint8_t*)icon + 0x10);
                float dx = p[0] - g_me[0], dy = p[1] - g_me[1];
                g_cand[g_candN++] = { entry[0], trigger, ty, dx * dx + dy * dy };
            }
        }
        if (!Allowed(entry[0])) return false;
        int i = 0;
        for (; i < g_mapExtraN; ++i) if (g_mapExtra[i].hash == entry[0]) break;
        if (i == g_mapExtraN && g_mapExtraN < (int)(sizeof(g_mapExtra) / sizeof(g_mapExtra[0]))) ++g_mapExtraN;
        if (i < g_mapExtraN) g_mapExtra[i] = { entry[0], trigger };
        g_mapStore = 1;
        return true;
    }    bool __cdecl MapDecide(uint32_t* entry, int won, void* trigger) {
        __try { return MapDecideImpl(entry, won, trigger); }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            __try { g_mapStore = (entry[1] >> 3) & 1; return g_mapStore && !won; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }
    }
    __declspec(naked) void hk_RaceIcon() {
        __asm {
            pushad
            push ebp
            movzx eax, bl
            push eax
            push ecx
            call MapDecide
            add esp, 12
            mov dl, g_mapStore
            mov [esp + 0x33], dl       ; the game's [esp+13] (pushad moved esp by 0x20)
            test al, al
            popad                      ; popad keeps the flags
            jz hide
            push 0x602926
            ret
        hide:
            push 0x602940
            ret
        }
    }
    bool InFreeRoam(void*& gm) {
        void* rs = nullptr; int mode = 0, ready = 0;
        if (!Mem::Read(0x91E00C, gm) || !gm || !Mem::Read((uintptr_t)gm + 0x118, ready) || !ready) return false;
        if (Mem::Read(0x91E000, rs) && rs && Mem::Read((uintptr_t)rs + 0x1960, mode) && mode != 0) return false;   // not in a race
        return true;
    }
    // returns true when the set of extra events changed (and the icons were updated)
    bool RefreshRaceIconsSafe(bool force) {
        __try {
            void* gm = nullptr;
            if (!InFreeRoam(gm)) return false;
            if (void* me = RigidBodyOf(LocalVehicle())) {
                const float* u = RB_GetVec(me, RB_GetPosition);
                g_me[0] = u[2]; g_me[1] = -u[0];            // UMath -> map space: b.x = u.z, b.y = -u.x
            }
            g_candN = 0; memset(g_nativeByType, 0, sizeof(g_nativeByType));
            g_seen = g_seenWon = g_seenOut = 0;
            g_mapExtraN = 0;                                 // re-recorded below: no trigger pointers from an older world
            g_collecting = true;
            ((void(__thiscall*)(void*))0x602840)(gm);        // with the current set: nothing changes on screen
            g_collecting = false;
            uint32_t next[40]; int nextN = 0;
            for (int ty = 0; ty < 6; ++ty) {
                int room = kSlots[ty] - g_nativeByType[ty];
                while (room-- > 0 && nextN < 40) {            // closest remaining candidate of this type
                    int best = -1;
                    for (int i = 0; i < g_candN; ++i) {
                        if (g_cand[i].type != ty) continue;
                        bool taken = false; for (int k = 0; k < nextN; ++k) if (next[k] == g_cand[i].hash) taken = true;
                        // an event already shown keeps its place unless another is clearly closer (no flicker)
                        if (taken) continue;
                        float di = g_cand[i].dist * (Allowed(g_cand[i].hash) ? 0.72f : 1.0f);
                        float db = best < 0 ? 0 : g_cand[best].dist * (Allowed(g_cand[best].hash) ? 0.72f : 1.0f);
                        if (best < 0 || di < db) best = i;
                    }
                    if (best < 0) break;
                    next[nextN++] = g_cand[best].hash;
                }
            }
            bool same = nextN == g_allowedN;
            for (int i = 0; same && i < nextN; ++i) same = Allowed(next[i]);
            if (same && !force) return false;
            memcpy(g_allowed, next, sizeof(uint32_t) * nextN); g_allowedN = nextN;
            g_mapExtraN = 0;
            ((void(__thiscall*)(void*))0x602840)(gm);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { g_collecting = false; return false; }
    }
}

// Every few seconds while roaming: keep the closest not-won events on the map.
void Game::MapEventsTick() {
    static DWORD next = 0;
    if (!g_ok || !g_mapHooked) return;
    if (GameFlowState() != 6) { g_allowedN = 0; g_mapExtraN = 0; return; }   // menus / loading: another career may come next
    if (!g_mapMode || (int)(GetTickCount() - next) < 0) return;
    next = GetTickCount() + 4000;
    if (RefreshRaceIconsSafe(false)) LOG("Map: %d not-won event(s) shown besides the game's own", g_allowedN);
}
bool Game::InstallMapEvents() {
    g_mapMode = g_cfg.mapEvents;
    const uint8_t expect[18] = { 0x8B, 0x41, 0x04, 0xC1, 0xE8, 0x03, 0x24, 0x01, 0x88, 0x44, 0x24, 0x13, 0x74, 0x1E, 0x84, 0xDB, 0x75, 0x1A };
    bool ok = PatchJmp(0x602914, expect, 18, (void*)&hk_RaceIcon);
    g_mapHooked = ok;
    LOG("Map: event icons hook %s (MapEvents=%d)", ok ? "installed" : "NOT installed (code differs)", g_mapMode);
    return ok;
}

// icon positions are bVector world space (z up); rigid bodies use UMath (y up): u = (-b.y, b.z, b.x)
#define MAP_TO_RB_X(p) (-(p)[1])
#define MAP_TO_RB_Y(p) ((p)[2])
#define MAP_TO_RB_Z(p) ((p)[0])

// Test command: "mapevents" lists the extra events shown, "mapevents N" switches the mode now.
static void MapListSafe(char* out, size_t n) {
    __try {
        for (int i = 0; i < g_mapExtraN && i < 40; ++i) {
            bool boss = false; const char* id = nullptr; BinOfHash(g_mapExtra[i].hash, boss, id);
            strcat_s(out, n, " "); strcat_s(out, n, (id && Mem::IsReadable(id, 2)) ? id : "?");
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { strcat_s(out, n, " (exception)"); }
}
std::string Game::DebugMapEvents(const char* arg) {
    if (!g_ok || !g_mapHooked) return "not in game or hook not installed";
    if (arg && *arg) {
        int m = atoi(arg);
        if (m < 0 || m > 2) return "mode must be 0, 1 or 2";
        g_mapMode = m;
        if (!m) g_allowedN = 0;
        if (!RefreshRaceIconsSafe(true)) return "mode set - icons update when you are back in free roam";
    }
    char b[480]; sprintf_s(b, "mode %d (race triggers %d, won %d, out of scope %d, candidates %d, rival %d), %d extra event(s):",
                           g_mapMode, g_seen, g_seenWon, g_seenOut, g_candN, CurrentBin(), g_mapExtraN);
    MapListSafe(b, sizeof(b));
    return b;
}

// Test command: "goto <event id>" puts our car on that (extra) event's marker.
static int GotoEventSafe(const char* want, char* out, size_t n) {
    __try {
        for (int i = 0; i < g_mapExtraN; ++i) {
            bool boss = false; const char* id = nullptr; BinOfHash(g_mapExtra[i].hash, boss, id);
            if (!id || strcmp(id, want)) continue;
            void* icon = nullptr;
            if (!Mem::Read((uintptr_t)g_mapExtra[i].trigger + 0xC0, icon) || !icon) return 0;
            const float* p = (const float*)((uint8_t*)icon + 0x10);
            void* me = RigidBodyOf(LocalVehicle());
            if (!me) return 0;
            const float* mp = RB_GetVec(me, RB_GetPosition);
            sprintf_s(out, n, "marker of %s at %.1f %.1f %.1f (we are at %.1f %.1f %.1f)", id, p[0], p[1], p[2], mp[0], mp[1], mp[2]);
            float pos[3] = { MAP_TO_RB_X(p), MAP_TO_RB_Y(p) + 2.0f, MAP_TO_RB_Z(p) }, zero[3] = { 0, 0, 0 };
            g_testJumpOkUntil = GetTickCount() + 400;
            RB_SetVec(me, RB_SetPosition, pos); RB_SetVec(me, RB_SetLinearVelocity, zero); RB_SetVec(me, RB_SetAngularVelocity, zero);
            return 1;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    return 0;
}
std::string Game::DebugGotoEvent(const char* id) {
    if (!g_ok || GameFlowState() != 6) return "not in game";
    RefreshRaceIconsSafe(false);                     // fresh trigger pointers
    char b[200] = "";
    int r = GotoEventSafe(id, b, sizeof(b));
    return r > 0 ? std::string(b) : r < 0 ? "exception" : "no extra marker for that event (run mapevents first)";
}