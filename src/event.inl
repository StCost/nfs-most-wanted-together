// Included from game.cpp (uses its vehicle helpers).
// ---------------------------------------------------------------------------
// Shared event start
//  When a player engages a career event (race parameters appear while roaming), their game sends the
//  event ID. The other game (if it's in free roam) looks the event up, teleports its car onto the
//  event's start marker and accepts the "engage event" dialog through Macro (input.cpp), so both
//  load the same event through the game's own flow (career bookkeeping stays intact).
// Verified/known addresses (v1.3):
//  GRaceStatus::fObj          *(0x91E000)   +0x1960 mPlayMode (0 roaming, 1 racing)
//                                            +0x1964 mRaceContext (0 quick race, 1 online, 2 career)
//                                            +0x1968 mRaceParms (GRaceParameters*)
//  GRaceDatabase::mObj        *(0x91E004)
//  GRaceDatabase::GetRaceFromHash   0x5FB710  thiscall(db, uint32 hash) -> GRaceParameters*
//  GRaceParameters::GetEventID      0x5FBA70  thiscall(parms) -> const char*
//  GRaceParameters::GetStartPosition 0x5FAD10 thiscall(parms, Vector3* out)
// ---------------------------------------------------------------------------
namespace {
    void*    g_lastParms = nullptr;          // race parameters we last saw on this machine
    uint32_t g_handledHash = 0; DWORD g_handledAt = 0;   // invitation we already acted on (repeats are ignored)
    uint32_t g_curEventHash = 0;             // event we are in (0 = none), sent with our state
    uint32_t g_joiningEvent = 0;             // event hash we're joining because the peer started it
    DWORD    g_joinStartedAt = 0;
    char     g_eventStatus[96] = "";

    void* RaceStatus() { void* p = nullptr; Mem::Read(0x91E000, p); return p; }
    void* CurrentParms(int* playMode = nullptr, int* context = nullptr) {
        void* rs = RaceStatus();
        if (!rs) return nullptr;
        void* parms = nullptr;
        Mem::Read((uintptr_t)rs + 0x1968, parms);
        if (playMode) Mem::Read((uintptr_t)rs + 0x1960, *playMode);
        if (context) Mem::Read((uintptr_t)rs + 0x1964, *context);
        return parms;
    }
    const char* EventIdOf(void* parms) {
        if (!parms) return nullptr;
        const char* id = ((const char*(__thiscall*)(void*))0x5FBA70)(parms);
        return (id && Mem::IsReadable(id, 1)) ? id : nullptr;
    }
    uint32_t HashName(const char* s) { return ((uint32_t(__cdecl*)(const char*))0x4519D0)(s); }

    // Challenge Series events run with race context 0 like quick races, but only they have a preset
    // player car (GRaceParameters::GetPlayerCarType 0x5FC5C0 returns a non-empty name).
    bool IsChallengeEvent(void* parms) {
        if (!parms) return false;
        const char* car = ((const char*(__thiscall*)(void*))0x5FC5C0)(parms);
        return car && Mem::IsReadable(car, 1) && *car;
    }
    uint32_t EventHashOf(void* parms) {           // GRaceParameters::GetEventHash (0x5FAB40)
        return parms ? ((uint32_t(__thiscall*)(void*))0x5FAB40)(parms) : 0;
    }

