// Included from game.cpp (after save.inl).
// ---------------------------------------------------------------------------
// Shared wins: when either player finishes an event first, both games count it as won
// (cash, unlocks, "event won" flag, winning post-race flow), each in its own career.
//  NotifyRaceFinished 0x6119F0 pays out only if the finishing local player is ranked 1st:
//    0x611B11  39 6B 10             cmp [ebx+0x10], ebp     ; ebx = local GRacerInfo, ebp = 1
//              0F 85 B4 01 00 00    jne 0x611CCE            ; not first: no payout
//    -> jmp to a stub that asks CoopWinCheck(ebx) and continues at 0x611B1A (win) / 0x611CCE.
//  GRaceStatus "who won" 0x5EB4E0 (thiscall, no args; returns the local GRacerInfo that finished
//    first, else NULL; UpdateRaceScore 0x602320 needs non-NULL to mark the event won) -> replaced.
//  ShowLosingPostRaceScreen 0x60E230 (6A 00 E8 E9 C9 FF FF 59 C3 = ShowPostRace(false)) ->
//    ShowPostRace(co-op won).
//  The other player's racer never "finishes" in our game by itself: when they report finishing, we
//  mark it finished (0x5E97B0, thiscall GRacerInfo), set 100 %, and re-sort (SortRacers 0x5FDE80).
//  The sender's CarState::finished carries its own rank at the finish (0 = still racing).
// ---------------------------------------------------------------------------
namespace {
    bool g_coopWinLogged = false;
    bool g_coopWinNotice = false;   // main.cpp shows a notice once

    void* GhostRacerInfo() { return (g_ghost && g_ghostKind == GK_RACER) ? RacerInfoOf(g_ghost) : nullptr; }

