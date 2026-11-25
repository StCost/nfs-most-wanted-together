// Included from game.cpp (after rivalgate.inl).
// ---------------------------------------------------------------------------
// Police roadblocks, the same in both games
//  Each game runs its own pursuit, and its cop manager builds roadblocks for it on its own (different
//  places, layouts, spike strips), so each player saw only their own. Now every roadblock either game
//  builds is shown in the other game exactly as built:
//   - its cop cars (patrol cars, or the Rhino SUVs of a heavy roadblock) are the owner's cops, sent in
//     PKT_TRAFFIC first and flagged TE_ROADBLOCK (traffic.inl); the copies stand where the owner's cars
//     stand, and one of our unseen traffic cars makes room for them at the 20-car limit;
//   - its sawhorses and spike strips are sent in PKT_ROADBLOCK with the exact matrix the owner's game
//     placed them with; we build the same pieces with the same matrix (snapped to the ground as there),
//     so the gaps are the same. Our spike strips are real ones: driving over them pops OUR tyres.
//   - while one of theirs stands within 600 m of us, our game builds none of its own (the request
//     waits and goes up once theirs is gone - then theirs is ours, shown on their side). Heavy support
//     roadblocks are never held back (their SUVs are already on the way).
//   - removed by the owner's game (all its cops gone) -> removed here; not heard of for 5 s, or co-op
//     unlinked -> removed here. A world reload forgets our pieces (the world freed them).
//  Verified in the exe (v1.3):
//   AICopManager::CreateRoadBlock 0x43DD30 (thiscall: IPursuit*, int cops, IVehicle* chopper,
//     IVehicle::List* suvList; ret 0x10) - called at 0x43E7D1 (heavy support, suvList set) and 0x43EC35
//     (the pursuit's latched request). It finds the road 250 m ahead of the chased car, picks a setup
//     (PickRoadblockSetup 0x4063D0, cdecl width/cars/spikes, called at 0x43E1D0; spikes when
//     SimRandom*100 < pursuitlevels roadblockspikechance, never in cooldown), puts the cars there
//     (ResetVehicleToRoadPos) and creates the pieces: IPlaceableScenery::CreateInstance 0x6816A0 (cdecl
//     name, smackable key) at 0x43E539 (sawhorse) / 0x43E54F (spike strip), then box->Place(matrix,
//     snap) = vt+8 at 0x43E69B (after `mov [esp+0x10C],edx` at 0x43E694, the matrix's z).
//     IPlaceableScenery vt+0xC Destroy.
//   AICopManager: ICopMgr singleton 0x92C67C = manager + 0x54; its IRoadBlock list (std::list head
//     node) at +0x134 (UpdateRoadBlocks 0x439EE0; node: next, prev, IRoadBlock*). A roadblock with no
//     cops left is released there (ReleaseAllSmackables destroys its pieces).
//   IPursuit vt+0x3C GetRoadBlock. IRoadBlock (AIRoadBlock+0x50, vtable 0x892130): vehicles vector
//     begin/end +0x0C/+0x10, pieces +0x1C/+0x20, centre +0x34, direction +0x40 (unit vector, seen in game: the centre
//     sits between the pieces, the direction is level along the road).
// ---------------------------------------------------------------------------
namespace {
    constexpr uintptr_t RbCreateFn = 0x43DD30;
    constexpr uintptr_t RbCreateCalls[2] = { 0x43E7D1, 0x43EC35 };
    constexpr uintptr_t RbPickFn = 0x4063D0, RbPickCall = 0x43E1D0;
    constexpr uintptr_t RbListNormal = 0x8EB5E8, RbListSpikes = 0x8EBC68;
    constexpr uintptr_t RbSceneryCreateFn = 0x6816A0, RbSawhorseCall = 0x43E539, RbSpikesCall = 0x43E54F;
    constexpr uintptr_t RbPlaceSite = 0x43E694;
    constexpr uintptr_t RbSawhorseName = 0x892EB8, RbSpikesName = 0x892EA0;   // the exe's own strings
    constexpr uint32_t  RbSawhorseKey = 0x9663AD06, RbSpikesKey = 0xCA89EF8F;
    constexpr uintptr_t RbCopMgrSingleton = 0x92C67C;

