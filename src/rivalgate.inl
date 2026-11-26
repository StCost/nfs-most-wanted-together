// Included from game.cpp (after bosshelper.inl).
// ---------------------------------------------------------------------------
// Blacklist rival race in co-op: only when both players are ready for it
//  A rival race opens once a player has the rival's races, milestones and bounty done. In co-op the
//  other player may not be there yet; then our rival race waits for them: its marker is hidden, the
//  Blacklist's "Challenge Rival" is off and says "<name> not ready", and a start from the race sheet is
//  refused. Both games send whether their current rival's race is open (CarState pad[1], see
//  rivalgateutil.h); with the other player ready (or not linked / an older build) nothing changes.
//  Verified in the exe (v1.3):
//   uiRepSheetMain::UpdateInfo 0x52EE50 (Blacklist screen; called at 0x551407 Setup, 0x551504
//   ScrollRival, 0x55A5C0 NotificationMessage) sets +0x149 "challenge available" from
//   GRaceParameters::GetIsAvailable(career = 2) 0x5FD160 (called at 0x52F1B8) of the viewed bin's
//   (iCurrentViewBin 0x91CA3C) boss races; +0x149 shows the button 0x55F6AA1A ("RIVAL_CHALLENGE_ACTIVATED")
//   and NotificationMessage 0xC519BFC3 (the button) only works with it set. GetIsAvailable(2) =
//   CheckRaceScoreFlags(kUnlocked_Career 8) && !IsCareerRaceComplete (or UnlockAllThings 0x926124).
//   The race sheet (uiRepSheetRival::NotificationMessage 0x551790, vtable slot 0x89D7BC): accept
//   0x406415E3 with +0x2D clear and FEDatabase+0x12C bit 17 clear starts the race +0x30 (in game:
//   GManager::StartRaceFromInGame 0x5FDCB0, from the safe house: RaceStarter).
//   The world's rival marker: MenuZoneTrigger::RequestEventInfoDialog 0x569050 (vtable slot 0x8A2518)
//   opens the race sheet (ERaceSheetOn) for a boss race; GManager::RefreshEngageTriggerIcons 0x602840
//   shows a race trigger's icon (GTrigger+0xC0 GIcon, flags +2: 1 world, 2 map) while its race is
//   unlocked and not completed. UISafehouseRaceSheet starts races at 0x55172B (StartRaceFromInGame).
// ---------------------------------------------------------------------------
namespace {
    constexpr uintptr_t RgIsAvailCall = 0x52F1B8, RgIsAvailFn = 0x5FD160;
    constexpr uintptr_t RgUpdateInfoFn = 0x52EE50;
    constexpr uintptr_t RgUpdateInfoCalls[3] = { 0x551407, 0x551504, 0x55A5C0 };
    constexpr uintptr_t RgSafehouseStartCall = 0x55172B, RgStartFromInGameFn = 0x5FDCB0;
    constexpr uintptr_t RgSlotSheetNotify = 0x89D7BC, RgOrigSheetNotify = 0x551790;
    constexpr uintptr_t RgSlotZoneInfo = 0x8A2518, RgOrigZoneInfo = 0x569050;
    constexpr uint32_t  RgChallengeButton = 0x55F6AA1A, RgMsgAccept = 0x406415E3;

    using RgNotify_t = void(__thiscall*)(void*, uint32_t, void*, uint32_t, uint32_t);
    using RgZoneInfo_t = void(__thiscall*)(void*, int);
    RgNotify_t   o_RgSheetNotify = nullptr;
    RgZoneInfo_t o_RgZoneInfo = nullptr;

    bool     g_rgOn = false;               // hooks in place
    bool     g_rgKnown = false, g_rgReady = false; int g_rgIdx = 0;   // our current rival's race
    int      g_rgBin = 0;
    uint32_t g_rgBoss[8] = {}; int g_rgBossN = 0;
    bool     g_rgGated = false;            // our rival race waits for the other player
    bool     g_rgSheetGated = false;       // the Blacklist screen's last UpdateInfo turned the button off
    void*    g_rgStr[6] = {}; uint32_t g_rgLabel[6] = {}; int g_rgStrN = 0;   // button strings we wrote
    DWORD    g_rgNoticeAt = 0;
    int      g_rgForce = 0;                // test (rivalgate force): gate as if the other player weren't ready

