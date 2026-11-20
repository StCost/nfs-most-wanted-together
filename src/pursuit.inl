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

void Game::PursuitTick(bool connected, const Net::Snapshot& peer) {
    if (!g_ok) return;
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