    using RbCreate_t = bool(__fastcall*)(void*, void*, void*, int, void*, void*);
    using RbPick_t = void* (__cdecl*)(float, int, bool);
    using RbScenery_t = void* (__cdecl*)(const char*, uint32_t);

    bool g_rbOn = false;
    // what CreateRoadBlock built (filled by the hooks while it runs)
    struct RbCap { void* obj; uint8_t kind; bool placed; float m[16]; };
    RbCap g_rbCap[8]; int g_rbCapN = 0; bool g_rbCapOn = false;
    int  g_rbCapSetup = -1; bool g_rbCapSpikes = false;
    int  g_rbForce = 0;              // test: 1 force a plain roadblock, 2 one with spike strips

    struct RbOurs { bool used; void* rb; Roadblock::Desc d; DWORD lastSend; int removalLeft; };
    RbOurs g_rbOurs[Roadblock::MAX_OURS] = {};
    uint32_t g_rbNextId = 0;

    Roadblock::PeerTable g_rbPeer;
    void* g_rbObj[Roadblock::MAX_PEER][Roadblock::MAX_PIECES] = {};
    int  g_rbSuppressed = 0, g_rbBuilt = 0, g_rbShown = 0;
    float g_rbLastSim = 0;

    void* RbCopMgr() {
        void* s = nullptr;
        return (Mem::Read(RbCopMgrSingleton, s) && s) ? (uint8_t*)s - 0x54 : nullptr;
    }
    // is this IRoadBlock still in the cop manager's list?
    bool RbStillThere(void* rb) {
        void* mgr = RbCopMgr();
        if (!mgr || !rb) return false;
        uintptr_t head = 0, n = 0;
        if (!Mem::Read((uintptr_t)mgr + 0x134, head) || !head || !Mem::Read(head, n)) return false;
        for (int i = 0; i < 32 && n && n != head; ++i) {
            void* v = nullptr;
            if (!Mem::Read(n + 8, v)) return false;
            if (v == rb) return true;
            if (!Mem::Read(n, n)) return false;
        }
        return false;
    }
    bool RbOurPos(float* out) {
        void* rb = RigidBodyOf(LocalVehicle());
        if (!rb) return false;
        memcpy(out, RB_GetVec(rb, RB_GetPosition), 12);
        return true;
    }

    // ---- hooks inside CreateRoadBlock ----
    void* __cdecl hk_RbPick(float width, int cars, bool spikes) {
        if (g_rbForce) spikes = g_rbForce == 2;
        void* r = ((RbPick_t)RbPickFn)(width, cars, spikes);
        if (r) {
            uintptr_t base = spikes ? RbListSpikes : RbListNormal;
            g_rbCapSetup = (int)(((uintptr_t)r - base) / 0x68);
            g_rbCapSpikes = spikes;
        }
        return r;
    }
    void* RbCapCreate(const char* name, uint32_t key, uint8_t kind) {
        void* obj = ((RbScenery_t)RbSceneryCreateFn)(name, key);
        if (obj && g_rbCapOn && g_rbCapN < 8) { g_rbCap[g_rbCapN] = RbCap{ obj, kind, false, {} }; ++g_rbCapN; }
        return obj;
    }
    void* __cdecl hk_RbSawhorse(const char* name, uint32_t key) { return RbCapCreate(name, key, Roadblock::PIECE_SAWHORSE); }
    void* __cdecl hk_RbSpikes(const char* name, uint32_t key) { return RbCapCreate(name, key, Roadblock::PIECE_SPIKES); }
    void __stdcall RbRecordPlace(void* obj, const float* m) {
        if (!g_rbCapOn || !m) return;
        for (int i = 0; i < g_rbCapN; ++i)
            if (g_rbCap[i].obj == obj) { memcpy(g_rbCap[i].m, m, sizeof(g_rbCap[i].m)); g_rbCap[i].placed = true; return; }
    }
    // replaces `mov [esp+0x10C],edx; call [eax+8]` (10 bytes) at 0x43E694 with `call hk_RbPlace` + 5 nops:
    // does the store, records (box = ecx, matrix = first argument) and jumps to Place, which returns
    // to the nops right after our call with the stack exactly as the original call left it.
    void __declspec(naked) hk_RbPlace() {
        __asm {
            mov [esp + 0x110], edx       // the original store (+4: our return address)
            pushad
            push dword ptr [esp + 0x24]  // matrix (pushad 0x20 + return address 4)
            push ecx                     // the box
            call RbRecordPlace
            popad
            jmp dword ptr [eax + 8]      // IPlaceableScenery::Place(matrix, snap)
        }
    }