    void* RgDb() { void* db = nullptr; Mem::Read(0x91E004, db); return db; }
    uint32_t RgEventHash(void* parms) { return parms ? ((uint32_t(__thiscall*)(void*))0x5FAB40)(parms) : 0; }
    std::string RgPeerName() { const std::string& n = Net::Stats().peerName; return n.empty() ? std::string("Your partner") : n; }

    bool RgOurBoss(uint32_t h) {
        if (!h) return false;
        for (int i = 0; i < g_rgBossN; ++i) if (g_rgBoss[i] == h) return true;
        return false;
    }
    bool RgGatedHash(uint32_t h) { return g_rgGated && RgOurBoss(h); }
    bool RgGatedParms(void* parms) { return g_rgGated && parms && RgOurBoss(RgEventHash(parms)); }

    void RgRefusedNotice(const char* where) {
        DWORD now = GetTickCount();
        if (now - g_rgNoticeAt < 4000) return;
        g_rgNoticeAt = now;
        Game::QueueNotice(RgPeerName() + " is not ready for the rival race yet", true);
        LOG("Rival: %s refused - the other player is not ready for the rival race", where);
    }

    // our current rival: its boss races, and the first one open in the career
    void RgScan() {
        g_rgKnown = false; g_rgReady = false; g_rgIdx = 0; g_rgBossN = 0;
        int bin = Game::CareerBin();
        g_rgBin = bin;
        if (bin < 1 || bin > 15) return;
        void* db = RgDb();
        if (!db) return;
        void* rb = ((void*(__thiscall*)(void*, int))0x5E3980)(db, bin);           // GRaceDatabase::GetBinNumber
        if (!rb) return;
        unsigned n = ((unsigned(__thiscall*)(void*))0x5DCF60)(rb);                  // GRaceBin::GetBossRaceCount
        if (n > 8) n = 8;
        for (unsigned i = 0; i < n; ++i) {
            uint32_t h = ((uint32_t(__thiscall*)(void*, unsigned))0x601020)(rb, i);  // GRaceBin::GetBossRaceHash
            g_rgBoss[g_rgBossN++] = h;
            void* parms = h ? ((void*(__thiscall*)(void*, uint32_t))0x5FB710)(db, h) : nullptr;
            if (!g_rgReady && parms && ((bool(__thiscall*)(void*, int))RgIsAvailFn)(parms, 2)) { g_rgReady = true; g_rgIdx = (int)i; }
        }
        g_rgKnown = true;
    }

    // hide (or leave alone) the world / map icons of our rival's races
    void RgHideIcons() {
        void* head = nullptr; Mem::Read(0x91DFF8, head);
        if (!head) return;
        void* db = RgDb();
        if (!db) return;
        char* t = (char*)head;
        int guard = 0;
        do {
            uint16_t kind = 0; Mem::Read((uintptr_t)t + 0x18, kind);
            if (kind & 0x200) {                                                     // a GTrigger
                void* inst = ((void*(__thiscall*)(void*, const void*, int))0x5E2850)(t, (const void*)0x8A3704, 0);
                if (inst) {
                    uint32_t key = ((uint32_t(__thiscall*)(void*))0x452430)((char*)inst + 4);   // Attrib::Instance::GetCollection
                    void* parms = ((void*(__thiscall*)(void*, uint32_t))0x600A20)(db, key);   // GetRaceFromKey
                    if (parms && RgOurBoss(RgEventHash(parms))) {
                        char* icon = nullptr; Mem::Read((uintptr_t)t + 0xC0, icon);
                        uint8_t f = 0;
                        if (icon && Mem::Read((uintptr_t)icon + 2, f) && (f & 3)) { f &= ~3; Mem::Write((uintptr_t)icon + 2, f); }
                    }
                }
            }
            char* next = nullptr;
            if (!Mem::Read((uintptr_t)t + 0x24, next) || !next) break;
            t = next;
        } while (t != head && ++guard < 4096);
    }
    void RgRefreshIcons() {
        void* gm = nullptr; Mem::Read(0x91E00C, gm);
        if (gm && Game::GameFlowState() == 6) ((void(__thiscall*)(void*))0x602840)(gm);   // GManager::RefreshEngageTriggerIcons
    }

