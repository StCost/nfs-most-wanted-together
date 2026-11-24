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
}

static void InstallBossHelper() {
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
    LOG("Race: rival-race helper hooked (%s; own name %s, normal catch-up %s)", g_cfg.bossHelper ? "on" : "off in the ini",
        n ? "yes" : "NO", s ? "yes" : "NO");
}