    // Start an event the way the game does (found by RE of the Safe House race sheet / Blacklist):
    //  free roam (flow 6, roaming): 0x5FDCB0 thiscall(GManager = *(0x91E00C), eventHash)
    //  front end (flow 3):          db = *(0x91E004); c = db->0x603440(parms); db->0x600AB0(c, 2 /*career*/);
    //                               db->0x5E39E0(c); 0x57ECA0()  (GameFlowManager::LoadTrack)
    void OnEventPacket(uint32_t eventHash, const char* eventId, bool fromPeer = true) {
        // the sender repeats an invitation for a few seconds (UDP may lose it): act on it once
        if (fromPeer && g_handledHash == eventHash && GetTickCount() - g_handledAt < 30000) return;
        int flow = Game::GameFlowState();
        int mode = 0;
        void* cur = CurrentParms(&mode);
        if (cur && (mode == 1 || fromPeer)) {     // racing, or already loading into an event
            static DWORD lastLog = 0;
            if (GetTickCount() - lastLog > 5000) { lastLog = GetTickCount(); LOG("Event: %s requested but we're already in an event - ignored", eventId); }
            return;
        }
        void* db = nullptr;
        Mem::Read(0x91E004, db);
        void* parms = db ? ((void*(__thiscall*)(void*, uint32_t))0x5FB710)(db, eventHash) : nullptr;
        if (!parms) { LOG("Event: unknown event %s (%08X)", eventId, eventHash); return; }
        g_joiningEvent = fromPeer ? eventHash : 0;   // a local test start is sent on to the peer
        // build the same AI opponents as the peer (roster packets arrive just before the event)
        g_usePeerRoster = fromPeer && g_peerRosterN > 0 && GetTickCount() - g_peerRosterAt < 5000;
        g_peerRosterNext = 0;
        if (fromPeer) LOG("Event: using %s", g_usePeerRoster ? "the peer's opponents" : "our own opponents (no roster received)");
        // free the vehicle budget so every racer can be built: our free-roam copy of the peer's car
        // and the traffic mirrors go (the race builds the peer's car itself)
        Game::ReleaseGhost();
        g_racerReplacedThisLoad = false;   // a new race (also when started from free roam: no world load)
        g_replacedRacer = nullptr;
        g_noSpawnUntil = GetTickCount() + 8000;
        for (auto& m : g_mirrors) if (m.used) RemoveMirror(m);
        g_joinStartedAt = GetTickCount();
        if (flow == 6) {
            void* gm = nullptr;
            Mem::Read(0x91E00C, gm);
            if (!gm) { LOG("Event: no GManager"); return; }
            ((void(__thiscall*)(void*, uint32_t))0x5FDCB0)(gm, eventHash);
            sprintf_s(g_eventStatus, "starting %s (free roam)", eventId);
            LOG("Event: starting %s from free roam (GManager 0x5FDCB0)", eventId);
            if (fromPeer) { g_handledHash = eventHash; g_handledAt = GetTickCount(); }   // only once it really started
        } else if (flow == 3) {
            void* c = ((void*(__thiscall*)(void*, void*))0x603440)(db, parms);
            if (!c) { LOG("Event: could not create the race for %s", eventId); return; }
            // challenge events use context 0 (the world load then builds the preset car), career 2
            ((void(__thiscall*)(void*, void*, int))0x600AB0)(db, c, IsChallengeEvent(parms) ? 0 : 2);
            ((void(__thiscall*)(void*, void*))0x5E39E0)(db, c);
            ((void(__cdecl*)())0x57ECA0)();
            sprintf_s(g_eventStatus, "starting %s (menu)", eventId);
            LOG("Event: starting %s from the front end (LoadTrack)", eventId);
            if (fromPeer) { g_handledHash = eventHash; g_handledAt = GetTickCount(); }
        } else {
            LOG("Event: %s requested while loading (flow %d) - ignored", eventId, flow);
            g_joiningEvent = 0;
        }
    }
}

namespace {
    RosterEntry g_inviteRoster[8]; int g_inviteN = 0;
    uint32_t g_inviteHash = 0; char g_inviteId[40] = ""; DWORD g_inviteUntil = 0, g_inviteNext = 0;
    void SendInvite() {
        for (int i = 0; i < g_inviteN; ++i) {
            RosterPacket rp{};
            rp.index = (uint8_t)i; rp.count = (uint8_t)g_inviteN;
            rp.carKey = g_inviteRoster[i].key;
            rp.hasCustom = g_inviteRoster[i].hasCustom ? 1 : 0;
            memcpy(rp.custom, g_inviteRoster[i].custom, sizeof(rp.custom));
            Net::SendRoster(rp);
        }
        Net::SendEvent(g_inviteHash, g_inviteId);
    }
    DWORD g_restartResendAt[2] = { 0, 0 }; uint32_t g_restartResendHash = 0;
    uint32_t g_restartSeq = 0, g_peerRestartSeq = 0; DWORD g_ownRestartAt = 0;
}

