// Included from game.cpp (after milestone.inl).
// ---------------------------------------------------------------------------
// Rival races together: one more AI racer on the rival's side
//  A Blacklist rival race has one opponent, the rival. When both players race it together, both
//  games add one more AI racer (the "helper"): one more car to beat. It drives with the game's normal
//  racing AI (the same character the game uses for quick-race rivals, "character_smart") and the
//  catch-up of a normal racer, not the rival's.
//  All verified in the exe:
//   InitRacers 0x61DBE0 adds the racers: count = 1 - humans + GRaceParameters::GetNumOpponents
//   (0x5FACD0, called at 0x61DCB7; the event's "Opponents" attribute 0x5839FA1A), and for each i
//   GetOpponent(i) (0x5FD2D0, called at 0x61DCC8; thiscall, ret 4) gives the driver record that
//   AddRacer 0x601F90 puts into the next GRacerInfo slot (GRaceStatus+0x18 + n*0x194, n at +0x1958,
//   room for 16). The cars are built after that, one per racer slot, so the helper gets a car and a
//   grid slot like any racer. The race script reads the racer count after InitRacers, so its per-racer
//   tables include the helper. The other GetNumOpponents callers (0x426CA6, 0x431533: "> 0" tests;
//   0x611902: knockout) are left alone.
//   Outside a career, GetOpponent of a rival race returns 0x5E1880(0x4549B0("character_smart")) for
//   every index; the helper gets that driver in every mode.
//   AddRacer names a career rival race's racers after the rival (0x600730, called at 0x601FC7): the
//   helper keeps the usual AI name instead.
//   AI skill 0x5FF990 (called once, at 0x429644, per AI racer with its GRacerInfo) raises the floor of
//   every racer in a rival race (IsBossRace 0x5FBD90 at 0x5FFA2E): for the helper that test says "no",
//   so it paces itself like a racer in a normal career race.
//  Both games add the helper when they are linked (or [Debug] BossHelperSolo), so the racer lists
//  match. The game that joins builds the starter's helper from its roster (the entry marked as the helper,
//  else entry = opponent count). The helper always gets a full parts record of its model (with none it
//  had only its wheels, no body): see CoopRace::ChooseHelperCar. The client's racers follow the host's
//  by racer slot and role (traffic.inl), so a rival and a helper of the same model can't swap.
// ---------------------------------------------------------------------------
namespace {
    constexpr uintptr_t NumOppCall = 0x61DCB7, NumOppFn = 0x5FACD0;
    constexpr uintptr_t GetOppCall = 0x61DCC8, GetOppFn = 0x5FD2D0;
    constexpr uintptr_t RivalNameCall = 0x601FC7, RivalNameFn = 0x600730;
    constexpr uintptr_t SkillCall = 0x429644, SkillFn = 0x5FF990;
    constexpr uintptr_t SkillBossCall = 0x5FFA2E, IsBossRaceFn = 0x5FBD90;
    void* g_skillRacer = nullptr;      // the GRacerInfo whose skill 0x5FF990 is working out

    char* RaceStatusPtr() { char* rs = nullptr; Mem::Read(0x91E000, rs); return rs; }
    bool IsHelperRacer(void* ri) {
        char* rs = RaceStatusPtr();
        return ri && rs && g_helperOn && g_helperSlot >= 0 && ri == rs + 0x18 + g_helperSlot * 0x194;
    }
    bool IsPartnerRacer(void* ri) {
        char* rs = RaceStatusPtr();
        return ri && rs && g_partnerOn && g_partnerSlot >= 0 && ri == rs + 0x18 + g_partnerSlot * 0x194;
    }

