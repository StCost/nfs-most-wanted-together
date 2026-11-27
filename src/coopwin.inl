// Included from game.cpp (after profile.inl).
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
//  The sender's CarState::finished carries its own rank at the finish (0 = still racing, 1 = won,
//  2+ = finished and did not win). A timeout / DNF / quit while leading is reported as 2, never 1.
//
// Finishing as the winner when the other player won (verified in the exe and in the race script).
//  The race flow is a Lua 5.0 state graph ("basicrace") in GLOBAL\gameplay.bin: each handler is a
//  HUFF-packed chunk (unpacked with the exe's own decoder 0x64DCA0). At the finish line,
//  stategraph_basicrace_racing_handler_triggerenter does, for any racer on its last lap:
//      NumberFinished += 1
//      placing = Game.CalculateRanking(Element, NumberFinished)     -- native 0x6058B0
//      Game.NotifyRaceFinished(Element)                              -- native 0x6119F0 (cdecl ISimable*)
//      Game.NotifyRacePlacement(activity, Element, placing)          -- native 0x60AA00
//  NotifyRacePlacement posts MNotifyRacePlacement (kind 0x604440, ctor 0x6044C0(activity key, ISimable*,
//  placing), 0x1C bytes) on channel 0x20D60DBF (Message::Post 0x654AF0 copies it). Its handlers:
//    racing_handler_notifyraceplacement_3: a human racer, all humans done -> Placing == 1 ?
//        ChangeState("postrace") (EnterPostRaceFlow 0x611F20 -> ShowPostRace(true) / the rival flow)
//      : ChangeState("raceover") (the losing screen, whose Continue the script then waits for)
//    race_bin_rival_unlocked_handler_notifyraceplacement: Placing <= 1 and Activity is one of the
//        rival's BossRaces -> NumRivalRacesWon += 1; all won -> "done" (rival beaten).
//  So the winner's screen, the rival progress and the payout (in NotifyRaceFinished) all follow the
//  placing the script gets. We (1) answer CalculateRanking with 1 for our racer once the other player
//  has won (a racer of theirs that crossed our line first must not make us 2nd), and (2) when their
//  win arrives while we still drive, do the script's two calls ourselves for our racer with placing 1:
//  the state graph moves on exactly as if we had crossed the line first, nothing is left waiting.
//  Activity key of the race: what GRaceStatus::SetRaceActivity (0x6031F0) hands GRaceDatabase 0x600A20,
//  which finds the race parameters by it: parms+4 ? *(parms+4) : *(*(*(parms+8)+4)+0x20). We check that
//  0x600A20 gives our parameters back before posting.
// ---------------------------------------------------------------------------
// The prologue's last race (16.2.1 against Razor) is a scripted loss: no shared win there.
static bool ScriptedLossEvent() {
    const char* id = EventIdOf(CurrentParms());
    return id && strcmp(id, "16.2.1") == 0;
}
static int RaceTypeOfCurrent() {
    void* rs = nullptr; void* parms = nullptr;
    if (!Mem::Read(0x91E000, rs) || !rs) return -1;
    if (!Mem::Read((uintptr_t)rs + 0x1968, parms) || !parms) return -1;
    return ((int(__thiscall*)(void*))0x5FAA20)(parms);
}
// busted in this run of the current event (pursuit.inl sets it; a pursuit challenge is lost then)
static bool BustedThisEventRun() {
    uint32_t ev = Game::CurrentEventHash();
    return ev && g_bustedEvent == ev;
}
namespace {
    bool g_coopWinLogged = false;
    uint32_t g_runGen = 0;     // +1 for every new run of an event (new event, restart here or there)
    uint32_t g_peerWonEvent = 0;    // event the other player has won (kept until our event changes)
    bool g_peerRacedHere = false;   // the other player was seen racing (not finished) in this run of the event:
                                    // a "finished" left over from their previous run of the same event is ignored

    void* GhostRacerInfo() { return (g_ghost && g_ghostKind == GK_RACER) ? RacerInfoOf(g_ghost) : nullptr; }

    bool CoopWonImpl(void* localRi) {
        int rank = 0; float pct = 0.f;
        if (localRi) {
            Mem::Read((uintptr_t)localRi + 0x10, rank);
            Mem::Read((uintptr_t)localRi + 0x18, pct);
        }
        // Rank 1 is a win only if the course was actually completed (a Tollbooth timeout
        // leaves the leader in 1st with the timer expired — that must not pay or mark the career).
        if (CoopRace::LocalObjectiveWin(rank, pct, RaceTypeOfCurrent(), BustedThisEventRun())) return true;
        if (!g_cfg.sharedWins) return false;
        if (ScriptedLossEvent()) return false;      // the prologue's Razor race: the story needs the loss
        uint32_t ev = Game::CurrentEventHash();
        if (CoopRace::LatchedPeerWin(ev, g_peerWonEvent, false)) {
            if (!g_coopWinLogged) { g_coopWinLogged = true; LOG("Race: the other player won - counted as our win too"); }
            return true;
        }
        if (!Net::Stats().connected || !g_coopLinked) return false;
        const Net::Snapshot& p = Net::Latest();
        if (!p.valid) return false;
        uint8_t restart = 0; Mem::Read(0x91CF28, restart);
        // their own result decides (our sort of their car may differ); same event only
        if (CoopRace::PeerReportedWin(ev, p.s.eventHash, p.s.inRace != 0, p.s.finished,
                                      g_peerRacedHere, false, restart != 0)) {
            g_peerWonEvent = ev;   // remembered: they may leave the race before we finish
            if (!g_coopWinLogged) { g_coopWinLogged = true; LOG("Race: the other player won - counted as our win too"); }
            return true;
        }
        return false;
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
        uint8_t patch[32];
        if (len < 5 || len > sizeof(patch)) return false;
        memset(patch, 0x90, sizeof(patch));
        patch[0] = 0xE9;
        *(int32_t*)(patch + 1) = (int32_t)((uintptr_t)to - (at + 5));
        return Mem::PatchBytes(at, expect, patch, len);
    }
}

// Rank of the local racer once finished, for the state packet (0 = still racing / not in a race).
uint8_t Game::LocalFinishRank(void* localRi) {
    uint8_t fin = 0; int rank = 0; float pct = 0.f;
    if (!localRi || !Mem::Read((uintptr_t)localRi + 0x21, fin) || !fin) return 0;
    Mem::Read((uintptr_t)localRi + 0x10, rank);
    Mem::Read((uintptr_t)localRi + 0x18, pct);
    uint32_t outs = 0; Mem::Read((uintptr_t)localRi + 0x1C, outs);
    return CoopRace::FinishRankAfterOut(CoopRace::FinishRankToReport(fin, rank, pct, RaceTypeOfCurrent(), BustedThisEventRun()), outs != 0);
}

// ---- the winner's photo for a shared win, naming the one who won ----
//  ShowPostRace 0x60AC20(won): a race (GRaceParameters flag 0x5FBE70 clear, racing mode) won shows the winner's
//  photo (InGamePhotoMaster.fng, PhotoFinishScreen, flag 0x91CF41); lost, it posts EShowResults (0x10 bytes, ctor
//  0x642140(0, 0)), the standings / results list. Milestone events (flag set) show their rewards / race-over screen.
//  A win we only have because the other player won (shared wins: finished as the winner for them, given place 1
//  by the race script, or counted after we were out - knocked out, totaled, on the Restart / Quit screen or the
//  losing results list) gets the winner's photo too. Its line under the photo (time @ speed, written
//  by PhotoFinishScreen::Setup 0x5A2AD0, called at 0x5A865D once the photo shows) names them instead:
//  "<name> won the race". Setup picks the line by the photo's group: TOLL_BOOTH_GROUP 0x424BB244, RIVAL_GROUP
//  0x37BEA03B, else 0xAB6AAFDD (FEngIsScriptSet 0x524A10(pkg, bStringHash 0x460BF0(group), APPEAR 0x5079C8F8)).
//  The cash line is the win's, paid by NotifyRaceFinished as before.
namespace {
    uint32_t g_teamWinEvent = 0;       // event whose win came from the other player
    uint32_t g_outWinEvent = 0;        // ... whose win was counted for us after we were out ("raceover")
    uint32_t g_teamOutroEvent = 0;     // ... whose results list stands in for the photo in the "postrace" state
    bool     g_inPostRaceFlow = false; // ShowPostRaceCoop called by EnterPostRaceFlow (the "postrace" state)
}
static void MarkTeamWin() { g_teamWinEvent = Game::CurrentEventHash(); }

