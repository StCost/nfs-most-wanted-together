// Included from game.cpp (after coopwin.inl).
// ---------------------------------------------------------------------------
// Spectate (spectateutil.h for the rules)
//  The losing screen: ShowPostRace(false) 0x60AC20 posts EPause(0, 1, 0) (ctor 0x623A90, 0x14 bytes from
//  the event pool 0x627400): Pause_Main.fng with its "race over" flag (screen +0x148), Restart / Quit.
//  nativeui.cpp adds Spectate there (UIPauseMain::Setup 0x546360 -> 0x52B4D0 builds the icons).
//  Picked: EUnPause (ctor 0x625870, 8 bytes) closes it, as the game's own Start does (0x545520).
//  The camera: CameraAI directors 0x911300 (count 0x911308), +8 view (1 = player 1), +0x20 action.
//  The drive camera CDActionDrive (vtable 0x895150, ctor 0x484C20) follows the IPlayer at +0x44
//  through its slot 1 (GetSimable); its update (0x4725D0) rebinds when that car changes. So for
//  watching, +0x44 points at a stand-in IPlayer whose slot 1 returns the other player's car and whose
//  other slots go to ours. The camera views (C / the pad's camera button) are the drive camera's own.
//  Not the drive camera (a wreck / busted camera): the director is asked for CDActionDrive as its own
//  update does (0x47992B: +0x10/+0x14 = 0x4522F0(name), +0x18 = 0x4519D0(name), +0x1C name, +0x2BC = 1).
// ---------------------------------------------------------------------------
namespace {
    constexpr uintptr_t kDirectors = 0x911300, kDirectorCount = 0x911308, kVtDriveAction = 0x895150;
    const char* const kDriveActionName = (const char*)0x894C20;     // "CDActionDrive"

    bool     g_spectating = false;
    uint32_t g_specEvent = 0;
    DWORD    g_specSince = 0;
    void*    g_specOrigPlayer = nullptr;      // our IPlayer the drive camera had
    void*    g_specAction = nullptr;          // the drive camera we changed
    void**   g_specVt = nullptr;              // the stand-in's vtable (thunks to ours)
    void*    g_specProxy[1] = { nullptr };    // the stand-in object: just its vtable
    constexpr int kSpecSlots = 48;

