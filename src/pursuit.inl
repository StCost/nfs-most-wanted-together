// Included from game.cpp (after traffic.inl).
// ---------------------------------------------------------------------------
// Pursuits
//  Every player has their own heat and their own pursuit (the game runs it locally); the other
//  player's pursuit cops are shown as mirrors (traffic.inl sends AICOPS both ways).
//  When both players are in a pursuit, every cop of OUR pursuit that is clearly closer to the other
//  player's car than to ours switches its own target to that car (AITarget::Aquire) and rams it; at
//  least one cop always stays on us. Heat, busting and evading still track the local player only: a
//  cop chasing the other player does not count as seeing us (hook in AIPursuit::OnTask, below).
//  Patrol cops: near the host the client spawns none of its own, so the patrol cops around both
//  players belong to one game and the game's own spotting only knows that game's player. Each game
//  therefore lets the other player's patrol cops (the copies shown here) spot its own player:
//  speeding close past one of them starts a pursuit here (SharedPursuit::PatrolCopSpotsPlayer).
//
//  IVehicle vt+0xAC GetAIVehiclePtr -> IVehicleAI;  IVehicleAI vt+0xA0 GetPursuit -> IPursuit (or null)
//  IVehicleAI vt+0x64 GetTarget -> AITarget (+0x1C ISimable*, +0x2C valid)
//  IPursuit vt+0x08 GetTarget -> AITarget;  cops: IVehicle** at +0x18, count at +0x20
//  AITarget::Aquire(ISimable*) 0x423860 (thiscall). IPerpetrator handle 0x4037E0, vt+4 GetHeat
//
//  [Coop] SharedPursuits (see the second half of this file). Free roam: everything below. Both in the
//  same event (Challenge Series, a career pursuit event, a race with cops): the cooldown / escape rule
//  (frozen until both are in cooldown, escape together), their cops engage us (a chasing one close to
//  us, a patrol one we speed past, ramming one, or their pursuit starting close to us starts one here),
//  and busted together (a bust fails the event for both: the other game busts its player too). Free
//  roam: shared heat, the other player's cops on the minimap and the radar detector, ramming one of
//  their cops starts a pursuit here. Evade bar is personal; a bust in a pursuit we share busts both.
//  The cooldown is one for both: a full evade bar waits at the evade limit until the other player's
//  is full too, then both enter the cooldown; when either is spotted in it, both leave it (even from a
//  hiding spot). Escape completes only when both have finished. A safe house (open in the cooldown, as
//  in the game) evades that player as usual, and PURSUIT_EVADED ends the other player's pursuit as an
//  escape/win too. The other player's strip is drawn under ours (peerbar.inl: a copy of the game's own).
//  While both are in cooldown the cooldown is one: the one further on sets it for both, and if one
//  player is in a hiding spot the other's cooldown runs at the hiding-spot speed too.
// ---------------------------------------------------------------------------
namespace {
    constexpr uintptr_t AquireFn = 0x423860, IPerpetratorHandle = 0x4037E0;
    // AIPursuit::OnTask (0x443BA0) takes the smallest IPursuitAI "time since target seen" (slot 29) of its
    // cops as the time since its perpetrator was seen: at 0x44416D `mov eax,[ebx]; mov ecx,ebx;
    // call [eax+0x74]`, ebx = the cop's IPursuitAI, edi = the cop (IVehicle*). A cop of ours that went
    // after the other player sees THEM: for those the hook answers "not seen", so a cop busy with the other
    // player never keeps our own pursuit from cooling down (the evade bar stays personal).
    constexpr uintptr_t PursuitSeenCall = 0x44416D;
    constexpr int MAX_RETARGET = 8;

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

    void*  g_rtCops[MAX_RETARGET] = {};   // cops of our pursuit currently chasing the other player
    int    g_rtN = 0;
    double g_nextPursuitCheck = 0;
    float  g_notSeen = 1000.f;

    bool IsRetargeted(void* cop) {
        for (int i = 0; i < g_rtN; ++i) if (g_rtCops[i] == cop) return true;
        return false;
    }
    bool __stdcall RetargetedForSeen(void* cop) { return cop && IsRetargeted(cop); }
    void __declspec(naked) hk_PursuitCopSeen() {
        __asm {
            push edi
            call RetargetedForSeen
            test al, al
            jnz not_seen
            mov eax, [ebx]
            mov ecx, ebx
            jmp dword ptr [eax + 0x74]     // the original call; it returns to the game
        not_seen:
            fld dword ptr [g_notSeen]
            ret
        }
    }
    // AICopManager's new-pursuit check (0x42F570, every tick of its OnTask) walks the players list (vehicle
    // list 1), then the AI racers, and starts a pursuit on the first car for which 0x429EF0 (thiscall,
    // IVehicle* -> bool) says so: a perpetrator that a cop sees speeding (over 29.06 m/s, the float at
    // 0x891694), with a recent infraction, or at heat level 4+. The racer loop skips driver classes 0
    // (human) and 6 (remote); the players loop does not. The other player's car here is a DRIVER_REMOTE
    // car in the players list, and class 6 gets the human AI (AIVehicleHuman, a perpetrator; driver class
    // table at 0x887610). So a cop here could start THE pursuit on that copy instead of on us: cops chase
    // the copy, we get no pursuit (no heat mode, no pursuit HUD), and ours only starts when the copy is
    // removed - at the end of an event. Their own game runs their pursuit; here the copy is never a perp.
    constexpr uintptr_t NewPursuitCall1 = 0x42F5A5, NewPursuitCall2 = 0x42F605, NewPursuitFn = 0x429EF0;
    void* g_noPerpLogged = nullptr;
    bool  g_pursuitFrozen = false;   // results / race-over screen of an event (SharedPursuitTickImpl): no new pursuit on us
    bool __fastcall hk_NewPursuitFor(void* self, void*, void* iv) {
        if (g_pursuitFrozen && iv && iv == LocalVehicle()) return false;   // the world runs on under that menu in co-op
        if (iv && (iv == g_ghost || (IsVehicle(iv) && DriverClass(iv) == DRV_REMOTE))) {
            if (iv != g_noPerpLogged) {
                g_noPerpLogged = iv;
                LOG("Pursuit: the other player's car here (%p) is never a pursuit target here - their own game chases them", iv);
            }
            return false;
        }
        return ((bool(__thiscall*)(void*, void*))NewPursuitFn)(self, iv);
    }
    bool PatchPursuitSeen() {
        const uint8_t expect[7] = { 0x8B, 0x03, 0x8B, 0xCB, 0xFF, 0x50, 0x74 };   // mov eax,[ebx]; mov ecx,ebx; call [eax+0x74]
        uint8_t bytes[7] = { 0xE8, 0, 0, 0, 0, 0x90, 0x90 };                    // call hook; nop; nop
        int32_t rel = (int32_t)((uintptr_t)&hk_PursuitCopSeen - (PursuitSeenCall + 5));
        memcpy(bytes + 1, &rel, 4);
        return Mem::PatchBytes(PursuitSeenCall, expect, bytes, 7);
    }