static void EventTickImpl(bool connected) {
    int mode = 0, ctx = 0;
    void* parms = CurrentParms(&mode, &ctx);
    if (parms != g_lastParms) {
        g_lastParms = parms;
        const char* id = EventIdOf(parms);
        g_curEventHash = (parms && id) ? EventHashOf(parms) : 0;
        if (!parms) g_handledHash = 0;                         // event over: the same one may be started again
        if (g_inviteHash && g_inviteHash != g_curEventHash) g_inviteHash = 0;   // we left it: stop inviting
        if (parms && id) {
            uint32_t h = EventHashOf(parms);
            LOG("Event: race parameters now %s (%08X), mode %d, context %d", id, h, mode, ctx);
            if (h == g_joiningEvent) {
                sprintf_s(g_eventStatus, "joined %s", id);
                g_joiningEvent = 0;                          // we started it because the peer did: don't echo
            } else if (connected && (ctx == 2 /*career*/ || (ctx == 0 && IsChallengeEvent(parms)))) {
                // our AI opponents first, so the peer builds the same ones. Kept and repeated for a
                // few seconds (SendInvite) until the peer is in the race too.
                g_inviteN = g_localRosterN;
                for (int i = 0; i < g_inviteN; ++i) g_inviteRoster[i] = g_localRoster[i];
                g_inviteHash = h; strncpy_s(g_inviteId, id, _TRUNCATE);
                g_inviteUntil = GetTickCount() + 6000; g_inviteNext = GetTickCount() + 1500;
                SendInvite();
                LOG("Event: sent %d opponent(s) to peer", g_inviteN);
                sprintf_s(g_eventStatus, "started %s - inviting peer", id);
                LOG("Event: sent %s to peer", id);
            }
        }
        g_localRosterN = 0;        // the next event collects its own opponents
        g_usePeerRoster = false;
    }
    // repeat the invitation until the peer is in the race (or 6 s passed)
    if (g_inviteHash) {
        DWORD now = GetTickCount();
        const Net::Snapshot& pr = Net::Latest();
        if (!connected || (int)(now - g_inviteUntil) > 0 || (pr.valid && pr.s.inRace)) g_inviteHash = 0;
        else if ((int)(now - g_inviteNext) >= 0) { g_inviteNext = now + 1500; SendInvite(); }
    }
    if (g_joiningEvent && GetTickCount() - g_joinStartedAt > 20000) {
        LOG("Event: joining timed out");
        strcpy_s(g_eventStatus, "join timed out");
        g_joiningEvent = 0;
    }
}

bool Game::InstallEventSync() {
    Net::OnRoster = [](const RosterPacket& p) {
        if (p.index >= 8 || p.count > 8) return;
        // a new roster only after a pause: repeats of the same invitation must not reset it mid-build
        if (GetTickCount() - g_peerRosterAt > 3000 && !g_usePeerRoster) g_peerRosterN = 0;
        RosterEntry& r = g_peerRoster[p.index];
        r.key = p.carKey; r.hasCustom = p.hasCustom != 0;
        memcpy(r.custom, p.custom, sizeof(r.custom));
        memcpy(r.name, p.name, sizeof(r.name)); r.name[23] = 0;
        if (p.index + 1 > g_peerRosterN) g_peerRosterN = p.index + 1;
        g_peerRosterAt = GetTickCount();
    };
    Net::OnEvent = [](uint32_t hash, const char* id) {
        __try { OnEventPacket(hash, id); } __except (EXCEPTION_EXECUTE_HANDLER) { LOG("Event: exception handling peer event"); }
    };
    return true;
}

void Game::EventTick(bool connected) {
    if (!g_ok) return;
    __try { EventTickImpl(connected); } __except (EXCEPTION_EXECUTE_HANDLER) { LOG("Event: exception"); }
}

const char* Game::EventStatus() { return g_eventStatus; }