    bool CoopWonImpl(void* localRi) {
        int rank = 0;
        if (localRi && Mem::Read((uintptr_t)localRi + 0x10, rank) && rank == 1) return true;
        if (!g_cfg.sharedWins || !Net::Stats().connected) return false;
        const Net::Snapshot& p = Net::Latest();
        if (!p.valid || !p.s.inRace || !p.s.eventHash || p.s.eventHash != Game::CurrentEventHash()) return false;   // same event only
        void* gri = GhostRacerInfo();
        if (!gri) return false;                                  // the peer isn't in our race
        uint8_t fin = 0; int grank = 0;
        Mem::Read((uintptr_t)gri + 0x21, fin); Mem::Read((uintptr_t)gri + 0x10, grank);
        bool won = p.s.finished == 1 || (fin && grank == 1);     // they say they won / they won here
        if (won && !g_coopWinLogged) { g_coopWinLogged = true; g_coopWinNotice = true; LOG("Race: the other player won - counted as our win too"); }
        return won;
    }
    bool __cdecl CoopWinCheck(void* localRi) {
        __try { return CoopWonImpl(localRi); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    __declspec(naked) void hk_WinGate() {
        __asm {
            pushad
            push ebx
            call CoopWinCheck
            add esp, 4
            test al, al
            popad                      ; popad keeps the flags
            jz lose
            push 0x611B1A
            ret
        lose:
            push 0x611CCE
            ret
        }
    }

    void* __fastcall hk_WhoWon(void* raceStatus, void*) {
        __try {
            void* ri = RacerInfoOfLocal();
            uint8_t fin = 0;
            if (ri && Mem::Read((uintptr_t)ri + 0x21, fin) && fin && CoopWonImpl(ri)) return ri;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return nullptr;
    }

    void __cdecl hk_ShowLosing() {
        bool won = false;
        __try { won = CoopWonImpl(RacerInfoOfLocal()); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        ((void(__cdecl*)(bool))0x60AC20)(won);                   // ShowPostRace
    }

    bool PatchJmp(uintptr_t at, const uint8_t* expect, size_t len, void* to) {
        uint8_t patch[16];
        memset(patch, 0x90, sizeof(patch));
        patch[0] = 0xE9;
        *(int32_t*)(patch + 1) = (int32_t)((uintptr_t)to - (at + 5));
        return Mem::PatchBytes(at, expect, patch, len);
    }
}

// Rank of the local racer once finished, for the state packet (0 = still racing / not in a race).
uint8_t Game::LocalFinishRank(void* localRi) {
    uint8_t fin = 0; int rank = 0;
    if (!localRi || !Mem::Read((uintptr_t)localRi + 0x21, fin) || !fin) return 0;
    Mem::Read((uintptr_t)localRi + 0x10, rank);
    return (uint8_t)(rank < 1 ? 1 : rank > 255 ? 255 : rank);
}

static void CoopWinTickImpl(bool connected, const Net::Snapshot& peer) {
    bool sameEvent = peer.valid && peer.s.eventHash && peer.s.eventHash == Game::CurrentEventHash();
    void* gri = (connected && sameEvent && peer.s.inRace) ? GhostRacerInfo() : nullptr;
    if (!gri) { g_coopWinLogged = false; return; }
    uint8_t fin = 0;
    if (!peer.s.finished || !Mem::Read((uintptr_t)gri + 0x21, fin) || fin) return;
    void* rs = nullptr;
    if (!Mem::Read(0x91E000, rs) || !rs) return;
    ((void(__thiscall*)(void*))0x5E97B0)(gri);                   // finished + race timer frozen
    float full = 100.f; Mem::Write((uintptr_t)gri + 0x18, full);
    ((void(__thiscall*)(void*))0x5FDE80)(rs);                    // SortRacers
    int rank = 0; Mem::Read((uintptr_t)gri + 0x10, rank);
    LOG("Race: the other player finished (rank %d there, %d here)", (int)peer.s.finished, rank);
}

void Game::CoopWinTick(bool connected, const Net::Snapshot& peer) {
    if (!g_ok || GameFlowState() != 6) return;
    __try { CoopWinTickImpl(connected, peer); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

bool Game::InstallCoopWin() {
    if (!g_cfg.sharedWins) return false;
    const uint8_t gate[9] = { 0x39, 0x6B, 0x10, 0x0F, 0x85, 0xB4, 0x01, 0x00, 0x00 };
    const uint8_t who[5]  = { 0xA1, 0x54, 0xD8, 0x92, 0x00 };
    const uint8_t lose[9] = { 0x6A, 0x00, 0xE8, 0xE9, 0xC9, 0xFF, 0xFF, 0x59, 0xC3 };
    // all three or none: a half-installed set would pay out without marking the event won
    if (memcmp((void*)0x611B11, gate, 9) || memcmp((void*)0x5EB4E0, who, 5)) {
        LOG("Career: shared wins NOT installed (code differs)");
        return false;
    }
    bool a = PatchJmp(0x611B11, gate, 9, (void*)&hk_WinGate);
    bool b = PatchJmp(0x5EB4E0, who, 5, (void*)&hk_WhoWon);
    // The losing post-race screen is NOT swapped for the winning one: the race script that asked for it
    // then waits for the losing screen's Continue and the player is stuck at the finish (seen on two PCs).
    // The win is still paid and recorded; the standings table shows who finished first.
    bool c = true; (void)&hk_ShowLosing;
    LOG("Career: shared wins %s/%s/%s", a ? "ok" : "FAIL", b ? "ok" : "FAIL", c ? "ok" : "FAIL");
    return a && b && c;
}

// ---- test commands ----
static int StopOpponentsSafe() {
    int n = 0;
    __try {
        void** b = nullptr;
        int cnt = ListSize(VL_AIRACERS, &b);
        for (int i = 0; i < cnt; ++i) {
            void* r = ListGet(VL_AIRACERS, i);
            if (r && r != g_ghost && !IsMirrorCar(r) && DisableAI(r)) ++n;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return n;
}
std::string Game::DebugStopOpponents() {
    if (!g_ok || GameFlowState() != 6) return "not in game";
    char b[64]; sprintf_s(b, "%d opponent(s) stopped driving", StopOpponentsSafe());
    return b;
}
static void CashSafe(char* out, size_t n) {
    __try {
        uint8_t* fedb = nullptr; uint8_t* up = nullptr; int cash = 0;
        if (!Mem::Read(0x91CF90, fedb) || !fedb || !Mem::Read((uintptr_t)fedb + 0x10, up) || !up) { strcpy_s(out, n, "no profile"); return; }
        Mem::Read((uintptr_t)up + 0xB4, cash);
        int won = -1; void* rs = nullptr; void* parms = nullptr; void* db = nullptr;
        if (Mem::Read(0x91E000, rs) && rs && Mem::Read((uintptr_t)rs + 0x1968, parms) && parms && Mem::Read(0x91E004, db) && db) {
            uint32_t h = ((uint32_t(__thiscall*)(void*))0x5FAB40)(parms);
            won = ((bool(__thiscall*)(void*, uint32_t, int))0x5DCA70)(db, h, 2) ? 1 : 0;
        }
        sprintf_s(out, n, "cash %d, current event won flag %d", cash, won);
    } __except (EXCEPTION_EXECUTE_HANDLER) { strcpy_s(out, n, "exception"); }
}
std::string Game::DebugCash() { char b[96]; CashSafe(b, sizeof(b)); return b; }
bool Game::TakeCoopWinNotice() { bool v = g_coopWinNotice; g_coopWinNotice = false; return v; }