    int __fastcall hk_NumOpponents(void* parms, void*) {
        int n = ((int(__thiscall*)(void*))NumOppFn)(parms);
        g_helperOn = false; g_helperOpp = -1; g_helperSlot = -1; g_bossRacerBuilds = 0; g_helperPeerRoster = false;
        g_partnerOn = false; g_partnerOpp = -1; g_partnerSlot = -1;
        g_bossRaceOn = false; g_bossRivalKey = 0; g_bossRivalHasRec = false;
        __try {
            if (parms) g_bossRaceOn = ((bool(__thiscall*)(void*))IsBossRaceFn)(parms);
            bool linked = Net::Stats().connected && g_coopLinked;
            bool helper = g_cfg.bossHelper, partner = g_cfg.partnerInRivalRaces && linked && g_cfg.ghostEnabled && Net::PeerCar();
            if (!(helper || partner) || !parms || n < 1 || !(linked || g_cfg.bossHelperSolo)) return n;
            if (!g_bossRaceOn) return n;
            {   // never in the prologue's scripted races
                const char* id = ((const char*(__thiscall*)(void*))0x5FBA70)(parms);
                if (id && Mem::IsReadable(id, 4) && Prologue::IsPrologueEvent(id)) return n;
            }
            char* rs = RaceStatusPtr();
            int have = 0;
            int extra = (helper ? 1 : 0) + (partner ? 1 : 0);
            if (!rs || !Mem::Read((uintptr_t)rs + 0x1958, have) || have < 0 || have + n + extra > 16) return n;
            int next = n;
            if (helper) { g_helperOn = true; g_helperOpp = next++; }
            // the partner's slot must be built as their car in this load too (the rival's next race may come
            // without a world load, and the "replaced once per load" flag would turn the slot into a free AI racer)
            if (partner) { g_partnerOn = true; g_partnerOpp = next++; g_racerReplacedThisLoad = false; g_replacedRacer = nullptr; }
            LOG("Race: rival race together - %d opponent(s)%s%s", n, helper ? " + 1 helper racer" : "", partner ? " + the other player's racer slot" : "");
            return n + extra;
        } __except (EXCEPTION_EXECUTE_HANDLER) { g_helperOn = false; return n; }
    }
    void* __fastcall hk_GetOpponent(void* parms, void*, int i) {
        bool isHelper = g_helperOn && i == g_helperOpp, isPartner = g_partnerOn && i == g_partnerOpp;
        if (!isHelper && !isPartner) return ((void*(__thiscall*)(void*, int))GetOppFn)(parms, i);
        char* rs = RaceStatusPtr();
        int slot = -1;
        if (rs) Mem::Read((uintptr_t)rs + 0x1958, slot);   // AddRacer puts it into this slot next
        if (isPartner) {
            g_partnerSlot = slot;
            uint32_t ph = ((uint32_t(__cdecl*)(const char*))0x4549B0)("character_smart");
            void* pd = ((void*(__cdecl*)(uint32_t))0x5E1880)(ph);
            LOG("Race: the other player's racer = opponent %d, racer slot %d", i, slot);
            return pd;   // a racer record; the car is built as theirs (vehicle factory) and its AI is switched off
        }
        g_helperSlot = slot;
        uint32_t h = ((uint32_t(__cdecl*)(const char*))0x4549B0)("character_smart");
        void* drv = ((void*(__cdecl*)(uint32_t))0x5E1880)(h);
        LOG("Race: helper racer = opponent %d, racer slot %d, driver %p", i, slot, drv);
        return drv;
    }
    bool __fastcall hk_RivalName(void* ri, void*) {
        if (IsHelperRacer(ri) || IsPartnerRacer(ri)) return false;   // the usual name (the other player's is set later), not the rival's
        return ((bool(__thiscall*)(void*))RivalNameFn)(ri);
    }
    bool __fastcall hk_RacerSkill(void* rs, void*, void* ri, void* a, void* b, void* c, uint32_t d) {
        g_skillRacer = ri;
        bool r = ((bool(__thiscall*)(void*, void*, void*, void*, void*, uint32_t))SkillFn)(rs, ri, a, b, c, d);
        g_skillRacer = nullptr;
        return r;
    }
    bool __fastcall hk_SkillIsBossRace(void* parms, void*) {
        if (g_skillRacer && IsHelperRacer(g_skillRacer)) return false;   // a normal racer's catch-up
        return ((bool(__thiscall*)(void*))IsBossRaceFn)(parms);
    }