// Test (F7, TestKeys=1): start an event here exactly as if the peer had started it, but without the
// "don't echo" flag, so once it engages it is also sent to the peer.
static bool DebugStartEventSafe(const char* eventId) {
    void* db = nullptr;
    __try {
        // the test key takes an event ID string: find its race to get the game's event hash
        Mem::Read(0x91E004, db);
        void* parms = db ? ((void*(__thiscall*)(void*, uint32_t))0x5FB710)(db, HashName(eventId)) : nullptr;
        OnEventPacket(parms ? EventHashOf(parms) : HashName(eventId), eventId, false);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
std::string Game::DebugStartEvent(const char* eventId) {
    if (!g_ok) return "game not recognised";
    if (!DebugStartEventSafe(eventId)) return "exception";
    return std::string("test start of event ") + eventId;
}

// ---------------------------------------------------------------------------
// Co-op rules: no speedbreaker (slowing time can't work when two games share one world).
//  Local IPlayer = first entry of the PLAYER_LIST local bucket (mBegin at 0x92D87C); its concrete vtable
//  is 0x8B0B10: slot 16 InGameBreaker (byte +0x34), 18 ResetGameBreaker(bool full) (false -> meter
//  [+0x38] = 0.0), 20 ToggleGameBreaker.
// ---------------------------------------------------------------------------
static void CoopRulesImpl() {
    void** begin = nullptr;
    if (!Mem::Read(0x92D87C, begin) || !begin) return;
    void* pl = nullptr;
    if (!Mem::Read((uintptr_t)begin, pl) || !pl || VTable(pl) != 0x8B0B10) return;
    bool active = VCall<bool(__thiscall*)(void*)>(pl, 16)(pl);
    if (active) { VCall<void(__thiscall*)(void*)>(pl, 20)(pl); LOG("Co-op: speedbreaker switched off"); }
    float meter = 0;
    if (Mem::Read((uintptr_t)pl + 0x38, meter) && meter > 0.f)
        VCall<void(__thiscall*)(void*, bool)>(pl, 18)(pl, false);
}

void Game::CoopRulesTick(bool connected) {
    if (!g_ok || !connected || !g_cfg.noSpeedbreaker || GameFlowState() != 6) return;
    __try { CoopRulesImpl(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ---------------------------------------------------------------------------
// No pause in co-op: SimKernel_ShouldRun (0x6EF150) stops the simulation while the front end has a
// pause request (pause menu, world map). Skipping that one check (jz -> jmp at 0x6EF1B1) keeps the
// world running while the menu still works. Only while connected; solo play pauses as usual.
// Race restarts: ERestartRace's constructor (0x6307F0) sets "restart pending" (byte 0x91CF28). We watch
// it and tell the peer, which creates the same event (0x627400(8) alloc + ctor) while racing.
// ---------------------------------------------------------------------------
namespace {
    bool  g_noPausePatched = false;
    bool  g_restartFromPeer = false;
    uint8_t g_lastRestartFlag = 0;

    void SetNoPause(bool on) {
        if (on == g_noPausePatched) return;
        const uint8_t jz[2] = { 0x74, 0x11 }, jmp[2] = { 0xEB, 0x11 };
        bool ok = on ? Mem::PatchBytes(0x6EF1B1, jz, jmp, 2) : Mem::PatchBytes(0x6EF1B1, jmp, jz, 2);
        if (ok) g_noPausePatched = on;
        LOG("Coop: pause menu %s%s", on ? "no longer stops the world" : "pauses again", ok ? "" : " (patch FAILED)");
    }

    bool TriggerRestart() {
        void* m = ((void*(__cdecl*)(uint32_t))0x627400)(8);
        if (!m) return false;
        ((void*(__thiscall*)(void*))0x6307F0)(m);
        return true;
    }
}

static void RestartTickImpl(bool connected) {
    uint8_t flag = 0;
    Mem::Read(0x91CF28, flag);
    if (flag && !g_lastRestartFlag) {
        if (g_restartFromPeer) g_restartFromPeer = false;        // we did it because the peer did
        else if (connected) {
            int mode = 0;
            void* parms = CurrentParms(&mode);
            ++g_restartSeq; g_ownRestartAt = GetTickCount();
            Net::SendRestart(EventHashOf(parms), g_restartSeq);
            g_restartResendHash = EventHashOf(parms);            // twice more, in case it was lost
            g_restartResendAt[0] = GetTickCount() + 700; g_restartResendAt[1] = GetTickCount() + 1600;
            LOG("Race: restarted here - telling the peer");
        }
    }
    g_lastRestartFlag = flag;
    for (DWORD& at : g_restartResendAt)
        if (at && (int)(GetTickCount() - at) >= 0) { at = 0; if (connected) Net::SendRestart(g_restartResendHash, g_restartSeq); }
}

void Game::PauseRuleTick(bool connected) {
    if (!g_ok) return;
    __try {
        SetNoPause(connected && g_cfg.noPause);
        RestartTickImpl(connected);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

bool Game::InstallRestartSync() {
    Net::OnRestart = [](uint32_t hash, uint32_t seq) {
        __try {
            int mode = 0;
            void* parms = CurrentParms(&mode);
            uint8_t pending = 0;
            Mem::Read(0x91CF28, pending);
            if (seq == g_peerRestartSeq) return;                              // a repeat of one we already handled
            if (hash && EventHashOf(parms) != hash) { LOG("Race: peer restarted a different event - ignored"); return; }
            g_peerRestartSeq = seq;
            if (g_ownRestartAt && GetTickCount() - g_ownRestartAt < 2500) return;   // we both restarted at once
            if (Game::GameFlowState() != 6 || mode != 1 || pending) { LOG("Race: peer restarted, but we're not racing - ignored"); return; }
            if (hash && EventHashOf(parms) != hash) { LOG("Race: peer restarted a different event - ignored"); return; }
            g_restartFromPeer = true;
            if (!TriggerRestart()) { g_restartFromPeer = false; LOG("Race: restart failed"); return; }
            LOG("Race: peer restarted the race - restarting here too");
        } __except (EXCEPTION_EXECUTE_HANDLER) { LOG("Race: exception restarting"); }
    };
    return true;
}
// Test command: restart the current race here (the restart is then sent to the peer as usual).
static const char* DebugRestartSafe() {
    __try {
        int mode = 0; CurrentParms(&mode);
        if (Game::GameFlowState() != 6 || mode != 1) return "not racing";
        return TriggerRestart() ? "restarting the race" : "restart failed";
    } __except (EXCEPTION_EXECUTE_HANDLER) { return "exception"; }
}
std::string Game::DebugRestart() {
    if (!g_ok) return "game not recognised";
    return DebugRestartSafe();
}
uint32_t Game::CurrentEventHash() { return g_curEventHash; }
