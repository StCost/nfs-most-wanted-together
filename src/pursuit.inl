// Included from game.cpp (after traffic.inl).
// ---------------------------------------------------------------------------
// Pursuits
//  Every player has their own heat and their own pursuit (the game runs it locally); the other
//  player's pursuit cops are shown as mirrors (traffic.inl sends AICOPS both ways).
//  When both players are in a pursuit and the other player's car is close, one cop of OUR pursuit
//  switches its own target to the other player's car (AITarget::Aquire), so cops can go after
//  either player. Heat, busting and evading still track the local player only.
//
//  IVehicle vt+0xAC GetAIVehiclePtr -> IVehicleAI;  IVehicleAI vt+0xA0 GetPursuit -> IPursuit (or null)
//  IVehicleAI vt+0x64 GetTarget -> AITarget (+0x1C ISimable*, +0x2C valid)
//  IPursuit vt+0x08 GetTarget -> AITarget;  cops: IVehicle** at +0x18, count at +0x20
//  AITarget::Aquire(ISimable*) 0x423860 (thiscall). IPerpetrator handle 0x4037E0, vt+4 GetHeat
//
//  [Coop] SharedPursuits (see the second half of this file). Free roam: everything below. Both in the
//  same event (Challenge Series, a career pursuit event, a race with cops): only the cooldown /
//  escape rule (frozen until both are in cooldown, escape together). Free roam: shared heat, the other
//  player's cops on the minimap and the radar detector, ramming one of their cops starts a pursuit
//  here. Evade bar and busted are personal. A player who evades ENTERS cooldown immediately;
//  the meter is frozen until both are in cooldown, then both run as usual. Escape completes
//  only when both have finished. If a player is spotted, only they drop back to pursuit; the
//  other stays in cooldown, their progress is reset to 0 and frozen until the spotted player
//  evades again. A hidden player is never pulled back into pursuit because the other was spotted.
//  During a live shared pursuit the safehouse world marker and map/radar icon are hidden, and
//  entry is blocked, until both are in cooldown. Then either player may enter; the game evades
//  that player as usual, and PURSUIT_EVADED forces the other player's pursuit to end as an
//  escape/win. Disconnect: markers and entry go back to vanilla (solo).
// ---------------------------------------------------------------------------
namespace {
    constexpr uintptr_t AquireFn = 0x423860, IPerpetratorHandle = 0x4037E0;
    constexpr float RETARGET_RANGE = 60.f, RELEASE_RANGE = 90.f;

    void* AIOf(void* iv) { return IsVehicle(iv) ? VCall<void* (__thiscall*)(void*)>(iv, 0xAC / 4)(iv) : nullptr; }
    void* PursuitOf(void* iv) {
        void* ai = AIOf(iv);
        return ai ? VCall<void* (__thiscall*)(void*)>(ai, 0xA0 / 4)(ai) : nullptr;
    }
    void* CopTarget(void* cop) {
        void* ai = AIOf(cop);
        return ai ? VCall<void* (__thiscall*)(void*)>(ai, 0x64 / 4)(ai) : nullptr;
    }
    void* SimOf(void* iv) { return iv ? VCall<void* (__thiscall*)(void*)>(iv, IV_GetSimable)(iv) : nullptr; }
    void Aquire(void* target, void* sim) { ((void(__thiscall*)(void*, void*))AquireFn)(target, sim); }

    void*  g_retargetCop = nullptr;    // the cop of our pursuit currently chasing the other player
    double g_nextPursuitCheck = 0;

    void ReleaseRetarget(void* pursuit) {
        if (!g_retargetCop) return;
        if (ListContains(-1, g_retargetCop) && pursuit) {
            void* pt = VCall<void* (__thiscall*)(void*)>(pursuit, 2)(pursuit);
            void* sim = nullptr;
            if (pt && Mem::Read((uintptr_t)pt + 0x1C, sim) && sim)
                if (void* t = CopTarget(g_retargetCop)) Aquire(t, sim);
        }
        LOG("Pursuit: cop %p back on the local player", g_retargetCop);
        g_retargetCop = nullptr;
    }
}