    // ---- hooks ----
    bool __fastcall hk_RgIsAvailable(void* parms, void*, int ctx) {
        bool r = ((bool(__thiscall*)(void*, int))RgIsAvailFn)(parms, ctx);
        __try {
            int view = 0; Mem::Read(0x91CA3C, view);
            if (r && ctx == 2 && view == g_rgBin && RgGatedParms(parms)) { g_rgSheetGated = true; return false; }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return r;
    }

    // the strings of a front end group (the Blacklist's challenge button)
    void RgStrings(char* obj, void** out, int& n, int max, int depth = 0) {
        if (!obj || depth > 3 || n >= max) return;
        int type = 0; Mem::Read((uintptr_t)obj + 0x18, type);
        if (type == 2) { out[n++] = obj; return; }
        if (type != 5) return;
        int count = 0; char* c = nullptr;
        Mem::Read((uintptr_t)obj + 0x60, count); Mem::Read((uintptr_t)obj + 0x64, c);
        for (int i = 0; i < count && i < 32 && c && n < max; ++i) {
            RgStrings(c, out, n, max, depth + 1);
            if (!Mem::Read((uintptr_t)c + 4, c)) break;
        }
    }
    void RgSheetText(void* sheet) {
        const char* pkg = nullptr; Mem::Read((uintptr_t)sheet + 0x10, pkg);
        void* fe = nullptr; Mem::Read(0x91CADC, fe);
        if (!pkg || !fe) return;
        void* fp = ((void*(__thiscall*)(void*, const char*))0x516AF0)(fe, pkg);      // cFEng::FindPackage
        char* btn = fp ? (char*)((void*(__thiscall*)(void*, uint32_t))0x5B8550)(fp, RgChallengeButton) : nullptr;
        void* cur[6] = {}; int n = 0;
        RgStrings(btn, cur, n, 6);
        if (g_rgSheetGated) {
            if (!btn || !n) return;
            g_rgStrN = 0;
            for (int i = 0; i < n; ++i) { g_rgStr[i] = cur[i]; g_rgLabel[i] = 0; Mem::Read((uintptr_t)cur[i] + 0x60, g_rgLabel[i]); ++g_rgStrN; }
            std::string text = RgPeerName() + " not ready";
            ((int(__cdecl*)(const char*, uint32_t, const char*, ...))0x5252D0)(pkg, RgChallengeButton, "%s", text.c_str());   // FEPrintf
            ((void(__cdecl*)(void*))0x514CC0)(btn);                                     // FEngSetVisible
            LOG("Rival: Blacklist - Challenge Rival off, \"%s\"", text.c_str());
        } else if (g_rgStrN) {
            // the button is the game's again: its own text back (only strings still on this screen)
            for (int i = 0; i < g_rgStrN; ++i)
                for (int j = 0; j < n; ++j)
                    if (cur[j] == g_rgStr[i] && g_rgLabel[i]) ((void(__cdecl*)(void*, uint32_t))0x515C00)(cur[j], g_rgLabel[i]);
            g_rgStrN = 0;
        }
    }
    void __fastcall hk_RgUpdateInfo(void* sheet, void*) {
        g_rgSheetGated = false;
        ((void(__thiscall*)(void*))RgUpdateInfoFn)(sheet);
        __try { RgSheetText(sheet); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    void __fastcall hk_RgSheetNotify(void* sheet, void*, uint32_t msg, void* obj, uint32_t a, uint32_t b) {
        if (msg == RgMsgAccept && g_rgGated) {
            bool refuse = false;
            __try {
                uint8_t post = 1; Mem::Read((uintptr_t)sheet + 0x2D, post);
                char* fdb = nullptr; uint32_t fl = 0; Mem::Read(0x91CF90, fdb);
                if (fdb) Mem::Read((uintptr_t)fdb + 0x12C, fl);
                void* parms = nullptr; Mem::Read((uintptr_t)sheet + 0x30, parms);
                refuse = !post && !(fl & (1u << 17)) && RgGatedParms(parms);
            } __except (EXCEPTION_EXECUTE_HANDLER) { refuse = false; }
            if (refuse) { RgRefusedNotice("rival race start (race sheet)"); return; }
        }
        o_RgSheetNotify(sheet, msg, obj, a, b);
    }
    void __fastcall hk_RgZoneInfo(void* zone, void*, int arg) {
        if (g_rgGated) {
            bool refuse = false;
            __try {
                void* act = nullptr; Mem::Read((uintptr_t)zone + 0x18, act);
                void* db = RgDb();
                void* parms = (act && db) ? ((void*(__thiscall*)(void*, void*))0x6022D0)(db, act) : nullptr;   // GetRaceFromActivity
                refuse = RgGatedParms(parms);
            } __except (EXCEPTION_EXECUTE_HANDLER) { refuse = false; }
            if (refuse) { RgRefusedNotice("rival race marker"); return; }
        }
        o_RgZoneInfo(zone, arg);
    }
    void __fastcall hk_RgSafehouseStart(void* gm, void*, uint32_t hash) {
        if (RgGatedHash(hash)) { RgRefusedNotice("rival race start (safe house race sheet)"); return; }
        ((void(__thiscall*)(void*, uint32_t))RgStartFromInGameFn)(gm, hash);
    }

    void RgTick(bool linked) {
        static DWORD nextScan = 0, nextHide = 0;
        static bool lastGated = false, lastWaits = false;
        DWORD now = GetTickCount();
        if (now - nextScan < 0x80000000u) {
            nextScan = now + 500;
            RgScan();
            const Net::Snapshot& p = Net::Latest();
            RivalGate::Peer peer = RivalGate::Decode(p.valid, p.s.pad[1]);
            int peerBin = p.valid ? p.s.pad[2] : 0;
            bool on = linked && Net::Stats().connected && g_rgKnown;
            g_rgGated = RivalGate::Gated(on, g_rgBin, g_rgReady, g_rgIdx, peerBin, peer);
            if (g_rgForce) g_rgGated = g_rgKnown && g_rgBossN > 0;
            bool waits = RivalGate::PartnerWaits(on, g_rgBin, g_rgReady, g_rgIdx, peerBin, peer);
            // The notices wait until the state has held for 3 s: right after a won rival race the other
            // game's career update arrives a moment after ours (seen: "not ready" for 0.5 s at the win).
            static DWORD gatedSince = 0; static bool toldThisGate = false;
            static DWORD toldGatedAt = 0; static int toldGatedBin = 0;   // said once a minute at most
            if (g_rgGated != lastGated) {
                lastGated = g_rgGated;
                LOG("Rival: rival race #%d %s (ours: open %d race %d, theirs: Blacklist #%d open %d race %d)", g_rgBin,
                    g_rgGated ? "waits for the other player" : "open again", (int)g_rgReady, g_rgIdx, peerBin, (int)peer.ready, peer.idx);
                if (g_rgGated) { gatedSince = now ? now : 1; toldThisGate = false; }
                else {
                    RgRefreshIcons();
                    if (toldThisGate && on && g_rgReady && peer.known && peer.ready && peerBin == g_rgBin) {
                        toldGatedAt = 0;
                        Game::QueueNotice(RgPeerName() + " is ready - Challenge Rival is on", false);
                    }
                    gatedSince = 0; toldThisGate = false;
                }
            }
            if (g_rgGated && gatedSince && !toldThisGate && now - gatedSince >= 3000) {
                toldThisGate = true;
                if (toldGatedBin != g_rgBin || now - toldGatedAt > 60000) {
                    toldGatedBin = g_rgBin; toldGatedAt = now;
                    Game::QueueNotice(RgPeerName() + " is not ready for the rival race yet - Challenge Rival waits for them", true);
                }
            }
            if (waits != lastWaits) {
                lastWaits = waits;
                static DWORD toldWaitsAt = 0;
                if (waits && peerBin == g_rgBin && (!toldWaitsAt || now - toldWaitsAt > 60000)) {
                    toldWaitsAt = now;
                    Game::QueueNotice(RgPeerName() + " is ready for the rival race - it waits for your races, milestones and bounty", false);
                    LOG("Rival: the other player's rival race waits for us");
                }
            }
        }
        if (g_rgGated && Game::GameFlowState() == 6 && now - nextHide < 0x80000000u) { nextHide = now + 250; RgHideIcons(); }
    }
}

void Game::RivalGateTick(bool linked) {
    if (!g_ok || !g_rgOn) return;
    __try { RgTick(linked); } __except (EXCEPTION_EXECUTE_HANDLER) { g_rgGated = false; }
}

std::string Game::DebugRivalGate(const char* arg) {
    std::string a = arg ? arg : "";
    if (a == "force") g_rgForce = 1; else if (a == "auto") g_rgForce = 0;
    const Net::Snapshot& p = Net::Latest();
    RivalGate::Peer peer = RivalGate::Decode(p.valid, p.s.pad[1]);
    char b[256];
    sprintf_s(b, "rival gate %s%s: Blacklist #%d, %d race(s), open %d (race %d); partner #%d known %d open %d (race %d)",
              g_rgGated ? "ON" : "off", g_rgForce ? " (forced)" : "", g_rgBin, g_rgBossN, (int)g_rgReady, g_rgIdx,
              p.valid ? p.s.pad[2] : 0, (int)peer.known, (int)peer.ready, peer.idx);
    return b;
}

uint8_t Game::RivalGatePad(bool allowed) {
    return RivalGate::Encode(allowed, g_rgOn && g_rgKnown, g_rgReady, g_rgIdx);
}

static void InstallRivalGate() {
    bool a = PatchCallChecked(RgIsAvailCall, RgIsAvailFn, (void*)&hk_RgIsAvailable);
    bool u = true;
    for (uintptr_t site : RgUpdateInfoCalls) u &= PatchCallChecked(site, RgUpdateInfoFn, (void*)&hk_RgUpdateInfo);
    bool s = PatchCallChecked(RgSafehouseStartCall, RgStartFromInGameFn, (void*)&hk_RgSafehouseStart);
    void* cur = nullptr;
    bool n = Mem::Read(RgSlotSheetNotify, cur) && cur == (void*)RgOrigSheetNotify;
    if (n) o_RgSheetNotify = (RgNotify_t)Mem::PatchPointer((void*)RgSlotSheetNotify, (void*)&hk_RgSheetNotify);
    n = n && o_RgSheetNotify == (RgNotify_t)RgOrigSheetNotify;
    bool z = Mem::Read(RgSlotZoneInfo, cur) && cur == (void*)RgOrigZoneInfo;
    if (z) o_RgZoneInfo = (RgZoneInfo_t)Mem::PatchPointer((void*)RgSlotZoneInfo, (void*)&hk_RgZoneInfo);
    z = z && o_RgZoneInfo == (RgZoneInfo_t)RgOrigZoneInfo;
    if (!o_RgSheetNotify) o_RgSheetNotify = (RgNotify_t)RgOrigSheetNotify;
    if (!o_RgZoneInfo) o_RgZoneInfo = (RgZoneInfo_t)RgOrigZoneInfo;
    g_rgOn = a && n;   // the Blacklist button and the race sheet are the must-haves
    LOG("Rival: co-op rival race gate %s (available %d, Blacklist %d, race sheet %d, marker %d, safe house %d)",
        g_rgOn ? "on" : "OFF (call sites differ)", (int)a, (int)u, (int)n, (int)z, (int)s);
}