    Roadblock::Desc RbDescribe(void* rb, bool heavy) {
        Roadblock::Desc d{};
        d.id = ++g_rbNextId ? g_rbNextId : ++g_rbNextId;
        d.setup = (uint8_t)(g_rbCapSetup >= 0 ? g_rbCapSetup : 0);
        d.flags = (uint8_t)((g_rbCapSpikes ? Roadblock::RB_SPIKES : 0) | (heavy ? Roadblock::RB_HEAVY : 0));
        Mem::SafeRead((uint8_t*)rb + 0x34, d.centre, 12);
        Mem::SafeRead((uint8_t*)rb + 0x40, d.dir, 12);
        uintptr_t vb = 0, ve = 0, sb = 0, se = 0;
        Mem::Read((uintptr_t)rb + 0x0C, vb); Mem::Read((uintptr_t)rb + 0x10, ve);
        Mem::Read((uintptr_t)rb + 0x1C, sb); Mem::Read((uintptr_t)rb + 0x20, se);
        d.nCars = (uint8_t)((vb && ve > vb && ve - vb <= 32) ? (ve - vb) / 4 : 0);
        if (sb && se > sb && se - sb <= 64)
            for (uintptr_t p = sb; p < se && d.nPieces < Roadblock::MAX_PIECES; p += 4) {
                void* obj = nullptr;
                if (!Mem::Read(p, obj)) break;
                for (int i = 0; i < g_rbCapN; ++i)
                    if (g_rbCap[i].obj == obj && g_rbCap[i].placed) {
                        Roadblock::Piece& pc = d.p[d.nPieces++];
                        pc.kind = g_rbCap[i].kind;
                        memcpy(pc.m, g_rbCap[i].m, sizeof(pc.m));
                        break;
                    }
            }
        return d;
    }
    void RbOnOurs(void* pursuit, bool heavy) {
        void* rb = pursuit ? VCall<void* (__thiscall*)(void*)>(pursuit, 0x3C / 4)(pursuit) : nullptr;
        if (!rb) { LOG("Roadblock: built, but the pursuit has none - not shared"); return; }
        Roadblock::Desc d = RbDescribe(rb, heavy);
        ++g_rbBuilt;
        LOG("Roadblock: our game built #%u (%s setup %d%s): %d car(s), %d sawhorse(s), %d spike strip(s) at %.1f %.1f %.1f, facing %.2f %.2f",
            d.id, (d.flags & Roadblock::RB_SPIKES) ? "spike" : "plain", (int)d.setup, heavy ? ", heavy SUVs" : "", (int)d.nCars,
            Roadblock::CountKind(d, Roadblock::PIECE_SAWHORSE), Roadblock::CountKind(d, Roadblock::PIECE_SPIKES),
            d.centre[0], d.centre[1], d.centre[2], d.dir[0], d.dir[2]);
        if (!Roadblock::ValidDesc(d)) { LOG("Roadblock: #%u doesn't read as a valid roadblock - not shared", d.id); return; }
        int slot = -1;
        for (int i = 0; i < Roadblock::MAX_OURS; ++i) if (!g_rbOurs[i].used) { slot = i; break; }
        if (slot < 0) {   // all taken: the oldest one is most likely gone
            slot = 0;
            for (int i = 1; i < Roadblock::MAX_OURS; ++i) if ((int32_t)(g_rbOurs[i].lastSend - g_rbOurs[slot].lastSend) < 0) slot = i;
        }
        g_rbOurs[slot] = RbOurs{ true, rb, d, 0, 0 };
    }