    // ---- the helper's spot on the start line ----
    // A rival race's grid has slots for its own two cars; the game put the helper (racer slot 2) well ahead
    // of everyone (reported: "too far forward, while we all got behind"). The race script saves every car's
    // spot (Lua SaveStartPositions 0x604E90, registered at 0x61EA13) before the intro and puts them back
    // after it (RestoreStartPositions 0x60AA50 -> 0x5E96D0 from GRacerInfo +0x170): right before that save
    // a helper that stands ahead of our row goes to a free slot of the grid behind us instead.
    constexpr uintptr_t SaveStartReg = 0x61EA13, SaveStartFn = 0x604E90;
    void FixHelperGridSpot() {
        if (!g_helperOn || g_helperSlot < 0) return;
        void* me = LocalVehicle();
        void* lrb = RigidBodyOf(me);
        char* rs = RaceStatusPtr();
        if (!lrb || !rs) return;
        void* helperRi = rs + 0x18 + g_helperSlot * 0x194;
        void* helper = nullptr;
        void** b = nullptr;
        for (int i = 0, total = ListSize(VL_ALL, &b); i < total && !helper; ++i) {
            void* iv = ListGet(VL_ALL, i);
            if (iv && iv != me && IsVehicle(iv) && RacerInfoOf(iv) == helperRi) helper = iv;
        }
        char cars[512]; GridCarsText(lrb, cars, sizeof(cars));
        LOG("Race: start grid before the intro (m ahead/right of us): %s", cars);
        void* hrb = helper ? RigidBodyOf(helper) : nullptr;
        if (!hrb) { LOG("Race: the helper racer has no car yet - its start spot stays the game's"); return; }
        float f[3];
        VCall<void(__thiscall*)(void*, float*)>(lrb, 13 /*GetForwardVector*/)(lrb, f);
        const float* p = RB_GetVec(lrb, RB_GetPosition);
        const float* h = RB_GetVec(hrb, RB_GetPosition);
        float fl = sqrtf(f[0] * f[0] + f[2] * f[2]);
        if (!(fl > 0.1f)) return;
        float fx = f[0] / fl, fz = f[2] / fl, rx = fz, rz = -fx;
        float hAlong = (h[0] - p[0]) * fx + (h[2] - p[2]) * fz, hSide = (h[0] - p[0]) * rx + (h[2] - p[2]) * rz;
        if (hAlong <= 3.f && fabsf(h[1] - p[1]) < 4.f) {
            LOG("Race: the helper racer starts %.1f m ahead / %.1f m right of us - in our grid, kept", hAlong, hSide);
            return;
        }
        // the grid around us without the helper (the other player's racer counts: it has a slot)
        CoopRace::GridCar gc[24]; int n = 0;
        for (int i = 0, total = ListSize(VL_ALL, &b); i < total && n < 24; ++i) {
            void* iv = ListGet(VL_ALL, i);
            if (!iv || iv == me || iv == helper || !IsVehicle(iv)) continue;
            void* orb = RigidBodyOf(iv);
            const float* q = orb ? RB_GetVec(orb, RB_GetPosition) : nullptr;
            if (!q) continue;
            float dx = q[0] - p[0], dy = q[1] - p[1], dz = q[2] - p[2];
            if (fabsf(dy) > 4.f || dx * dx + dz * dz > 40.f * 40.f) continue;
            gc[n].along = dx * fx + dz * fz; gc[n].side = dx * rx + dz * rz; ++n;
        }
        float along = 0.f, side = 0.f;
        bool found = CoopRace::GridSlotFor(gc, n, along, side);
        float to[3] = { p[0] + fx * along + rx * side, p[1] + 0.05f, p[2] + fz * along + rz * side }, zero[3] = { 0, 0, 0 };
        RB_SetVec(hrb, RB_SetPosition, to);
        RB_SetVec(hrb, RB_SetOrientationQuat, RB_GetVec(lrb, RB_GetOrientation));   // grid slots face the same way
        RB_SetVec(hrb, RB_SetLinearVelocity, zero); RB_SetVec(hrb, RB_SetAngularVelocity, zero);
        LOG("Race: the helper racer stood %.1f m ahead / %.1f m right of us - moved to %s %.1f m %s, %.1f m %s of us",
            hAlong, hSide, found ? "a free grid slot" : "behind us (no free slot read)",
            fabsf(along), along < 0 ? "behind" : "ahead", fabsf(side), side < 0 ? "left" : "right");
    }
    void __cdecl hk_SaveStartPositions() {
        __try { FixHelperGridSpot(); } __except (EXCEPTION_EXECUTE_HANDLER) { LOG("Race: exception moving the helper to the grid"); }
        ((void(__cdecl*)())SaveStartFn)();
    }
}