    void* PartnerSimable() {
        if (!g_ghost || g_ghostKind != GK_RACER || !IsVehicle(g_ghost)) return nullptr;
        return VCall<void* (__thiscall*)(void*)>(g_ghost, IV_GetSimable)(g_ghost);
    }
    void* __fastcall SpecGetSimable(void*, void*) {
        void* s = nullptr;
        __try { s = PartnerSimable(); } __except (EXCEPTION_EXECUTE_HANDLER) { s = nullptr; }
        if (s) return s;
        void* p = g_specOrigPlayer;           // their car is gone: ours again
        return p ? VCall<void* (__thiscall*)(void*)>(p, 1)(p) : nullptr;
    }
    // the stand-in's vtable: slot i = "mov ecx, [g_specOrigPlayer]; jmp [ourVtable + 4*i]", slot 1 ours
    bool BuildProxy(void* player) {
        void** vt = *(void***)player;
        if (!g_specVt) {
            uint8_t* code = (uint8_t*)VirtualAlloc(nullptr, kSpecSlots * 12, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            g_specVt = (void**)VirtualAlloc(nullptr, kSpecSlots * sizeof(void*), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (!code || !g_specVt) return false;
            for (int i = 0; i < kSpecSlots; ++i) {
                uint8_t* t = code + i * 12;
                t[0] = 0x8B; t[1] = 0x0D; *(void***)(t + 2) = (void**)&g_specOrigPlayer;   // mov ecx, [g_specOrigPlayer]
                t[6] = 0xFF; t[7] = 0x25; *(void***)(t + 8) = &vt[i];                         // jmp [vt + 4*i]
                g_specVt[i] = t;
            }
            g_specVt[1] = (void*)&SpecGetSimable;
        }
        g_specProxy[0] = g_specVt;
        return true;
    }
    void* DriveDirector(void** actionOut) {
        void** list = nullptr; int n = 0;
        if (!Mem::Read(kDirectors, list) || !Mem::Read(kDirectorCount, n) || !list || n <= 0 || n > 8) return nullptr;
        for (int i = 0; i < n; ++i) {
            uint8_t* d = (uint8_t*)list[i];
            int view = 0; void* act = nullptr;
            if (!d || !Mem::Read((uintptr_t)d + 8, view) || view != 1) continue;
            Mem::Read((uintptr_t)d + 0x20, act);
            *actionOut = act;
            return d;
        }
        return nullptr;
    }
    void AskForDriveCamera(uint8_t* dir) {
        uint64_t h64 = ((uint64_t(__cdecl*)(const char*))0x4522F0)(kDriveActionName);
        uint32_t h32 = ((uint32_t(__cdecl*)(const char*))0x4519D0)(kDriveActionName);
        *(uint64_t*)(dir + 0x10) = h64;
        *(uint32_t*)(dir + 0x18) = h32;
        *(const char**)(dir + 0x1C) = kDriveActionName;
        *(uint8_t*)(dir + 0x2BC) = 1;
    }
    void RestoreCamera() {
        if (g_specAction && g_specOrigPlayer) {
            void* act = nullptr;
            DriveDirector(&act);
            void* cur = nullptr;
            if (act == g_specAction && VTable(act) == kVtDriveAction && Mem::Read((uintptr_t)act + 0x44, cur) && cur == (void*)g_specProxy)
                Mem::Write((uintptr_t)act + 0x44, g_specOrigPlayer);
        }
        g_specAction = nullptr;
    }
    void CameraTick() {
        void* act = nullptr;
        uint8_t* dir = (uint8_t*)DriveDirector(&act);
        if (!dir) return;
        if (!act || VTable(act) != kVtDriveAction) {
            static DWORD next = 0;
            if (GetTickCount() >= next) { next = GetTickCount() + 500; AskForDriveCamera(dir); }
            return;
        }
        void* cur = nullptr;
        if (!Mem::Read((uintptr_t)act + 0x44, cur) || !cur) return;
        if (cur == (void*)g_specProxy) return;                       // already watching
        g_specOrigPlayer = cur;
        if (!BuildProxy(cur)) return;
        void* proxy = (void*)g_specProxy;
        Mem::Write((uintptr_t)act + 0x44, proxy);
        g_specAction = act;
        LOG("Spectate: the camera follows the other player's car");
    }

    Spectate::Peer SpecPeer() {
        const Net::Snapshot& p = Net::Latest();
        Spectate::Peer r{};
        r.connected = Net::Stats().connected && g_coopLinked && p.valid;
        uint32_t ev = Game::CurrentEventHash();
        r.sameEvent = p.valid && ev && p.s.eventHash == ev;
        r.inRace = p.valid && p.s.inRace;
        r.finished = p.valid ? p.s.finished : 0;
        r.out = p.valid && p.s.pad[0] == 3;
        r.racedHere = g_peerRacedHere;
        return r;
    }
    void StopSpectating(const char* why) {
        RestoreCamera();
        g_spectating = false;
        LOG("Spectate: stopped (%s)", why);
    }
}

bool Game::SpectateOffered() {
    if (!g_ok || g_spectating) return false;
    __try { return g_cfg.sharedWins && Spectate::Offer(g_coopLinked, LocalRacerOut(), SpecPeer()) && PartnerSimable() != nullptr; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool SpectateStartImpl() {
    __try {
        if (!Game::SpectateOffered()) return false;
        void* ev = ((void*(__cdecl*)(uint32_t))0x627400)(8);         // EUnPause: the screen goes, the world shows
        if (ev) ((void*(__thiscall*)(void*))0x625870)(ev);
        g_spectating = true; g_specEvent = Game::CurrentEventHash(); g_specSince = GetTickCount();
        LOG("Spectate: watching the other player");
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_spectating = false; return false; }
}
void Game::SpectatePicked() {
    if (SpectateStartImpl()) Game::QueueNotice("Watching your partner - pause for Restart / Quit", false);
}

static void SpectateTickImpl() {
    if (!g_spectating) return;
    uint8_t restart = 0; Mem::Read(0x91CF28, restart);
    Spectate::Peer p = SpecPeer();
    Spectate::Next n = Spectate::Tick(p, restart != 0, Game::CurrentEventHash() != g_specEvent || Game::GameFlowState() != 6,
                                      PeerWonThisEvent());
    if (n == Spectate::Next::Keep) { CameraTick(); return; }
    if (n == Spectate::Next::EndQuiet) { StopSpectating("race restarted or left"); return; }
    if (n == Spectate::Next::EndWon) {
        StopSpectating("the other player won");
        if (void* sim = LocalSimable()) {
            g_forcedFinishTried = true; g_forcedFinishEvent = Game::CurrentEventHash();
            ((void(__cdecl*)(void*))0x6119F0)(sim);                   // NotifyRaceFinished: the win gate counts their win
        }
        bool won = CoopWonImpl(RacerInfoOfLocal());
        LOG("Spectate: their win counts for us too - %s", won ? "the winner's screen" : "not counted (shared wins off / scripted loss)");
        ((void(__cdecl*)(bool))0x60AC20)(won);                       // ShowPostRace
        return;
    }
    StopSpectating("the other player is out too");
    ((void(__cdecl*)(bool))0x60AC20)(false);                         // our Restart / Quit again
}
void Game::SpectateTick() {
    if (!g_ok) return;
    __try { SpectateTickImpl(); } __except (EXCEPTION_EXECUTE_HANDLER) { g_spectating = false; LOG("Spectate: exception"); }
}
bool Game::Spectating() { return g_spectating; }