    bool __fastcall hk_RbCreate(void* self, void*, void* pursuit, int cops, void* chopper, void* suvList) {
        if (!suvList && g_coopLinked && Net::Stats().connected) {
            float me[3];
            if (RbOurPos(me) && Roadblock::SuppressOurs(g_rbPeer, me, GetTickCount(), true)) {
                if (++g_rbSuppressed == 1 || g_rbSuppressed % 600 == 0)
                    LOG("Roadblock: the other player's roadblock stands near us - our game builds none of its own (%d request(s) held)", g_rbSuppressed);
                return false;
            }
        }
        g_rbCapN = 0; g_rbCapSetup = -1; g_rbCapSpikes = false; g_rbCapOn = true;
        bool ok = ((RbCreate_t)RbCreateFn)(self, nullptr, pursuit, cops, chopper, suvList);
        g_rbCapOn = false;
        if (ok) { __try { RbOnOurs(pursuit, suvList != nullptr && !g_rbForce); } __except (EXCEPTION_EXECUTE_HANDLER) { LOG("Roadblock: exception reading our new roadblock"); } }
        g_rbCapN = 0;
        return ok;
    }

    // ---- the other player's roadblocks ----
    void RbDestroyPieces(int i, bool worldGone) {
        for (int k = 0; k < Roadblock::MAX_PIECES; ++k) {
            void* obj = g_rbObj[i][k];
            g_rbObj[i][k] = nullptr;
            if (!obj || worldGone) continue;
            __try { VCall<void(__thiscall*)(void*)>(obj, 0xC / 4)(obj); }
            __except (EXCEPTION_EXECUTE_HANDLER) { LOG("Roadblock: exception removing a piece"); }
        }
        g_rbPeer.e[i].placed = false;
    }
    void RbPlacePieces(int i) {
        const Roadblock::Desc& d = g_rbPeer.e[i].d;
        int built = 0;
        for (int k = 0; k < d.nPieces && k < Roadblock::MAX_PIECES; ++k) {
            bool spikes = d.p[k].kind == Roadblock::PIECE_SPIKES;
            void* obj = ((RbScenery_t)RbSceneryCreateFn)((const char*)(spikes ? RbSpikesName : RbSawhorseName), spikes ? RbSpikesKey : RbSawhorseKey);
            if (!obj) continue;
            VCall<bool(__thiscall*)(void*, const float*, bool)>(obj, 8 / 4)(obj, d.p[k].m, true);
            g_rbObj[i][k] = obj;
            ++built;
        }
        g_rbPeer.e[i].placed = true;
        ++g_rbShown;
        LOG("Roadblock: showing the other player's roadblock #%u (%s setup %d%s): %d of %d piece(s) built (%d spike strip(s)), %d car(s) come as their cops",
            d.id, (d.flags & Roadblock::RB_SPIKES) ? "spike" : "plain", (int)d.setup, (d.flags & Roadblock::RB_HEAVY) ? ", heavy SUVs" : "",
            built, (int)d.nPieces, Roadblock::CountKind(d, Roadblock::PIECE_SPIKES), (int)d.nCars);
    }
    void RbForget(int i, bool worldGone, const char* why) {
        if (!g_rbPeer.e[i].used) return;
        LOG("Roadblock: the other player's roadblock #%u %s", g_rbPeer.e[i].d.id, why);
        RbDestroyPieces(i, worldGone);
        g_rbPeer.e[i] = Roadblock::PeerEntry{};
    }
    void OnRoadblockPacket(const Roadblock::Desc& d, bool alive) {
        if (!g_rbOn || !g_coopLinked) return;
        int i = g_rbPeer.Find(d.id);
        if (!alive) { if (i >= 0) RbForget(i, false, "was removed in their game - removed here"); return; }
        // the same id for another roadblock (their game restarted): build the new one
        if (i >= 0 && (Roadblock::Dist3(g_rbPeer.e[i].d.centre, d.centre) > 1.f || g_rbPeer.e[i].d.nPieces != d.nPieces)) {
            RbForget(i, false, "is replaced by another one with its id");
            i = -1;
        }
        if (i < 0) {
            bool evicted = false;
            i = g_rbPeer.Alloc(&evicted);
            if (evicted) RbForget(i, false, "makes room for a newer one");
            g_rbPeer.e[i] = Roadblock::PeerEntry{};
            g_rbPeer.e[i].used = true;
            g_rbPeer.e[i].d = d;
            LOG("Roadblock: the other player's game built roadblock #%u at %.1f %.1f %.1f", d.id, d.centre[0], d.centre[1], d.centre[2]);
        }
        g_rbPeer.e[i].lastMs = GetTickCount() ? GetTickCount() : 1;
    }

