// Included from game.cpp (after boot.inl).
// ---------------------------------------------------------------------------
// Cutscenes (NIS) watched together: skip voting
//  INIS::mInstance 0x9885C8; +0x2AC state (3 loaded, 4/6 pre/post movie, 5 playing, 7 skipped),
//  +0x34C scene name hash. The skip button reaches SkipOverNIS (INIS vtable +0x2C, 0x6EA960, thiscall,
//  no stack arguments, only skips in state 5) from one place, 0x6EF611:
//    8B 11        mov edx,[ecx]
//    FF 52 2C     call [edx+0x2C]
//  We put a 5-byte CALL to our stub there (ecx = INIS*).
//  Votes travel as state (CarState::nisHash = scene I'm in, nisVote = scene I want skipped), so a lost
//  packet can't lose a vote and a vote ends with its scene. A scene both players are in (or that the
//  other one is still loading into) is skipped when both voted; alone, skip works like in the game.
// ---------------------------------------------------------------------------
namespace {
    constexpr uintptr_t NisSkipSite = 0x6EF611, SkipOverNISFn = 0x6EA960;
    uint32_t g_myVote = 0;        // scene I voted to skip
    uint32_t g_voteHold = 0;      // keep advertising the vote briefly after skipping, so the peer sees it
    DWORD    g_voteHoldUntil = 0;
    bool     g_nisConnected = false;
    DWORD    g_peerGoneSince = 0; // the other player has been past my voted scene since (0 = isn't)

    void* Nis() { void* n = nullptr; Mem::Read(0x9885C8, n); return n; }
    int NisState(void* n) { int s = 0; if (n) Mem::Read((uintptr_t)n + 0x2AC, s); return s; }
    // the scene we're in (loaded .. post movie), 0 = none
    uint32_t NisScene(void* n) {
        int s = NisState(n);
        uint32_t h = 0;
        if (n && s >= 3 && s <= 6) Mem::Read((uintptr_t)n + 0x34C, h);
        return h;
    }
    uint32_t SkipNow(void* n) { return ((bool(__thiscall*)(void*))SkipOverNISFn)(n) ? 1u : 0u; }
    void Skipped(uint32_t h) { g_voteHold = h; g_voteHoldUntil = GetTickCount() + 2000; g_myVote = 0; }

    // is the other player in this scene too (or still loading, about to be)?
    bool PeerInScene(uint32_t h) {
        const Net::Snapshot& p = Net::Latest();
        if (!g_nisConnected || !p.valid || !h) return false;
        return p.s.nisHash == h || p.s.gameFlowState == 4 || p.s.gameFlowState == 5;
    }
    bool PeerVoted(uint32_t h) {
        const Net::Snapshot& p = Net::Latest();
        return g_nisConnected && p.valid && h && p.s.nisVote == h;
    }

    // full eax result: the caller may test more than al
    uint32_t __fastcall hk_NisSkip(void* nis, void*) {
        __try {
            uint32_t h = NisScene(nis);
            if (!h || NisState(nis) != 5 || !PeerInScene(h)) return SkipNow(nis);     // alone: like the game
            if (PeerVoted(h)) { uint32_t r = SkipNow(nis); if (r) { Skipped(h); LOG("Cutscene: both voted - skipping"); } return r; }
            if (g_myVote != h) { g_myVote = h; LOG("Cutscene: voted to skip %08X - waiting for the other player", h); }
            return 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }
}

// For the state packet: the scene we're in, and the scene we want skipped.
uint32_t Game::NisSceneHash() {
    if (!g_ok) return 0;
    __try { return NisScene(Nis()); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
uint32_t Game::NisVoteHash() {
    if (g_myVote) return g_myVote;
    return (g_voteHold && (int)(g_voteHoldUntil - GetTickCount()) > 0) ? g_voteHold : 0;
}

void Game::NisTick(bool connected) {
    if (!g_ok) return;
    g_nisConnected = connected;
    __try {
        void* n = Nis();
        uint32_t h = NisScene(n);
        uint32_t cur = Game::SceneHash();                                   // the NIS, or a story movie (story.inl)
        if (g_myVote && (g_myVote != cur || !connected)) g_myVote = 0;     // scene over / alone again
        // a vote on a story movie is handled in story.inl
        if (!g_myVote || g_myVote != h || NisState(n) != 5) { g_peerGoneSince = 0; return; }   // a new vote starts its own wait
        // I voted earlier: skip once the other player votes too, or has already left the scene (skipped)
        const Net::Snapshot& p = Net::Latest();
        bool peerGone = p.valid && p.s.nisHash != h && p.s.gameFlowState != 4 && p.s.gameFlowState != 5;
        if (!peerGone) g_peerGoneSince = 0; else if (!g_peerGoneSince) g_peerGoneSince = GetTickCount();
        if (PeerVoted(h) || (peerGone && GetTickCount() - g_peerGoneSince > 500)) {
            // (log only a skip that happened: a refused one is tried again every frame)
            if (SkipNow(n)) { Skipped(h); LOG("Cutscene: %s - skipping", PeerVoted(h) ? "both voted" : "the other player is past it"); }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

const char* Game::NisStatus() {
    __try {
        uint32_t h = Game::SceneHash();
        if (!h || !g_nisConnected) return "";
        if (g_myVote == h) return "skip voted - waiting for the other player";
        if (PeerVoted(h)) return "the other player wants to skip (press skip)";
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return "";
}

bool Game::InstallNisHook() {
    uint8_t expect[5] = { 0x8B, 0x11, 0xFF, 0x52, 0x2C };
    uint8_t patch[5] = { 0xE8, 0, 0, 0, 0 };
    *(int32_t*)(patch + 1) = (int32_t)((uintptr_t)&hk_NisSkip - (NisSkipSite + 5));
    bool ok = Mem::PatchBytes(NisSkipSite, expect, patch, 5);
    LOG("Cutscene: skip voting %s", ok ? "hooked" : "NOT hooked (bytes differ)");
    return ok;
}

// Test command: press "skip" on the current cutscene, exactly as the game's skip button would.
static const char* DebugNisSkipSafe() {
    __try {
        void* n = Nis();
        if (!n || !NisScene(n)) return "no cutscene";
        return hk_NisSkip(n, nullptr) ? "skipped" : "vote cast (or not skippable yet)";
    } __except (EXCEPTION_EXECUTE_HANDLER) { return "exception"; }
}
std::string Game::DebugNisSkip() { return g_ok ? DebugNisSkipSafe() : "game not recognised"; }