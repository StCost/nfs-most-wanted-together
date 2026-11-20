// Included from game.cpp (after nis.inl).
// ---------------------------------------------------------------------------
// Story scenes outside events ([Coop] SyncStoryScenes)
//  The career shows its story in three ways:
//   - in-game movies (FMV over the world): PlayInGameMovie 0x57E470 (cdecl, name) copies the name to
//     0x8F3F40, sets the "movie playing" byte 0x91CF68 and pushes InGameAnyMovie.fng (screen vtable
//     0x8A0FC8; a two-movie variant 0x8A0FD4 reads its name from 0x8F3F80). The byte is cleared when
//     the movie ends or is skipped (0x56B5C0 / 0x56B6A0, screen destructors). Used by EPlayRaceMovie
//     (0x62F820), e.g. the movie an in-world NIS ends with (INIS+0x354).
//   - front-end movies in the Safe House (Blacklist rival movies "blacklist_%02d" from the career
//     sequence 0x5302C0, storyfmv_rap30 / saf25): name in 0x8F3750, FEAnyMovie.fng (vtable 0x89E3C4).
//   - in-world NIS scenes (NIS manager, nis.inl). Outside events only the scenes in kStoryNis count
//     (arrests, pursuit breakers and replays are not story).
//  Movie names are bare ("blacklist_05", "storyfmv_pin11"); we send bStringHash (0x460BF0: h*33+c,
//  start -1) of the lower-case name in CarState::nisHash, the career rival number (+0xB0) in pad[2].
//  The movie screens' notification handler (vtable slot 1) gets PAD_ACCEPT / PAD_START to skip:
//  we hook that slot for the skip vote (skip when both voted, as for race intro cutscenes).
//  The other player's game waits meanwhile: the front-end pause request counter 0x91CAE4 (as the
//  start-together hold in event.inl), and with SyncStoryScenes=2 plays the same movie when its own
//  career has reached it.
// ---------------------------------------------------------------------------
namespace {
    constexpr uintptr_t MoviePlayingFlag = 0x91CF68, InGameMovieName = 0x8F3F40, InGameMovieName2 = 0x8F3F80,
                        FEMovieName = 0x8F3750, PlayInGameMovieFn = 0x57E470, PauseRequests = 0x91CAE4,
                        InGameMovieVt = 0x8A0FC8, InGameMovie2Vt = 0x8A0FD4, FEMovieVt = 0x89E3C4;
    constexpr uint32_t PadAccept = 0x406415E3, PadStart = 0xB5AF2461;

    const char* const kStoryMovies[] = {
        "blacklist_01", "blacklist_02", "blacklist_03", "blacklist_04", "blacklist_05", "blacklist_06",
        "blacklist_07", "blacklist_08", "blacklist_09", "blacklist_10", "blacklist_11", "blacklist_12",
        "blacklist_13", "blacklist_14", "blacklist_15",
        "storyfmv_bla134", "storyfmv_bus12", "storyfmv_cro06_coh06a", "storyfmv_dda01", "storyfmv_epi138",
        "storyfmv_her136", "storyfmv_pin11", "storyfmv_pol17_mot21", "storyfmv_rap30", "storyfmv_raz08",
        "storyfmv_roc02", "storyfmv_saf25", "storyfmv_rac01",
    };
    // in-world story NIS (NIS\Scene_<name>_BundleB.bun); the name case the game hashes is not known,
    // so each is matched as written, upper and lower case
    const char* const kStoryNis[] = {
        "FMV17to21", "FMV20T1", "FMVBLIntro", "FMVNoCars", "FMVPIN11X06", "FMVPSA", "FMVroc02",
        "EndingNis01", "EndingNis02", "EndingNis03", "EndingNis04",
    };
    constexpr int kMovies = sizeof(kStoryMovies) / sizeof(kStoryMovies[0]);
    constexpr int kNis = sizeof(kStoryNis) / sizeof(kStoryNis[0]);
    uint32_t g_movieHashes[kMovies], g_nisHashes[kNis * 3];
    bool     g_hashesReady = false;