    void RoadblockTickImpl(bool linked) {
        DWORD now = GetTickCount();
        float sim = Game::SimTimeSec();
        bool reload = sim + 1.f < g_rbLastSim;
        g_rbLastSim = sim;
        if (reload) {   // the world was loaded again: its pieces (ours and theirs) are gone with it
            for (int i = 0; i < Roadblock::MAX_PEER; ++i) if (g_rbPeer.e[i].used) RbDestroyPieces(i, true);
            for (auto& o : g_rbOurs) if (o.used && o.rb) { o.rb = nullptr; o.removalLeft = Roadblock::kRemovalRepeats; }
        }
        // ours: still standing? tell the other game (every second), or that it is gone (3x)
        for (auto& o : g_rbOurs) {
            if (!o.used) continue;
            if (o.rb && !RbStillThere(o.rb)) {
                LOG("Roadblock: our roadblock #%u is gone", o.d.id);
                o.rb = nullptr; o.removalLeft = Roadblock::kRemovalRepeats;
            }
            if (!o.rb) {
                if (linked) Net::SendRoadblock(o.d, false);
                if (--o.removalLeft <= 0 || !linked) o = RbOurs{};
                continue;
            }
            if (linked && now - o.lastSend >= Roadblock::kResendMs) {
                uintptr_t vb = 0, ve = 0;
                if (Mem::Read((uintptr_t)o.rb + 0x0C, vb) && Mem::Read((uintptr_t)o.rb + 0x10, ve) && ve >= vb && ve - vb <= 32)
                    o.d.nCars = (uint8_t)((ve - vb) / 4);
                Net::SendRoadblock(o.d, true);
                o.lastSend = now;
            }
        }
        // theirs
        float me[3]; bool haveMe = RbOurPos(me);
        for (int i = 0; i < Roadblock::MAX_PEER; ++i) {
            Roadblock::PeerEntry& x = g_rbPeer.e[i];
            if (!x.used) continue;
            if (!linked) { RbForget(i, false, "- co-op not linked, removed here"); continue; }
            if (Roadblock::Expired(x, now)) { RbForget(i, false, "- not heard of for 5 s, removed here"); continue; }
            if (!x.placed && haveMe && Game::GameFlowState() == 6 && Roadblock::InRange(x, me, Roadblock::kPlaceRange)) RbPlacePieces(i);
        }
    }
}

static void RoadblockOnRestart() {
    // a race restart unloads the pursuits (and their roadblocks); the world stays: remove what we built
    for (int i = 0; i < Roadblock::MAX_PEER; ++i) if (g_rbPeer.e[i].used) RbForget(i, false, "- race restart, removed here");
}

void Game::RoadblockTick(bool linked) {
    if (!g_rbOn) return;
    __try { RoadblockTickImpl(linked); }
    __except (EXCEPTION_EXECUTE_HANDLER) { LOG("Roadblock: exception in the tick"); }
}

static void InstallRoadblockSync() {
    g_rbNextId = (GetTickCount() & 0xFFFFF) << 8;   // a restarted game doesn't reuse the ids of the last session
    int calls = 0;
    for (uintptr_t site : RbCreateCalls) calls += PatchCallChecked(site, RbCreateFn, (void*)&hk_RbCreate) ? 1 : 0;
    bool pick = PatchCallChecked(RbPickCall, RbPickFn, (void*)&hk_RbPick);
    bool saw = PatchCallChecked(RbSawhorseCall, RbSceneryCreateFn, (void*)&hk_RbSawhorse);
    bool spk = PatchCallChecked(RbSpikesCall, RbSceneryCreateFn, (void*)&hk_RbSpikes);
    // mov [esp+0x10C],edx ; call [eax+8]
    const uint8_t expect[10] = { 0x89, 0x94, 0x24, 0x0C, 0x01, 0x00, 0x00, 0xFF, 0x50, 0x08 };
    uint8_t repl[10] = { 0xE8, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90, 0x90 };
    int32_t rel = (int32_t)((uintptr_t)&hk_RbPlace - (RbPlaceSite + 5));
    memcpy(repl + 1, &rel, 4);
    bool place = Mem::PatchBytes(RbPlaceSite, expect, repl, sizeof(repl));
    g_rbOn = calls == 2 && pick && saw && spk && place;
    if (!g_rbOn) {   // half the hooks would share roadblocks without their pieces: undo the call patches
        for (uintptr_t site : RbCreateCalls) {
            uint8_t op = 0; int32_t r = 0;
            if (Mem::Read(site, op) && Mem::Read(site + 1, r) && op == 0xE8 && site + 5 + r == (uintptr_t)&hk_RbCreate) Mem::PatchCall(site, (void*)RbCreateFn);
        }
    }
    LOG("Roadblock: shared roadblocks %s (create %d/2, setup %s, pieces %s/%s, place %s)", g_rbOn ? "on" : "OFF",
        calls, pick ? "ok" : "FAIL", saw ? "ok" : "FAIL", spk ? "ok" : "FAIL", place ? "ok" : "FAIL");
    Net::OnRoadblock = [](const Roadblock::Desc& d, bool alive) {
        __try { OnRoadblockPacket(d, alive); } __except (EXCEPTION_EXECUTE_HANDLER) { LOG("Roadblock: exception on a packet"); }
    };
}

