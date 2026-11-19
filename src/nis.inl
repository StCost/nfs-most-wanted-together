// Included from game.cpp (after boot.inl).
// ---------------------------------------------------------------------------
// Cutscenes (NIS) watched together: skip voting
//  INIS::mInstance 0x9885C8; +0x2AC state (5 = playing), +0x34C scene name hash, +0x2B4 type.
//  The skip button reaches SkipOverNIS (INIS vtable +0x2C, 0x6EA960) only from 0x6EF611:
//    8B 11        mov edx,[ecx]
//    FF 52 2C     call [edx+0x2C]
//  We put a 5-byte CALL to our stub there (ecx = INIS*, result in al like the original).
//  Pressing skip casts a vote; the scene is skipped on both PCs once both voted. If the other player
//  isn't watching the same scene (or isn't connected), skipping works at once, like the game.
// ---------------------------------------------------------------------------
namespace {
    constexpr uintptr_t NisSkipSite = 0x6EF611, SkipOverNISFn = 0x6EA960;
    uint32_t g_myVote = 0, g_peerVote = 0;   // scene hash we / the peer voted to skip (0 = none)
    DWORD    g_myVoteAt = 0, g_peerVoteAt = 0;
    bool     g_nisConnected = false;

    void* Nis() { void* n = nullptr; Mem::Read(0x9885C8, n); return n; }
    int NisState(void* n) { int s = 0; if (n) Mem::Read((uintptr_t)n + 0x2AC, s); return s; }
    uint32_t NisHash(void* n) { uint32_t h = 0; if (n) Mem::Read((uintptr_t)n + 0x34C, h); return h; }
    bool SkipNow(void* n) { return ((bool(__thiscall*)(void*))SkipOverNISFn)(n); }

    bool PeerWatching(uint32_t hash) {
        const Net::Snapshot& p = Net::Latest();
        return g_nisConnected && p.valid && p.s.nisHash == hash && hash != 0;
    }

    bool __fastcall hk_NisSkip(void* nis, void*) {
        __try {
            uint32_t h = NisHash(nis);
            if (!PeerWatching(h)) return SkipNow(nis);          // alone: skip right away
            if (g_myVote != h) {
                g_myVote = h; g_myVoteAt = GetTickCount();
                Net::SendNisSkip(h);
                LOG("Cutscene: voted to skip %08X%s", h, g_peerVote == h ? "" : " - waiting for the other player");
            }
            if (g_peerVote == h) { g_myVote = g_peerVote = 0; return SkipNow(nis); }
            return false;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
}

// Our scene (hash) while one plays, for the state packet; 0 otherwise.
uint32_t Game::NisSceneHash() {
    if (!g_ok) return 0;
    __try { void* n = Nis(); return (n && NisState(n) == 5) ? NisHash(n) : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

void Game::NisTick(bool connected) {
    if (!g_ok) return;
    g_nisConnected = connected;
    __try {
        void* n = Nis();
        uint32_t h = (n && NisState(n) == 5) ? NisHash(n) : 0;
        if (g_myVote && g_myVote != h) g_myVote = 0;            // scene over
        if (g_peerVote && GetTickCount() - g_peerVoteAt > 60000) g_peerVote = 0;
        if (h && g_myVote == h && g_peerVote == h) {            // both voted (the peer's vote came last)
            g_myVote = g_peerVote = 0;
            LOG("Cutscene: both voted - skipping");
            SkipNow(n);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

const char* Game::NisStatus() {
    if (g_myVote && !g_peerVote) return "skip voted - waiting for the other player";
    if (g_peerVote && !g_myVote) return "the other player wants to skip (press skip)";
    return "";
}

bool Game::InstallNisHook() {
    Net::OnNisSkip = [](uint32_t h) { g_peerVote = h; g_peerVoteAt = GetTickCount(); LOG("Cutscene: the other player voted to skip %08X", h); };
    uint8_t expect[5] = { 0x8B, 0x11, 0xFF, 0x52, 0x2C };
    uint8_t patch[5] = { 0xE8, 0, 0, 0, 0 };
    *(int32_t*)(patch + 1) = (int32_t)((uintptr_t)&hk_NisSkip - (NisSkipSite + 5));
    bool ok = Mem::PatchBytes(NisSkipSite, expect, patch, 5);
    LOG("Cutscene: skip voting %s", ok ? "hooked" : "NOT hooked (bytes differ)");
    return ok;
}