    // that cop back on the pursuit's own target (our car)
    void RetargetBack(void* pursuit, void* cop) {
        if (!pursuit || !ListContains(-1, cop)) return;
        void* pt = VCall<void* (__thiscall*)(void*)>(pursuit, 2)(pursuit);
        void* sim = nullptr;
        if (pt && Mem::Read((uintptr_t)pt + 0x1C, sim) && sim)
            if (void* t = CopTarget(cop)) Aquire(t, sim);
    }
    void ReleaseRetargets(void* pursuit) {
        if (!g_rtN) return;
        int n = g_rtN;
        for (int i = 0; i < n; ++i) RetargetBack(pursuit, g_rtCops[i]);
        g_rtN = 0;
        LOG("Pursuit: %d cop(s) back on the local player%s", n, pursuit ? "" : " (no pursuit any more)");
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

static void PursuitTickImpl(bool connected, const Net::Snapshot& peer);
static void SharedPursuitTickImpl(bool connected, const Net::Snapshot& peer);
static void PeerBarTickImpl(bool connected, const Net::Snapshot& peer);
static bool PeerBarNativeSet(int kind, int value, int own);      // peerbar.inl: the copy of the game's own meter (guards itself)
static bool PeerBarForced(int* kind, int* value);       // test: DebugPeerBar
static void HidePeerBar() { PeerBarNativeSet(0, 0, 0); }

void Game::PursuitTick(bool connected, const Net::Snapshot& peer) {
    if (!g_ok) return;
    __try { SharedPursuitTickImpl(connected, peer); }
    __except (EXCEPTION_EXECUTE_HANDLER) { LOG("Pursuit: exception in the shared pursuit"); }
    __try { PeerBarTickImpl(connected, peer); }
    __except (EXCEPTION_EXECUTE_HANDLER) { HidePeerBar(); }
    __try { PursuitTickImpl(connected, peer); }
    __except (EXCEPTION_EXECUTE_HANDLER) { g_rtN = 0; LOG("Pursuit: exception"); }
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
    constexpr uintptr_t GManagerPtr = 0x91E00C;
    constexpr uint32_t P_EvadeLevel = 0xFC;
    // Hiding spots. The human driver's update (0x4293D0) clears IPerpetrator +0x2C (hidden from cars) and
    // +0x2D (hidden from helicopters) every tick, then sets them while the player stands in a "Car" /
    // "Heli" hiding zone during the cooldown (evade level >= 0.05, after 0.05 s there). AIPursuit::OnTask
    // asks them through IPerpetrator slots 11 / 12 (AIPerpVehicle vtable 0x892250: 0x433360 / 0x433370)
    // at 0x444829 / 0x444835 and, while hidden and not seen, adds rate x dt to the search time (+0x124
    // here; rate = the heat level's attribute 0x7FCEE250, else 3.0 at 0x890604): the faster cooldown.
    // The cops' sight checks (0x416540, 0x4170D0) ask the same slots; we answer "hidden" only to OnTask.
    constexpr uint32_t PERP_HiddenCar = 0x2C, PERP_HiddenHeli = 0x2D;
    constexpr uintptr_t PerpVtable = 0x892250, PerpHiddenCarFn = 0x433360, PerpHiddenHeliFn = 0x433370;
    constexpr uintptr_t OnTaskAsksHiddenCar = 0x44482C, OnTaskAsksHiddenHeli = 0x444838;   // return addresses

    struct SharedState {
        uint16_t heat100 = 0;        // what we send (refreshed every 50 ms)
        uint8_t  flags = 0;
        uint8_t  bustSeq = 0;
        uint16_t left10 = 0;
        int8_t   bar = 0;                // our pursuit bar, sent for the other player's screen (SharedPursuit::BarFromMeter)
        uint8_t  barKind = 0;
        bool     localBusted = false;    // our own pursuit is busted right now (any pursuit: free roam, event, prologue)
        SharedPursuit::ReadyDebounce peerReady;   // the partner's ready, debounced (shared cooldown decisions)
        DWORD    coolEnteredAt = 0;       // when our cooldown began (SharedPursuit::InCooldownGrace)
        bool     holdValid = false;       // waited at the evade limit: the progress then, put back at the cooldown (UndoEvadeWait)
        float    holdProgress = 0.f;
        void*    holdPursuit = nullptr;   // the pursuit that snapshot belongs to
        float    coolPrevLeft = -1.f;     // cooldown left at the last tick (progress per tick, EscapeHoldLeft)
        float    coolPerTick = 0.f;       // the most progress seen in one tick of this cooldown
        bool     coolSkipRate = false;    // we moved the meter ourselves (catch-up, the undone evade wait): don't measure that step
        SharedPursuit::HeatTracker heat;
        SharedPursuit::BustTracker bust;
        void*    lastBustedPursuit = nullptr;
        DWORD    bustingForPeerAt = 0;      // we bust ourselves because they were busted: not reported back
        DWORD    pendingBustUntil = 0;     // busting us because they were busted in our event (until then)
        bool     pendingStarted = false;
        uint32_t pendingEvent = 0;
        DWORD    nextForce = 0;
        SharedPursuit::EventBust eventBust;
        bool     peerCopsEngage = false;   // their cops may start a pursuit on us now (SharedPursuit::PeerCopsEngage)
        int      lastStatus = -2;
        int      lastCool = -1;
        DWORD    lastCopHit = 0;
        DWORD    peerCopHitAt = 0;        // we rammed one of their cops while chased (counts as seen)
        DWORD    peerSpotLog = 0;
        bool     peerSpotting = false;   // one of their cops sees us now (for the log)
        int      radarCops = -1, mapCops = -1;
        bool     peerCopsShown = false;   // their cops on our minimap / radar detector: same place (free roam or same event)
        bool     sentEvaded = false;     // keep PURSUIT_EVADED on the wire after the pursuit object is gone
        uint32_t lastEvent = 0;          // our event: a new one clears an old escape signal
        bool     peerChasedInEvent = false;   // their pursuit was live in this same event
        bool     peerLiveWithOurs = false;    // their pursuit was live while this pursuit of ours was
        void*    sharedHidePerp = nullptr;    // our IPerpetrator: its pursuit runs the cooldown as if hidden
        float    catchUpLogged = 0.f;
        float    catchUpLeftSeen = -1.f;      // our P_CoolLeft when we last added a catch-up
    } g_sp;

    // IPerpetrator slots 11 / 12 (hidden from cars / helicopters). Vanilla for everyone, except that our
    // pursuit's OnTask hears "hidden" while the other player hides for both of us (SharedPursuit::ShareHidingSpot).
    bool __fastcall hk_PerpHiddenCar(void* self, void*) {
        if (*((uint8_t*)self + PERP_HiddenCar)) return true;
        return self == g_sp.sharedHidePerp && (uintptr_t)_ReturnAddress() == OnTaskAsksHiddenCar;
    }
    bool __fastcall hk_PerpHiddenHeli(void* self, void*) {
        if (*((uint8_t*)self + PERP_HiddenHeli)) return true;
        return self == g_sp.sharedHidePerp && (uintptr_t)_ReturnAddress() == OnTaskAsksHiddenHeli;
    }
    bool PatchPerpHidden() {
        uintptr_t s11 = PerpVtable + 11 * 4, s12 = PerpVtable + 12 * 4, c11 = 0, c12 = 0;
        if (!Mem::Read(s11, c11) || !Mem::Read(s12, c12)) return false;
        if (c11 != PerpHiddenCarFn || c12 != PerpHiddenHeliFn) return false;
        return Mem::PatchPointer((void*)s11, (void*)&hk_PerpHiddenCar) && Mem::PatchPointer((void*)s12, (void*)&hk_PerpHiddenHeli);
    }

    // HUD pursuit board (IPursuitBoard vtable 0x8A2428): slot 2 SetIsHiding(bool), slot 3 SetTimeUntilHidden(float)
    // write [this+9] / [this+0xC]; a time > 0 shows the cooldown meter's HIDDEN backing. While the other player
    // hides for both of us our cooldown runs at the hiding-spot speed, so our board says HIDDEN too.
    constexpr uintptr_t BoardVtable = 0x8A2428, BoardSetIsHidingFn = 0x568C90, BoardSetTimeUntilHiddenFn = 0x568CA0;
    void __fastcall hk_BoardSetIsHiding(uint8_t* self, void*, bool hiding) {
        if (g_sp.sharedHidePerp) hiding = true;
        self[9] = hiding ? 1 : 0;
    }
    void __fastcall hk_BoardSetTimeUntilHidden(uint8_t* self, void*, float t) {
        if (g_sp.sharedHidePerp && !(t > 0.f)) t = 1.f;
        *(float*)(self + 0xC) = t;
    }
    bool PatchBoardHidden() {
        uintptr_t s2 = BoardVtable + 2 * 4, s3 = BoardVtable + 3 * 4, c2 = 0, c3 = 0;
        if (!Mem::Read(s2, c2) || !Mem::Read(s3, c3)) return false;
        if (c2 != BoardSetIsHidingFn || c3 != BoardSetTimeUntilHiddenFn) return false;
        return Mem::PatchPointer((void*)s2, (void*)&hk_BoardSetIsHiding) && Mem::PatchPointer((void*)s3, (void*)&hk_BoardSetTimeUntilHidden);
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
            if (IsRetargeted(c)) continue;                                                   // it sees the other player
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
    // when each of the other player's cops last saw us in our current pursuit (minimap: SharedPursuit::PeerCopOnOurMap)
    struct PeerCopSeen { uint16_t id; DWORD at; };
    PeerCopSeen g_peerCopSeen[32] = {};
    void NotePeerCopSawUs(uint16_t id, DWORD now) {
        int freeSlot = -1, oldest = 0;
        for (int i = 0; i < 32; ++i) {
            if (g_peerCopSeen[i].id == id) { g_peerCopSeen[i].at = now ? now : 1; return; }
            if (!g_peerCopSeen[i].id && freeSlot < 0) freeSlot = i;
            if ((int)(g_peerCopSeen[i].at - g_peerCopSeen[oldest].at) < 0) oldest = i;
        }
        int s = freeSlot >= 0 ? freeSlot : oldest;
        g_peerCopSeen[s] = PeerCopSeen{ id, now ? now : 1 };
    }
    bool PeerCopSawUs(uint16_t id, DWORD now, uint32_t* since) {
        for (auto& x : g_peerCopSeen) if (x.id == id && x.at) { *since = now - x.at; return true; }
        return false;
    }
    void ForgetPeerCopsSawUs() { for (auto& x : g_peerCopSeen) x = PeerCopSeen{}; }
    // every copy of their cops that sees us now (chased): remembered for the minimap
    void NotePeerCopsSeeingUs(void* me, DWORD now) {
        void* rb = me ? RigidBodyOf(me) : nullptr;
        if (!rb) return;
        const float* mp = RB_GetVec(rb, RB_GetPosition);
        for (auto& m : g_mirrors) {
            if (!IsPeerCop(m, false)) continue;
            void* crb = RigidBodyOf(m.iv);
            if (!crb) continue;
            const float* cp = RB_GetVec(crb, RB_GetPosition);
            float d = Dist(cp, mp);
            if (!(d <= SharedPursuit::PEER_COP_SEES_PLAYER_M)) continue;
            float fwd[3] = {};
            VCall<void(__thiscall*)(void*, float*)>(crb, 13 /*GetForwardVector*/)(crb, fwd);
            float to[3] = { mp[0] - cp[0], mp[1] - cp[1], mp[2] - cp[2] };
            float facing = d > 0.01f ? (fwd[0] * to[0] + fwd[1] * to[1] + fwd[2] * to[2]) / d : 1.f;
            if (SharedPursuit::PeerCopSpotsWanted((m.s[m.n - 1].e.flags & 4) != 0, d, facing)) NotePeerCopSawUs(m.id, now);
        }
    }
    // One of the other player's cops (its copy here, at its local interpolated place - not the owner's
    // remote coordinates or a traffic id) sees us while we are already wanted. Returns its id, or -1.
    int PeerCopSpottingUs(void* me, float* distOut) {
        void* rb = me ? RigidBodyOf(me) : nullptr;
        if (!rb) return -1;
        const float* mp = RB_GetVec(rb, RB_GetPosition);
        for (auto& m : g_mirrors) {
            if (!IsPeerCop(m, false)) continue;
            void* crb = RigidBodyOf(m.iv);
            if (!crb) continue;
            const float* cp = RB_GetVec(crb, RB_GetPosition);
            float d = Dist(cp, mp);
            if (!(d <= SharedPursuit::PEER_COP_SEES_PLAYER_M)) continue;
            float fwd[3] = {};
            VCall<void(__thiscall*)(void*, float*)>(crb, 13 /*GetForwardVector*/)(crb, fwd);
            float to[3] = { mp[0] - cp[0], mp[1] - cp[1], mp[2] - cp[2] };
            float facing = d > 0.01f ? (fwd[0] * to[0] + fwd[1] * to[1] + fwd[2] * to[2]) / d : 1.f;
            if (SharedPursuit::PeerCopSpotsWanted((m.s[m.n - 1].e.flags & 4) != 0, d, facing)) {
                if (distOut) *distOut = d;
                return (int)m.id;
            }
        }
        return -1;
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
}
// busted in this run of the current event (event.inl: a restart from the other player is not queued then)
static bool LocalBustedThisRun() {
    uint32_t ev = Game::CurrentEventHash();
    return ev && g_bustedEvent == ev;
}
namespace {

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
        theirs[T_ROADBLOCKS] = 0;   // personal: each player counts the roadblocks they drove through (roadblock.inl)
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
    bool openWorld = OpenWorldShared(freeRoam, place, pcShared);
    // results / win / Continue screens: no bust meter, nothing new starts (see PursuitFrozenForResults)
    bool resultsFreeze = false;
    {
        bool done = false; uint32_t ev = Game::CurrentEventHash();
        if (ev) if (void* ri = RacerInfoOfLocal()) {
            uint8_t fin = 0; uint32_t outs = 0;
            Mem::Read((uintptr_t)ri + 0x21, fin); Mem::Read((uintptr_t)ri + 0x1C, outs);
            done = fin || outs;
        }
        // busted in this run: the event is over for us (its race-over menu is up while the world runs on in
        // co-op) - no second bust there, and nobody's cops start a pursuit on us until a restart
        if (ev && g_bustedEvent == ev) done = true;
        bool screen = FEScreens::IsPushed("PostRace_Results.fng") || FEScreens::IsPushed("PostRace_Pursuit.fng") ||
                      FEScreens::IsPushed("PostRace_MilestoneRewards.fng") || FEScreens::IsPushed("InGamePhotoMaster.fng");
        resultsFreeze = Game::GameFlowState() == 6 && PursuitFrozenForResults(ev != 0, done, screen);
        static bool lastFreeze = false;
        if (resultsFreeze != lastFreeze) {
            lastFreeze = resultsFreeze;
            LOG("Pursuit: %s", resultsFreeze ? "results screen - pursuit and bust meter frozen for us" : "results closed - pursuit runs again");
        }
        if (resultsFreeze) openWorld = false;
        g_pursuitFrozen = resultsFreeze && connected;
    }
    g_sp.peerCopsShown = here && !resultsFreeze;
    // their cops spot / hit / pull us in: free roam and a pursuit challenge as before, and now any event
    // both of us are in (a Challenge Series tollbooth race with cops: their cops ignored us there)
    {
        uint8_t stage = Game::RaceStage();
        bool engage = SharedPursuit::PeerCopsEngage(openWorld, place, resultsFreeze, NisActive(),
                                                    stage == 1 || stage == CoopRace::kStageLoadHeld);
        if (engage != g_sp.peerCopsEngage && (engage || g_sp.peerCopsEngage))
            LOG("Pursuit: the other player's cops %s", engage ? (openWorld ? "can start a pursuit on us (open world)"
                                                                         : "can start a pursuit on us in this event too")
                                                          : "no longer start a pursuit on us");
        g_sp.peerCopsEngage = engage;
    }
    SharedPursuit::EventSeenTogether(g_sp.eventBust, sameEvent, Game::CurrentEventHash(), now);
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
    if (resultsFreeze && pursuitAll && PRead<float>(pursuitAll, P_BustTimer) > 0.f)
        PWrite<float>(pursuitAll, P_BustTimer, 0.f);       // results screen: no bust meter (slot 79's own reset)
    g_sp.localBusted = pursuitAll && (PRead<uint8_t>(pursuitAll, P_PerpBusted) || PRead<int>(pursuitAll, P_Status) == PS_BUSTED);
    if (g_sp.localBusted)
        if (uint32_t ev = Game::CurrentEventHash()) g_bustedEvent = ev;   // a pursuit challenge run is lost
    {   // Restart on the race-over menu after a bust doesn't raise the restart flag (coopwin.inl's new-run reset
        // misses it): that menu closing back to the race (Restart; Quit leaves the event) starts a new run
        static bool sawRaceOver = false;
        uint32_t ev = Game::CurrentEventHash();
        bool bustedRun = ev && g_bustedEvent == ev;
        bool menu = FEScreens::IsPushed("Pause_Main.fng");
        bool bustedNow = g_sp.localBusted;
        if (!bustedRun) sawRaceOver = false;
        else if (menu) sawRaceOver = true;
        else if (sawRaceOver && !bustedNow && FEScreens::IsPushed("HUD_SingleRace.fng")) {
            sawRaceOver = false; g_bustedEvent = 0;
            LOG("Pursuit: restarted from the race-over menu - a new run, the pursuit runs again");
        }
    }
    void* me = here ? meAll : nullptr;
    void* pursuit = here ? pursuitAll : nullptr;

    // ---- the other player is chased: so are we, wherever we are (one shared pursuit) ----
    // Only a pursuit that has just started there pulls us in (within 30 s of its start), and never right after
    // our own pursuit ended: at an escape the two games end a moment apart, and the later one must not drag
    // the earlier one back into a chase.
    static bool peerWasActive = false; static DWORD peerStartAt = 0, ownEndAt = 0; static bool hadOwn = false;
    bool peerActive = peer.valid && (peer.s.pursuitFlags & PURSUIT_ACTIVE) && peer.s.gameFlowState == 6 &&
                      (!peer.s.eventHash || (sameEvent && peer.s.eventHash == Game::CurrentEventHash()));
    if (peerActive && !peerWasActive) peerStartAt = now;
    peerWasActive = peerActive;
    if (hadOwn && !pursuit) ownEndAt = now;
    hadOwn = pursuit != nullptr;
    if (me && g_sp.peerCopsEngage && !pursuit && peerActive && now - peerStartAt < 30000 && (!ownEndAt || now - ownEndAt > 30000)) {
        static DWORD nextJoin = 0;
        float peerDist = -1.f;
        if (!openWorld) {   // in a race: only when we are close to them
            void* orb = RigidBodyOf(g_ghost); void* mrb = RigidBodyOf(me);
            if (orb && mrb) peerDist = Dist(RB_GetVec(orb, RB_GetPosition), RB_GetVec(mrb, RB_GetPosition));
        }
        if ((int)(now - nextJoin) >= 0 && SharedPursuit::JoinTheirPursuit(openWorld, true, peerDist)) {
            nextJoin = now + 20000;   // if the game starts none (no cops around yet), try again later
            bool ok = DebugForcePursuitImpl();
            LOG("Pursuit: the other player is chased%s - %s", openWorld ? "" : " close to us in this event",
                ok ? "starting the same pursuit on us" : "no cop manager, no pursuit");
        }
    }
    // ---- the other player's cops spot us too: a chasing one close to our car, or a patrol one we speed past ----
    if (me && !pursuit && g_sp.peerCopsEngage) {
        static DWORD nextSpot = 0;
        void* myRb = RigidBodyOf(me);
        if (myRb && (int)(now - nextSpot) >= 0) {
            const float* mp = RB_GetVec(myRb, RB_GetPosition);
            const float* mv = RB_GetVec(myRb, RB_GetLinearVelocity);
            float speed = sqrtf(mv[0] * mv[0] + mv[1] * mv[1] + mv[2] * mv[2]);
            float heat = 0.f;
            if (void* pp = Find(me, IPerpetratorHandle)) heat = VCall<float(__thiscall*)(void*)>(pp, 1)(pp);
            for (auto& m : g_mirrors) {
                if (!IsPeerCop(m, false)) continue;
                void* rb = RigidBodyOf(m.iv);
                if (!rb) continue;
                const float* cp = RB_GetVec(rb, RB_GetPosition);
                float d = Dist(cp, mp);
                if (m.s[m.n - 1].e.flags & 4) {          // a cop in a pursuit there
                    if (d >= SharedPursuit::PEER_COP_SEES_PLAYER_M) continue;
                    nextSpot = now + 15000;
                    bool ok = DebugForcePursuitImpl();
                    LOG("Pursuit: the other player's cop %u spotted us (%.0f m) - %s", (unsigned)m.id, d, ok ? "starting a pursuit on us" : "no cop manager");
                    break;
                }
                // a patrol cop there: it sees us speeding past it (the game's own spotting only knows its own player)
                float fwd[3] = {};
                VCall<void(__thiscall*)(void*, float*)>(rb, 13 /*GetForwardVector*/)(rb, fwd);
                float to[3] = { mp[0] - cp[0], mp[1] - cp[1], mp[2] - cp[2] };
                float facing = d > 0.01f ? (fwd[0] * to[0] + fwd[1] * to[1] + fwd[2] * to[2]) / d : 1.f;
                if (d <= SharedPursuit::PATROL_SEES_M) {   // reported: their patrol cops never chase us - why not, when one is close
                    static DWORD nextNearLog = 0;
                    if ((int)(now - nextNearLog) >= 0) {
                        nextNearLog = now + 10000;
                        LOG("Pursuit: the other player's patrol cop %u is %.0f m from us (%s of it, %.0f km/h, heat %.1f) - %s", (unsigned)m.id, d,
                            facing >= 0.f ? "in front" : "behind", speed * 3.6f, heat,
                            SharedPursuit::PatrolCopSpotsPlayer(d, facing, speed, heat) ? "it sees us" : "not speeding / not seen");
                    }
                }
                if (SharedPursuit::PatrolCopSpotsPlayer(d, facing, speed, heat)) {
                    nextSpot = now + 15000;
                    bool ok = DebugForcePursuitImpl();
                    LOG("Pursuit: the other player's patrol cop %u saw us (%.0f m, %.0f km/h, heat %.1f) - %s", (unsigned)m.id, d, speed * 3.6f, heat,
                        ok ? "starting a pursuit on us" : "no cop manager");
                    break;
                }
            }
        }
    }

    // ---- our own pursuit, as the other game sees it ----
    g_sp.heat100 = 0; g_sp.flags = 0; g_sp.left10 = 0; g_sp.bar = 0; g_sp.barKind = 0;
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
            float unseen = PRead<float>(pursuit, P_UnseenInState), sinceSeen = PRead<float>(pursuit, P_SinceSeen);
            if (EvadeBarFull((g_sp.flags & PURSUIT_SEEN) != 0, status == PS_COOLDOWN, unseen, sinceSeen))
                g_sp.flags |= PURSUIT_READY;
            g_sp.left10 = LeftToWire(left);
            // in a hiding spot (the game's own flags, never the ones we answer for the other player's)
            if (status == PS_COOLDOWN && me)
                if (void* hp = Find(me, IPerpetratorHandle))
                    if (PRead<uint8_t>(hp, PERP_HiddenCar) || PRead<uint8_t>(hp, PERP_HiddenHeli)) g_sp.flags |= PURSUIT_HIDING;
            // the other player's cops see us too (our game knows only its own): a cop of theirs next to us keeps
            // us in the chase, and in cooldown it spots us again - the game's own "seen" (time since seen 0)
            {
                float d = -1.f;
                if (me && g_sp.peerCopsEngage) NotePeerCopsSeeingUs(me, now);
                int id = (me && g_sp.peerCopsEngage) ? PeerCopSpottingUs(me, &d) : -1;
                bool hidingNow = status == PS_COOLDOWN && me && [&] {
                    void* hp = Find(me, IPerpetratorHandle);
                    return hp && (PRead<uint8_t>(hp, PERP_HiddenCar) || PRead<uint8_t>(hp, PERP_HiddenHeli));
                }();
                uint32_t sinceHit = g_sp.peerCopHitAt ? now - g_sp.peerCopHitAt : 0xFFFFFFFFu;
                if (SharedPursuit::PeerCopKeepsUsSeen(true, hidingNow, g_sp.peerCopsEngage, id >= 0, sinceHit)) {
                    bool ownSeen = (g_sp.flags & PURSUIT_SEEN) != 0;
                    if (!ownSeen) PWrite<float>(pursuit, P_SinceSeen, 0.f);   // OnTask: seen -> search 0, cooldown ends
                    g_sp.flags |= PURSUIT_SEEN;
                    g_sp.flags &= ~PURSUIT_READY;
                    if (!g_sp.peerSpotting || now - g_sp.peerSpotLog > 5000) {
                        g_sp.peerSpotLog = now;
                        if (id >= 0) LOG("Pursuit: the other player's cop %d sees us (%.0f m, %s)%s", id, d,
                                         status == PS_COOLDOWN ? "in cooldown - back to the chase" : "chase",
                                         ownSeen ? " - our cops see us too" : "");
                        else LOG("Pursuit: we rammed the other player's cop - seen (%s)", status == PS_COOLDOWN ? "cooldown ends" : "chase");
                    }
                    g_sp.peerSpotting = true;
                } else if (g_sp.peerSpotting) {
                    g_sp.peerSpotting = false;
                    LOG("Pursuit: the other player's cops no longer see us%s", hidingNow ? " (hiding spot)" : "");
                }
            }
        }
        {   // our bar for the other player's screen (after the "seen" above: their cops count too): exactly what our
            // own meter shows - LocalPlayer::UpdateHud (0x6F1946) gives the board busted ? 1 : min(TimeUntilBusted, 0.99)
            // (AIPursuit::TimeUntilBusted 0x40AEC0: the bust timer over a base the pursuit task sets from the cops'
            // distance, our speed and the time unseen - not a plain timer ratio, which left the strip empty or jumping)
            float t = ((float(__thiscall*)(void*))0x40AEC0)(pursuit);
            SharedPursuit::Bar b = SharedPursuit::BarFromMeter((g_sp.flags & PURSUIT_ACTIVE) != 0, status == PS_COOLDOWN, busted, t,
                left, PRead<float>(pursuit, P_CoolNeed));
            g_sp.bar = b.value; g_sp.barKind = b.kind;
        }
        if (busted) g_sp.flags |= PURSUIT_BUSTED;
        if (status == PS_EVADED) { g_sp.flags |= PURSUIT_EVADED; g_sp.sentEvaded = true; }
        else if (status != PS_BUSTED && (g_sp.flags & PURSUIT_ACTIVE)) g_sp.sentEvaded = false;
    } else if (g_sp.sentEvaded) {
        g_sp.flags |= PURSUIT_EVADED;   // pursuit object already gone: keep the escape signal on the wire
    }
    if (status == PS_COOLDOWN && g_sp.lastStatus != PS_COOLDOWN) g_sp.coolEnteredAt = now;
    if (status != g_sp.lastStatus) {
        if (on && (status >= 0 || g_sp.lastStatus >= 0))
            LOG("Pursuit: ours %s -> %s", StatusName(g_sp.lastStatus < -1 ? -1 : g_sp.lastStatus), StatusName(status));
        g_sp.lastStatus = status;
    }
    // ---- 4. busted: count our own busts (the other game busts its player too if it shares this pursuit) ----
    if (!(g_sp.flags & PURSUIT_ACTIVE)) ForgetPeerCopsSawUs();   // a new pursuit: their cops have to find us again
    if (!pursuit || !busted) g_sp.lastBustedPursuit = nullptr;   // the next pursuit may get the same address
    if (pursuit && busted && pursuit != g_sp.lastBustedPursuit) {
        g_sp.lastBustedPursuit = pursuit;
        g_sp.pendingBustUntil = 0;
        if (g_sp.bustingForPeerAt && now - g_sp.bustingForPeerAt < 15000)
            LOG("Pursuit: busted here too, as the other player was");
        else if (on) {
            ++g_sp.bustSeq;
            LOG("Pursuit: we were busted (count %u) - %s", (unsigned)g_sp.bustSeq,
                Game::CurrentEventHash() ? "in an event the other player is busted with us" : "the other player is busted too if they're in this pursuit with us");
        }
        g_sp.bustingForPeerAt = 0;
    }
    if (!on) {
        // alone again (they left / disconnected): our pursuit is the plain game's from here on
        if (g_sp.sharedHidePerp || g_sp.peerSpotting || g_sp.peerCopHitAt)
            LOG("Pursuit: the other player is gone - our pursuit and cooldown run as in the plain game");
        g_sp.heat = {}; g_sp.bust = {}; g_sp.pendingBustUntil = 0; g_sp.lastCool = -1; g_sp.peerCopsEngage = false;
        g_sp.holdValid = false; g_sp.coolPrevLeft = -1.f; g_sp.coolPerTick = 0.f; g_sp.coolSkipRate = false;
        g_sp.sentEvaded = false;
        g_sp.sharedHidePerp = nullptr;
        g_sp.peerCopHitAt = 0; g_sp.peerSpotting = false;
        ForgetPeerCopsSawUs();
        if (g_tally.live) SharedPursuit::TallyStop(g_tally);
        g_tallyPursuit = nullptr;
        return;
    }
    bool peerHere = freeRoam ? PeerInFreeRoam(peer) : sameEvent;

    // ---- 1. heat: the hotter of the two, both ways ----
    void* perp = (me && openWorld && !NisActive()) ? Find(me, IPerpetratorHandle) : nullptr;
    if (perp) {
        float mine = VCall<float(__thiscall*)(void*)>(perp, 1)(perp);
        g_sp.heat100 = HeatToWire(mine);
        float take = peerHere ? HeatToTake(g_sp.heat, mine, peer.s.heat100) : 0.f;
        if (!peerHere) g_sp.heat.lastPeer = 0;   // a gap: their next value is a baseline (synced stays)
        if (take > 0.f) {
            VCall<void(__thiscall*)(void*, float)>(perp, 2)(perp, take);
            g_sp.heat100 = HeatToWire(take);
            LOG("Pursuit: heat %.2f -> %.2f (the other player's heat)", mine, take);
        }
    } else g_sp.heat.lastPeer = 0;

    // ---- 4. the other player was busted: in an event both of us are in it fails the event for both, and in free
    //      roam a pursuit we share busts both - so we are busted too (the game's own arrest and cutscene) ----
    {
        uint32_t ev = Game::CurrentEventHash();
        bool bustedHere = pursuitAll && (PRead<uint8_t>(pursuitAll, P_PerpBusted) || PRead<int>(pursuitAll, P_Status) == PS_BUSTED);
        if (peer.valid && PeerJustBusted(g_sp.bust, peer.s.bustSeq)) {
            bool ourLive = pursuitAll && (g_sp.flags & PURSUIT_ACTIVE);
            if (BustWithThem(g_sp.eventBust, ev, now, resultsFreeze, bustedHere)) {
                g_sp.pendingBustUntil = now + SHARED_BUST_TRY_MS; g_sp.pendingStarted = false; g_sp.pendingEvent = ev;
                g_sp.nextForce = 0;
                LOG("Pursuit: the other player was busted in event %08X - %s", ev,
                    pursuitAll ? "busting us too (the event is lost for both)" : "no pursuit here, starting one to bust us too");
            } else if (BustTogetherInFreeRoam(ev, ourLive, g_sp.peerLiveWithOurs, resultsFreeze, bustedHere)) {
                g_sp.pendingBustUntil = now + SHARED_BUST_TRY_MS; g_sp.pendingStarted = false; g_sp.pendingEvent = 0;
                g_sp.nextForce = 0;
                LOG("Pursuit: the other player was busted in our shared pursuit - busting us too");
            } else if (ev && bustedHere) LOG("Pursuit: the other player was busted - we already are");
            else if (ev && resultsFreeze) LOG("Pursuit: the other player was busted - we are done with this event, nothing happens");
            else LOG("Pursuit: the other player was busted - %s", ev ? "not in the same event, ours continues" : "not in a pursuit with us, nothing happens here");
        }
        if (g_sp.pendingBustUntil) {
            if (ev != g_sp.pendingEvent || resultsFreeze || (int)(now - g_sp.pendingBustUntil) > 0) {
                LOG("Pursuit: gave up busting us with the other player (%s)", ev != g_sp.pendingEvent ? "left the event" :
                                                                              resultsFreeze ? "results screen" : "no pursuit came");
                g_sp.pendingBustUntil = 0;
            } else if (bustedHere) {
                g_sp.pendingBustUntil = 0;
            } else if (pursuitAll && !PRead<uint8_t>(pursuitAll, P_Bailed)) {
                PWrite<float>(pursuitAll, P_BustTimer, 10.f);   // above the 5 s limit: the game busts us on its next sim tick
                g_sp.bustingForPeerAt = now;
            } else if (!pursuitAll && !g_sp.pendingEvent) {
                g_sp.pendingBustUntil = 0;          // free roam: our pursuit ended meanwhile - nothing to bust
            } else if (!pursuitAll && (int)(now - g_sp.nextForce) >= 0) {
                g_sp.nextForce = now + 2500;
                bool ok = DebugForcePursuitImpl();
                if (!g_sp.pendingStarted) LOG("Pursuit: starting a pursuit to bust us with the other player - %s", ok ? "requested" : "no cop manager");
                g_sp.pendingStarted = true;
            }
        }
    }

    // ---- 5. cooldown: enter immediately; freeze / reset the meter; escape only when both finish ----
    // peer.evaded is honoured even if they already left free roam (they entered a safehouse).
    int coolNow = -1;
    void* hidePerp = nullptr;
    View mine{};
    mine.inPursuit = (g_sp.flags & PURSUIT_ACTIVE) != 0;
    mine.seen = (g_sp.flags & PURSUIT_SEEN) != 0;
    mine.cooling = status == PS_COOLDOWN;
    mine.left = left;
    mine.ready = (g_sp.flags & PURSUIT_READY) != 0;
    mine.busted = busted;
    mine.hiding = (g_sp.flags & PURSUIT_HIDING) != 0;
    // cooldown progress per tick (a hiding spot, ours or shared, starts at its ~3x rate); our hold margin
    if (mine.cooling && !mine.seen) {
        float d = g_sp.coolPrevLeft - left;
        // P_CoolLeft moves only on the game's own pursuit task (~400 ms): a step we caused (catch-up, undone
        // wait) is the next change of it, however many frames later, and is not measured
        if (g_sp.coolPrevLeft >= 0.f && d != 0.f) {
            if (!g_sp.coolSkipRate && d > g_sp.coolPerTick) g_sp.coolPerTick = d;
            g_sp.coolSkipRate = false;
        }
        g_sp.coolPrevLeft = left;
    } else { g_sp.coolPrevLeft = -1.f; g_sp.coolPerTick = 0.f; g_sp.coolSkipRate = false; }
    float seedTick = PURSUIT_TICK_S * (mine.hiding || g_sp.sharedHidePerp ? 3.f : 1.f);
    float holdLeft = EscapeHoldLeft(g_sp.coolPerTick > seedTick ? g_sp.coolPerTick : seedTick);
    // the cooldown starts after a wait at the evade limit: the wait doesn't count (the progress goes back)
    bool undidWait = false;
    if (g_sp.holdValid && (!pursuit || pursuit != g_sp.holdPursuit || mine.seen)) g_sp.holdValid = false;
    float need0 = (g_sp.holdValid && status == PS_COOLDOWN) ? PRead<float>(pursuit, P_CoolNeed) : 0.f;
    // (OnTask sets the requirement every run before the cooldown can start (0x443D5E): 0 only without heat
    // attributes - nothing to undo then)
    if (g_sp.holdValid && status == PS_COOLDOWN && need0 > 0.f) {
        g_sp.holdValid = false;
        float search = PRead<float>(pursuit, P_Search), since = PRead<float>(pursuit, P_SinceSeen);
        UndoEvadeWait(need0, g_sp.holdProgress, search, since);
        PWrite<float>(pursuit, P_Search, search);
        PWrite<float>(pursuit, P_SinceSeen, since);
        undidWait = true; g_sp.coolSkipRate = true;
        float before = left;
        // what is left now, here and on the wire: the old value still held the wait (a finished meter, sent as 0)
        left = need0 - (search + since);
        if (left < 0.f) left = 0.f;
        mine.left = left;
        g_sp.left10 = LeftToWire(left);
        LOG("Pursuit: cooldown after the wait at the evade limit - %.1f s left (%.1f before; the wait doesn't count)", left, before);
    }
    mine.held = mine.cooling && !mine.seen && (g_sp.lastCool == (int)Cool::HoldEscape || left <= holdLeft);
    View theirs = peer.valid ? FromWire(peer.s.pursuitFlags, peer.s.coolLeft10) : View{};
    // their "ready" debounced (SharedPursuit::DebounceReady): every route into and out of the shared cooldown
    // (the evade hold, back to the chase) decides on it, so a flicker of theirs can't make ours flap. A fresh
    // pursuit of theirs (or none) starts it over.
    if (!theirs.inPursuit || !(g_sp.flags & PURSUIT_ACTIVE)) g_sp.peerReady = SharedPursuit::ReadyDebounce{};
    else theirs.ready = SharedPursuit::DebounceReady(g_sp.peerReady, theirs.ready, now);
    // In an event their escape counts only once their pursuit was live in this event (an older
    // escape signal must not end ours - that would win the event for us).
    if (sameEvent && theirs.inPursuit) g_sp.peerChasedInEvent = true;
    if (!sameEvent) g_sp.peerChasedInEvent = false;
    // their pursuit live during this pursuit of ours (reset whenever ours isn't live)
    if (!mine.inPursuit) g_sp.peerLiveWithOurs = false;
    else if (peer.valid && theirs.inPursuit) g_sp.peerLiveWithOurs = true;
    bool peerEvadedHere = peer.valid && PeerEscapeCounts(place, theirs.evaded, g_sp.peerChasedInEvent, g_sp.peerLiveWithOurs);
    if (!peerEvadedHere) theirs.evaded = false;
    bool peerForCool = peerHere || peerEvadedHere;
    if ((g_sp.flags & PURSUIT_ACTIVE) && peerForCool) {
        Decision d = peerHere || theirs.evaded ? DecideCooldown(mine, theirs, true) : Decision{ Cool::Free };
        if (d.what == Cool::BackToChase && InCooldownGrace(status == PS_COOLDOWN, now - g_sp.coolEnteredAt)) d = Decision{ Cool::Free };
        coolNow = (int)d.what;
        if (d.what == Cool::ForceEscape) {
            // not once the event is over for us: the game started a pursuit again on every forced escape
            // (the screen flashed "evaded" / "pursuit" for seconds on a pursuit challenge win)
            if (!resultsFreeze) ForceLocalEscape(pursuit);
            if (coolNow != g_sp.lastCool && !resultsFreeze) LOG("Pursuit: the other player evaded (safehouse) - ending ours as an escape");
        } else if (d.what == Cool::HoldEvade && pursuit && status != PS_COOLDOWN) {
            // the game starts the cooldown once the unseen time passes 2.5 s: keep it just under
            if (PRead<float>(pursuit, P_UnseenInState) > HOLD_UNSEEN_S) PWrite<float>(pursuit, P_UnseenInState, HOLD_UNSEEN_S);
            if (!g_sp.holdValid || g_sp.holdPursuit != pursuit) {
                g_sp.holdValid = true; g_sp.holdPursuit = pursuit;
                g_sp.holdProgress = PRead<float>(pursuit, P_Search) + PRead<float>(pursuit, P_SinceSeen);
            }
        } else if (d.what == Cool::BackToChase && pursuit && status == PS_COOLDOWN) {
            PWrite<float>(pursuit, P_SinceSeen, 0.f);   // seen: the game ends the cooldown (as for a spot)
        } else if (d.what == Cool::HoldEscape && pursuit && status == PS_COOLDOWN && !mine.seen) {
            float search = PRead<float>(pursuit, P_Search), since = PRead<float>(pursuit, P_SinceSeen);
            float need = PRead<float>(pursuit, P_CoolNeed);
            DelayProgress(need, holdLeft, search, since);
            PWrite<float>(pursuit, P_Search, search);
            PWrite<float>(pursuit, P_SinceSeen, since);
            g_sp.left10 = 0;   // finished, as the partner sees it (we only hold it back)
        } else if (d.what == Cool::Free && mine.held && pursuit && status == PS_COOLDOWN && peerHere &&
                   theirs.inPursuit && !theirs.busted && !g_sp.pendingBustUntil && CooldownFinished(theirs)) {
            // the wait is over: escape now, not after the held-back rest of the meter (both escape together).
            // Only when they finished too: near the end with no hold (left under the margin), or with them busted
            // (we are being busted with them), the meter runs on as usual.
            float search = PRead<float>(pursuit, P_Search), since = PRead<float>(pursuit, P_SinceSeen);
            CompleteCooldown(PRead<float>(pursuit, P_CoolNeed), search, since);
            PWrite<float>(pursuit, P_Search, search);
            PWrite<float>(pursuit, P_SinceSeen, since);
        }
        // both in cooldown: one cooldown (the one further on), and the hiding spot of either counts for both
        if (d.what == Cool::Free && peerHere && !resultsFreeze && pursuit && status == PS_COOLDOWN && !undidWait) {
            float add = CooldownCatchUp(mine, theirs, true);
            // P_CoolLeft is only recomputed by the game's own pursuit task: while it hasn't run since our
            // last catch-up (pause menu, a sim stall) the same difference must not be added again
            if (add > 0.f && mine.left == g_sp.catchUpLeftSeen) add = 0.f;
            if (add > 0.f) {
                g_sp.coolSkipRate = true;
                g_sp.catchUpLeftSeen = mine.left;
                float search = PRead<float>(pursuit, P_Search);
                PWrite<float>(pursuit, P_Search, search + add);
                if (add >= 1.f || g_sp.catchUpLogged == 0.f) {
                    g_sp.catchUpLogged = add;
                    LOG("Pursuit: cooldown caught up with the other player's (%.1f s left here -> %.1f s, as there)", mine.left, theirs.left);
                }
            }
            if (ShareHidingSpot(mine, theirs, true) && me) hidePerp = Find(me, IPerpetratorHandle);
        } else { g_sp.catchUpLogged = 0.f; g_sp.catchUpLeftSeen = -1.f; }
        if (coolNow != g_sp.lastCool) {
            if (d.what == Cool::HoldEvade) LOG("Pursuit: our evade bar is full - waiting at the evade limit until the other player's is full too");
            else if (d.what == Cool::BackToChase) LOG("Pursuit: the other player is not in the cooldown (spotted) - we leave it too, back to the chase%s",
                                                       mine.hiding ? " (from a hiding spot)" : "");
            else if (d.what == Cool::HoldEscape) LOG("Pursuit: our cooldown is finished - waiting for the other player to finish before we escape");
            else if (d.what != Cool::ForceEscape && theirs.inPursuit) LOG("Pursuit: cooldown runs as usual (%s here, %s there)",
                                           mine.seen ? "seen" : (mine.cooling ? "in cooldown" : "hiding"),
                                           theirs.seen ? "seen" : (theirs.cooling ? "in cooldown" : "hiding"));
        }
    }
    g_sp.lastCool = coolNow;
    if (hidePerp != g_sp.sharedHidePerp) {
        if (hidePerp) LOG("Pursuit: the other player is in a hiding spot - our cooldown runs at the hiding-spot speed too (HUD: HIDDEN)");
        else if (g_sp.sharedHidePerp) LOG("Pursuit: hiding spot no longer shared (%s)",
                                         !theirs.hiding ? "they left it" : mine.hiding ? "we are in one ourselves" : "not both in cooldown");
        g_sp.sharedHidePerp = hidePerp;
    }

    // ---- 7. cost to state / bounty: what either of you earned counts for both ----
    {
        bool live = pursuit && (g_sp.flags & PURSUIT_ACTIVE) && !busted;
        bool joint = live && peerHere && theirs.inPursuit && !theirs.busted;
        void* perpAll = (pursuit && me) ? Find(me, IPerpetratorHandle) : nullptr;
        SharedTallyTick(pursuit, perpAll, live, joint);
    }

    // status line every 5 s while either of you is chased
    static DWORD nextLog = 0;
    if ((pursuit || (peer.valid && (peer.s.pursuitFlags & PURSUIT_ACTIVE))) && (int)(now - nextLog) > 0) {
        nextLog = now + 5000;
        uint8_t pf = peer.valid ? peer.s.pursuitFlags : 0;
        LOG("Pursuit: shared - here %s%s%s, %.1f s left, heat %.2f | there %s%s%s, %.1f s left, heat %.2f",
            StatusName(status), (g_sp.flags & PURSUIT_SEEN) ? " seen" : "",
            (g_sp.flags & PURSUIT_HIDING) ? " hiding" : (g_sp.sharedHidePerp ? " hiding (theirs)" : ""), left, g_sp.heat100 / 100.f,
            (pf & PURSUIT_ACTIVE) ? ((pf & PURSUIT_COOLDOWN) ? "cooldown" : "chase") : (pf & PURSUIT_BUSTED) ? "busted" : "none",
            (pf & PURSUIT_SEEN) ? " seen" : "", (pf & PURSUIT_HIDING) ? " hiding" : "", peer.valid ? peer.s.coolLeft10 / 10.f : 0.f, peer.valid ? peer.s.heat100 / 100.f : 0.f);
    }
}

// ---- cops of our pursuit go after whichever player is closer (SharedPursuit::CopChasesPeer) ----
// ---- the other player's pursuit strip under ours (bottom centre; peerbar.inl: a copy of the game's own) ----
// Shown while they are chased in the same place as us (free roam, or the same event), over the plain
// HUD only (not over menus, the map or a cutscene).
static void PeerBarTickImpl(bool connected, const Net::Snapshot& peer) {
    static int lastKind = -1;
    static const char* lastWhy = "";
    struct { int kind = 0, value = 0; } b;
    int forceKind = 0, forceValue = 0;
    bool forced = PeerBarForced(&forceKind, &forceValue);     // test: a fixed bar, no partner needed
    // (1.8.0 asked FEScreens::ListAll for exactly the plain HUD - never true in a pursuit, so it never showed)
    const char* why = forced ? nullptr : !g_cfg.sharedPursuits ? "SharedPursuits=0" : !connected || !g_coopLinked ? "not linked"
                    : !peer.valid ? "no state" : !peer.s.pursuitBarKind ? "they are not chased (or an older build)" : nullptr;
    if (!why) why = Game::GameFlowState() != 6 || !LocalVehicle() ? "not in the world" : NisActive() ? "a cutscene"
                    : g_sp.localBusted ? "we are busted"
                    : !forced && SharedPlaceNow(peer) == SharedPursuit::Place::None ? "not in the same place"
                    : !FEScreens::IsPushed("HUD_SingleRace.fng") ? "no HUD"
                    : FEScreens::AnyCloseableOverlay() || FEScreens::IsPushed("Pause_Main.fng") || FEScreens::IsPushed("WorldMapMain.fng") ? "a menu is open"
                    : nullptr;
    bool show = why == nullptr;
    if (!show && strcmp(why, lastWhy)) { lastWhy = why; if (peer.valid && peer.s.pursuitBarKind) LOG("Pursuit: the other player's bar not shown - %s", why); }
    if (show) lastWhy = "";
    if (show) {
        b.kind = forced ? forceKind : peer.s.pursuitBarKind; b.value = forced ? forceValue : peer.s.pursuitBar;
    }
    if (b.kind == 2) b.kind = 0;        // (shared cooldown)
    int own = 0;                         // our own meter on screen: 1 the chase, 2 the cooldown, 3 the chase + backup timer
    if (void* mp = b.kind ? PursuitOf(LocalVehicle()) : nullptr) {
        int st = PRead<int>(mp, P_Status);
        own = st == PS_COOLDOWN ? 2 : st == PS_BACKUP ? 3 : st == PS_CHASE ? 1 : 0;
    }
    // in an event with no pursuit of ours the game shows no police HUD (a race): theirs neither
    if (b.kind && !forced && own == 0 && Game::CurrentEventHash()) b.kind = 0;
    if (!PeerBarNativeSet(b.kind, b.value, own)) b.kind = 0;      // no copy of the meter (said in the log once): nothing shown
    if (b.kind != lastKind) {
        lastKind = b.kind;
        LOG("Pursuit: the other player's bar %s", b.kind == 1 ? "shown (chase)" : "hidden");
    }
}

static void PursuitTickImpl(bool connected, const Net::Snapshot& peer) {
    double now = Net::LocalClockMs();
    if (g_nextPursuitCheck - now > 1000) g_nextPursuitCheck = now;   // sim clock restarted
    if (now < g_nextPursuitCheck) return;
    g_nextPursuitCheck = now + 400;
    void* me = LocalVehicle();
    // our pursuit whether or not the other player is still there: after a disconnect the cops we sent after
    // their car must come back to us. (Up to 1.7.14 the pursuit was looked up only while connected, so at a
    // disconnect they were "released" with no pursuit to put them back on us and kept chasing the other
    // player's car - our cooldown / hiding spot then never came, HIDDEN never showed.)
    void* ownPursuit = Game::GameFlowState() == 6 ? PursuitOf(me) : nullptr;
    void* pursuit = connected ? ownPursuit : nullptr;
    void* other = g_ghost;
    {   // one status line every 5 s while either of us is being chased
        static DWORD nextLog = 0;
        int n0 = 0; if (pursuit) Mem::Read((uintptr_t)pursuit + 0x20, n0);
        if ((pursuit || (peer.valid && peer.s.pursuitHeat > 0)) && (int)(GetTickCount() - nextLog) > 0) {
            nextLog = GetTickCount() + 5000;
            int mirrors = 0; for (auto& m : g_mirrors) if (m.used && m.iv && m.n && (m.s[m.n - 1].e.flags & 1)) ++mirrors;
            LOG("Pursuit: mine %s (%d cops, %d on the other player, heat %.1f) | theirs heat %.1f, %d of their cops shown here",
                pursuit ? "yes" : "no", n0, g_rtN, Game::LocalPursuitHeat() / 10.f, peer.valid ? peer.s.pursuitHeat / 10.f : 0.f, mirrors);
        }
    }
    bool peerInPursuit = peer.valid && peer.s.hasCar && peer.s.pursuitHeat > 0;
    void* otherRb = RigidBodyOf(other);
    void* myRb = RigidBodyOf(me);
    void* sim = SimOf(other);
    if (!pursuit || !peerInPursuit || !otherRb || !myRb || !sim) { ReleaseRetargets(ownPursuit); return; }

    void** cops = nullptr; int n = 0;
    Mem::Read((uintptr_t)pursuit + 0x18, cops);
    Mem::Read((uintptr_t)pursuit + 0x20, n);
    if (!cops || n <= 0 || n > 64) { ReleaseRetargets(pursuit); return; }
    const float* op = RB_GetVec(otherRb, RB_GetPosition);
    const float* mp = RB_GetVec(myRb, RB_GetPosition);

    // decide per cop (helicopters stay on us); ground cops only
    struct C { void* cop; float dMe; bool want; } c[64]; int nc = 0;
    for (int i = 0; i < n && nc < 64; ++i) {
        void* cop = nullptr;
        if (!Mem::Read((uintptr_t)&cops[i], cop) || !IsVehicle(cop) || !VehActive(cop) || VehIsHeli(cop)) continue;
        if (VCall<bool(__thiscall*)(void*)>(cop, 31)(cop)) continue;                 // destroyed
        void* rb = RigidBodyOf(cop);
        if (!rb) continue;
        const float* p = RB_GetVec(rb, RB_GetPosition);
        float dMe = Dist(p, mp), dPeer = Dist(p, op);
        c[nc].cop = cop; c[nc].dMe = dMe;
        c[nc].want = SharedPursuit::CopChasesPeer(IsRetargeted(cop), dMe, dPeer);
        ++nc;
    }
    // at least one cop stays on us: the one closest to us
    int onUs = 0, closest = -1;
    for (int i = 0; i < nc; ++i) {
        if (!c[i].want) ++onUs;
        if (closest < 0 || c[i].dMe < c[closest].dMe) closest = i;
    }
    if (!onUs && closest >= 0) c[closest].want = false;

    // apply: back on us first, then the new ones (up to MAX_RETARGET)
    void* keep[MAX_RETARGET]; int nk = 0;
    for (int i = 0; i < g_rtN; ++i) {
        void* cop = g_rtCops[i];
        bool stays = false;
        for (int j = 0; j < nc; ++j) if (c[j].cop == cop) { stays = c[j].want; break; }
        if (stays && nk < MAX_RETARGET) keep[nk++] = cop;
        else { RetargetBack(pursuit, cop); LOG("Pursuit: cop %p back on the local player", cop); }
    }
    for (int i = 0; i < nk; ++i) g_rtCops[i] = keep[i];
    g_rtN = nk;
    for (int i = 0; i < nc && g_rtN < MAX_RETARGET; ++i) {
        if (!c[i].want || IsRetargeted(c[i].cop)) continue;
        g_rtCops[g_rtN++] = c[i].cop;
        LOG("Pursuit: cop %p goes after the other player (%.0f m from us, closer to them)", c[i].cop, c[i].dMe);
    }
    // re-apply every check: the pursuit logic may put a cop back on us
    for (int i = 0; i < g_rtN; ++i) {
        void* t = CopTarget(g_rtCops[i]);
        void* cur = nullptr;
        if (t && Mem::Read((uintptr_t)t + 0x1C, cur) && cur != sim) Aquire(t, sim);
    }
}

// ---- 3. we rammed one of the other player's cops (traffic.inl MirrorHitTick) ----
static void SharedPursuitCopHit(uint16_t mirrorId) {
    if (!g_cfg.sharedPursuits || !g_coopLinked || !g_sp.peerCopsEngage) return;   // free roam, or the same event
    DWORD now = GetTickCount();
    if (now - g_sp.lastCopHit < 3000) return;
    g_sp.lastCopHit = now;
    if (PursuitOf(LocalVehicle())) {
        g_sp.peerCopHitAt = now ? now : 1;   // counts as seen (ends our cooldown, see the pursuit tick)
        LOG("Pursuit: hit the other player's cop %u - we are chased, it counts as seeing us", (unsigned)mirrorId);
        return;
    }
    bool ok = DebugForcePursuitImpl();
    LOG("Pursuit: hit the other player's cop %u - %s", (unsigned)mirrorId, ok ? "starting a pursuit on us" : "no cop manager, no pursuit");
}

void Game::FillSharedPursuit(CarState& s) {
    s.heat100 = g_sp.heat100;
    s.pursuitFlags = g_sp.flags;
    s.bustSeq = g_sp.bustSeq;
    s.coolLeft10 = g_sp.left10;
    s.pursuitBar = g_sp.bar;
    s.pursuitBarKind = g_sp.barKind;
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
        if (!IsPeerCop(m, false) || !VehActive(m.iv) || VehIsHeli(m.iv)) continue;
        if (g_lastTrafficPkt - m.lastSeen > 500) continue;   // the other game no longer sends it (despawned there): no marker
        {   // as our own cops: only while we are chased, and once that cop has seen us (engaged)
            uint32_t since = 0;
            bool saw = PeerCopSawUs(m.id, GetTickCount(), &since);
            if (!SharedPursuit::PeerCopOnOurMap(showAll != 0, !none, saw, since)) continue;
        }
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
    if (!g_cfg.sharedPursuits || !g_coopLinked || !g_sp.peerCopsShown) return;
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
    int added = (g_cfg.sharedPursuits && g_coopLinked && g_sp.peerCopsShown) ? AddPeerCopsToList(&oldSize, saved) : 0;
    ((void(__thiscall*)(void*, uint32_t, uint32_t))RadarFn)(self, a, b);
    if (added) RemovePeerCopsFromList(oldSize, added, saved);
    if (added != g_sp.radarCops && (added || g_sp.radarCops > 0)) {
        g_sp.radarCops = added;
        LOG("Radar: %d of the other player's cops in the radar detector", added);
    }
}

bool Game::InstallPursuitHooks() {
    bool seen = PatchPursuitSeen();
    LOG("Pursuit: cops chasing the other player %s count as seeing us", seen ? "no longer" : "STILL (seen hook failed)");
    bool np1 = PatchCallChecked(NewPursuitCall1, NewPursuitFn, (void*)&hk_NewPursuitFor);
    bool np2 = PatchCallChecked(NewPursuitCall2, NewPursuitFn, (void*)&hk_NewPursuitFor);
    LOG("Pursuit: the other player's car %s a pursuit target here (new-pursuit check %s/%s)",
        np1 && np2 ? "is never" : "may still be", np1 ? "ok" : "FAIL", np2 ? "ok" : "FAIL");
    if (!g_cfg.sharedPursuits) { LOG("Pursuit: SharedPursuits=0 - each player has their own heat and pursuit"); return false; }
    bool m = PatchCallChecked(DrawCopsCall, DrawCopsFn, (void*)&hk_DrawCops);
    bool r = PatchCallChecked(RadarCall, RadarFn, (void*)&hk_RadarDetector);
    bool hs = PatchPerpHidden();
    bool hb = PatchBoardHidden();
    LOG("Pursuit: shared pursuits on (minimap cops %s, radar detector %s, shared hiding spot %s, HIDDEN on the HUD %s)",
        m ? "hooked" : "NOT hooked", r ? "hooked" : "NOT hooked", hs ? "ok" : "FAIL", hb ? "ok" : "FAIL");
    return m && r;
}