#ifdef MWT_DEVTOOLS
// Test command 'roadblock [plain|spikes|list]': build a roadblock for our pursuit now (we must be
// chased - see 'pursuit'), plain or with spike strips; 'list' shows ours and theirs.
static int DebugRoadblockImpl(int force) {
    __try {
        void* mgr = RbCopMgr();
        void* pursuit = PursuitOf(LocalVehicle());
        if (!mgr) return -1;
        if (!pursuit) return -2;
        void* old = VCall<void* (__thiscall*)(void*)>(pursuit, 0x3C / 4)(pursuit);
        if (old) return -3;
        g_rbForce = force;
        bool ok = hk_RbCreate(mgr, nullptr, pursuit, 4, nullptr, nullptr);
        if (!ok) {
            // the game found too few free cop cars: hand it the cops we have, as heavy support does
            // (IVehicle::List as CreateRoadBlock reads it: +4 the IVehicle* array, +0xC the count)
            void* cops[6] = {}; int n = 0;
            void** b = nullptr;
            int cnt = ListSize(VL_AICOPS, &b);
            for (int i = 0; i < cnt && n < 6; ++i) {
                void* c = ListGet(VL_AICOPS, i);
                if (c && IsVehicle(c) && c != g_ghost && !IsMirrorCar(c)) cops[n++] = c;
            }
            uintptr_t list[4] = { 0, (uintptr_t)cops, 0, (uintptr_t)n };
            if (n >= 2) ok = hk_RbCreate(mgr, nullptr, pursuit, 4, nullptr, list);
            LOG("Roadblock: test - the game's own pick refused, with our %d cop(s): %s", n, ok ? "built" : "refused too");
        }
        g_rbForce = 0;
        return ok ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_rbForce = 0; return -4; }
}
std::string Game::DebugRoadblock(const char* arg) {
    if (!g_ok || GameFlowState() != 6) return "not in game";
    if (!g_rbOn) return "roadblock sync is off (hooks failed)";
    std::string a = arg ? arg : "";
    if (a == "list") {
        char buf[512]; int n = 0;
        n += sprintf_s(buf + n, sizeof(buf) - n, "ours:");
        for (auto& o : g_rbOurs) if (o.used) n += sprintf_s(buf + n, sizeof(buf) - n, " #%u%s", o.d.id, o.rb ? "" : "(gone)");
        n += sprintf_s(buf + n, sizeof(buf) - n, "; theirs:");
        for (auto& x : g_rbPeer.e) if (x.used) n += sprintf_s(buf + n, sizeof(buf) - n, " #%u%s", x.d.id, x.placed ? "(shown)" : "(not here yet)");
        n += sprintf_s(buf + n, sizeof(buf) - n, "; built %d, shown %d, held %d", g_rbBuilt, g_rbShown, g_rbSuppressed);
        return buf;
    }
    int force = a == "spikes" ? 2 : 1;
    switch (DebugRoadblockImpl(force)) {
    case 1: return force == 2 ? "roadblock with spike strips built" : "plain roadblock built";
    case 0: return "the game refused (no straight road 250 m ahead, too few cops, or the other player's roadblock is near)";
    case -1: return "no cop manager";
    case -2: return "not chased (start a pursuit first: 'pursuit')";
    case -3: return "our pursuit already has a roadblock";
    default: return "exception";
    }
}
#endif