    uint32_t BHash(const char* s) { uint32_t h = 0xFFFFFFFFu; while (*s) h = h * 33 + (uint8_t)*s++; return h; }
    void InitStoryHashes() {
        if (g_hashesReady) return;
        for (int i = 0; i < kMovies; ++i) g_movieHashes[i] = BHash(kStoryMovies[i]);
        for (int i = 0; i < kNis; ++i) {
            char up[32], lo[32];
            strcpy_s(up, kStoryNis[i]); strcpy_s(lo, kStoryNis[i]);
            _strupr_s(up); _strlwr_s(lo);
            g_nisHashes[i * 3] = BHash(kStoryNis[i]); g_nisHashes[i * 3 + 1] = BHash(up); g_nisHashes[i * 3 + 2] = BHash(lo);
        }
        g_hashesReady = true;
    }
    const char* KnownMovie(uint32_t h) {
        InitStoryHashes();
        for (int i = 0; h && i < kMovies; ++i) if (g_movieHashes[i] == h) return kStoryMovies[i];
        return nullptr;
    }
    bool StoryNis(uint32_t h) {
        InitStoryHashes();
        for (int i = 0; h && i < kNis * 3; ++i) if (g_nisHashes[i] == h) return true;
        return false;
    }

    // ---- the movie this game shows right now ----
    char        g_movieName[64] = "";
    uint32_t    g_movieHash = 0;          // story movie playing here (0 = none / not a story movie)
    bool        g_movieInGame = false;    // over the world (pauses it) or in the Safe House front end
    const char* g_moviePkg = nullptr;     // its front-end package (to send it the skip button)
    uintptr_t   g_lastMovieVt = 0;        // vtable of the movie screen that last got a message
    DWORD       g_lastMoviePoll = 0;