// ---- the winner's photo is for whoever really won (as up to 1.7.x early builds, asked for again) ----
//  ShowPostRace 0x60AC20(won) shows the photo for a won race; a win we only have because the other player won
//  gets the results list (EShowResults 0x642140(0, 0)) instead: the win, cash and progress counted as before.
//  The list's way out for that win: hk_ResultsNotify (CoopRace::TeamResultsAction).
static bool ShowTeamResultsImpl() {
    void* rs = nullptr; void* parms = nullptr; int mode = 0;
    if (!Mem::Read(0x91E000, rs) || !rs || !Mem::Read((uintptr_t)rs + 0x1968, parms) || !parms) return false;
    if (!Mem::Read((uintptr_t)rs + 0x1960, mode) || mode != 1) return false;
    if (((bool(__thiscall*)(void*))0x5FBE70)(parms)) return false;     // a milestone event: no photo there
    ((void(__thiscall*)(void*))0x6021B0)(rs);                          // as ShowPostRace starts
    if (((bool(__cdecl*)())0x605690)()) return true;                    // (it posted the results itself)
    void* ev = ((void*(__cdecl*)(uint32_t))0x627400)(0x10);
    if (!ev) return true;
    ((void(__thiscall*)(void*, int, int))0x642140)(ev, 0, 0);          // EShowResults: the standings
    return true;
}
static bool ShowTeamResults(const char* why) {
    bool done = false;
    __try { done = ShowTeamResultsImpl(); } __except (EXCEPTION_EXECUTE_HANDLER) { done = false; }
    if (done) LOG("Race: the other player won - the results list here (%s), the winner's photo is theirs", why);
    return done;
}
static void __cdecl ShowPostRaceCoop(bool won) {
    uint32_t ev = Game::CurrentEventHash();
    if (CoopRace::TeamWinResults(won, ev, g_teamWinEvent) && ShowTeamResults(g_inPostRaceFlow ? "we were given place 1" : "won screen")) {
        if (g_inPostRaceFlow) g_teamOutroEvent = ev;                  // its exit must end the race like the photo's
        return;
    }
    ((void(__cdecl*)(bool))0x60AC20)(won);                             // ShowPostRace
}
// EnterPostRaceFlow 0x611F20 (only from stategraph_basicrace_postrace_handler_stateenter) -> ShowPostRace(true)
static void __cdecl ShowPostRaceFromFlow(bool won) {
    g_inPostRaceFlow = true;
    ShowPostRaceCoop(won);
    g_inPostRaceFlow = false;
}
namespace {
    constexpr uint32_t kFeAppear = 0x5079C8F8u, kPhotoObj = 0x286A9CD4u, kPhotoShown = 0x0016A259u;
    bool PhotoGroupShown(const char* pkg, const char* group) {
        uint32_t h = ((uint32_t(__cdecl*)(const char*))0x460BF0)(group);
        return ((bool(__cdecl*)(const char*, uint32_t, uint32_t))0x524A10)(pkg, h, kFeAppear);
    }
    void NamePhotoWinner(void* scr) {
        const char* pkg = nullptr; int type = -1;
        if (!Mem::Read((uintptr_t)scr + 0x10, pkg) || !pkg || !Mem::Read((uintptr_t)scr + 0x34, type) || type != 0) return;   // a race's photo
        uint32_t line = PhotoGroupShown(pkg, "TOLL_BOOTH_GROUP") ? 0x424BB244u
                      : PhotoGroupShown(pkg, "RIVAL_GROUP") ? 0x37BEA03Bu : 0xAB6AAFDDu;
        char name[32], text[64];
        NetUtil::CopyHudName(name, Net::Stats().peerName.c_str());
        sprintf_s(text, "%s won the race", name[0] ? name : "Your partner");
        ((int(__cdecl*)(const char*, uint32_t, const char*, ...))0x5252D0)(pkg, line, "%s", text);
        LOG("Race: the winner's photo for the other player's win - \"%s\"", text);
    }
    void __fastcall hk_PhotoSetup(void* scr, void*) {
        ((void(__thiscall*)(void*))0x5A2AD0)(scr);
        __try {
            if (CoopRace::TeamWinPhoto(Game::CurrentEventHash(), g_teamWinEvent)) NamePhotoWinner(scr);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}

// ---- leaving the photo of a win counted after we were out ----
//  The race script waits in different states for the two screens' Continue:
//   - "postrace" (placing 1 -> EnterPostRaceFlow) handles only MEnterFreeRoam, MFlowReadyForOutro (-> outronis
//     -> outro_message -> done) and RestartRace. The photo (vtable 0x8A0F30, NotificationMessage 0x5A7EA0)
//     answers Continue (0x406415E3), once its pictures are in (object 0x286A9CD4: script 0x16A259 not set, APPEAR
//     not running), by its mode (+0x34; 0 = a race): career race -> EUnPause + MFlowReadyForOutro; a Blacklist
//     event -> by the bin's events left, the race sheet (ERaceSheetOn) / MFlowReadyForOutro /
//     SafeHouseRivalChallenge.fng; quick race -> EQuitToFE.
//   - "raceover" (lost, busted, knocked out, totaled) handles MNotifyRaceAbandoned (-> abandoned: AbandonRace ->
//     done). PostRace_Results.fng (vtable 0x8A1708, NotificationMessage 0x5A7120) ends a career race there on
//     its exit (EXITCOMPLETE 0xE1FDE1D1, after Continue's animation) with GRaceStatus::RaceAbandoned 0x603220,
//     MNotifyRaceAbandoned (ctor 0x510840, channel 0x20D60DBF) and EUnPause. Its Continue first asks
//     InGameDialog.fng in a Blacklist event if our racer is not 1st (0x5A727F) - we can be out with the win counted.
//  So the photo shown in "raceover" (the win counted after we were out) would leave the script waiting for ever:
//  its Continue there posts MNotifyRaceAbandoned and EUnPause as the results list's exit does in a career race
//  (without RaceAbandoned, see EndRaceOut). In "postrace" the photo is the
//  game's own. The results list opened from the photo (its "results" button) skips that dialog for a shared win.
namespace {
    using FeNotifyFn = void(__thiscall*)(void*, uint32_t, void*, uint32_t, uint32_t);
    constexpr uintptr_t kResultsNotifySlot = 0x8A170C, kResultsNotifyFn = 0x5A7120;
    constexpr uintptr_t kPhotoNotifySlot = 0x8A0F34, kPhotoNotifyFn = 0x5A7EA0;
    FeNotifyFn g_origResultsNotify = nullptr, g_origPhotoNotify = nullptr;
    uint32_t g_swapEvent = 0;          // the losing screen is closing for the winner's photo (LoseScreenWinTickImpl)
    bool     g_photoOutOk = false;     // the photo's and the list's hooks are in: the photo may show after we were out

    bool PhotoReady(void* scr) {
        const char* pkg = nullptr;
        if (!Mem::Read((uintptr_t)scr + 0x10, pkg) || !pkg) return false;
        if (((bool(__cdecl*)(const char*, uint32_t, uint32_t))0x524A10)(pkg, kPhotoObj, kPhotoShown)) return false;
        return !((bool(__cdecl*)(const char*, uint32_t, uint32_t))0x524AA0)(pkg, kPhotoObj, kFeAppear);
    }
    bool CareerRace() {
        void* rs = nullptr; int ctx = 0;
        return Mem::Read(0x91E000, rs) && rs && Mem::Read((uintptr_t)rs + 0x1964, ctx) && ctx == 2;
    }
    // (not GRaceStatus::RaceAbandoned 0x603220 as the list does: in a Blacklist rival race it clears the
    // "complete" flag of all the rival's races - the win just counted)
    void EndRaceOut() {
        alignas(4) uint8_t msg[0x40] = {};
        ((void*(__thiscall*)(void*))0x510840)(msg);                                   // MNotifyRaceAbandoned
        ((void(__thiscall*)(void*, uint32_t))0x654AF0)(msg, 0x20D60DBF);
        void* un = ((void*(__cdecl*)(uint32_t))0x627400)(8);                          // EUnPause: the photo goes
        if (un) ((void(__thiscall*)(void*))0x625870)(un);
    }
    void __fastcall hk_PhotoNotify(void* scr, void*, uint32_t msg, void* obj, uint32_t a, uint32_t b) {
        if (msg == CoopRace::kFeAccept) {
            int step = CoopRace::kPhotoAsIs; int type = -1;
            __try {
                uint32_t ev = Game::CurrentEventHash();
                Mem::Read((uintptr_t)scr + 0x34, type);
                step = CoopRace::TeamPhotoAction(msg, ev, g_teamWinEvent == ev ? g_outWinEvent : 0, type == 0,
                                                 CareerRace(), PhotoReady(scr));
            } __except (EXCEPTION_EXECUTE_HANDLER) { step = CoopRace::kPhotoAsIs; }
            if (step == CoopRace::kPhotoEndOut) {
                bool ok = false;
                __try { EndRaceOut(); ok = true; } __except (EXCEPTION_EXECUTE_HANDLER) {}
                if (ok) { LOG("Race: photo closed - the race ends as after the results list (we were out)"); return; }
                LOG("Race: exception ending the race after the photo - the photo's own Continue");
            }
        }
        g_origPhotoNotify(scr, msg, obj, a, b);
    }
    // the photo's own Continue with a race's mode (0): EUnPause + MFlowReadyForOutro / the Blacklist flow.
    // It reads only +0x10 (the package: null = no button check) and +0x34 of its screen.
    void PhotoContinue() {
        alignas(4) static uint8_t photo[0x174];
        memset(photo, 0, sizeof(photo));
        ((FeNotifyFn)kPhotoNotifyFn)(photo, CoopRace::kFeAccept, nullptr, 0, 0);
    }
    void __fastcall hk_ResultsNotify(void* scr, void*, uint32_t msg, void* obj, uint32_t a, uint32_t b) {
        int step = CoopRace::kResultsAsIs;
        if (msg == CoopRace::kFeAccept || msg == CoopRace::kFeExitComplete) {
            __try {
                uint32_t ev = Game::CurrentEventHash();
                step = CoopRace::TeamResultsAction(msg, ev, g_teamWinEvent, g_swapEvent,
                                                   FEScreens::IsPushed("PostRace_Results.fng"), g_teamOutroEvent, g_outWinEvent);
            } __except (EXCEPTION_EXECUTE_HANDLER) { step = CoopRace::kResultsAsIs; }
        }
        if (step == CoopRace::kResultsHold) return;              // closing for the photo: no race end from the list
        if (step == CoopRace::kResultsOutro || step == CoopRace::kResultsEndOut) {
            bool ok = false;
            __try { if (step == CoopRace::kResultsOutro) PhotoContinue(); else EndRaceOut(); ok = true; } __except (EXCEPTION_EXECUTE_HANDLER) {}
            if (ok) {
                if (step == CoopRace::kResultsOutro) g_teamOutroEvent = 0;
                LOG("Race: results list closed - the race ends as after the winner's photo (%s)", step == CoopRace::kResultsOutro ? "given place 1" : "we were out");
                return;
            }
            LOG("Race: exception ending the race after the results list - the game's own exit");
        }
        if (step == CoopRace::kResultsNoDialog) {
            void* pkg = nullptr; bool busy = true;
            __try {
                Mem::Read((uintptr_t)scr + 0x10, pkg);
                busy = ((bool(__cdecl*)(void*, uint32_t, uint32_t))0x524A10)(pkg, 0x57EFB2FB, 0x7DE71E0F);   // as 0x5A7213
            } __except (EXCEPTION_EXECUTE_HANDLER) { busy = true; }
            if (!busy) msg = 0x30ED2368;                            // the dialog's OK: the list animates out
        }
        g_origResultsNotify(scr, msg, obj, a, b);
    }
}

// Forget the other player's win when the event changes, the race is restarted (here or there: their
// "finished" drops back to 0) or we leave to the menus. Runs in every flow state.
static void CoopWinLatchTick(bool connected, const Net::Snapshot& peer) {
    static uint32_t lastEvent = 0; static uint8_t lastPeerFin = 0, lastRestart = 0;
    uint32_t ev = Game::GameFlowState() == 6 ? Game::CurrentEventHash() : 0;
    uint8_t restart = 0; Mem::Read(0x91CF28, restart);
    bool sameEvent = peer.valid && peer.s.eventHash && peer.s.eventHash == ev;
    uint8_t pfin = sameEvent ? peer.s.finished : 0;
    if (ev != lastEvent || (restart && !lastRestart) || (lastPeerFin && !pfin && sameEvent && peer.s.inRace)) {
        g_coopWinLogged = false; g_peerWonEvent = 0; g_peerRacedHere = false; g_teamWinEvent = 0; g_outWinEvent = 0; g_swapEvent = 0;
        g_teamOutroEvent = 0;
        g_bustedEvent = 0;   // a new run: not busted in it (yet)
        ++g_runGen;
    }
    lastEvent = ev; lastRestart = restart; lastPeerFin = pfin;
    // their win counts only for a race we drove together: they were racing while our race was running too,
    // for a few seconds (not a run they finished while we were still in the intro / on the grid)
    static DWORD togetherSince = 0; static uint32_t togetherEvent = 0;
    if (ev != togetherEvent) { togetherEvent = ev; togetherSince = 0; }
    bool weRace = false;
    if (void* ri = ev ? RacerInfoOfLocal() : nullptr) {
        uint8_t timer = 0, fin = 0;
        Mem::Read((uintptr_t)ri + 0x154, timer); Mem::Read((uintptr_t)ri + 0x21, fin);
        weRace = timer != 0 && !fin;
    }
    if (CoopRace::RacedTogether(ev && connected && sameEvent && peer.s.inRace && !peer.s.finished, weRace, togetherSince, GetTickCount()))
        g_peerRacedHere = true;
    // (never in the prologue's Razor race 16.2.1: the story needs that loss - the car breaks down on both)
    if (connected && CoopRace::PeerReportedWin(ev, sameEvent ? peer.s.eventHash : 0, sameEvent && peer.s.inRace != 0,
                                               pfin, g_peerRacedHere, ScriptedLossEvent(), restart != 0))
        g_peerWonEvent = ev;   // also without a ghost racer (rival races)
}

// ---- finish our racer as the winner (see the top of this file) ----
namespace {
    bool     g_forcedFinishTried = false;
    uint32_t g_forcedFinishEvent = 0;

    bool PeerWonThisEvent() {
        return g_cfg.sharedWins && g_coopLinked && g_peerWonEvent && g_peerWonEvent == Game::CurrentEventHash();
    }
    void* LocalSimable() {
        void* iv = LocalVehicle();
        return IsVehicle(iv) ? VCall<void* (__thiscall*)(void*)>(iv, IV_GetSimable)(iv) : nullptr;
    }
    // the key the race script's activity is known by (see the top of this file); 0 if unknown
    uint32_t ActivityKeyOf(void* parms) {
        void* idx = nullptr; void* a = nullptr; void* b = nullptr; uint32_t key = 0;
        if (!parms || !Mem::Read((uintptr_t)parms + 4, idx)) return 0;
        if (idx) return Mem::Read((uintptr_t)idx, key) ? key : 0;
        if (!Mem::Read((uintptr_t)parms + 8, a) || !a || !Mem::Read((uintptr_t)a + 4, b) || !b) return 0;
        return Mem::Read((uintptr_t)b + 0x20, key) ? key : 0;
    }
    // MNotifyRacePlacement(activity, racer, placing) posted like the native NotifyRacePlacement does
    bool PostRacePlacement(void* parms, void* sim, int placing) {
        void* db = nullptr;
        uint32_t key = ActivityKeyOf(parms);
        if (!key || !Mem::Read(0x91E004, db) || !db) return false;
        void* back = ((void*(__thiscall*)(void*, uint32_t))0x600A20)(db, key);
        if (back != parms) { LOG("Race: activity key %08X does not lead back to this race (%p, not %p) - not posting the placement", key, back, parms); return false; }
        // the message carries the racer's handle (*(ISimable+8)), like the native NotifyRacePlacement 0x60AA00
        uint32_t handle = 0;
        if (!Mem::Read((uintptr_t)sim + 8, handle) || !handle) { LOG("Race: our racer has no handle - not posting the placement"); return false; }
        alignas(4) uint8_t msg[0x20] = {};
        ((void*(__thiscall*)(void*, uint32_t, uint32_t, int))0x6044C0)(msg, key, handle, placing);
        ((void(__thiscall*)(void*, uint32_t))0x654AF0)(msg, 0x20D60DBF);
        return true;
    }
    bool IsLocalRacerSim(void* sim) {
        void* rs = nullptr;
        if (!sim || !Mem::Read(0x91E000, rs) || !rs) return false;
        void* ri = ((void*(__thiscall*)(void*, void*))0x5E8B20)(rs, sim);
        return ri && ri == RacerInfoOfLocal();
    }

    // Lua native CalculateRanking (0x6058B0, cdecl (ISimable*, int numberFinished) -> placing). Its first
    // instruction is `call 0x6E8DF0` (5 bytes, replaced by our jmp): the trampoline makes that call and
    // goes on at 0x6058B5 with the caller's stack untouched.
    __declspec(naked) int __cdecl CalcRankingOrig(void*, int) {
        __asm {
            mov eax, 0x6E8DF0
            call eax
            push 0x6058B5
            ret
        }
    }
    int __cdecl hk_CalcRanking(void* element, int numberFinished) {
        int r = CalcRankingOrig(element, numberFinished);
        __try {
            bool local = IsLocalRacerSim(element);
            int placing = CoopRace::PlacingForScript(local, local && PeerWonThisEvent(), r);
            if (placing != r) { LOG("Race: we crossed the line %d. - the other player won, so the race script gets place 1", r); r = placing; MarkTeamWin(); }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return r;
    }
}

static void CoopFinishTickImpl() {
    uint32_t ev = Game::CurrentEventHash();
    if (ev != g_forcedFinishEvent || !PeerWonThisEvent()) { g_forcedFinishEvent = ev; g_forcedFinishTried = false; }   // a new win (or restart) may finish us again
    void* rs = nullptr; void* parms = nullptr; int mode = 0;
    if (!Mem::Read(0x91E000, rs) || !rs || !Mem::Read((uintptr_t)rs + 0x1960, mode) || !Mem::Read((uintptr_t)rs + 0x1968, parms) || !parms) return;
    void* ri = RacerInfoOfLocal();
    uint8_t fin = 0, timer = 0, restart = 0;
    if (ri) { Mem::Read((uintptr_t)ri + 0x21, fin); Mem::Read((uintptr_t)ri + 0x154, timer); }
    Mem::Read(0x91CF28, restart);
    CoopRace::FinishInputs in{};
    in.sharedWins = g_cfg.sharedWins; in.peerWon = PeerWonThisEvent(); in.racing = mode == 1;
    in.haveLocalRacer = ri != nullptr; in.localFinished = fin != 0; in.timerRunning = timer != 0;
    in.restartPending = restart != 0; in.cutscene = NisActive(); in.alreadyTried = g_forcedFinishTried;
    in.raceType = in.peerWon ? ((int(__thiscall*)(void*))0x5FAA20)(parms) : -1;
    static DWORD peerWonAt = 0;
    if (!in.peerWon) peerWonAt = 0; else if (!peerWonAt) peerWonAt = GetTickCount();
    in.peerWonForMs = peerWonAt ? (int)(GetTickCount() - peerWonAt) : 0;
    if (in.peerWon && in.raceType == CoopRace::kTypePursuitChallenge) {
        static uint32_t loggedFor = 0;
        if (loggedFor != ev) { loggedFor = ev; LOG("Race: the other player won this pursuit challenge - our goal should follow from the shared stats / escape; if not, we finish in %d s", CoopRace::kPursuitChallengeFinishDelayMs / 1000); }
    }
    if (!CoopRace::ShouldFinishAsWinner(in)) return;
    bool pursuitChallenge = in.raceType == CoopRace::kTypePursuitChallenge;
    {   // not under the pause menu / map (co-op keeps the race running there), and not before we drove off
        int req = 0; Mem::Read(0x91CAE4, req);
        if (req > 0 || FEScreens::IsPushed("Pause_Main.fng") || FEScreens::IsPushed("WorldMapMain.fng")) return;   // next tick
        float pct = 0.f; if (ri) Mem::Read((uintptr_t)ri + 0x18, pct);
        if (!pursuitChallenge && !(pct > 1.0f)) return;   // (a pursuit challenge has no course)
    }
    g_forcedFinishTried = true;
    void* sim = LocalSimable();
    if (!sim) return;
    LOG("Race: the other player won while we still drive - finishing our racer as the winner (race type %d)", in.raceType);
    MarkTeamWin();
    ((void(__cdecl*)(void*))0x6119F0)(sim);                      // NotifyRaceFinished: finished, paid, car to the AI
    int rank = 0; fin = 0;
    Mem::Read((uintptr_t)ri + 0x21, fin); Mem::Read((uintptr_t)ri + 0x10, rank);
    bool posted = PostRacePlacement(parms, sim, 1);              // the script's next step: place 1 -> winning flow
    LOG("Race: our racer finished %d (rank %d here), placement 1 %s", (int)fin, rank,
        posted ? "posted - the race script goes on to the winner's screen" : "NOT posted - the race ends when the game's AI drives our car over the line");
}

// ---- out of the race, and then the other player wins ----
//  Totaled / knocked out / beaten: the race script is in "raceover" (it showed "race over", then the losing
//  screen). Their win counts for us too (shared wins), once per run of the event, while we are still in it -
//  the losing screen (results list or Restart / Quit) or the knockout wait. A player who left
//  with Quit / Continue before they won gave the race up: not counted (the race is no longer running here).
//   - NotifyRaceFinished (0x6119F0): finished, the win gate pays the win and records the race result.
//   - MNotifyRacePlacement(placing 1): the career counts a won event only from that message - the Blacklist
//     scripts (race_bin_rival_locked: the bin's races, race_bin_rival_unlocked: the rival's races won) and
//     completion_unlock_monitor. "raceover" itself does not answer it: the race script stays where it is.
//  The winner's photo then shows, naming them, and the race ends the way "raceover" does: its Continue ->
//  MNotifyRaceAbandoned -> back to free roam with control (hk_PhotoNotify, as the results list's exit).
//  The bin's race sheet / rival challenge comes from the world map / safehouse as after any won race.
static bool CreditTeamWinOut(const char* where) {
    uint32_t ev = Game::CurrentEventHash();
    if (!ev || g_outWinEvent == ev) return false;
    g_outWinEvent = ev;
    void* sim = LocalSimable();
    if (sim) {
        g_forcedFinishTried = true; g_forcedFinishEvent = ev;
        ((void(__cdecl*)(void*))0x6119F0)(sim);                   // NotifyRaceFinished: the win gate counts their win
    }
    bool won = CoopWonImpl(RacerInfoOfLocal());
    bool posted = false;
    if (won) {
        MarkTeamWin();
        posted = sim && PostRacePlacement(CurrentParms(), sim, 1);
        Game::QueueNotice("Your partner won - the win counts for you too", false);
    }
    LOG("Race: the other player won after we were out (%s) - %s, placement 1 %s", where,
        won ? "counted for us too" : "not counted (shared wins off / scripted loss)", posted ? "posted" : "not posted");
    return won;
}
static bool RacingMode() {
    void* rs = nullptr; int mode = 0;
    return Mem::Read(0x91E000, rs) && rs && Mem::Read((uintptr_t)rs + 0x1960, mode) && mode == 1;
}
// The screen for a race we are out of ("raceover"): won, the winner's photo naming the other player (its
// Continue ends the race the "raceover" way, hk_PhotoNotify); lost, the results list as the game does. A milestone
// event's winning screen (EShowMilestones) ends with MFlowReadyForOutro, which "raceover" does not answer:
// there the race-over Restart / Quit screen stays the way out (the win is counted all the same).
static void ShowPostRaceOut(bool won) {
    bool milestone = false;
    __try {
        void* rs = nullptr; void* parms = nullptr;
        milestone = won && Mem::Read(0x91E000, rs) && rs && Mem::Read((uintptr_t)rs + 0x1968, parms) && parms &&
                    ((bool(__thiscall*)(void*))0x5FBE70)(parms);
    } __except (EXCEPTION_EXECUTE_HANDLER) { milestone = false; }
    if (won && !milestone && !g_photoOutOk) {
        LOG("Race: no way out of the winner's photo here (hooks missing) - the results list (the win is counted)");
        won = false;
    }
    if (milestone) {
        LOG("Race: a milestone event - its Restart / Quit screen (the win is counted)");
        ((void(__cdecl*)(bool))0x60AC20)(false);
        return;
    }
    // their win: the results list (its exit ends the race the "raceover" way, hk_ResultsNotify), not the photo
    if (won && ShowTeamResults("we were out")) return;
    ((void(__cdecl*)(bool))0x60AC20)(won);                       // ShowPostRace
}

// ---- lap knockout, and the shared loss ----
//  The race script knocks a racer out with the native KnockoutRacer (0x611440, cdecl(ISimable*)): the racer's
//  "knocked out" flag (0x600210), MNotifyKnockedOut, its AI told (+0x84), and the race status takes the car
//  out of the world (0x5FF2E0(3, ...)). Then stategraph_basicrace_racing_handler_notifyknockedout: a human
//  racer, all humans done -> ChangeState("raceover"). raceover shows "knocked out" (ShowRaceOverSummary)
//  and, RaceOverMessageDuration later, ShowLosingPostRaceScreen (0x60E230 = ShowPostRace(false)): the
//  results with Restart / Quit.
//  Co-op:
//   - the other player's car is a racer in our game, and our script knocked it out when it was last at a
//     lap's end - their car vanished from our world while they still raced. Their own game decides that:
//     KnockoutRacer skips their racer here.
//   - wins and losses are shared: once the other player is out of the event without a win (time ran out,
//     knocked out, wrecked, busted, beaten), our racer is knocked out the same way (SharedLossTickImpl) and
//     the game's own race-over and losing screen follow - in every race type.
namespace {
    void __cdecl hk_ShowLosingKO() {
        __try {
            // they won while "race over" was shown: the winner's photo instead of the losing screen
            void* ri = RacerInfoOfLocal(); int rank = 0; uint8_t fin = 0;
            if (ri) { Mem::Read((uintptr_t)ri + 0x10, rank); Mem::Read((uintptr_t)ri + 0x21, fin); }
            if (CoopRace::CreditBeforeLoseScreen(PeerWonThisEvent(), RacingMode(), LocalRacerOut(),
                                                 fin && rank == 1, g_outWinEvent == Game::CurrentEventHash())
                && CreditTeamWinOut("before the losing screen")) {
                ShowPostRaceOut(true);
                return;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        ((void(__cdecl*)(bool))0x60AC20)(false);                 // ShowPostRace(false), as the game does
    }

    // KnockoutRacer: the original's first two instructions (7 bytes), then on at 0x611447
    __declspec(naked) void __cdecl KnockoutRacerOrig(void*) {
        __asm {
            mov eax, dword ptr [esp + 4]
            sub esp, 0x14
            push 0x611447
            ret
        }
    }
    void* RacerInfoOfScriptRacer(void* racer) {
        if (!racer) return nullptr;
        int idx = ((int(__cdecl*)(void*))0x60DAE0)(racer);
        void* rs = nullptr;
        if (idx < 1 || !Mem::Read(0x91E000, rs) || !rs) return nullptr;
        return ((void*(__thiscall*)(void*, int))0x5DBED0)(rs, idx - 1);
    }
    void __cdecl hk_KnockoutRacer(void* racer) {
        __try {
            void* gri = GhostRacerInfo();
            if (gri && g_coopLinked && Net::Stats().connected && RacerInfoOfScriptRacer(racer) == gri) {
                LOG("Race: the race knocked out the other player's racer here - skipped (their own game decides)");
                return;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        KnockoutRacerOrig(racer);
    }

    uint32_t g_lossEvent = 0, g_lossGen = 0;   // the run we lost with the other player
}
static void SharedLossTickImpl(bool connected, const Net::Snapshot& peer) {
    static DWORD lostSince = 0, screenCheckAt = 0; static uint32_t lastEv = 0, lastGen = 0;
    uint32_t ev = Game::CurrentEventHash();
    DWORD now = GetTickCount();
    // the knockout gave no losing screen (a race whose script has no "raceover" for it; our racer still not
    // finished 5 s later): show it ourselves
    if (screenCheckAt && (int)(now - screenCheckAt) >= 0) {
        screenCheckAt = 0;
        void* ri = RacerInfoOfLocal(); uint8_t fin = 0;
        if (ri) Mem::Read((uintptr_t)ri + 0x21, fin);
        if (!fin && ev == g_lossEvent && g_runGen == g_lossGen && Game::GameFlowState() == 6 && !FEScreens::IsPushed("PostRace_Results.fng") &&
            !FEScreens::IsPushed("Pause_Main.fng") && !FEScreens::IsPushed("InGamePhotoMaster.fng")) {
            LOG("Race: no losing screen 5 s after the shared loss - showing it");
            ShowPostRaceOut(false);
        }
    }
    // only a live report counts, for the whole confirm time: a disconnect, a new run here or a gap in their
    // reports starts it over
    bool live = CoopRace::PeerLossReportLive(peer.valid, now - peer.recvTick, peer.s.gameFlowState, peer.s.pad[0]);
    if (!connected || !live || ev != lastEv || g_runGen != lastGen) lostSince = 0;
    lastEv = ev; lastGen = g_runGen;
    if (!connected || !live) return;
    bool sameEvent = g_coopLinked && ev && peer.s.eventHash == ev;
    bool lost = CoopRace::PeerReportedLoss(sameEvent, g_peerRacedHere, peer.s.inRace != 0, peer.s.finished, peer.s.pad[0]);
    // busted with them (their bust busts us): the bust ends the race here, no knockout on top of it
    if (g_sp.pendingBustUntil || LocalBustedThisRun()) lost = false;
    if (!lost) { lostSince = 0; return; }
    if (!lostSince) lostSince = now | 1;
    uint8_t restart = 0; Mem::Read(0x91CF28, restart);
    if (!CoopRace::ShouldLoseWithPeer(g_cfg.sharedWins, (int)(now - lostSince), PeerWonThisEvent(), RacingMode(),
                                      LocalRacerOut(), ScriptedLossEvent(), restart != 0, NisActive(),
                                      g_lossEvent == ev && g_lossGen == g_runGen))
        return;
    g_lossEvent = ev; g_lossGen = g_runGen;
    void* sim = LocalSimable();
    if (!sim) return;
    LOG("Race: the other player is out without a win (finished %d, stage %d) - the race is lost for us too", (int)peer.s.finished, (int)peer.s.pad[0]);
    Game::QueueNotice("Your partner lost - the race is lost for both", false);
    KnockoutRacerOrig(sim);                                        // knocked out: race over, then the losing screen
    void* ri = RacerInfoOfLocal(); uint32_t outs = 0;
    if (ri) Mem::Read((uintptr_t)ri + 0x1C, outs);
    if (!outs) { LOG("Race: our racer is NOT out after the knockout - the losing screen comes from us"); ShowPostRaceOut(false); }
    else screenCheckAt = now + 5000;   // (the game's own screen comes ~3 s after the knockout)
}

// Out of the event (busted, totaled, beaten), and then the other player wins: the win counts for us too
// (CreditTeamWinOut). The screen up - the race-over Restart / Quit screen (Pause_Main) or the losing results
// list (PostRace_Results) - closes (EUnPause, as the game's own Start / Continue) and the winner's photo naming
// them comes instead. The list's own end of the race is held back meanwhile (hk_ResultsNotify). Once per run
// of the event.
namespace { uint32_t g_loseToWinEvent = 0, g_loseToWinGen = 0; DWORD g_loseToWinAt = 0; const char* g_loseToWinFrom = ""; }
static void LoseScreenWinTickImpl() {
    uint32_t ev = Game::CurrentEventHash();
    if (g_loseToWinAt) {                                           // step 2: the screen has gone (or 1.5 s passed)
        if (ev != g_loseToWinEvent || g_loseToWinGen != g_runGen || Game::GameFlowState() != 6) { g_loseToWinAt = 0; g_swapEvent = 0; return; }
        if ((FEScreens::IsPushed("Pause_Main.fng") || FEScreens::IsPushed("PostRace_Results.fng")) &&
            GetTickCount() - g_loseToWinAt < 1500) return;
        g_loseToWinAt = 0; g_swapEvent = 0;
        bool won = CreditTeamWinOut(g_loseToWinFrom) || (g_outWinEvent == ev && g_teamWinEvent == ev);
        ShowPostRaceOut(won);
        return;
    }
    if (!ev || !PeerWonThisEvent()) return;
    void* ri = RacerInfoOfLocal(); int rank = 0; uint8_t fin = 0;
    if (ri) { Mem::Read((uintptr_t)ri + 0x10, rank); Mem::Read((uintptr_t)ri + 0x21, fin); }
    CoopRace::OutWin step = CoopRace::OutOfRaceWin(true, RacingMode(), LocalRacerOut(),
                                                   fin && rank == 1, (g_loseToWinEvent == ev && g_loseToWinGen == g_runGen) || g_outWinEvent == ev,
                                                   FEScreens::IsPushed("Pause_Main.fng"),
                                                   FEScreens::IsPushed("PostRace_Results.fng"), FEScreens::IsPushed("InGamePhotoMaster.fng"));
    if (step == CoopRace::OutWin::None) return;
    g_loseToWinEvent = ev; g_loseToWinGen = g_runGen;
    bool list = step == CoopRace::OutWin::ViaResults;
    if (list) { CreditTeamWinOut("the results list"); return; }   // the list stays, now a won race (its exit: hk_ResultsNotify)
    g_loseToWinFrom = list ? "the results list" : "the Restart / Quit screen";
    if (list) g_swapEvent = ev;                                   // the list must not end the race on its way out
    void* un = ((void*(__cdecl*)(uint32_t))0x627400)(8);          // step 1, EUnPause: the screen goes
    if (!un) { g_swapEvent = 0; return; }
    ((void(__thiscall*)(void*))0x625870)(un);
    g_loseToWinAt = GetTickCount();
    LOG("Race: the other player won while we were out - %s closes for the winner's photo", g_loseToWinFrom);
}

static void CoopWinTickImpl(bool connected, const Net::Snapshot& peer) {
    bool sameEvent = peer.valid && peer.s.eventHash && peer.s.eventHash == Game::CurrentEventHash();
    void* gri = (connected && sameEvent && peer.s.inRace) ? GhostRacerInfo() : nullptr;
    if (!gri || !g_peerRacedHere) return;
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
    if (!g_ok) return;
    __try { CoopWinLatchTick(connected, peer); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (GameFlowState() != 6) return;
    // before their racer is marked finished here: our racer then sorts first (rank 1) as well
    __try { if (connected) CoopFinishTickImpl(); } __except (EXCEPTION_EXECUTE_HANDLER) { LOG("Race: exception finishing our racer"); }
    __try { CoopWinTickImpl(connected, peer); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    __try { SharedLossTickImpl(connected, peer); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    __try { LoseScreenWinTickImpl(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
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
    // Instead our racer gets place 1 from the race script's own CalculateRanking once the other player
    // won, and is finished as the winner when their win arrives while we drive (CoopFinishTickImpl).
    const uint8_t calc[5] = { 0xE8, 0x3B, 0x35, 0x0E, 0x00 };   // call 0x6E8DF0
    bool d = PatchJmp(0x6058B0, calc, 5, (void*)&hk_CalcRanking);
    // (lose = the bytes of ShowLosingPostRaceScreen 0x60E230)
    bool e1 = PatchJmp(0x60E230, lose, 9, (void*)&hk_ShowLosingKO);
    const uint8_t koStart[7] = { 0x8B, 0x44, 0x24, 0x04, 0x83, 0xEC, 0x14 };   // mov eax, [esp+4]; sub esp, 0x14
    bool e2 = PatchJmp(0x611440, koStart, 7, (void*)&hk_KnockoutRacer);
    bool e = e1 && e2;
    // the photo's and the list's way out of "raceover" for a shared win, then the photo's line naming the winner
    uintptr_t cur = 0, curPhoto = 0;
    bool g = Mem::Read(kResultsNotifySlot, cur) && cur == kResultsNotifyFn &&
             Mem::Read(kPhotoNotifySlot, curPhoto) && curPhoto == kPhotoNotifyFn &&
             (g_origResultsNotify = (FeNotifyFn)Mem::PatchPointer((void*)kResultsNotifySlot, (void*)&hk_ResultsNotify)) != nullptr &&
             (g_origPhotoNotify = (FeNotifyFn)Mem::PatchPointer((void*)kPhotoNotifySlot, (void*)&hk_PhotoNotify)) != nullptr;
    g_photoOutOk = g;
    bool tr = g && PatchCallChecked(0x611F9C, 0x60AC20, (void*)&ShowPostRaceFromFlow) &&  // EnterPostRaceFlow
                   PatchCallChecked(0x60E242, 0x60AC20, (void*)&ShowPostRaceCoop);       // ShowWinningPostRaceScreen
    LOG("Career: the winner's photo only for whoever really won (the partner: the results list) %s", tr ? "ok" : "FAIL (code differs)");
    bool f = g && PatchCallChecked(0x5A865D, 0x5A2AD0, (void*)&hk_PhotoSetup);       // PhotoFinishScreen::Setup
    LOG("Career: the winner's photo for a shared win, naming who won %s", f ? "ok" : "FAIL (code differs)");
    LOG("Career: shared wins %s/%s/%s, winner's place for the race script %s, shared loss / knockout hooks %s", a ? "ok" : "FAIL", b ? "ok" : "FAIL", c ? "ok" : "FAIL", d ? "ok" : "FAIL (code differs)", e ? "ok" : "FAIL (code differs)");
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

// Test: drive our car into the other player's car (placed 12 m away, moving at 90 km/h towards it),
// and print our own position, to check that a hit pushes the car that was hit on its own PC.
// `ram cop`: the same against the nearest of the other player's police cars shown here (a mirror).
static void RamSafe(char* out, size_t n, bool cop) {
    __try {
        void* me = RigidBodyOf(LocalVehicle()); void* him = cop ? nullptr : RigidBodyOf(g_ghost);
        if (cop && me) {
            float best = 1e9f;
            for (auto& m : g_mirrors) {
                void* rb = (m.used && m.iv && m.n && (m.s[m.n - 1].e.flags & 1)) ? RigidBodyOf(m.iv) : nullptr;
                float d = rb ? Dist(RB_GetVec(rb, RB_GetPosition), RB_GetVec(me, RB_GetPosition)) : 1e9f;
                if (d < best) { best = d; him = rb; }
            }
        }
        if (!me || !him) { strcpy_s(out, n, cop ? "none of the other player's cops shown here" : "no car to ram"); return; }
        const float* hp = RB_GetVec(him, RB_GetPosition);
        const float* mp = RB_GetVec(me, RB_GetPosition);
        float dx = hp[0] - mp[0], dz = hp[2] - mp[2], len = sqrtf(dx * dx + dz * dz);
        if (len < 0.5f) { dx = 1; dz = 0; len = 1; }
        dx /= len; dz /= len;
        // A jump of more than 30 m re-arms spawn protection on both PCs (collisions off for 3 s), so from
        // far away first park 25 m from the other car; the second `ram` (5 s later) then does the hit.
        float zero[3] = { 0, 0, 0 };
        if (len > 30.f) {
            float park[3] = { hp[0] - dx * 25.f, hp[1] + 0.3f, hp[2] - dz * 25.f };
            // A car put on ground the game hasn't loaded is moved back to its old road a moment later
            // (seen in two-PC tests): load the world there first (as `tp`), park on the next `ram`.
            static float loadAt[3] = { 1e9f, 1e9f, 1e9f }; static DWORD loadedAt = 0;
            bool loaded = Dist(loadAt, park) < 50.f && GetTickCount() - loadedAt >= 3000 && GetTickCount() - loadedAt < 60000;
            StreamHoldAt(park, 30000);
            if (!loaded) {
                memcpy(loadAt, park, 12); loadedAt = GetTickCount();
                sprintf_s(out, n, "loading the world next to %s - send ram%s again in 5 s", cop ? "their cop" : "the other player", cop ? " cop" : "");
                return;
            }
            loadedAt = 0;
            RB_SetVec(me, RB_SetPosition, park); RB_SetVec(me, RB_SetLinearVelocity, zero); RB_SetVec(me, RB_SetAngularVelocity, zero);
            sprintf_s(out, n, cop ? "parked 25 m from their cop - send ram cop again in 5 s" : "parked 25 m from the other player - send ram again in 5 s");
            return;
        }
        float pos[3] = { hp[0] - dx * 12.f, hp[1] + 0.3f, hp[2] - dz * 12.f }, vel[3] = { dx * 25.f, 0, dz * 25.f };
        g_testJumpOkUntil = GetTickCount() + 400;      // this short hop is the test itself, not a teleport to ignore
        RB_SetVec(me, RB_SetPosition, pos); RB_SetVec(me, RB_SetLinearVelocity, vel); RB_SetVec(me, RB_SetAngularVelocity, zero);
        sprintf_s(out, n, cop ? "ramming the other player's cop from 12 m at 90 km/h" : "ramming the other player from 12 m at 90 km/h");
    } __except (EXCEPTION_EXECUTE_HANDLER) { strcpy_s(out, n, "exception"); }
}
std::string Game::DebugRam(bool cop) { char b[96] = "not in game"; if (g_ok && GameFlowState() == 6) RamSafe(b, sizeof(b), cop); return b; }
static void PosSafe(char* out, size_t n) {
    __try {
        void* me = RigidBodyOf(LocalVehicle());
        if (!me) { strcpy_s(out, n, "no car"); return; }
        const float* p = RB_GetVec(me, RB_GetPosition); const float* v = RB_GetVec(me, RB_GetLinearVelocity);
        CarState cs{}; ReadCarControls(LocalVehicle(), cs);
        sprintf_s(out, n, "pos %.1f %.1f %.1f speed %.1f m/s damage %08X lights %08X", p[0], p[1], p[2],
                  sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]), cs.damageZones, cs.brokenLights);
    } __except (EXCEPTION_EXECUTE_HANDLER) { strcpy_s(out, n, "exception"); }
}
std::string Game::DebugPos() { char b[128] = "not in game"; if (g_ok && GameFlowState() == 6) PosSafe(b, sizeof(b)); return b; }
// ---------------------------------------------------------------------------
// Catch-up ("rubber banding") follows the player who is ahead.
//  Once a second each AI racer measures its gap to the humans: 0x4293D0 calls 0x4255A0(&out, 1, 0, 0)
//  at 0x42956F, which sums GRacerInfo race % (+0x18) over the players list and returns {sum, count};
//  the caller turns it into an average. In co-op the other player is a racer slot, not in that list,
//  so the AI only ever measured itself against the local player. We answer that call ourselves with
//  the progress of whichever human is further along.
// ---------------------------------------------------------------------------
namespace {
    struct PctAvg { float sum, cnt; };
    using AvgFn = PctAvg* (__cdecl*)(PctAvg*, int, float, float);
    // Verified in the exe: 0x4255A0 has this one caller, and every racer AI - the Blacklist rival too -
    // gets its catch-up only through 0x4293D0 (vtable 0x8923C0; called from 0x42AAD4 and 0x42F1D5): the
    // gap to the players feeds a PID (0x653E20 on AI+0x7B0), whose output GRaceStatus 0x5FF990 turns into
    // the AI's skill (AI+0x7B8 / +0x7AC). The rival-only branches there (race flags 0x5FC800 / 0x5FBD90:
    // the race's own catch-up tables, a higher floor for a boss) only reshape that output; they measure
    // nothing themselves. The other players-list users in the AI are not about progress (0x428450:
    // career performance matching to the player's car; 0x42C390 / 0x42F570: cops).
    // Rival races: the partner's car isn't a racer here, so their own race % (sent with their state,
    // moved on to now like the standings do) is the measure. A partner whose state drops out for a
    // moment (a lost packet burst, a menu flicker) keeps counting for 3 s, so the AI never falls back to
    // waiting for the player behind in between.
    bool LeaderPct(float& out) {
        __try {
            void* mine = RacerInfoOfLocal();
            float a = 0, b = 0;
            if (!mine || !Mem::Read((uintptr_t)mine + 0x18, a)) return false;
            const Net::Snapshot& p = Net::Latest();
            void* gri = GhostRacerInfo();
            uint32_t ev = Game::CurrentEventHash();
            // a partner who already finished is out of the race: the AI goes back to racing just us
            bool linked = Net::Stats().connected && g_coopLinked;
            bool peerRacing = linked && p.valid && p.s.inRace && !p.s.finished && p.s.eventHash && p.s.eventHash == ev;
            static DWORD lastOk = 0; static uint32_t lastOkEvent = 0; static float lastPeer = 0;
            DWORD now = GetTickCount();
            bool peerOk = false;
            const char* how = "";
            uint8_t restart = 0; Mem::Read(0x91CF28, restart);   // a restart is pending: no old values
            if (peerRacing && gri && Mem::Read((uintptr_t)gri + 0x18, b)) { peerOk = true; }   // their racer slot (WritePeerProgress keeps it current)
            else if (peerRacing && !gri) {                                                                 // rival races: their car isn't a racer here
                b = p.s.racePct;
                int ping = Net::Stats().pingMs;
                Net::RemoteRacePctNow(ping > 0 && ping < 1000 ? ping * 0.5 : 0.0, b);
                peerOk = true; how = ", rival race";
            } else if (linked && lastOk && ev && ev == lastOkEvent && now - lastOk < 3000 && !(p.valid && p.s.finished) && !restart) {
                b = lastPeer; peerOk = true; how = ", partner's last value";                               // a short gap in their state
            }
            if (peerOk && peerRacing) { lastOk = now; lastOkEvent = ev; lastPeer = b; }
            if (!peerOk) {
                if (linked && ev) {                                                                        // why the partner doesn't count (diagnostics)
                    static DWORD lastSoloLog = 0;
                    if (now - lastSoloLog > 15000) {
                        lastSoloLog = now;
                        LOG("Race: catch-up measured against us only (ours %.1f%%; partner state %d, in race %d, finished %d, event %08X, ours %08X)",
                            a, (int)p.valid, (int)p.s.inRace, (int)p.s.finished, p.s.eventHash, ev);
                    }
                }
                return false;                                                                      // solo: leave the game alone
            }
            NetUtil::CatchUpLeaderPct(a, true, b, out);
            static DWORD lastLog = 0;
            if (now - lastLog > 15000) { lastLog = now; LOG("Race: catch-up measured against %.1f%% (ours %.1f%%, partner %.1f%%%s)", out, a, b, how); }
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    PctAvg* __cdecl hk_PlayersPct(PctAvg* out, int list, float s0, float c0) {
        float pct = 0;
        if (g_cfg.catchUpLeader && list == 1 && LeaderPct(pct)) { out->sum = pct; out->cnt = 1.0f; return out; }
        return ((AvgFn)0x4255A0)(out, list, s0, c0);
    }
}

// ---------------------------------------------------------------------------
// Standings: the other player's place.
//  GRaceStatus update 0x602C80 (racing only), once per sim step: for each racer slot (array at
//  GRaceStatus+0x18, 0x194 bytes each) it calls GRacerInfo update 0x5E92E0 (call at 0x602D67), which
//  recomputes race % (+0x18) and lap % (+0x24) from laps (+0x28), checkpoints this lap (+0x2C) and the
//  car's remaining distance to the next checkpoint (IVehicleAI 0x403700, vtable +0x60); then it sorts
//  (SortRacers 0x5FDE80; compare 0x5E31D0: finished first, then more laps, then higher race %) and
//  hands out ranks (+0x10). All verified in the exe.
//  The other player's racer has its racing AI switched off (we drive it), so that AI distance is not
//  kept up and their % was mostly worked out from the checkpoints they had passed: they only moved up
//  when they reached the next checkpoint (seen: ~100 m ahead before the order changed). Our own write
//  of their % came after the sort (at Present), so it never decided the order.
//  Now, right after the game updates their slot and before the sort, we put in the % their own game
//  computed (the same formula on the same route, so both PCs use one scale), moved on to this moment
//  by their recent rate (NetUtil::RacePctAt), plus their laps and checkpoints.
//  Why their row takes an AI opponent's slot instead of being one extra row: the race script reads
//  Game.GetNumRacers() once (basicrace initial state, right after InitRacers 0x61DBE0, whose count is
//  1 - humans + GRaceParameters 0x5FACD0 and whose opponents come from 0x5FD2D0(i)) and sizes its
//  per-racer tables (InRace, GoalList, NextGoal, LapsLeft) by it. Its checkpoint / finish handler looks
//  every car that enters a trigger up with GetRacerIndex and indexes those tables: a racer slot added
//  after that has no entries (a Lua error at their first checkpoint), and one more opponent before it
//  needs an opponent record the event doesn't have (0x5FD2D0(N)) plus a grid slot. Not done.
// ---------------------------------------------------------------------------
namespace {
    // the other player's progress into their racer slot; false when it isn't theirs to write
    bool WritePeerProgress(void* ri, float* usedPct = nullptr) {
        const Net::Snapshot& p = Net::Latest();
        if (!ri || !Net::Stats().connected || !g_coopLinked || !p.valid || !p.s.inRace || !p.s.eventHash ||
            p.s.eventHash != Game::CurrentEventHash()) return false;
        uint8_t fin = 0;
        if (!Mem::Read((uintptr_t)ri + 0x21, fin) || fin) return false;   // finished: CoopWinTick owns the slot
        float pct = p.s.racePct;
        int ping = Net::Stats().pingMs;
        Net::RemoteRacePctNow(ping > 0 && ping < 1000 ? ping * 0.5 : 0.0, pct);   // to now: + their one-way delay
        int laps = p.s.laps, cps = p.s.checkpoints;
        float lapPct = p.s.lapPct;
        Mem::Write((uintptr_t)ri + 0x18, pct);
        Mem::Write((uintptr_t)ri + 0x24, lapPct);
        Mem::Write((uintptr_t)ri + 0x28, laps);
        Mem::Write((uintptr_t)ri + 0x2C, cps);
        // +0xF8: distance to their next checkpoint. The game takes it from the racer's AI, which is off for
        // their car (we drive it): it stayed at a value from where the car stood (the standings showed them
        // ~5 km away all race). Worked back from their % the way 0x5E92E0 builds the %: covered = lap base
        // + the checkpoint legs passed + (this leg - distance to the next) ; % = covered / race length.
        char* rs = nullptr;
        if (Mem::Read(0x91E000, rs) && rs && laps >= 0 && cps >= 0 && cps < 64) {
            using LenFn = float(__thiscall*)(void*, int);
            using LegFn = float(__thiscall*)(void*, int, int);
            float total = ((LenFn)0x5DB420)(rs, laps);
            float base = 0, lapFirst = 0, lapOther = 0;
            Mem::Read((uintptr_t)rs + 0x4488, lapFirst); Mem::Read((uintptr_t)rs + 0x448C, lapOther);
            if (laps > 0) base = lapFirst;
            if (laps > 1) base += (laps - 1) * lapOther;
            float passed = 0;
            for (int i = 0; i < cps; ++i) passed += ((LegFn)0x5DC720)(rs, i, laps);
            float leg = ((LegFn)0x5DC720)(rs, cps, laps);
            float dist = leg - (pct * 0.01f * total - base - passed);
            if (dist == dist && total > 0 && leg > 0) {
                if (dist < 0) dist = 0; if (dist > leg) dist = leg;
                Mem::Write((uintptr_t)ri + 0xF8, dist);
            }
        }
        if (usedPct) *usedPct = pct;
        return true;
    }
    // client: the host's opponents' progress (PKT_RACEPROG), by traffic id
    struct HostRacerProg { uint16_t id; RaceProgEntry e; DWORD at; };
    HostRacerProg g_hostProg[MAX_RACEPROG] = {};
    void OnHostRaceProg(const RaceProgEntry* e, int n) {
        DWORD now = GetTickCount();
        for (int i = 0; i < n; ++i) {
            HostRacerProg* slot = nullptr;
            for (auto& h : g_hostProg) if (h.at && h.id == e[i].id) { slot = &h; break; }
            if (!slot) for (auto& h : g_hostProg) if (!h.at || now - h.at > 5000) { slot = &h; break; }
            if (slot) { slot->id = e[i].id; slot->e = e[i]; slot->at = now; }
        }
    }
    // one of our racers that copies a host opponent: their progress into its slot (its AI is off, so the
    // game's own distance - and with it the standings distance and %% - stood still)
    void WriteHostOpponentProgress(void* ri) {
        if (g_cfg.isHost || !g_coopLinked) return;
        DWORD now = GetTickCount();
        for (auto& m : g_mirrors) {
            if (!m.used || !m.racer || !m.iv || RacerInfoOf(m.iv) != ri) continue;
            for (auto& h : g_hostProg) {
                if (!h.at || h.id != m.id || now - h.at > 2000) continue;
                uint8_t fin = 0; uint32_t outs = 0;
                Mem::Read((uintptr_t)ri + 0x21, fin); Mem::Read((uintptr_t)ri + 0x1C, outs);
                if (fin || outs) return;                                   // over here: the game owns the slot
                int laps = h.e.laps, cps = h.e.cps;
                Mem::Write((uintptr_t)ri + 0x18, h.e.pct); Mem::Write((uintptr_t)ri + 0x24, h.e.lapPct);
                Mem::Write((uintptr_t)ri + 0x28, laps); Mem::Write((uintptr_t)ri + 0x2C, cps);
                Mem::Write((uintptr_t)ri + 0xF8, h.e.toNext);
                return;
            }
            return;
        }
    }
    using RacerUpdateFn = void(__thiscall*)(void*, float);
    void __fastcall hk_RacerUpdate(void* ri, void*, float dt) {
        ((RacerUpdateFn)0x5E92E0)(ri, dt);
        __try {
            if (ri && g_ghost && g_ghostKind == GK_RACER && ri == GhostRacerInfo()) WritePeerProgress(ri);
            else if (ri) WriteHostOpponentProgress(ri);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}

// ---------------------------------------------------------------------------
// Split time to the leader (diagnostics). Seen in co-op knockouts: the client 2nd behind the host and
// the split read -5:00, the whole race time - the leader's recorded times are wrong for the other
// player. The HUD's split (0x57AB10, called by the race timer widget at 0x58C56E while its +0x44 flag
// is set) reads the widget's rows (0x78 each from +0x58: progress float, then 29 time samples from
// +0x5C), [+0x40] our row, [+0x3C] a sample count: split = our row's time at our sample - row 0's
// (leading: row 1's at its sample - ours). Logged each time a split is shown, to find which time is off.
// ---------------------------------------------------------------------------
namespace {
    using SplitShowFn = bool(__thiscall*)(void*, void*);
    void LogSplitRows(void* w) {
        int cnt = 0, our = 0;
        Mem::Read((uintptr_t)w + 0x3C, cnt); Mem::Read((uintptr_t)w + 0x40, our);
        LOG("Split: shown - our row %d, sample count %d, peer in race %d", our, cnt, (int)(Net::Latest().valid && Net::Latest().s.inRace));
        for (int r = 0; r < 4; ++r) {
            uintptr_t row = (uintptr_t)w + 0x58 + r * 0x78;
            float prog = 0; Mem::Read(row, prog);
            char buf[400]; size_t len = 0; int set = 0;
            for (int i = 0; i < 29; ++i) {
                float t = 0; Mem::Read(row + 4 + i * 4, t);
                if (t != 0.f) ++set;
                int wr = _snprintf_s(buf + len, sizeof(buf) - len, _TRUNCATE, "%s%.1f", i ? " " : "", t);
                if (wr < 0) break;
                len += (size_t)wr;
            }
            LOG("Split: row %d progress %.3f, %d time(s) set: %s", r, prog, set, buf);
        }
    }
    bool __fastcall hk_SplitShow(void* w, void*, void* arg) {
        bool shown = ((SplitShowFn)0x57AB10)(w, arg);
        __try { if (shown && w) LogSplitRows(w); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return shown;
    }
}

bool Game::InstallCatchUp() {
    bool sp = PatchCallChecked(0x58C56E, 0x57AB10, (void*)&hk_SplitShow);
    LOG("Split: diagnostics %s", sp ? "hooked" : "NOT hooked (call site differs)");
    bool st = PatchCallChecked(0x602D67, 0x5E92E0, (void*)&hk_RacerUpdate);
    Net::OnRaceProg = [](const RaceProgEntry* e, int n) { OnHostRaceProg(e, n); };
    LOG("Race: standings use the other player's own progress %s", st ? "hooked" : "NOT hooked (call site differs)");
    bool ok = PatchCallChecked(0x42956F, 0x4255A0, (void*)&hk_PlayersPct);
    LOG("Race: catch-up follows the leading player %s", ok ? "hooked" : "NOT hooked (call site differs)");
    return ok;
}
static void TeleportSafe(const float* to, char* out, size_t n) {
    __try {
        void* me = RigidBodyOf(LocalVehicle());
        if (!me) { strcpy_s(out, n, "no car"); return; }
        float pos[3] = { to[0], to[1] + 2.f, to[2] }, zero[3] = { 0, 0, 0 };
        RB_SetVec(me, RB_SetPosition, pos); RB_SetVec(me, RB_SetLinearVelocity, zero); RB_SetVec(me, RB_SetAngularVelocity, zero);
        sprintf_s(out, n, "teleported to %.1f %.1f %.1f", to[0], to[1], to[2]);
    } __except (EXCEPTION_EXECUTE_HANDLER) { strcpy_s(out, n, "exception"); }
}
// The first 'tp' to a place loads the world there (the game puts a car standing on unloaded ground back
// on its old road); the same 'tp' again a few seconds later moves the car.
std::string Game::DebugTeleport(const char* args) {
    float p[3];
    if (sscanf_s(args, "%f %f %f", &p[0], &p[1], &p[2]) != 3) return "usage: tp x y z (as printed by pos)";
    if (!g_ok || GameFlowState() != 6) return "not in game";
    static float last[3] = { 1e9f, 1e9f, 1e9f }; static DWORD loadedAt = 0;
    bool same = fabsf(last[0] - p[0]) < 1.f && fabsf(last[1] - p[1]) < 1.f && fabsf(last[2] - p[2]) < 1.f;
    StreamHoldAt(p, 30000);
    if (Game::CloseInGameOverlays())
        return "closing the menu first - send the same tp again";
    if (!same || GetTickCount() - loadedAt > 60000) {
        memcpy(last, p, 12); loadedAt = GetTickCount();
        return "loading the world there - send the same tp again in about 5 s";
    }
    char b[96] = "";
    TeleportSafe(p, b, sizeof(b));
    return b;
}