// The game composites a racer's paint and vinyls into one of 4 skin slots (DUMMY_SKIN1..4, picked from a
// bit mask at 0x75D2A0; slots 5..12 are only used in its pre-composited mode). A rival race already fills
// all 4 (us, the rival and Big Lou's two partners in the intro), so with the helper and the other player's
// racer added the intro's partner cars got no slot and were drawn without a body. Two more slots fix it.
static void InstallSkinSlots() {
    static const uint8_t pick[3] = {0x83, 0xFA, 0x04}, pickNew[3] = {0x83, 0xFA, 0x06};   // cmp edx,4 -> 6
    static const uint8_t kind[3] = {0x83, 0xFF, 0x04}, kindNew[3] = {0x83, 0xFF, 0x06};   // slot 5,6: composited kind
    bool ok = Mem::PatchBytes(0x747F20, kind, kindNew, 3) && Mem::PatchBytes(0x75D2B4, pick, pickNew, 3);   // kind first: alone it is harmless
    LOG("Race: car skin slots %s", ok ? "6" : "NOT raised (code differs)");
}
static void InstallBossHelper() {
    InstallSkinSlots();   // needed with or without the helper: the other player's racer takes a slot too
    // the two racer-list hooks go together (a count without the driver would index past the event's list)
    uint8_t op1 = 0, op2 = 0; int32_t r1 = 0, r2 = 0;
    bool sites = Mem::Read(NumOppCall, op1) && Mem::Read(NumOppCall + 1, r1) && op1 == 0xE8 && NumOppCall + 5 + r1 == NumOppFn &&
                 Mem::Read(GetOppCall, op2) && Mem::Read(GetOppCall + 1, r2) && op2 == 0xE8 && GetOppCall + 5 + r2 == GetOppFn;
    if (!sites) { LOG("Race: rival-race helper NOT hooked (call sites differ)"); return; }
    Mem::PatchCall(NumOppCall, (void*)&hk_NumOpponents);
    Mem::PatchCall(GetOppCall, (void*)&hk_GetOpponent);
    bool n = PatchCallChecked(RivalNameCall, RivalNameFn, (void*)&hk_RivalName);
    bool s = PatchCallChecked(SkillCall, SkillFn, (void*)&hk_RacerSkill) &&
             PatchCallChecked(SkillBossCall, IsBossRaceFn, (void*)&hk_SkillIsBossRace);
    uint8_t expect[5] = { 0x68, 0, 0, 0, 0 }, repl[5] = { 0x68, 0, 0, 0, 0 };     // push SaveStartPositions (Lua binding)
    uint32_t fnOld = SaveStartFn, fnNew = (uint32_t)(uintptr_t)&hk_SaveStartPositions;
    memcpy(expect + 1, &fnOld, 4); memcpy(repl + 1, &fnNew, 4);
    bool g = Mem::PatchBytes(SaveStartReg, expect, repl, 5);
    LOG("Race: rival-race helper hooked (%s; own name %s, normal catch-up %s, start spot %s)", g_cfg.bossHelper ? "on" : "off in the ini",
        n ? "yes" : "NO", s ? "yes" : "NO", g ? "yes" : "NO");
}