    // "MOVIES\Blacklist_05.vp6" -> "blacklist_05"
    void NormalizeMovie(const char* src, char* out, size_t n) {
        const char* s = src;
        for (const char* p = src; *p; ++p) if (*p == '\\' || *p == '/') s = p + 1;
        size_t i = 0;
        for (; s[i] && i + 1 < n; ++i) { char c = s[i]; out[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
        out[i] = 0;
        if (char* dot = strstr(out, ".vp6")) *dot = 0;
    }
    bool IsStoryMovieName(const char* s) { return !strncmp(s, "blacklist_", 10) || !strncmp(s, "storyfmv_", 9); }

    void PollMovieRaw() {
        uint8_t playing = 0;
        Mem::Read(MoviePlayingFlag, playing);
        static const char* const pkgs[4] = { "InGameAnyMovie.fng", "WS_InGameAnyMovie.fng", "FEAnyMovie.fng", "WS_FEAnyMovie.fng" };
        g_moviePkg = nullptr;
        for (const char* p : pkgs) if (FEScreens::IsPushed(p)) { g_moviePkg = p; break; }
        char raw[64] = "";
        uintptr_t src = 0;
        if (playing) src = g_lastMovieVt == InGameMovie2Vt ? InGameMovieName2 : InGameMovieName;
        else if (g_moviePkg && strstr(g_moviePkg, "FEAnyMovie")) src = FEMovieName;
        if (src && Mem::SafeRead((const void*)src, raw, sizeof(raw) - 1)) raw[sizeof(raw) - 1] = 0; else raw[0] = 0;
        NormalizeMovie(raw, g_movieName, sizeof(g_movieName));
        g_movieInGame = playing != 0;
        g_movieHash = (src && g_movieName[0] && IsStoryMovieName(g_movieName)) ? BHash(g_movieName) : 0;
    }
    void PollMovie(bool force) {
        DWORD now = GetTickCount();
        if (!force && now - g_lastMoviePoll < 200) return;
        g_lastMoviePoll = now;
        __try { PollMovieRaw(); } __except (EXCEPTION_EXECUTE_HANDLER) { g_movieHash = 0; }
    }

    uint8_t CareerBinRaw() {
        uint8_t* fedb = nullptr; uint8_t* up = nullptr;
        if (!Mem::Read(0x91CF90, fedb) || !fedb || !fedb[3] || !Mem::Read((uintptr_t)fedb + 0x10, up) || !up) return 0;
        return up[0xB0];
    }

    // my story scene (0 = none): a story movie, or a story NIS outside events
    uint32_t MyStoryScene() {
        if (g_movieHash) return g_movieHash;
        uint32_t n = Game::NisSceneHash();
        return (n && !Game::CurrentEventHash() && StoryNis(n)) ? n : 0;
    }
    uint32_t PeerStoryScene() {
        const Net::Snapshot& p = Net::Latest();
        if (!p.valid || !p.s.nisHash || p.s.eventHash) return 0;
        return (KnownMovie(p.s.nisHash) || StoryNis(p.s.nisHash)) ? p.s.nisHash : 0;
    }

    // ---- skip vote on movies ----
    using MovieNotifyFn = void(__thiscall*)(void*, uint32_t, void*, uint32_t, uint32_t);
    MovieNotifyFn g_movieNotify[3] = {};
    uint32_t g_movieForceSkip = 0;        // let this skip through (the other player has skipped / voted)

    bool MovieMessageAllowed(void* self, uint32_t msg) {
        __try {
            uintptr_t vt = 0; Mem::Read((uintptr_t)self, vt);
            g_lastMovieVt = vt;
            if (msg != PadAccept && msg != PadStart) return true;
            if (g_cfg.syncStoryScenes <= 0 || !g_nisConnected) return true;
            PollMovie(true);
            uint32_t h = g_movieHash;
            const Net::Snapshot& p = Net::Latest();
            if (!h || !p.valid || p.s.nisHash != h) return true;               // watching alone: like the game
            if (p.s.nisVote == h || g_movieForceSkip == h) {
                LOG("Story: %s - skipping %s", g_movieForceSkip == h ? "the other player is past it" : "both voted", g_movieName);
                g_movieForceSkip = 0; Skipped(h);
                return true;
            }
            if (g_myVote != h) { g_myVote = h; LOG("Story: voted to skip %s - waiting for the other player", g_movieName); }
            return false;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return true; }
    }
    template <int I>
    void __fastcall hk_MovieNotify(void* self, void*, uint32_t msg, void* obj, uint32_t a, uint32_t b) {
        if (MovieMessageAllowed(self, msg)) g_movieNotify[I](self, msg, obj, a, b);
    }

    // ---- the other player watches: wait, or watch along ----
    bool     g_storyHold = false;
    DWORD    g_storyHoldSince = 0, g_peerStorySince = 0, g_startAt = 0, g_startedAt = 0, g_doneUntil = 0;
    uint32_t g_peerStory = 0, g_startedFor = 0, g_startPending = 0;
    DWORD    g_lastSkipSend = 0, g_peerLeftSince = 0;
    char     g_storyStatus[128] = "";

    const char* PeerName() { const std::string& n = Net::Stats().peerName; return n.empty() ? "The other player" : n.c_str(); }

    void StoryHoldOn() {
        if (g_storyHold) return;
        int req = 0;
        if (!Mem::Read(PauseRequests, req)) return;
        ++req; Mem::Write(PauseRequests, req);
        g_storyHold = true; g_storyHoldSince = GetTickCount();
        LOG("Story: the other player is watching a story scene (%08X) - holding the world", g_peerStory);
    }
    void StoryHoldOff(const char* why) {
        if (!g_storyHold) return;
        g_storyHold = false;
        int req = 0;
        if (Mem::Read(PauseRequests, req) && req > 0) { --req; Mem::Write(PauseRequests, req); }
        LOG("Story: hold released (%s, %.0f s)", why, (GetTickCount() - g_storyHoldSince) / 1000.f);
    }

    bool MovieFileExists(const char* name) {
        char dir[MAX_PATH]; GetModuleFileNameA(nullptr, dir, MAX_PATH);
        char* slash = strrchr(dir, '\\'); if (slash) slash[1] = 0;
        char pat[MAX_PATH]; sprintf_s(pat, "%sMOVIES\\%s_*.vp6", dir, name);
        WIN32_FIND_DATAA fd; HANDLE f = FindFirstFileA(pat, &fd);
        if (f == INVALID_HANDLE_VALUE) return false;
        FindClose(f); return true;
    }
    // has my career reached this scene? (rival numbers count down: 15 first, 1 last)
    bool CareerHasMovie(const char* name, uint8_t peerBin) {
        uint8_t mine = CareerBinRaw();
        if (mine < 1 || mine > 15) return false;
        if (!strncmp(name, "blacklist_", 10)) { int nn = atoi(name + 10); return nn >= 1 && nn <= 15 && mine <= nn; }
        return peerBin >= 1 && peerBin <= 15 && mine <= peerBin;
    }
    bool PlayInGameMovie(const char* name) {
        __try { ((void(__cdecl*)(const char*))PlayInGameMovieFn)(name); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    void StoryTickImpl(bool connected) {
        DWORD now = GetTickCount();
        PollMovie(false);
        const Net::Snapshot& p = Net::Latest();
        uint32_t mine = MyStoryScene();
        uint32_t peer = (connected && g_cfg.syncStoryScenes > 0) ? PeerStoryScene() : 0;
        if (peer != g_peerStory) {
            if (g_peerStory && !peer && g_storyHold) { sprintf_s(g_storyStatus, "%s finished the story scene", PeerName()); g_doneUntil = now + 3000; }
            g_peerStory = peer; g_peerStorySince = now;
            if (!peer) { g_startedFor = 0; g_startPending = 0; }
        }
        int flow = Game::GameFlowState();
        bool roaming = flow == 6 && !Game::CurrentEventHash() && !Game::NisSceneHash() && !g_movieInGame;

        // watch along (SyncStoryScenes=2): the same movie, when this career has reached it
        const char* movie = peer ? KnownMovie(peer) : nullptr;
        if (movie && g_cfg.syncStoryScenes >= 2 && roaming && !Game::LocalPursuitHeat() && g_startedFor != peer && !g_startPending &&
            now - g_peerStorySince > 300 && CareerHasMovie(movie, p.s.pad[2]) && MovieFileExists(movie)) {
            g_startPending = peer; g_startAt = now + 150;
            StoryHoldOff("starting the same movie here");   // a pushed screen must find no pause request of ours
        }
        if (g_startPending && (int)(now - g_startAt) >= 0) {
            uint32_t h = g_startPending; g_startPending = 0; g_startedFor = h;
            const char* name = KnownMovie(h);
            if (name && roaming && PlayInGameMovie(name)) {
                g_startedAt = now;
                LOG("Story: playing %s here too (the other player is watching it)", name);
            }
        }
        // a start that pushed no screen would leave the "movie playing" byte set: clear it
        if (g_startedAt && now - g_startedAt > 2500) {
            g_startedAt = 0;
            PollMovie(true);
            if (g_movieInGame && !g_moviePkg) { uint8_t z = 0; Mem::Write(MoviePlayingFlag, z); LOG("Story: the movie did not start here"); }
        }

        // otherwise wait: hold the world while they watch (free roam only)
        bool want = peer && !mine && roaming && !g_startPending && now - g_peerStorySince > 700;
        if (want && !g_storyHold) StoryHoldOn();
        if (g_storyHold) {
            if (!want) StoryHoldOff(peer ? "left free roam" : "scene over");
            else if (now - g_storyHoldSince > 8 * 60 * 1000) { StoryHoldOff("safety limit"); g_startedFor = peer; }
            else { int req = 0; if (Mem::Read(PauseRequests, req) && req <= 0) { req = 1; Mem::Write(PauseRequests, req); } }  // a closed menu took ours
        }

        // my vote on a movie: skip once the other player votes too, or has left it
        if (g_myVote && g_myVote == g_movieHash && connected) {
            bool gone = !p.valid || p.s.nisHash != g_myVote;
            if (!gone) g_peerLeftSince = 0; else if (!g_peerLeftSince) g_peerLeftSince = now;
            bool go = (p.valid && p.s.nisVote == g_myVote) || (gone && now - g_peerLeftSince > 500);
            if (go && g_moviePkg && now - g_lastSkipSend > 500) {
                g_lastSkipSend = now;
                g_movieForceSkip = g_myVote;
                void* fe = nullptr;
                if (Mem::Read(0x91CADC, fe) && fe)
                    ((void(__thiscall*)(void*, uint32_t, const char*, uint32_t))0x516C50)(fe, PadAccept, g_moviePkg, 0xFF);
            }
        } else g_peerLeftSince = 0;

        // status line / notice
        if (g_storyHold) sprintf_s(g_storyStatus, "%s is watching a story scene - your game waits", PeerName());
        else if (peer && mine == peer && g_startedFor == peer) sprintf_s(g_storyStatus, "watching %s's story scene here too", PeerName());
        else if (peer && !mine && !g_startPending) sprintf_s(g_storyStatus, "%s is watching a story scene", PeerName());
        else if (g_doneUntil && (int)(g_doneUntil - now) > 0) {}                 // "... finished" for a moment
        else if (!g_startPending) { g_doneUntil = 0; g_storyStatus[0] = 0; }
    }
}

uint32_t Game::SceneHash() {
    if (!g_ok) return 0;
    if (g_cfg.syncStoryScenes > 0) { PollMovie(false); if (g_movieHash) return g_movieHash; }
    return NisSceneHash();
}

uint8_t Game::CareerBin() {
    if (!g_ok) return 0;
    __try { return CareerBinRaw(); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

void Game::StoryTick(bool connected) {
    if (!g_ok) return;
    __try { StoryTickImpl(connected); }
    __except (EXCEPTION_EXECUTE_HANDLER) { g_storyHold = false; g_storyStatus[0] = 0; }
    if (!connected || g_cfg.syncStoryScenes <= 0) { __try { StoryHoldOff("disconnected / off"); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
}

// the world must really pause while we hold, or while we watch a story movie over it
bool Game::StoryPauseWanted() { return g_storyHold || (g_cfg.syncStoryScenes > 0 && g_movieHash && g_movieInGame); }
const char* Game::StoryStatus() { return g_storyStatus; }

bool Game::InstallStoryHooks() {
    struct Slot { uintptr_t slot, expect; void* hook; };
    const Slot slots[3] = {
        { InGameMovieVt + 4, 0x57E400, (void*)&hk_MovieNotify<0> },
        { InGameMovie2Vt + 4, 0x57E4F0, (void*)&hk_MovieNotify<1> },
        { FEMovieVt + 4, 0x559880, (void*)&hk_MovieNotify<2> },
    };
    int ok = 0;
    for (int i = 0; i < 3; ++i) {
        uintptr_t cur = 0;
        if (!Mem::Read(slots[i].slot, cur) || cur != slots[i].expect) { LOG("Story: movie screen slot %08X holds %08X - not hooked", (unsigned)slots[i].slot, (unsigned)cur); continue; }
        g_movieNotify[i] = (MovieNotifyFn)cur;
        Mem::PatchPointer((void*)slots[i].slot, slots[i].hook);
        ++ok;
    }
    LOG("Story: movie skip voting hooked on %d/3 movie screens (SyncStoryScenes=%d)", ok, g_cfg.syncStoryScenes);
    return ok == 3;
}

// Test command: "story" = what we see; "story <movie>" = play that movie over the world here (as the
// career would), e.g. story storyfmv_pin11 / story blacklist_15.
static const char* DebugStorySafe(const char* arg) {
    static char out[160];
    __try {
        if (arg && arg[0]) {
            char name[64]; NormalizeMovie(arg, name, sizeof(name));
            if (!KnownMovie(BHash(name))) return "not a story movie name (blacklist_NN / storyfmv_...)";
            if (Game::GameFlowState() != 6) return "not in the world";
            if (!MovieFileExists(name)) return "no such file in MOVIES";
            return PlayInGameMovie(name) ? "movie started" : "exception";
        }
        PollMovie(true);
        sprintf_s(out, "movie '%s' story %08X in-game %d pkg %s | scene %08X | peer story %08X | hold %d | rival %u",
                  g_movieName, g_movieHash, g_movieInGame ? 1 : 0, g_moviePkg ? g_moviePkg : "-",
                  Game::SceneHash(), PeerStoryScene(), g_storyHold ? 1 : 0, (unsigned)CareerBinRaw());
        return out;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return "exception"; }
}
std::string Game::DebugStory(const char* arg) { return g_ok ? DebugStorySafe(arg) : "game not recognised"; }