// Heat x10 of the local player's pursuit, 0 = not in a pursuit. Sent to the peer.
int16_t Game::LocalPursuitHeat() {
    if (!g_ok || Game::GameFlowState() != 6) return 0;
    __try {
        void* me = LocalVehicle();
        if (!PursuitOf(me)) return 0;
        float heat = 1.f;
        void* perp = Find(me, IPerpetratorHandle);
        if (perp) heat = VCall<float(__thiscall*)(void*)>(perp, 1)(perp);
        int h = (int)(heat * 10.f + 0.5f);
        return (int16_t)(h < 1 ? 1 : h);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static void PursuitTickImpl(bool connected, const Net::Snapshot& peer) {
    double now = Net::LocalClockMs();
    if (g_nextPursuitCheck - now > 1000) g_nextPursuitCheck = now;   // sim clock restarted
    if (now < g_nextPursuitCheck) return;
    g_nextPursuitCheck = now + 400;
    void* me = LocalVehicle();
    void* pursuit = (connected && Game::GameFlowState() == 6) ? PursuitOf(me) : nullptr;
    void* other = g_ghost;
    {   // one status line every 5 s while either of us is being chased
        static DWORD nextLog = 0;
        int n0 = 0; if (pursuit) Mem::Read((uintptr_t)pursuit + 0x20, n0);
        if ((pursuit || (peer.valid && peer.s.pursuitHeat > 0)) && (int)(GetTickCount() - nextLog) > 0) {
            nextLog = GetTickCount() + 5000;
            int mirrors = 0; for (auto& m : g_mirrors) if (m.used && m.iv && m.n && (m.s[m.n - 1].e.flags & 1)) ++mirrors;
            LOG("Pursuit: mine %s (%d cops, heat %.1f) | theirs heat %.1f, %d of their cops shown here",
                pursuit ? "yes" : "no", n0, Game::LocalPursuitHeat() / 10.f, peer.valid ? peer.s.pursuitHeat / 10.f : 0.f, mirrors);
        }
    }
    bool peerInPursuit = peer.valid && peer.s.hasCar && peer.s.pursuitHeat > 0;
    void* otherRb = RigidBodyOf(other);
    if (!pursuit || !peerInPursuit || !otherRb) { ReleaseRetarget(pursuit); return; }

    const float* op = RB_GetVec(otherRb, RB_GetPosition);
    void** cops = nullptr; int n = 0;
    Mem::Read((uintptr_t)pursuit + 0x18, cops);
    Mem::Read((uintptr_t)pursuit + 0x20, n);
    if (!cops || n <= 1) { ReleaseRetarget(pursuit); return; }    // keep a lone cop on us

    // keep the current one while it is in our pursuit and close enough
    if (g_retargetCop) {
        bool still = false;
        for (int i = 0; i < n; ++i) if (cops[i] == g_retargetCop) still = true;
        void* rb = still ? RigidBodyOf(g_retargetCop) : nullptr;
        if (!rb || Dist(RB_GetVec(rb, RB_GetPosition), op) > RELEASE_RANGE) ReleaseRetarget(pursuit);
    }
    if (!g_retargetCop) {
        float best = RETARGET_RANGE; void* pick = nullptr;
        for (int i = 0; i < n; ++i) {
            void* rb = RigidBodyOf(cops[i]);
            if (!rb) continue;
            float d = Dist(RB_GetVec(rb, RB_GetPosition), op);
            if (d < best) { best = d; pick = cops[i]; }
        }
        if (!pick) return;
        g_retargetCop = pick;
        LOG("Pursuit: cop %p (%.0f m away) goes after the other player", pick, best);
    }
    // re-apply every check: the pursuit logic may put the cop back on us
    void* t = CopTarget(g_retargetCop);
    void* sim = SimOf(other);
    void* cur = nullptr;
    if (t && sim && Mem::Read((uintptr_t)t + 0x1C, cur) && cur != sim) Aquire(t, sim);
}

static void SharedPursuitTickImpl(bool connected, const Net::Snapshot& peer);

void Game::PursuitTick(bool connected, const Net::Snapshot& peer) {
    if (!g_ok) return;
    __try { SharedPursuitTickImpl(connected, peer); }
    __except (EXCEPTION_EXECUTE_HANDLER) { LOG("Pursuit: exception in the shared pursuit"); }
    __try { PursuitTickImpl(connected, peer); }
    __except (EXCEPTION_EXECUTE_HANDLER) { g_retargetCop = nullptr; LOG("Pursuit: exception"); }
}

// Test command 'uncop': remove every police car, so a pursuit can end in an escape (test of
// [UI] SkipPursuitResults on one machine).
static int DebugKillCopsImpl(bool* inPursuit) {
    int killed = 0;
    __try {
        *inPursuit = PursuitOf(LocalVehicle()) != nullptr;
        void** b = nullptr;
        for (int i = ListSize(VL_AICOPS, &b) - 1; i >= 0; --i) {
            void* c = ListGet(VL_AICOPS, i);
            if (c && IsVehicle(c) && c != g_ghost && !IsMirrorCar(c)) { KillVehicle(c); ++killed; }   // not the other player's cops
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return killed;
}
std::string Game::DebugKillCops() {
    if (!g_ok || GameFlowState() != 6) return "not in game";
    bool inPursuit = false;
    int n = DebugKillCopsImpl(&inPursuit);
    char buf[80];
    sprintf_s(buf, "removed %d police car(s), pursuit %s", n, inPursuit ? "active" : "none");
    return buf;
}
// Test command 'pursuit': start a pursuit on the local player the way the game's scripts do
// (ForcePursuitStart, 0x60AAC0 cdecl(int minHeatLevel); needs the cop manager *0x92C67C).
static bool DebugForcePursuitImpl() {
    __try {
        void* mgr = nullptr;
        if (!Mem::Read(0x92C67C, mgr) || !mgr) return false;
        ((void(__cdecl*)(int))0x60AAC0)(1);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
std::string Game::DebugForcePursuit() {
    if (!g_ok || GameFlowState() != 6) return "not in game";
    return DebugForcePursuitImpl() ? "pursuit start requested" : "no cop manager";
}

// ---------------------------------------------------------------------------
// Shared pursuits ([Coop] SharedPursuits=1, free roam only: neither player in an event or a race)
//
//  AIPursuit (ctor 0x442D00) carries IPursuit at +0x48 (vtable 0x892770). The getters in that vtable
//  give the fields below, relative to the IPursuit* that IVehicleAI::GetPursuit returns:
//    +0xDC  busted timer (slot 24 TimeUntilBusted; slot 79 SetBustedTimerToZero writes 0)
//    +0xE8  perp busted (slot 29 IsPerpBusted)   +0xE9 bailed (slot 68 BailPursuit sets it)
//    +0xFC  evade level 0..1 (slot 7)            +0x100 cooldown time left (slot 8)
//    +0x104 cooldown time needed (slot 9 returns it minus 7)
//    +0x114 perp in sight (slot 10)              +0x118 last known position (slot 14)
//    +0x218 status: 0 chase, 1 backup, 2 cooldown, 3 busted, 4 evaded (slot 69)
//  AIPursuit::OnTask (0x443BA0, this = AIPursuit+8, so its offsets are these + 0x40) each sim tick:
//    busted timer += rate; busted when it is above 5.0 (0x890DA4) and not busted / bailed yet: then
//      +0xE8 = 1, status 3 and the arrest message (0x20D60DBF) - the game's own busted flow.
//    +0x128 "time since seen" = min(+0x128 + dt, every cop's IPursuitAI::GetTimeSinceTargetSeen
//      (IPursuitAI 0x4038E0, slot 29)); seen while it is <= 7.0 (0x890858): then +0x124 (search
//      time) = 0 and the last known position = the player's.
//    progress = +0x124 + +0x128; evade level = progress / needed; not seen for 2.5 s in the current
//      state (+0x130 > 0x890D34) starts the cooldown (+0x12C = 1, status 2); evade level >= 1 is the
//      escape (status 4); seen again during the cooldown ends it (status 0). Left = needed - progress.
//  So: enter cooldown is vanilla. Freeze / Reset write +0x124 / +0x128 (never down to the
//  "seen" limit, which would cancel cooldown). HoldEscape keeps a sliver of time left so
//  evade level stays < 1 until the other player has also finished.
//  Heat: IPerpetrator (handle 0x4037E0) on the player's car (AIPerpVehicle vtable 0x892250): slot 1
//    GetHeat 0x409400 (fld [ecx+0x1C]), slot 2 SetHeat 0x409060 (raises the heat-level events too;
//    Game_SetWorldHeat 0x612660 calls the same slot).
//  Pursuit start: ForcePursuitStart 0x60AAC0 cdecl(int minHeat) (the `pursuit` test command).
//  Minimap: Minimap::DrawCops 0x579E00 thiscall(IVehicle* player), called at 0x59DCFD. It draws
//    VEHICLE_LIST 4 (AICOPS) into 8 icons at Minimap+0xD0, only while the player is in a pursuit,
//    only cops whose IPursuitAI (slot 35) says engaged; colour flashes by Minimap+0xC0 when the cop's
//    slot 5 GetInPursuit. Our mirror cops are not in list 4 and have no IPursuitAI: we draw them into
//    the icons it left hidden (after it ran).
//  Radar detector: 0x6EDE30 thiscall(a, b) (LocalPlayer, called at 0x6F2170) walks list 4 for the
//    nearest cop within 300 m (skips helis, inactive and destroyed cars) and feeds the HUD radar
//    detector (IRadarDetector 0x565A30, its slot 1 also sets 0x91CEF8, which the sound reads). We put
//    our mirror cops into list 4 for the length of that one call.
// ---------------------------------------------------------------------------
namespace {
    constexpr uint32_t P_BustTimer = 0xDC, P_PerpBusted = 0xE8, P_Bailed = 0xE9, P_CoolLeft = 0x100,
                       P_CoolNeed = 0x104, P_Search = 0x124, P_SinceSeen = 0x128,
                       P_UnseenInState = 0x130, P_Status = 0x218;
    enum { PS_CHASE = 0, PS_BACKUP = 1, PS_COOLDOWN = 2, PS_BUSTED = 3, PS_EVADED = 4 };
    constexpr uintptr_t DrawCopsCall = 0x59DCFD, DrawCopsFn = 0x579E00;
    constexpr uintptr_t RadarCall = 0x6F2170, RadarFn = 0x6EDE30;
    // GManager (*0x91E00C)+0x2CF = AllowEngageSafehouse, written by 0x5EBA50 (recomputed each tick).
    // Vanilla: not racing && (!inPursuit || cooldown). GetIsIconVisible (0x5FDAE0) uses this byte
    // for type 8 (SAFEHOUSEINDICATOR) — world marker, minimap and world-map icons. Lua / engage
    // and Jump to Safe House read the same flag (getter 0x605660).
    constexpr uintptr_t GManagerPtr = 0x91E00C, AllowSafehouseOff = 0x2CF;
    constexpr uintptr_t WriteAllowSafehouse1 = 0x5EBAEE, WriteAllowSafehouse2 = 0x5EBAFC;
    constexpr uint32_t P_EvadeLevel = 0xFC;

    struct SharedState {
        uint16_t heat100 = 0;        // what we send (refreshed every 50 ms)
        uint8_t  flags = 0;
        uint8_t  bustSeq = 0;
        uint16_t left10 = 0;
        SharedPursuit::HeatTracker heat;
        SharedPursuit::BustTracker bust;
        void*    lastBustedPursuit = nullptr;
        DWORD    bustingForPeerAt = 0;      // we bust ourselves because they were busted: not reported back
        DWORD    pendingBustUntil = 0;
        bool     pendingStarted = false;
        int      lastStatus = -2;
        int      lastCool = -1;
        bool     peerWasCooling = false;   // previous tick, for Reset vs Freeze
        bool     haveFreeze = false;
        float    freezeSearch = 0.f, freezeSince = 0.f;
        DWORD    lastCopHit = 0;
        int      radarCops = -1, mapCops = -1;
        bool     sentEvaded = false;     // keep PURSUIT_EVADED on the wire after the pursuit object is gone
        bool     hideSafehouse = false;
        uint32_t lastEvent = 0;          // our event: a new one clears an old escape signal
        bool     peerChasedInEvent = false;   // their pursuit was live in this same event
    } g_sp;

    // 0x5EBA50 writes [esi+0x2CF] = al at these two sites. When hideSafehouse is set, store 0 instead.
    void __declspec(naked) hk_WriteAllowSafehouse() {
        __asm {
            cmp byte ptr [g_sp.hideSafehouse], 0
            je write
            xor al, al
        write:
            mov byte ptr [esi + 0x2CF], al
            ret
        }
    }
    bool PatchAllowSafehouseWrite(uintptr_t at) {
        const uint8_t expect[6] = { 0x88, 0x86, 0xCF, 0x02, 0x00, 0x00 };   // mov [esi+0x2CF], al
        uint8_t bytes[6] = { 0xE8, 0, 0, 0, 0, 0x90 };                     // call hook; nop
        int32_t rel = (int32_t)((uintptr_t)&hk_WriteAllowSafehouse - (at + 5));
        memcpy(bytes + 1, &rel, 4);
        return Mem::PatchBytes(at, expect, bytes, 6);
    }
    void ApplySafehouseFlag(bool hide) {
        g_sp.hideSafehouse = hide;
        void* gm = nullptr;
        if (!Mem::Read(GManagerPtr, gm) || !gm) return;
        uint8_t cur = 1;
        if (!Mem::Read((uintptr_t)gm + AllowSafehouseOff, cur)) return;
        if (hide && cur) Mem::Write((uintptr_t)gm + AllowSafehouseOff, (uint8_t)0);
    }
    template <typename T> T PRead(void* p, uint32_t off) { T v{}; Mem::Read((uintptr_t)p + off, v); return v; }
    template <typename T> void PWrite(void* p, uint32_t off, T v) { Mem::Write((uintptr_t)p + off, v); }
    using namespace SharedPursuit;

    void ForceLocalEscape(void* pursuit) {
        if (!pursuit) return;
        PWrite<float>(pursuit, P_EvadeLevel, 1.f);
        int st = PRead<int>(pursuit, P_Status);
        if (st == PS_COOLDOWN) {
            float search = PRead<float>(pursuit, P_Search), since = PRead<float>(pursuit, P_SinceSeen);
            float need = PRead<float>(pursuit, P_CoolNeed);
            CompleteCooldown(need, search, since);
            PWrite<float>(pursuit, P_Search, search);
            PWrite<float>(pursuit, P_SinceSeen, since);
            return;
        }
        if (st == PS_EVADED || st == PS_BUSTED) return;
        PWrite<int>(pursuit, P_Status, PS_EVADED);
        __try {
            void* vt = nullptr, *fn = nullptr;
            if (Mem::Read((uintptr_t)pursuit, vt) && vt && Mem::Read((uintptr_t)vt + 0x8C, fn) && fn)
                ((bool(__thiscall*)(void*))fn)(pursuit);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    bool FreeRoamHere() {
        if (Game::GameFlowState() != 6 || Game::CurrentEventHash() != 0) return false;
        void* rs = nullptr; int mode = 0;
        return !(Mem::Read(0x91E000, rs) && rs && Mem::Read((uintptr_t)rs + 0x1960, mode) && mode == 1);
    }
    bool PeerInFreeRoam(const Net::Snapshot& p) {
        return p.valid && p.s.hasCar && p.s.gameFlowState == 6 && p.s.eventHash == 0 && !p.s.inRace;
    }
    // Free roam on both sides, or both in the same event (see SharedPursuit::SharedPlace)
    SharedPursuit::Place SharedPlaceNow(const Net::Snapshot& p) {
        SharedPursuit::Where me{ Game::GameFlowState() == 6, Game::CurrentEventHash(), !FreeRoamHere() && Game::CurrentEventHash() == 0 };
        SharedPursuit::Where them{ p.valid && p.s.hasCar && p.s.gameFlowState == 6, p.valid ? p.s.eventHash : 0u, p.valid && p.s.inRace != 0 };
        return SharedPursuit::SharedPlace(me, them);
    }
    bool VehActive(void* iv) { return VCall<bool(__thiscall*)(void*)>(iv, 34)(iv); }        // 0x688200
    bool VehIsHeli(void* iv) {                                                                // class == *0x92C4F4
        const uint32_t* c = VCall<const uint32_t* (__thiscall*)(void*)>(iv, 17)(iv);         // 0x688080
        uint32_t cls = 0, heli = 0;
        return c && Mem::Read((uintptr_t)c, cls) && Mem::Read(0x92C4F4, heli) && cls == heli;
    }
    // A cop of OUR pursuit saw us in the last 7 s (the game's own rule, without any co-op hold)
    bool SeenByOwnCops(void* pursuit) {
        void** cops = nullptr; int n = 0;
        if (!Mem::Read((uintptr_t)pursuit + 0x18, cops) || !Mem::Read((uintptr_t)pursuit + 0x20, n) || !cops || n <= 0 || n > 64) return false;
        for (int i = 0; i < n; ++i) {
            void* c = nullptr;
            if (!Mem::Read((uintptr_t)&cops[i], c) || !IsVehicle(c) || !VehActive(c)) continue;
            if (VCall<bool(__thiscall*)(void*)>(c, 31)(c)) continue;                         // destroyed
            void* pai = Find(c, 0x4038E0);
            if (pai && VCall<float(__thiscall*)(void*)>(pai, 29)(pai) <= SharedPursuit::SEEN_LIMIT_S) return true;
        }
        return false;
    }
    const char* StatusName(int s) {
        switch (s) { case -1: return "none"; case PS_CHASE: return "chase"; case PS_BACKUP: return "backup";
                     case PS_COOLDOWN: return "cooldown"; case PS_BUSTED: return "busted"; case PS_EVADED: return "escaped"; }
        return "?";
    }
    // a mirror of one of the other player's cops (spawned here, driven by their snapshots)
    bool IsPeerCop(const Mirror& m, bool inPursuitOnly) {
        if (!m.used || !m.iv || !m.n || m.racer || m.bound) return false;
        uint16_t f = m.s[m.n - 1].e.flags;
        return (f & 1) && (!inPursuitOnly || (f & 4)) && IsVehicle(m.iv);
    }
    // Local interpolated positions only (not the owner's remote coords, not a traffic id).
    bool PeerCopsSeeUs() {
        void* me = LocalVehicle();
        void* rb = me ? RigidBodyOf(me) : nullptr;
        if (!rb) return false;
        const float* mp = RB_GetVec(rb, RB_GetPosition);
        for (auto& m : g_mirrors) {
            if (!IsPeerCop(m, true)) continue;
            void* crb = RigidBodyOf(m.iv);
            if (!crb) continue;
            if (SharedPursuit::PeerCopSeesPlayer(true, true, RB_GetVec(crb, RB_GetPosition), mp)) return true;
        }
        return false;
    }
}

// ---- 7. cost to state / bounty: both players get credit (see SharedPursuit::TallyShare) ----
//  IPursuit counters (only ever raised by the game; slot 56 0x40AA00 prices them into Cost to State):
//    +0x13C cops immobilized (slot 38; also GManager stat "cops_destroyed_in_pursuit")  +0x154 roadblocks
//    dodged (slot 58, stat "roadblocks_dodged")  +0x15C cops damaged (slot 60, "cops_damaged")  +0x16C spike
//    strips dodged (slot 63, "tire_spikes_dodged")  +0x174 property damage $ (slot 61, via IPerpetrator
//    slot 6 0x409410 which also adds to the perp's cost +0x20)  +0x158 +0x168 +0x17C +0x180 +0x184 +0x188
//    the other priced counters (slots 59, 62, 64..67). The rap sheet (0x580170) reads them at the end.
//  IPerpetrator bounty: +0x28 (slot 14 0x4094C0 adds) and +0x24 (slot 15 0x409480 adds); both are
//    "bounty_in_pursuit" (0x601400). Not shared: helicopters deployed +0x150 / units +0x138 (the
//    game sets those itself) and infractions (personal tickets).
namespace {
    constexpr uint32_t TallyOff[SharedPursuit::T_COUNT] = {
        0x13C, 0x154, 0x158, 0x15C, 0x168, 0x16C, 0x174, 0x17C, 0x180, 0x184, 0x188, 0x28, 0x24 };
    constexpr uintptr_t GM_SetStat = 0x5EDA20, GM_GetStat = 0x5EDBC0;
    constexpr uintptr_t S_CopsDestroyedInPursuit = 0x891120, S_CopsDestroyed = 0x892EF0, S_Roadblocks = 0x892B98,
                        S_Spikes = 0x892B7C, S_CopsDamaged = 0x890EB0;
    SharedPursuit::TallyShare g_tally;
    uint32_t g_bustedEvent = 0;      // we were busted in this run of this event (coopwin.inl clears it)
    uint32_t g_tallyId = 0, g_tallyPeerCount = 0;
    void*    g_tallyPursuit = nullptr;
    DWORD    g_tallyNext = 0, g_tallySendUntil = 0;

    bool TallyRead(void* pursuit, void* perp, int32_t* v) {
        for (int i = 0; i < SharedPursuit::T_COUNT; ++i) {
            void* base = i >= SharedPursuit::T_BOUNTY28 ? perp : pursuit;
            v[i] = 0;
            if (base && !Mem::Read((uintptr_t)base + TallyOff[i], v[i])) return false;
            if (v[i] < 0) v[i] = 0;
        }
        return true;
    }
    void SetStat(uintptr_t name, float v) {
        void* gm = nullptr;
        if (Mem::Read(GManagerPtr, gm) && gm) ((void(__thiscall*)(void*, const char*, float))GM_SetStat)(gm, (const char*)name, v);
    }
    // only a stat the game already keeps in this context (an unset one reads -1)
    void AddStatIfSet(uintptr_t name, int add) {
        void* gm = nullptr;
        if (add <= 0 || !Mem::Read(GManagerPtr, gm) || !gm) return;
        float cur = ((float(__thiscall*)(void*, const char*))GM_GetStat)(gm, (const char*)name);
        if (cur >= 0.f) SetStat(name, cur + (float)add);
    }
    void TallyApply(void* pursuit, void* perp, const int32_t* add) {
        using namespace SharedPursuit;
        for (int i = 0; i < T_COUNT; ++i) {
            if (add[i] <= 0) continue;
            if (i == T_PROPERTY) {
                int32_t before = PRead<int32_t>(pursuit, TallyOff[i]);
                if (perp) VCall<void(__thiscall*)(void*, int)>(perp, 6)(perp, add[i]);   // the game's own path
                if (PRead<int32_t>(pursuit, TallyOff[i]) <= before)                     // not counted there: direct
                    PWrite<int32_t>(pursuit, TallyOff[i], before + add[i]);
            } else if (i == T_BOUNTY28) {
                if (perp) VCall<void(__thiscall*)(void*, int)>(perp, 14)(perp, add[i]);  // the game's gates apply
            } else if (i == T_BOUNTY24) {
                if (perp) VCall<void(__thiscall*)(void*, int)>(perp, 15)(perp, add[i]);
            } else {
                PWrite<int32_t>(pursuit, TallyOff[i], PRead<int32_t>(pursuit, TallyOff[i]) + add[i]);
            }
        }
        if (add[T_COPS_DESTROYED] > 0) {
            SetStat(S_CopsDestroyedInPursuit, (float)PRead<int32_t>(pursuit, TallyOff[T_COPS_DESTROYED]));
            AddStatIfSet(S_CopsDestroyed, add[T_COPS_DESTROYED]);
        }
        AddStatIfSet(S_Roadblocks, add[T_ROADBLOCKS]);
        AddStatIfSet(S_Spikes, add[T_SPIKES]);
        AddStatIfSet(S_CopsDamaged, add[T_COPS_DAMAGED]);
    }
}
// live: our pursuit runs (not busted / escaped). joint: theirs runs too, in the same place.
static void SharedTallyTick(void* pursuit, void* perp, bool live, bool joint) {
    using namespace SharedPursuit;
    DWORD now = GetTickCount();
    if ((int)(now - g_tallyNext) < 0) return;
    g_tallyNext = now + 250;
    int32_t cur[T_COUNT];
    if (live && pursuit != g_tallyPursuit) {
        g_tallyPursuit = pursuit;
        ++g_tallyId;
        if (TallyRead(pursuit, perp, cur)) TallyStart(g_tally, cur);
    }
    if (!live) {
        if (g_tally.live) { TallyStop(g_tally); g_tallySendUntil = now + 3000; }
        g_tallyPursuit = nullptr;
    } else if (TallyRead(pursuit, perp, cur)) {
        TallyObserve(g_tally, cur);
    }
    uint32_t pid = 0, cnt = 0; int32_t theirs[T_COUNT];
    if (Net::PeerPursuitStats(pid, theirs, cnt) && cnt != g_tallyPeerCount) {
        g_tallyPeerCount = cnt;
        TallyPeer(g_tally, pid, theirs, joint);
    }
    int32_t add[T_COUNT];
    if (live && TallyToAdd(g_tally, add)) {
        TallyApply(pursuit, perp, add);
        if (TallyRead(pursuit, perp, cur)) TallyGave(g_tally, add, cur);
        int cost = VCall<int(__thiscall*)(void*)>(pursuit, 56)(pursuit);
        LOG("Pursuit: credit from the other player - cops immobilized +%d, damaged +%d, roadblocks +%d, spikes +%d, property $%d, "
            "other $%lld, bounty +%d (cost to state here now $%d)",
            add[T_COPS_DESTROYED], add[T_COPS_DAMAGED], add[T_ROADBLOCKS], add[T_SPIKES], add[T_PROPERTY],
            (long long)(TallyCost(add) - 5000LL * add[T_COPS_DESTROYED] - 250LL * add[T_COPS_DAMAGED] - add[T_PROPERTY]),
            add[T_BOUNTY28] + add[T_BOUNTY24], cost);
    }
    static DWORD nextSend = 0;
    if ((g_tally.live || (int)(g_tallySendUntil - now) > 0) && (int)(now - nextSend) >= 0) {
        nextSend = now + 500;
        Net::SendPursuitStats(g_tallyId, g_tally.own);
    }
}

static void SharedPursuitTickImpl(bool connected, const Net::Snapshot& peer) {
    using namespace SharedPursuit;
    DWORD now = GetTickCount();
    static DWORD next = 0;
    if ((int)(now - next) < 0) return;
    next = now + 50;
    // (in the prologue only while both are on the same step of its chain: a pursuit there must not pull a
    // player who is in another scripted part of it)
    Game::PrologueState pst = Game::PrologueNow();
    bool on = g_cfg.sharedPursuits && connected && g_coopLinked && Prologue::SameStep(pst.inPrologue, pst.step, PeerPrologue());
    // Free roam: the whole shared pursuit. Both in the same event (Challenge Series, a career pursuit
    // event or race): only the cooldown / escape together (sections 5 and the status we send).
    SharedPursuit::Place place = on ? SharedPlaceNow(peer) : SharedPursuit::Place::None;
    bool freeRoam = on && FreeRoamHere();
    bool sameEvent = place == SharedPursuit::Place::SameEvent;
    bool here = freeRoam || sameEvent;
    // A Challenge Series pursuit (race type 8) is open-world driving with cops and no opponents: there
    // the whole pursuit is shared as in free roam too (heat, joining their chase, their cops spotting
    // us), except the safehouse.
    bool pcShared = false;
    if (sameEvent) {
        void* rs = nullptr; void* parms = nullptr;
        if (Mem::Read(0x91E000, rs) && rs && Mem::Read((uintptr_t)rs + 0x1968, parms) && parms)
            pcShared = ((int(__thiscall*)(void*))0x5FAA20)(parms) == CoopRace::kTypePursuitChallenge;
    }
    bool openWorld = freeRoam || pcShared;
    {
        static SharedPursuit::Place lastPlace = SharedPursuit::Place::None;
        if (place != lastPlace && on) {
            if (place == SharedPursuit::Place::SameEvent) LOG("Pursuit: both in event %08X - %s", Game::CurrentEventHash(),
                                                             pcShared ? "pursuit challenge: heat, chase, cooldown, escape and cost to state are shared"
                                                                      : "cooldown, escape and cost to state are shared");
            else if (lastPlace == SharedPursuit::Place::SameEvent) LOG("Pursuit: no longer in the same event");
        }
        lastPlace = place;
    }
    // A pursuit in a race must still be able to spawn cops next to the other player: this flag is
    // the local pursuit, not "shared free-roam pursuit is active".
    void* meAll = LocalVehicle();
    void* pursuitAll = meAll ? PursuitOf(meAll) : nullptr;
    g_localChased = pursuitAll != nullptr;
    if (pursuitAll && (PRead<uint8_t>(pursuitAll, P_PerpBusted) || PRead<int>(pursuitAll, P_Status) == PS_BUSTED))
        if (uint32_t ev = Game::CurrentEventHash()) g_bustedEvent = ev;   // a pursuit challenge run is lost
    void* me = here ? meAll : nullptr;
    void* pursuit = here ? pursuitAll : nullptr;

    // ---- the other player is chased: so are we, wherever we are (one shared pursuit) ----
    // Only a pursuit that has just started there pulls us in (within 30 s of its start), and never right after
    // our own pursuit ended: at an escape the two games end a moment apart, and the later one must not drag
    // the earlier one back into a chase.
    static bool peerWasActive = false; static DWORD peerStartAt = 0, ownEndAt = 0; static bool hadOwn = false;
    bool peerActive = peer.valid && (peer.s.pursuitFlags & PURSUIT_ACTIVE) && peer.s.gameFlowState == 6 &&
                      (!peer.s.eventHash || (pcShared && peer.s.eventHash == Game::CurrentEventHash()));
    if (peerActive && !peerWasActive) peerStartAt = now;
    peerWasActive = peerActive;
    if (hadOwn && !pursuit) ownEndAt = now;
    hadOwn = pursuit != nullptr;
    if (me && openWorld && !pursuit && peerActive && now - peerStartAt < 30000 && (!ownEndAt || now - ownEndAt > 30000)) {
        static DWORD nextJoin = 0;
        if ((int)(now - nextJoin) >= 0) {
            nextJoin = now + 20000;   // if the game starts none (no cops around yet), try again later
            bool ok = DebugForcePursuitImpl();
            LOG("Pursuit: the other player is chased - %s", ok ? "starting the same pursuit on us" : "no cop manager, no pursuit");
        }
    }
    // ---- the other player's chasing cops spot us too: one of them close to our car starts a pursuit ----
    if (me && !pursuit && openWorld) {
        static DWORD nextSpot = 0;
        void* myRb = RigidBodyOf(me);
        if (myRb && (int)(now - nextSpot) >= 0) {
            const float* mp = RB_GetVec(myRb, RB_GetPosition);
            for (auto& m : g_mirrors) {
                if (!m.used || !m.iv || !m.n || !(m.s[m.n - 1].e.flags & 4)) continue;   // a cop in a pursuit there
                void* rb = RigidBodyOf(m.iv);
                if (!rb) continue;
                float d = Dist(RB_GetVec(rb, RB_GetPosition), mp);
                if (d < 45.f) {
                    nextSpot = now + 15000;
                    bool ok = DebugForcePursuitImpl();
                    LOG("Pursuit: the other player's cop %u spotted us (%.0f m) - %s", (unsigned)m.id, d, ok ? "starting a pursuit on us" : "no cop manager");
                    break;
                }
            }
        }
    }

    // ---- our own pursuit, as the other game sees it ----
    g_sp.heat100 = 0; g_sp.flags = 0; g_sp.left10 = 0;
    {   // a new event (or back to free roam): an escape from before is not news there
        uint32_t ev = Game::CurrentEventHash();
        if (ev != g_sp.lastEvent) { g_sp.lastEvent = ev; g_sp.sentEvaded = false; g_sp.peerChasedInEvent = false; }
    }
    bool busted = false, bailed = false;
    int status = -1;
    float left = 0.f;
    if (pursuit) {
        busted = PRead<uint8_t>(pursuit, P_PerpBusted) != 0;
        bailed = PRead<uint8_t>(pursuit, P_Bailed) != 0;
        status = PRead<int>(pursuit, P_Status);
        left = PRead<float>(pursuit, P_CoolLeft);
        if (!busted && !bailed && status != PS_BUSTED && status != PS_EVADED) {
            g_sp.flags |= PURSUIT_ACTIVE;
            // evade bar / seen: local cops only (vanilla). Mirrored peer cops must not fill or empty it.
            if (SeenByOwnCops(pursuit)) g_sp.flags |= PURSUIT_SEEN;
            if (status == PS_COOLDOWN) g_sp.flags |= PURSUIT_COOLDOWN;
            float unseen = PRead<float>(pursuit, P_UnseenInState);
            if (EvadeBarFull((g_sp.flags & PURSUIT_SEEN) != 0, status == PS_COOLDOWN, unseen))
                g_sp.flags |= PURSUIT_READY;
            g_sp.left10 = LeftToWire(left);
        }
        if (busted) g_sp.flags |= PURSUIT_BUSTED;
        if (status == PS_EVADED) { g_sp.flags |= PURSUIT_EVADED; g_sp.sentEvaded = true; }
        else if (status != PS_BUSTED && (g_sp.flags & PURSUIT_ACTIVE)) g_sp.sentEvaded = false;
    } else if (g_sp.sentEvaded) {
        g_sp.flags |= PURSUIT_EVADED;   // pursuit object already gone: keep the escape signal on the wire
    }
    if (status != g_sp.lastStatus) {
        if (on && (status >= 0 || g_sp.lastStatus >= 0))
            LOG("Pursuit: ours %s -> %s", StatusName(g_sp.lastStatus < -1 ? -1 : g_sp.lastStatus), StatusName(status));
        g_sp.lastStatus = status;
    }
    // ---- 4. busted: count our own busts (personal; the other game is not busted with us) ----
    if (!pursuit || !busted) g_sp.lastBustedPursuit = nullptr;   // the next pursuit may get the same address
    if (pursuit && busted && pursuit != g_sp.lastBustedPursuit) {
        g_sp.lastBustedPursuit = pursuit;
        g_sp.pendingBustUntil = 0;
        if (g_sp.bustingForPeerAt && now - g_sp.bustingForPeerAt < 15000)
            LOG("Pursuit: busted here too, as the other player was");
        else if (on) {
            ++g_sp.bustSeq;
            LOG("Pursuit: we were busted (count %u) - busts are personal, the other player is not busted with us", (unsigned)g_sp.bustSeq);
        }
        g_sp.bustingForPeerAt = 0;
    }
    if (!on) {
        g_sp.heat = {}; g_sp.bust = {}; g_sp.pendingBustUntil = 0; g_sp.lastCool = -1;
        g_sp.sentEvaded = false;
        if (g_tally.live) SharedPursuit::TallyStop(g_tally);
        g_tallyPursuit = nullptr;
        ApplySafehouseFlag(false);
        return;
    }
    bool peerHere = freeRoam ? PeerInFreeRoam(peer) : sameEvent;

    // ---- 1. heat: the hotter of the two, both ways ----
    void* perp = (me && openWorld && !NisActive()) ? Find(me, IPerpetratorHandle) : nullptr;
    if (perp) {
        float mine = VCall<float(__thiscall*)(void*)>(perp, 1)(perp);
        g_sp.heat100 = HeatToWire(mine);
        float take = peerHere ? HeatToTake(g_sp.heat, mine, peer.s.heat100) : 0.f;
        if (!peerHere) g_sp.heat = {};
        if (take > 0.f) {
            VCall<void(__thiscall*)(void*, float)>(perp, 2)(perp, take);
            g_sp.heat100 = HeatToWire(take);
            LOG("Pursuit: heat %.2f -> %.2f (the other player's heat)", mine, take);
        }
    } else g_sp.heat = {};

    // ---- 4. the other player was busted: personal, we are not busted with them ----
    if (peer.valid && PeerJustBusted(g_sp.bust, peer.s.bustSeq))
        LOG("Pursuit: the other player was busted - busts are personal, ours continues");
    g_sp.pendingBustUntil = 0;   // 1.7.1: do not start or continue a shared bust

    // ---- 5. cooldown: enter immediately; freeze / reset the meter; escape only when both finish ----
    // peer.evaded is honoured even if they already left free roam (they entered a safehouse).
    int coolNow = -1;
    View mine{};
    mine.inPursuit = (g_sp.flags & PURSUIT_ACTIVE) != 0;
    mine.seen = (g_sp.flags & PURSUIT_SEEN) != 0;
    mine.cooling = status == PS_COOLDOWN;
    mine.left = left;
    mine.ready = (g_sp.flags & PURSUIT_READY) != 0;
    mine.busted = busted;
    View theirs = peer.valid ? FromWire(peer.s.pursuitFlags, peer.s.coolLeft10) : View{};
    // In an event their escape counts only once their pursuit was live in this event (an older
    // escape signal must not end ours - that would win the event for us).
    if (sameEvent && theirs.inPursuit) g_sp.peerChasedInEvent = true;
    if (!sameEvent) g_sp.peerChasedInEvent = false;
    bool peerEvadedHere = peer.valid && PeerEscapeCounts(place, theirs.evaded, g_sp.peerChasedInEvent);
    if (!peerEvadedHere) theirs.evaded = false;
    bool peerForCool = peerHere || peerEvadedHere;
    if ((g_sp.flags & PURSUIT_ACTIVE) && peerForCool) {
        bool was = g_sp.peerWasCooling;
        Decision d = DecideCooldown(mine, theirs, true, was);
        if (peerHere) g_sp.peerWasCooling = theirs.cooling;
        coolNow = (int)d.what;
        if (d.what == Cool::ForceEscape) {
            g_sp.haveFreeze = false;
            ForceLocalEscape(pursuit);
            if (coolNow != g_sp.lastCool) LOG("Pursuit: the other player evaded (safehouse) - ending ours as an escape");
        } else if (pursuit && status == PS_COOLDOWN && !mine.seen &&
            (d.what == Cool::Freeze || d.what == Cool::Reset || d.what == Cool::HoldEscape)) {
            float search = PRead<float>(pursuit, P_Search), since = PRead<float>(pursuit, P_SinceSeen);
            float need = PRead<float>(pursuit, P_CoolNeed);
            if (d.what == Cool::Reset) {
                ResetCooldownProgress(search, since);
                g_sp.haveFreeze = true; g_sp.freezeSearch = search; g_sp.freezeSince = since;
            } else if (d.what == Cool::Freeze) {
                if (!g_sp.haveFreeze) {
                    if (since < COOLDOWN_FLOOR_S) since = COOLDOWN_FLOOR_S;
                    g_sp.haveFreeze = true; g_sp.freezeSearch = search; g_sp.freezeSince = since;
                }
                FreezeProgress(g_sp.freezeSearch, g_sp.freezeSince, search, since);
            } else {
                g_sp.haveFreeze = false;
                DelayProgress(need, d.left > 0.f ? d.left : ESCAPE_HOLD_LEFT_S, search, since);
            }
            PWrite<float>(pursuit, P_Search, search);
            PWrite<float>(pursuit, P_SinceSeen, since);
        } else {
            g_sp.haveFreeze = false;
        }
        if (coolNow != g_sp.lastCool) {
            if (d.what == Cool::Freeze) LOG("Pursuit: in cooldown - meter frozen until the other player is in cooldown too");
            else if (d.what == Cool::Reset) LOG("Pursuit: the other player was spotted - we stay in cooldown, meter reset and frozen");
            else if (d.what == Cool::HoldEscape) LOG("Pursuit: our cooldown is finished - waiting for the other player to finish before we escape");
            else if (d.what != Cool::ForceEscape && theirs.inPursuit) LOG("Pursuit: cooldown runs as usual (%s here, %s there)",
                                           mine.seen ? "seen" : (mine.cooling ? "in cooldown" : "hiding"),
                                           theirs.seen ? "seen" : (theirs.cooling ? "in cooldown" : "hiding"));
        }
    } else {
        g_sp.haveFreeze = false;
        if (!peerHere) g_sp.peerWasCooling = false;
    }
    g_sp.lastCool = coolNow;

    // ---- 7. cost to state / bounty: what either of you earned counts for both ----
    {
        bool live = pursuit && (g_sp.flags & PURSUIT_ACTIVE) && !busted;
        bool joint = live && peerHere && theirs.inPursuit && !theirs.busted;
        void* perpAll = (pursuit && me) ? Find(me, IPerpetratorHandle) : nullptr;
        SharedTallyTick(pursuit, perpAll, live, joint);
    }

    // ---- 6. safehouse markers / entry: hidden until both are in cooldown (solo if they disconnect) ----
    ApplySafehouseFlag(freeRoam && HideSafehouse(mine, theirs, peerHere));

    // status line every 5 s while either of you is chased
    static DWORD nextLog = 0;
    if ((pursuit || (peer.valid && (peer.s.pursuitFlags & PURSUIT_ACTIVE))) && (int)(now - nextLog) > 0) {
        nextLog = now + 5000;
        uint8_t pf = peer.valid ? peer.s.pursuitFlags : 0;
        LOG("Pursuit: shared - here %s%s, %.1f s left, heat %.2f | there %s%s, %.1f s left, heat %.2f",
            StatusName(status), (g_sp.flags & PURSUIT_SEEN) ? " seen" : "", left, g_sp.heat100 / 100.f,
            (pf & PURSUIT_ACTIVE) ? ((pf & PURSUIT_COOLDOWN) ? "cooldown" : "chase") : (pf & PURSUIT_BUSTED) ? "busted" : "none",
            (pf & PURSUIT_SEEN) ? " seen" : "", peer.valid ? peer.s.coolLeft10 / 10.f : 0.f, peer.valid ? peer.s.heat100 / 100.f : 0.f);
    }
}

// ---- 3. we rammed one of the other player's cops (traffic.inl MirrorHitTick) ----
static void SharedPursuitCopHit(uint16_t mirrorId) {
    if (!g_cfg.sharedPursuits || !g_coopLinked || !FreeRoamHere()) return;
    DWORD now = GetTickCount();
    if (now - g_sp.lastCopHit < 3000) return;
    g_sp.lastCopHit = now;
    if (PursuitOf(LocalVehicle())) { LOG("Pursuit: hit the other player's cop %u - we are already chased", (unsigned)mirrorId); return; }
    bool ok = DebugForcePursuitImpl();
    LOG("Pursuit: hit the other player's cop %u - %s", (unsigned)mirrorId, ok ? "starting a pursuit on us" : "no cop manager, no pursuit");
}

void Game::FillSharedPursuit(CarState& s) {
    s.heat100 = g_sp.heat100;
    s.pursuitFlags = g_sp.flags;
    s.bustSeq = g_sp.bustSeq;
    s.coolLeft10 = g_sp.left10;
}

// ---- 2. the other player's cops on the minimap and the radar detector ----
static void DrawPeerCopsImpl(void* mm, void* playerIv) {
    // which of the 8 cop icons did the game's DrawCops leave free? (the same walk it does)
    uint8_t showAll = 0; Mem::Read(0x91CF00, showAll);
    void* p = PursuitOf(playerIv);
    bool none = !showAll && (!p || VCall<bool(__thiscall*)(void*)>(p, 11)(p));          // IsPursuitBailed
    int slot = 0;
    if (!none) {
        void** b = nullptr;
        int n = ListSize(VL_AICOPS, &b);
        for (int i = 0; i < n && slot < 8; ++i) {
            void* c = ListGet(VL_AICOPS, i);
            if (!IsVehicle(c) || !VehActive(c) || VehIsHeli(c)) continue;
            ++slot;
        }
    }
    int counter = 0; Mem::Read((uintptr_t)mm + 0xC0, counter);
    int drawn = 0;
    for (auto& m : g_mirrors) {
        if (slot >= 8) break;
        if (!IsPeerCop(m, true) || !VehActive(m.iv) || VehIsHeli(m.iv)) continue;
        void* sim = VCall<void* (__thiscall*)(void*)>(m.iv, IV_GetSimable)(m.iv);
        void* icon = nullptr;
        if (!sim || !Mem::Read((uintptr_t)mm + 0xD0 + slot * 4, icon) || !icon) continue;
        float pos[2], dir[2];
        ((void(__cdecl*)(float*, float*, void*))0x579840)(pos, dir, sim);
        ((void(__thiscall*)(void*, float*, float*, void*, bool))0x5675D0)(mm, pos, dir, icon, false);
        uint32_t col = counter <= 2 ? 0xFF0000FF : (counter >= 4 && counter <= 6) ? 0xFFA00000 : 0xFFCCCCCC;   // as a chasing cop
        ((void(__cdecl*)(void*, uint32_t))0x5157E0)(icon, col);
        ++slot; ++drawn;
    }
    if (drawn != g_sp.mapCops) { g_sp.mapCops = drawn; LOG("Minimap: %d of the other player's cops shown", drawn); }
}
void __fastcall hk_DrawCops(void* mm, void*, void* playerIv) {
    ((void(__thiscall*)(void*, void*))DrawCopsFn)(mm, playerIv);
    if (!g_cfg.sharedPursuits || !g_coopLinked || !FreeRoamHere()) return;
    __try { DrawPeerCopsImpl(mm, playerIv); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Our mirror cops into VEHICLE_LIST 4 (behind the game's own entries, within its capacity) for one call
static int AddPeerCopsToList(uint32_t* oldSize, void** saved) {
    __try {
        uintptr_t bucket = Addr::VehicleLists + VL_AICOPS * Addr::VehicleBucket;
        void** b = nullptr; uint32_t cap = 0, size = 0;
        if (!Mem::Read(bucket + 4, b) || !Mem::Read(bucket + 8, cap) || !Mem::Read(bucket + 0xC, size) || !b || size > cap || cap > 64) return 0;
        int n = 0;
        for (auto& m : g_mirrors) {
            if (n >= 8 || size + n >= cap) break;
            if (!IsPeerCop(m, false) || ListContainsOne(VL_AICOPS, m.iv)) continue;
            saved[n] = b[size + n];
            b[size + n] = m.iv;
            ++n;
        }
        if (n) { *oldSize = size; uint32_t ns = size + n; Mem::Write(bucket + 0xC, ns); }
        return n;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static void RemovePeerCopsFromList(uint32_t oldSize, int n, void** saved) {
    __try {
        uintptr_t bucket = Addr::VehicleLists + VL_AICOPS * Addr::VehicleBucket;
        void** b = nullptr; uint32_t size = 0;
        if (!Mem::Read(bucket + 4, b) || !Mem::Read(bucket + 0xC, size) || !b) return;
        if (size == oldSize + n) {
            for (int i = 0; i < n; ++i) b[oldSize + i] = saved[i];
            Mem::Write(bucket + 0xC, oldSize);
        } else LOG("Radar: the cop list changed during the radar update (%u -> %u) - left as it is", oldSize + n, size);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
void __fastcall hk_RadarDetector(void* self, void*, uint32_t a, uint32_t b) {
    uint32_t oldSize = 0; void* saved[8] = {};
    int added = (g_cfg.sharedPursuits && g_coopLinked && FreeRoamHere()) ? AddPeerCopsToList(&oldSize, saved) : 0;
    ((void(__thiscall*)(void*, uint32_t, uint32_t))RadarFn)(self, a, b);
    if (added) RemovePeerCopsFromList(oldSize, added, saved);
    if (added != g_sp.radarCops && (added || g_sp.radarCops > 0)) {
        g_sp.radarCops = added;
        LOG("Radar: %d of the other player's cops in the radar detector", added);
    }
}

bool Game::InstallPursuitHooks() {
    if (!g_cfg.sharedPursuits) { LOG("Pursuit: SharedPursuits=0 - each player has their own heat and pursuit"); return false; }
    bool m = PatchCallChecked(DrawCopsCall, DrawCopsFn, (void*)&hk_DrawCops);
    bool r = PatchCallChecked(RadarCall, RadarFn, (void*)&hk_RadarDetector);
    bool s1 = PatchAllowSafehouseWrite(WriteAllowSafehouse1);
    bool s2 = PatchAllowSafehouseWrite(WriteAllowSafehouse2);
    LOG("Pursuit: shared pursuits on (minimap cops %s, radar detector %s, safehouse flag %s/%s)",
        m ? "hooked" : "NOT hooked", r ? "hooked" : "NOT hooked", s1 ? "ok" : "FAIL", s2 ? "ok" : "FAIL");
    return m && r && s1 && s2;
}
