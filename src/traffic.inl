// Included from game.cpp (uses its vehicle helpers).
// ---------------------------------------------------------------------------
// Traffic sync
//  Host: owns traffic. ~20x/s sends its traffic cars nearest to the client (when the client is near).
//  Client: near the host (enter 350 m / leave 450 m) it zeroes its own traffic density (hook on the
//  ComputeDensity call at 0x439BBD), removes its own traffic cars and spawns "mirror" cars that copy
//  the host's traffic (no AI: hard-set, interpolated transforms). Far from the host it keeps its own.
//  In a race the client keeps its own traffic (race scripts own some of it). With [Traffic]
//  SharedInRaces=1 its own traffic cars are bound to the host's cars and moved like mirrors instead
//  (nothing spawned, deleted or AI-disabled); with 0 each player sees their own traffic in races.
//  [Traffic] SmoothHandover=1 (client, free roam): leaving the host, each host-car copy becomes one of
//  our own traffic cars (traffic AI, adopted by the game's traffic manager) instead of vanishing.
//  Coming near, our own traffic cars stay until they are off screen, and a host car only appears where
//  the camera can't see it (or one of our cars in / near its spot takes it over and glides there).
//  [Traffic] NoSpawnNearPlayers=1 (both sides): our game never builds a new traffic car or cop within
//  ~60 m in front of the other player (spot checks hooked, see below), and no copy of the other player's
//  cars is created inside our view within ~60 m (it waits until out of view or farther).
// ---------------------------------------------------------------------------
namespace {
    constexpr uintptr_t DensityCallSite  = 0x439BBD;   // call AITrafficManager::ComputeDensity (0x41EC20)
    constexpr uintptr_t ComputeDensityFn = 0x41EC20;   // returns the traffic density (float, ST0)
    bool  g_suppressTraffic = false;   // also suppresses the client's own cop spawns
    bool  g_raceDensityOff = false;    // client in a race near the host: no new traffic cars of our own (cops still spawn)

    // AICopManager spawn calls (thiscall). Redirected so the client spawns no cops near the host.
    constexpr uintptr_t PatrolCallSite   = 0x4315CF, SpawnPatrolFn  = 0x430D90; // bool SpawnPatrolCar()
    constexpr uintptr_t PursuitCallSite1 = 0x42ED7C, PursuitCallSite2 = 0x42ED9B, SpawnPursuitFn = 0x42EA90; // bool SpawnPursuitCar(IPursuit*)
    bool __fastcall hk_SpawnPatrol(void* self, void*) {
        if (g_suppressTraffic) return false;
        return ((bool(__fastcall*)(void*, void*))SpawnPatrolFn)(self, nullptr);
    }
    bool __fastcall hk_SpawnPursuit(void* self, void*, void* pursuit) {
        // pursuit cops are always allowed: every player owns the cops of their own pursuit
        // (the other player sees them as mirrors)
        return ((bool(__fastcall*)(void*, void*, void*))SpawnPursuitFn)(self, nullptr, pursuit);
    }
    bool PatchCallChecked(uintptr_t site, uintptr_t expect, void* to) {
        uint8_t op = 0; int32_t rel = 0;
        if (!Mem::Read(site, op) || !Mem::Read(site + 1, rel) || op != 0xE8 || site + 5 + rel != expect) return false;
        Mem::PatchCall(site, to);
        return true;
    }
    uint32_t& GlareBits(void* iv) { return *(uint32_t*)((uint8_t*)iv + 0x9C); }

    // ---- [Traffic] NoSpawnNearPlayers: no new traffic / cops right in front of the other player ----
    // The game chooses spots for new cars around its own camera only (all verified in the exe):
    //  Traffic: AITrafficManager::SpawnTraffic 0x439A10 (from its update 0x439BB0) asks 0x437DC0 for a
    //   road spot (a WRoadNav at mgr+0xB4; the spot's position is at nav+0x98, game space). 0x437DC0
    //   tries a spot ahead of each spawn anchor and takes the first one 0x4109C0 (thiscall, const
    //   Vector3*) doesn't call blocked (true = blocked: cars there); else no traffic car this update.
    //   The car itself (0x426380: a re-used culled car or a new one) is only made after a spot is found.
    //  Cops: AICopManager's spot finders (0x42FA40 for SpawnPatrolCar 0x430D90 / 0x430E60, 0x41EED0 for
    //   pursuit cops from SpawnPursuitCar 0x42EA90, 0x430E60's own two spots) all accept a spot only
    //   when 0x419FC0 (thiscall, const Vector3*, 4 more args, true = fine; it checks the distance from
    //   our camera) says so; on false they give up for this try and the cop manager tries again later.
    // We answer those calls too: a spot close in front of the other player's car is refused as well.
    // The game then takes another anchor / tries again; nothing is created at the refused spot.
    constexpr uintptr_t TrafficSpotCallSite = 0x438076, TrafficSpotBlockedFn = 0x4109C0;
    constexpr uintptr_t CopSpotCallSites[4] = { 0x41F0AF, 0x42FB56, 0x43117A, 0x4313B3 };
    constexpr uintptr_t CopSpotOkFn = 0x419FC0;
    int g_noSpawnTraffic = 0, g_noSpawnCops = 0;   // spots refused (logged by TrafficTick when they change)
    int g_noSpawnLogs = 0;

    // The other player's car as the spawn check sees it: our copy of their car (what we draw, its body
    // direction and motion), else their latest state. False = no check (not linked, they have no car).
    bool PeerForSpawnCheck(float pos[3], float dirs[6], int& nDirs) {
        nDirs = 0;
        if (!g_cfg.trafficNoSpawnNearPlayers || !g_coopLinked || !Net::Stats().connected) return false;
        const Net::Snapshot& p = Net::Latest();
        if (!p.valid || !p.s.hasCar || p.s.gameFlowState != 6) return false;
        const float* vel = p.s.linVel;
        float gv[3];
        if (void* grb = RigidBodyOf(g_ghost)) {
            memcpy(pos, RB_GetVec(grb, RB_GetPosition), 12);
            float f[3] = {};
            VCall<void(__thiscall*)(void*, float*)>(grb, 13 /*GetForwardVector*/)(grb, f);
            if (NetUtil::FiniteWithin(f, 3, 2.f)) { memcpy(dirs, f, 12); nDirs = 1; }
            memcpy(gv, RB_GetVec(grb, RB_GetLinearVelocity), 12); vel = gv;
        } else memcpy(pos, p.s.pos, 12);
        if (!NetUtil::FiniteWithin(pos, 3, NetUtil::MAX_POS)) return false;
        float sp = NetUtil::FiniteWithin(vel, 3, NetUtil::MAX_VEL) ? sqrtf(vel[0] * vel[0] + vel[2] * vel[2]) : 0.f;
        if (sp > 3.f) { memcpy(dirs + 3 * nDirs, vel, 12); ++nDirs; }   // where it is going (reversing, sliding)
        return true;
    }
    // true = this new-car spot is right in front of the other player (refuse it)
    bool SpotNearPeer(const float* spot, bool cop) {
        __try {
            float pos[3], dirs[6]; int nd = 0;
            float s[3];
            if (!spot || !Mem::SafeRead(spot, s, 12) || !NetUtil::FiniteWithin(s, 3, NetUtil::MAX_POS)) return false;
            if (!PeerForSpawnCheck(pos, dirs, nd) || !NetUtil::SpawnSpotBlocked(s, pos, dirs, nd)) return false;
            ++(cop ? g_noSpawnCops : g_noSpawnTraffic);
            if (g_noSpawnLogs < 6) {
                ++g_noSpawnLogs;
                LOG("Traffic: a new %s at %.0f %.0f %.0f would appear %.0f m in front of the other player - the game picks another spot",
                    cop ? "police car" : "traffic car", s[0], s[1], s[2], NetUtil::Dist3(s, pos));
            }
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool __fastcall hk_TrafficSpotBlocked(void* self, void*, const float* pos) {
        if (((bool(__fastcall*)(void*, void*, const float*))TrafficSpotBlockedFn)(self, nullptr, pos)) return true;
        return SpotNearPeer(pos, false);
    }
    bool g_localChased = false;   // set by pursuit.inl each tick
    bool __fastcall hk_CopSpotOk(void* self, void*, const float* pos, int a, int b, int c, int d) {
        if (!((bool(__fastcall*)(void*, void*, const float*, int, int, int, int))CopSpotOkFn)(self, nullptr, pos, a, b, c, d)) return false;
        // a pursuit on us: its cops may come from anywhere near us (as in the plain game), but not in the
        // other player's face far from us (they saw our pursuit cops pop up right in front of them)
        if (g_localChased) {
            void* rb = RigidBodyOf(LocalVehicle());
            float s[3];
            if (!rb || !pos || !Mem::SafeRead(pos, s, 12) || NetUtil::Dist3(s, RB_GetVec(rb, RB_GetPosition)) < 100.f) return true;
        }
        if (Game::CurrentEventHash() != 0) return true;  // both on the same route; first pursuit cops must be able to spawn
        return !SpotNearPeer(pos, true);
    }

    float __fastcall hk_ComputeDensity(void* self, void* edx) {
        using Fn = float(__fastcall*)(void*, void*);
        float d = ((Fn)ComputeDensityFn)(self, edx);
        return (g_suppressTraffic || g_raceDensityOff) ? 0.0f : d;
    }

    struct MirrorSample { double sendMs; TrafficEntry e; };
    struct Mirror {
        bool     used = false;
        uint16_t id = 0;
        uint32_t model = 0;
        void*    iv = nullptr;
        MirrorSample s[4];
        int      n = 0;        // valid samples (newest at index n-1)
        double   lastSeen = 0; // local ms
        int      fails = 0;
        double   noSpawnUntil = 0; // local ms: the game just deleted our copy to make room - don't build it again at once
        double   swapNotBefore = 0; // local ms: we just swapped a car of another model here - not again at once
        bool     racer = false; // a race opponent of the host: drives one of our own racers (no spawn)
        uint32_t wid = 0;       // racer: world id of our racer it drives (a new race may build another car at that address)
        double   unboundSince = 0;   // racer: local ms since it waits for one of our racers
        bool     bound = false; // in a race: iv is one of OUR OWN traffic cars following the host's car
                                // (never killed, its AI is left alone: unbinding just lets it drive on)
        char     name[31] = {};  // its display name on the host (race opponents)
        // free roam (SmoothHandover): one of our own cars took this host car over and glides from where
        // it was to the host car's place instead of jumping there
        bool     blendPending = false; // start the glide at the next drive (from where the car is then)
        DWORD    localSince = 0, localUntil = 0;   // a hit: our physics moves it until localUntil (TakeLocalHits)
        DWORD    localAgainAt = 0;                 // after a release: not taken over again before this
        bool     localTouched = false;             // the hit happened: the owner follows our physics (SendHeld)
        DWORD    localSentAt = 0;
        float    localV0[3] = {};                  // its speed when we took it over
        DWORD    blendT0 = 0;    // GetTickCount when the glide started (0 = none)
        float    blendOff[3] = {};
        float    blendRot[4] = {};
        // on the ground (groundutil.h): the car's ride height, learned from full-physics samples, and
        // the height correction we apply now (follows its target smoothly)
        float    ride = 0; bool rideKnown = false;
        bool     ownSampled = false;          // our own car's ride height read when it took this host car over
        float    groundFix = 0; DWORD groundAt = 0, learnAt = 0;
        // semi trucks: the owner's states of its trailer (PKT_TRAILER), and our own hitch fixes
        NetUtil::MotionKey ts[4] = {};
        int      tn = 0;              // valid trailer keys (newest at tn-1)
        uint8_t  tflags = 0;          // TRAILER_* of the newest key
        bool     trailerSnap = false; // just spawned: put our trailer at the hitch once the truck is in place
        bool     trailerLogged = false;
        DWORD    trailerFixAt = 0;    // last re-hitch / put-back (at most every 2 s)
    };
    Mirror g_mirrors[128];   // the other player's cars we show (MaxCars at most 64)
    bool   g_trafficNear = false;
    // In a race the client never removes or replaces its own traffic: race scripts own some of those
    // cars (drag races), and removing one crashed the game (0x5FD94A). Cops and race opponents are
    // still mirrored. With [Traffic] SharedInRaces the host's traffic cars are shown by binding each one
    // to one of our own traffic cars (see BindRaceTraffic): no car is spawned, deleted or has its AI
    // switched off; our spare traffic cars keep driving on their own.
    bool   g_clientRacing = false;
    bool   SharedRaceTraffic() { return !g_cfg.isHost && g_clientRacing && g_cfg.trafficSharedInRaces; }
    DWORD  g_raceEndedTick = 0;        // the race scripts may still own cars for a moment: no culling for 8 s
    int    g_mirrorCount = 0, g_killedOwn = 0;

    Mirror* FindMirror(uint16_t id) {
        for (auto& m : g_mirrors) if (m.used && m.id == id) return &m;
        return nullptr;
    }
    int g_budgetWaits = 0;   // mirror spawns that waited because the world was full (20 cars)
    int g_copRoomWaits = 0, g_copViewWaits = 0;   // copies of the other player's cops that waited (room / out of view)
    bool IsMirrorCar(void* iv) {
        for (auto& m : g_mirrors) if (m.used && m.iv == iv) return true;
        return false;
    }

    using NetUtil::CLIENT_ID_BIT;   // ids of cars owned by the client (its pursuit cops), see netutil.h

    double g_lastTrafficPkt = 0;   // local ms of the owner's last traffic packet

    void RemoveMirror(Mirror& m);   // below
    int  HandToTrafficAI(void* iv); // below
    void Retire(void* iv, bool ai); // below
    void OnTrafficPacket(const TrafficEntry* e, int count, double sendMs) {
        double now = Net::LocalClockMs();
        g_lastTrafficPkt = now;
        TrafficEntry tmp[MAX_TRAFFIC_PER_PACKET];
        if (count > MAX_TRAFFIC_PER_PACKET) count = MAX_TRAFFIC_PER_PACKET;
        if (count <= 0) return;
        memcpy(tmp, e, (size_t)count * sizeof(TrafficEntry));
        count = NetUtil::DedupTrafficEntries(tmp, count);
        e = tmp;
        for (int i = 0; i < count; ++i) {
            // the host accepts only the client's own cops; the client accepts the host's cars
            if (!NetUtil::TrafficIdAccepted(e[i].id, g_cfg.isHost)) continue;
            bool racer = (e[i].flags & 2) != 0;
            if (!g_cfg.isHost && !racer && !g_trafficNear) continue;   // far from the host: own traffic
            if (!g_cfg.isHost && !racer && g_clientRacing && !g_cfg.trafficSharedInRaces && !(e[i].flags & 1)) continue;   // in a race: own traffic too
            Mirror* m = FindMirror(e[i].id);
            if (m && m->n) {
                uint16_t oldF = m->s[m->n - 1].e.flags;
                if (m->racer) oldF = (uint16_t)(oldF | 2);
                uint16_t newF = (uint16_t)(e[i].flags | (racer ? 2 : 0));
                if (NetUtil::TrafficRoleChanged(oldF, m->bound, newF)) {
                    // do not keep driving a traffic car as a racer (stacked spawn / unsynced opponents)
                    if (m->bound) { m->iv = nullptr; m->bound = false; m->wid = 0; }
                    else if (m->iv) { RemoveMirror(*m); m = nullptr; }
                }
            }
            // the owner reuses ids: a new car (another model - e.g. a cop where a traffic car was) under an id we
            // already show must be built fresh, not shown by the old car (it carried the cop's light bar and
            // counted as their cop - a pursuit started on a traffic car - until it was rebuilt later)
            if (m && m->n && m->bound && m->model != e[i].model) m->model = e[i].model;   // shown by our own car (race traffic): it just shows the new one
            else if (m && m->n && !m->racer && !racer && m->model != e[i].model) {
                LOG("Traffic: the other player's car %u is another car now (model %08X -> %08X) - building it again", (unsigned)e[i].id, m->model, e[i].model);
                // the old copy (not a cop) drives on as our traffic and goes once out of sight - it vanished
                // in front of the player otherwise; the new car appears only where it can't be seen pop in
                bool wasCop = (m->s[m->n - 1].e.flags & 1) != 0;
                if (m->iv && !wasCop && IsVehicle(m->iv)) {
                    int h = HandToTrafficAI(m->iv);
                    if (h != 2 /*HAND_AI_MANAGED*/) Retire(m->iv, h == 1 /*HAND_AI*/);
                    m->iv = nullptr; m->localSince = m->localUntil = 0; m->localTouched = false;
                }
                RemoveMirror(*m);
                m = nullptr;
            }
            if (!m) {
                for (auto& slot : g_mirrors) if (!slot.used) { slot = Mirror{}; slot.used = true; slot.id = e[i].id; m = &slot; break; }
                if (!m) continue;
            }
            m->lastSeen = now;   // the owner still has it (its clock stands still while its game is paused)
            if (m->n && sendMs <= m->s[m->n - 1].sendMs) continue; // out of order / duplicate
            if (m->n == 4) { memmove(&m->s[0], &m->s[1], 3 * sizeof(MirrorSample)); m->n = 3; }
            m->s[m->n].sendMs = sendMs;
            m->s[m->n].e = e[i];
            m->n++;
            m->model = e[i].model;
            m->racer = racer;
        }
    }

    // The owner's trailer states (sent right after its traffic packet, same clock): kept with the
    // truck's mirror. A truck we have no mirror for yet just loses this one (the next comes in 50 ms).
    double g_lastTrailerPkt = 0;   // local ms of the host's last trailer packet (newer builds send one with every traffic packet)
    int    g_trailerKeys = 0;      // statistics
    void OnTrailerPacket(const TrailerEntry* e, int count, double sendMs) {
        g_lastTrailerPkt = Net::LocalClockMs();
        for (int i = 0; i < count; ++i) {
            if (!NetUtil::TrafficIdAccepted(e[i].id, g_cfg.isHost)) continue;
            Mirror* m = FindMirror(e[i].id);
            if (!m || m->racer) continue;
            NetUtil::MotionKey k;
            k.t = sendMs;
            memcpy(k.pos, e[i].pos, 12); memcpy(k.rot, e[i].rot, 16);
            memcpy(k.linVel, e[i].linVel, 12); memcpy(k.angVel, e[i].angVel, 12);
            if (NetUtil::PushMotionKey(m->ts, m->tn, 4, k)) { m->tflags = e[i].flags; ++g_trailerKeys; }
        }
    }
    // The host sends trailer packets (a newer build): then a host car without trailer data has none.
    bool HostSendsTrailers() {
        double age = Net::LocalClockMs() - g_lastTrailerPkt;
        return !g_cfg.isHost && g_lastTrailerPkt > 0 && age >= 0 && age < 3000;
    }

    // GRacerInfo of a car in the current race (GRaceStatus::GetRacerInfo(ISimable*)), or null
    void* RacerInfoOf(void* iv) {
        void* rs = nullptr;
        if (!IsVehicle(iv) || !Mem::Read(0x91E000, rs) || !rs) return nullptr;
        void* sim = VCall<void* (__thiscall*)(void*)>(iv, IV_GetSimable)(iv);
        return sim ? ((void*(__thiscall*)(void*, void*))0x5E8B20)(rs, sim) : nullptr;
    }
    void OnRacerName(uint16_t id, const char* name) {
        if (Mirror* m = FindMirror(id)) NetUtil::CopyHudName(m->name, name);        // HUD slots are 16 bytes
    }

    // Copies to delete once the cutscene is over: its camera may have picked one (0x44E9FD)
    struct Doomed { void* iv; uint32_t wid; };
    Doomed g_nisDoomed[64]; int g_nisDoomedN = 0;
    void FlushNisDoomed(bool inWorld) {
        if (!g_nisDoomedN || (inWorld && NisActive())) return;
        int gone = 0;
        for (int i = 0; i < g_nisDoomedN; ++i) {
            void* iv = g_nisDoomed[i].iv;   // (the game may have deleted it meanwhile, or reused the memory)
            if (inWorld && ListContains(-1, iv) && WorldIdOf(iv) == g_nisDoomed[i].wid) { KillVehicle(iv); ++gone; }
        }
        if (gone) LOG("Traffic: cutscene over - %d copy car(s) removed that had to wait", gone);
        g_nisDoomedN = 0;
    }
    void RemoveMirror(Mirror& m) {
        if (m.racer) { if (m.iv && (!m.wid || WorldIdOf(m.iv) == m.wid)) RestoreAI(m.iv); }   // our own racer: give it its AI back
        else if (m.bound) {}                                  // our own traffic car: it just drives on
        else if (m.iv && ListContains(-1, m.iv)) {           // (a car the game deleted was already dropped by g_onMirrorRemoved)
            if (NisActive() && g_nisDoomedN < 64) g_nisDoomed[g_nisDoomedN++] = { m.iv, WorldIdOf(m.iv) };   // after the cutscene
            else KillVehicle(m.iv);
        }
                                                              // a truck's destructor (0x6B7E00) removes its trailer too: never Kill the trailer ourselves
        m = Mirror{};
    }

    // The world height under a point of our game: WCollisionMgr::GetWorldHeightAtPointRigorous 0x789870
    // (thiscall on a {0, 3} collision-manager struct, (const Vector3* pt, float* y, Vector3* normal), ret
    // 0xC; a fast lookup, then a ray from above). False where we have no world collision.
    bool GroundHeightAt(const float* p, float& y) {
        __try {
            uint32_t mgr[2] = { 0, 3 };
            float pt[3] = { p[0], p[1], p[2] }, h = 0;
            if (!((bool(__thiscall*)(void*, const float*, float*, float*))0x789870)(mgr, pt, &h, nullptr)) return false;
            if (!std::isfinite(h)) return false;
            y = h; return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    // The owner's car runs in the emulated physics mode (height from the road path: it may float).
    // IVehicle (vtable 0x8AA828) slot 14 = physics mode (PVehicle+0x158: 1 full, 2 emulated); the racer
    // AI switches mode 2 on when there's no world collision under the car (0x680840 -> 0x415E50) and
    // back off once there is (0x415E80), checked every AI update.
    bool EmulatedPhysics(void* iv) {
        __try {
            if (!iv || VTable(iv) != 0x8AA828) return false;
            return VCall<int(__thiscall*)(void*)>(iv, 14)(iv) == 2;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    int   g_groundFixed = 0, g_groundLearned = 0;   // statistics for the log
    float g_groundMaxFix = 0;
    int   g_groundFixedOther = 0; float g_groundMaxFixOther = 0;   // of those: shown by a car of another model
    int   g_modelSwaps = 0, g_modelOtherFrames = 0;      // free roam: another model swapped for the right one; frames shown by another model
    // ride heights by model (any copy's full-physics samples, or our own car of that model on our ground)
    struct ModelRide { uint32_t model; float ride; bool known; };
    ModelRide g_modelRide[64]; int g_modelRideN = 0;
    ModelRide* ModelRideOf(uint32_t model, bool add) {
        for (int i = 0; i < g_modelRideN; ++i) if (g_modelRide[i].model == model) return &g_modelRide[i];
        if (!add || !model) return nullptr;
        ModelRide* r = &g_modelRide[g_modelRideN < 64 ? g_modelRideN++ : (int)(model % 64)];
        r->model = model; r->ride = 0; r->known = false;
        return r;
    }
    float HalfHeightOf(void* rb) {          // IRigidBody slot 19 GetDimension (half extents, +0x98)
        __try {
            float d[3] = {};
            VCall<void(__thiscall*)(void*, float*)>(rb, 19)(rb, d);
            return std::isfinite(d[1]) ? d[1] : 0.f;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return 0.f; }
    }
    // Our copy of a car that may float on the owner: put it on our ground (see groundutil.h). Marked by
    // the owner (emulated physics, flag 8), or far from the owner's player, where its game has no world
    // collision (the host's traffic around the client: it never had full physics there, so it has no
    // ride height of its own - the model's, or the car's shape, then).
    uint32_t ModelKeyOf(void* iv);
    void GroundMirror(Mirror& m, void* rb, float* pos, const float* vel, const float* ownPosBefore) {
        const TrafficEntry& e = m.s[m.n - 1].e;
        const Net::Snapshot& owner = Net::Latest();
        float dOwner = 0.f;
        if (owner.valid && owner.s.hasCar) dOwner = Dist(pos, owner.s.pos);
        bool floating = Ground::LikelyFloating((e.flags & 8) != 0, dOwner, vel[1]);
        DWORD now = GetTickCount();
        float dt = m.groundAt ? (now - m.groundAt) / 1000.f : 0.f;
        m.groundAt = now;
        // the ride height of the car we SHOW (one of our own cars of another model may show it): a truck's
        // height on a sedan floated it
        uint32_t shownModel = m.iv ? ModelKeyOf(m.iv) : e.model;
        bool otherModel = shownModel && shownModel != e.model;
        ModelRide* mr = ModelRideOf(shownModel ? shownModel : e.model, true);
        if (ownPosBefore && !m.ownSampled) {      // our own car, on our ground, just took this host car over
            m.ownSampled = true;
            float gy, zero = 0;
            // only when our car is that model (a takeover may pair another model): else it's not that model's height
            if (mr && ModelKeyOf(m.iv) == e.model && GroundHeightAt(ownPosBefore, gy) && Ground::Learn(ownPosBefore[1], gy, zero, mr->ride, mr->known)) ++g_groundLearned;
        }
        float target = 0;
        if (!floating) {
            if (now - m.learnAt >= 500) {                                     // learn the ride height now and then
                m.learnAt = now;
                float gy;
                if (GroundHeightAt(pos, gy) && Ground::Learn(pos[1], gy, vel[1], m.ride, m.rideKnown)) {
                    ++g_groundLearned;
                    if (mr) { float r = mr->ride; bool k = mr->known; float z = 0; if (Ground::Learn(gy + m.ride, gy, z, r, k)) { mr->ride = r; mr->known = k; } }
                }
            }
        } else {
            float gy;
            bool have = GroundHeightAt(pos, gy);
            float half = (m.rideKnown || (mr && mr->known)) ? 0.f : HalfHeightOf(rb);
            switch (Ground::PickRide(m.rideKnown, mr && mr->known, half)) {
            case Ground::RideFrom::Car:   target = Ground::Target(true, have, pos[1], gy, true, m.ride); break;
            case Ground::RideFrom::Model: target = Ground::Target(true, have, pos[1], gy, true, mr->ride); break;
            case Ground::RideFrom::Shape: target = Ground::ShapeTarget(Ground::Target(true, have, pos[1], gy, true, half)); break;
            default: break;
            }
        }
        if (target == 0 && m.groundFix == 0) return;
        m.groundFix = Ground::Step(m.groundFix, target, dt);
        if (m.groundFix == 0) return;
        ++g_groundFixed;
        if (fabsf(m.groundFix) > g_groundMaxFix) g_groundMaxFix = fabsf(m.groundFix);
        if (otherModel) { ++g_groundFixedOther; if (fabsf(m.groundFix) > g_groundMaxFixOther) g_groundMaxFixOther = fabsf(m.groundFix); }
        pos[1] += m.groundFix;
        RB_SetVec(rb, RB_SetPosition, pos);
    }

    // A copy our physics moves after a hit: the owner's game puts its real car where ours is (PKT_CARHOLD),
    // so the hit is the same on both screens and the car drives on from there when we let it go.
    void SendHeld(Mirror& m, void* rb, bool last) {
        DWORD now = GetTickCount();
        // held up (a cutscene, a stall): their game gave up waiting (0.5 s) and drives it - no late word
        if (m.localSentAt && now - m.localSentAt > 400) { m.localTouched = false; return; }
        if (!last && now - m.localSentAt < 30) return;
        m.localSentAt = now;
        Net::SendCarHold(m.id, last, RB_GetVec(rb, RB_GetPosition), RB_GetVec(rb, RB_GetOrientation),
                         RB_GetVec(rb, RB_GetLinearVelocity), RB_GetVec(rb, RB_GetAngularVelocity));
    }

    void DriveMirror(Mirror& m, double renderSendMs) {
        if (!m.iv || m.n == 0) return;
        void* rb = RigidBodyOf(m.iv);
        if (!rb) return;
        bool logBack = false;
        if (m.localSince) {   // we hit it: our physics moves it for a moment, then the owner drives on from there
            if ((int)(GetTickCount() - m.localUntil) < 0) { if (m.localTouched) SendHeld(m, rb, false); return; }
            if (m.localTouched) { SendHeld(m, rb, true); logBack = m.localTouched; }
            m.localSince = m.localUntil = 0; m.localTouched = false;
            m.localAgainAt = GetTickCount() + HitUtil::LOCAL_HIT_AGAIN_MS;
            m.blendPending = true; m.groundFix = 0;
        }
        const MirrorSample* a = nullptr; const MirrorSample* b = nullptr;
        for (int i = m.n - 1; i >= 0; --i) {
            if (m.s[i].sendMs <= renderSendMs) { a = &m.s[i]; b = (i + 1 < m.n) ? &m.s[i + 1] : nullptr; break; }
        }
        float pos[3], vel[3], av[3];
        Quat q;
        if (a && b) {
            double span = b->sendMs - a->sendMs;
            float u = span > 0 ? (float)((renderSendMs - a->sendMs) / span) : 1.f;
            float dt = (float)(span / 1000.0);
            float u2 = u * u, u3 = u2 * u;
            float h00 = 2 * u3 - 3 * u2 + 1, h10 = u3 - 2 * u2 + u, h01 = -2 * u3 + 3 * u2, h11 = u3 - u2;
            for (int i = 0; i < 3; ++i) {
                pos[i] = h00 * a->e.pos[i] + h10 * dt * a->e.linVel[i] + h01 * b->e.pos[i] + h11 * dt * b->e.linVel[i];
                vel[i] = a->e.linVel[i] + (b->e.linVel[i] - a->e.linVel[i]) * u;
                av[i]  = a->e.angVel[i] + (b->e.angVel[i] - a->e.angVel[i]) * u;
            }
            q = Nlerp(*(const Quat*)a->e.rot, *(const Quat*)b->e.rot, u);
        } else {
            const MirrorSample& n = m.s[m.n - 1];
            float ex = (float)((renderSendMs - n.sendMs) / 1000.0);
            if (ex < 0) ex = 0;
            bool stale = ex > 0.25f;   // no fresh data (the owner's game paused): hold still
            if (ex > 0.25f) ex = 0.25f;
            for (int i = 0; i < 3; ++i) { pos[i] = n.e.pos[i] + n.e.linVel[i] * ex; vel[i] = stale ? 0.f : n.e.linVel[i]; av[i] = stale ? 0.f : n.e.angVel[i]; }
            q = *(const Quat*)n.e.rot;
        }
        float ownBefore[3]; bool haveOwnBefore = false;
        if (m.blendPending) {   // start the glide from where the car is right now
            m.blendPending = false;
            const float* cp = RB_GetVec(rb, RB_GetPosition);
            memcpy(ownBefore, cp, sizeof(ownBefore)); haveOwnBefore = true;
            for (int i = 0; i < 3; ++i) m.blendOff[i] = cp[i] - pos[i];
            memcpy(m.blendRot, RB_GetVec(rb, RB_GetOrientation), 16);
            if (logBack) LOG("Collision: the other player's car %u back to their game - %.1f m to glide", (unsigned)m.id,
                             sqrtf(m.blendOff[0] * m.blendOff[0] + m.blendOff[1] * m.blendOff[1] + m.blendOff[2] * m.blendOff[2]));
            m.blendT0 = GetTickCount() | 1;
        }
        if (m.blendT0) {   // a car of ours that took this host car over: glide into its place
            constexpr DWORD BlendMs = 1500;
            DWORD el = GetTickCount() - m.blendT0;
            if (el >= BlendMs) m.blendT0 = 0;
            else {
                float k = 1.f - (float)el / BlendMs;          // 1 -> 0: how much of the old offset is left
                k = k * k * (3.f - 2.f * k);
                for (int i = 0; i < 3; ++i) pos[i] += m.blendOff[i] * k;
                q = Nlerp(q, *(const Quat*)m.blendRot, k);
            }
        }
        RB_SetVec(rb, RB_SetOrientationQuat, &q);
        RB_SetVec(rb, RB_SetPosition, pos);
        RB_SetVec(rb, RB_SetLinearVelocity, vel);
        RB_SetVec(rb, RB_SetAngularVelocity, av);
        GroundMirror(m, rb, pos, vel, haveOwnBefore ? ownBefore : nullptr);
    }

    // ---- semi truck trailers (all verified in speed.exe v1.3) -------------------------------------
    // A truck's trailer is built by the truck itself: its rigid-body behaviour "RBTractor" (factory
    // registered at 0x888020, Construct 0x6B95C0, ctor 0x6B8060) reads the trailer model from the truck's
    // pvehicle attributes (0x454810, key 0x9A5537FE), builds a "PVehicle" with DRIVER_NONE (0x41CB10),
    // keeps its IVehicle at +0x1A4 and hitches it: SetHitch 0x69B100 makes a joint between the two
    // "5THWHEEL" collision points (0x6718F0 -> 0x48CDD0; the joint list 0x91132C is solved by position
    // each physics step, 0x48C910 from 0x6B947A) and puts the trailer at the hitch. The trailer is class
    // TRAILER: only in the lists ALL and TRAILERS (9) (0x67ECA0), never AITRAFFIC - so it was never a
    // traffic entry, and our copy of a truck (SpawnVehicle with the truck's model) builds its own
    // trailer. The truck's destructor (0x6B7E00) unhitches and Kill()s its trailer, and its vehicle
    // cache (+0x19C, OnQuery 0x68BFE0) keeps the game from taking the trailer away for room.
    // IArticulatedVehicle (handle 0x4040D0) at RBTractor+0x194, vtable 0x8AC9C0: slot 1 GetTrailer
    // (0x6B7F30, +0x10), 2 SetHitch(bool) (0x69B100), 3 put the trailer at the hitch on the ground
    // (0x68C080 -> trailer IVehicle::SetVehicleOnGround), 4 IsHitched (0x6B7F40, byte +0x1C).
    // The traffic manager's cull check (0x410B00) asks the same interface for a car's trailer.
    constexpr uint32_t H_IArticulated = 0x4040D0;
    constexpr uintptr_t VT_RBTractor_IArticulated = 0x8AC9C0;
    void* ArticulatedOf(void* iv) {
        static int slotsOk = -1;   // the four slots we call hold the functions named above
        if (slotsOk < 0) {
            static const uintptr_t want[5] = { 0, 0x6B7F30, 0x69B100, 0x68C080, 0x6B7F40 };
            slotsOk = 1;
            for (int s = 1; s <= 4; ++s) { uintptr_t f = 0; if (!Mem::Read(VT_RBTractor_IArticulated + s * 4, f) || f != want[s]) slotsOk = 0; }
            if (!slotsOk) LOG("Traffic: truck trailer interface doesn't match - trailers are left to the game");
        }
        if (!slotsOk || !IsVehicle(iv)) return nullptr;
        void* a = Find(iv, H_IArticulated);
        return (a && VTable(a) == VT_RBTractor_IArticulated) ? a : nullptr;
    }
    void* TrailerOfArt(void* art) {
        void* t = VCall<void* (__thiscall*)(void*)>(art, 1)(art);
        return (IsVehicle(t) && ListContains(-1, t)) ? t : nullptr;
    }
    void* TrailerOf(void* iv) { void* a = ArticulatedOf(iv); return a ? TrailerOfArt(a) : nullptr; }
    bool  TrailerHitched(void* art) { return VCall<bool(__thiscall*)(void*)>(art, 4)(art); }
    void  SetTrailerHitch(void* art) { VCall<void(__thiscall*)(void*, bool)>(art, 2)(art, true); }   // also puts it at the hitch
    void  PutTrailerAtHitch(void* art) { VCall<void(__thiscall*)(void*)>(art, 3)(art); }

    int g_trlDriven = 0, g_trlOwnHitch = 0, g_trlRehitched = 0, g_trlPutBack = 0;   // statistics (frames / events)
    int g_trailerLogs = 0;

    // After DriveMirror: the trailer of our copy of a truck. With the owner's trailer states it is moved
    // exactly like the truck (same render time, same ground correction), so the hitch joint has nothing
    // to correct; without them (an older host) the truck's own hitch pulls it, and a trailer that came
    // off or was left behind is hitched / put back at the hitch.
    void DriveTrailer(Mirror& m, double renderSendMs) {
        if (m.localSince) return;   // our physics moves the truck (a hit): the hitch pulls its trailer along
        void* art = ArticulatedOf(m.iv);
        if (!art) return;
        void* tr = TrailerOfArt(art);
        void* trb = RigidBodyOf(tr);
        void* rb = RigidBodyOf(m.iv);
        if (!trb || !rb) return;
        DWORD tick = GetTickCount();
        bool hitched = TrailerHitched(art);
        bool fresh = m.n && NetUtil::TrailerDataFresh(m.tn, m.tn ? m.ts[m.tn - 1].t : 0.0, m.s[m.n - 1].sendMs);
        // (while one of our own cars glides into a host car's place the hitch pulls its trailer along)
        if (fresh && !m.blendT0) {
            float pos[3], rot[4], vel[3], av[3];
            if (!NetUtil::SampleMotion(m.ts, m.tn, renderSendMs, pos, rot, vel, av)) return;
            pos[1] += m.groundFix;              // the same height correction as its truck
            RB_SetVec(trb, RB_SetOrientationQuat, rot);
            RB_SetVec(trb, RB_SetPosition, pos);
            RB_SetVec(trb, RB_SetLinearVelocity, vel);
            RB_SetVec(trb, RB_SetAngularVelocity, av);
            ++g_trlDriven;
            m.trailerSnap = false;
            if ((m.tflags & TRAILER_HITCHED) && !hitched && tick - m.trailerFixAt >= 2000) {
                m.trailerFixAt = tick;
                SetTrailerHitch(art);
                ++g_trlRehitched;
                if (g_trailerLogs < 12) { ++g_trailerLogs; LOG("Traffic: re-hitched the trailer %p of the host's truck %u (%s): %s", tr, (unsigned)m.id, VehName(m.iv), TrailerHitched(art) ? "ok" : "failed"); }
            }
            if (!m.trailerLogged && g_trailerLogs < 12) {
                m.trailerLogged = true; ++g_trailerLogs;
                LOG("Traffic: the host's truck %u (%s): its trailer %p (%s) follows the host's trailer (hitched there %d, here %d)",
                    (unsigned)m.id, VehName(m.iv), tr, VehName(tr), (int)(m.tflags & TRAILER_HITCHED), (int)hitched);
            }
            return;
        }
        ++g_trlOwnHitch;
        bool lost = NetUtil::TrailerLost(RB_GetVec(rb, RB_GetPosition), RB_GetVec(trb, RB_GetPosition));
        if (!m.trailerSnap && hitched && !lost) return;
        if (!m.trailerSnap && tick - m.trailerFixAt < 2000) return;
        const char* why = m.trailerSnap ? "the truck was just placed" : !hitched ? "it came off" : "it was left behind";
        m.trailerSnap = false;
        m.trailerFixAt = tick;
        if (!hitched) { SetTrailerHitch(art); ++g_trlRehitched; }
        else { PutTrailerAtHitch(art); ++g_trlPutBack; }
        if (g_trailerLogs < 12) {
            ++g_trailerLogs;
            LOG("Traffic: the trailer %p of the host's truck %u (%s) put back at the hitch (%s; no trailer data from the host%s), hitched now %d",
                tr, (unsigned)m.id, VehName(m.iv), why, HostSendsTrailers() ? "" : " - an older build?", (int)TrailerHitched(art));
        }
    }

    // host: stable ids for its traffic cars
    struct HostId { void* iv; uint16_t id; };
    HostId   g_hostIds[64];
    uint16_t g_nextHostId = 1;
    uint16_t HostIdFor(void* iv) {
        for (auto& h : g_hostIds) if (h.iv == iv) return NetUtil::TrafficWireId(h.id, g_cfg.isHost);
        for (auto& h : g_hostIds) {
            if (!h.iv || !ListContains(-1, h.iv)) {
                h.iv = iv; h.id = g_nextHostId;
                g_nextHostId = NetUtil::NextTrafficLocalId(g_nextHostId);
                return NetUtil::TrafficWireId(h.id, g_cfg.isHost);
            }
        }
        return 0;
    }

    uint32_t ModelKeyOf(void* iv) {
        const char* nm = VehName(iv);
        using HashFn = uint32_t(__cdecl*)(const char*);
        if (nm && nm[0] && nm[0] != '?') return ((HashFn)0x4519D0)(nm);
        return VehicleKey(iv);
    }

    // ---- smooth handover (client, free roam, [Traffic] SmoothHandover) -------------------------
    bool SmoothHandover() { return !g_cfg.isHost && g_cfg.trafficSmoothHandover; }

    // The player's camera in game coordinates. eViews[EVIEW_PLAYER1] = 0x9195E0 + 0x70: Active byte +8
    // (0x919658), Camera* +0x40 (0x919690). Camera::CurrentKey Position +0x40 / Direction +0x50 are in
    // render space; the game's own GetCameraPosition (0x40B0D0) / GetCameraDirection (0x40B140) read the
    // same fields and swizzle them to game space as (-r.y, r.z, r.x). Unknown camera: valid = false and
    // pos = the fallback (the player).
    NetUtil::CamView ReadCamera(const float* fallbackPos) {
        NetUtil::CamView c{};
        uint8_t active = 0; uintptr_t cam = 0;
        float p[3], d[3];
        if (Mem::Read(0x919658, active) && active && Mem::Read(0x919690, cam) && cam &&
            Mem::SafeRead((void*)(cam + 0x40), p, 12) && Mem::SafeRead((void*)(cam + 0x50), d, 12) &&
            NetUtil::FiniteWithin(p, 3, NetUtil::MAX_POS) && NetUtil::FiniteWithin(d, 3, 100.f)) {
            c.pos[0] = -p[1]; c.pos[1] = p[2]; c.pos[2] = p[0];
            c.dir[0] = -d[1]; c.dir[1] = d[2]; c.dir[2] = d[0];
            c.valid = true;
        } else if (fallbackPos) memcpy(c.pos, fallbackPos, 12);
        return c;
    }

    // The game's per-car screen timers, updated every frame by PVehicle (0x686060: +0x180 / +0x184,
    // from its render connection's "visible" flag; both 0 when it isn't drawn at all).
    // (Not a vehicle: counts as on screen / inactive / loading, so nothing is done to it.)
    float OnScreenS(void* iv)  { return IsVehicle(iv) ? VCall<float(__thiscall*)(void*)>(iv, 25)(iv) : 1.f; }   // IVehicle::GetOnScreenTime
    float OffScreenS(void* iv) { return IsVehicle(iv) ? VCall<float(__thiscall*)(void*)>(iv, 24)(iv) : 0.f; }   // IVehicle::GetOffScreenTime
    bool  IsActiveCar(void* iv) { return IsVehicle(iv) && VCall<bool(__thiscall*)(void*)>(iv, 34)(iv); }        // IVehicle::IsActive
    bool  IsLoadingCar(void* iv) { return !IsVehicle(iv) || VCall<bool(__thiscall*)(void*)>(iv, IV_IsLoading)(iv); }
    bool  OnScreen(void* iv) { float s = OnScreenS(iv); return !(s <= 0.f); }

    // AITrafficManager: its constructor (0x4418D0) stores the object + 0x48 in 0x92C674. Object vtable
    // 0x89221C; IAttachable at +0x2C (vtable 0x892200, slot 2 = Attach(IUnknown*) -> 0x402900 ->
    // 0x6FBF80). Its OnAttached (0x4399C0) adds an attached vehicle to its car list (+0x90); its update
    // (0x439BB0) un-spawns a listed car once 0x410B00 says it isn't needed (off screen longer than, and
    // farther than, its density tables allow) and re-uses such cars for new traffic (0x426380) - the
    // same Attach call it makes for every traffic car it builds (0x4264BB).
    void* TrafficMgrAttachable() {
        uintptr_t p = 0, vt = 0, vt2 = 0;
        if (!Mem::Read(0x92C674, p) || p < 0x48) return nullptr;
        uintptr_t mgr = p - 0x48;
        if (!Mem::Read(mgr, vt) || vt != 0x89221C || !Mem::Read(mgr + 0x2C, vt2) || vt2 != 0x892200) return nullptr;
        return (void*)(mgr + 0x2C);
    }

    // Turn one of our spawned host-car copies into a normal traffic car of ours that keeps its motion.
    // IVehicle::SetDriverClass (slot 21, 0x6876E0) stores the class, re-lists the car (0x67ECA0: traffic
    // = the AI + AI-traffic lists) and reloads its behaviours (0x6863B0 -> 0x685240), which picks the AI
    // by driver class (table 0x8F8890, read at 0x67F383: traffic -> "AIVehicleTraffic", none ->
    // "AIVehicleEmpty"). Then the traffic manager adopts it and its AI looks for its lane.
    enum { HAND_FAILED, HAND_AI, HAND_AI_MANAGED };
    int HandToTrafficAI(void* iv) {
        if (!IsVehicle(iv) || IsLoadingCar(iv)) return HAND_FAILED;
        void* rb = RigidBodyOf(iv);
        if (!rb) return HAND_FAILED;
        float pos[3], q[4], v[3], w[3];
        memcpy(pos, RB_GetVec(rb, RB_GetPosition), 12);
        memcpy(q, RB_GetVec(rb, RB_GetOrientation), 16);
        memcpy(v, RB_GetVec(rb, RB_GetLinearVelocity), 12);
        memcpy(w, RB_GetVec(rb, RB_GetAngularVelocity), 12);
        if (DriverClass(iv) != DRV_TRAFFIC)
            VCall<void(__thiscall*)(void*, int)>(iv, IV_SetDriverClass)(iv, DRV_TRAFFIC);
        if (DriverClass(iv) != DRV_TRAFFIC) return HAND_FAILED;
        if ((rb = RigidBodyOf(iv)) != nullptr) {      // the behaviour reload must not stop or move it
            RB_SetVec(rb, RB_SetOrientationQuat, q);
            RB_SetVec(rb, RB_SetPosition, pos);
            RB_SetVec(rb, RB_SetLinearVelocity, v);
            RB_SetVec(rb, RB_SetAngularVelocity, w);
        }
        bool managed = false;
        if (void* att = TrafficMgrAttachable()) {
            void* sim = VCall<void* (__thiscall*)(void*)>(iv, IV_GetSimable)(iv);
            if (sim) managed = VCall<bool(__thiscall*)(void*, void*)>(att, 2)(att, sim);
        }
        if (void* ai = VCall<void* (__thiscall*)(void*)>(iv, 0xAC / 4)(iv))     // GetAIVehiclePtr
            VCall<void(__thiscall*)(void*, int)>(ai, 0x50 / 4)(ai, 1);         // ResetDriveToNav(current lane)
        if (managed) ForgetCar(iv);   // the game's own now: it culls and re-uses it like any traffic car
        return managed ? HAND_AI_MANAGED : HAND_AI;
    }

    // Cars we let go of that we still remove ourselves once nobody sees them: copies that couldn't get
    // the traffic AI (they coast on: removed as soon as they are off screen) and traffic cars the
    // traffic manager didn't adopt (removed the way the game culls traffic: off screen for a while and
    // not close, or very far away). They stay in our vehicle cache so we hear if the game deletes one.
    struct RetiredCar { void* iv; bool ai; };
    RetiredCar g_retired[32] = {};
    bool IsRetired(void* iv) { for (auto& r : g_retired) if (r.iv && r.iv == iv) return true; return false; }
    void Retire(void* iv, bool ai) {
        if (!iv || IsRetired(iv)) return;
        for (auto& r : g_retired) if (!r.iv) { r.iv = iv; r.ai = ai; return; }
        if (!ai) KillVehicle(iv);    // no room to track it (never seen in practice): the old behaviour
    }
    int TickRetired(const NetUtil::CamView& cam) {
        int removed = 0;
        for (auto& r : g_retired) {
            if (!r.iv) continue;
            if (WasRemovedByGame(r.iv) || !IsKeptCar(r.iv) || !ListContains(-1, r.iv)) { r.iv = nullptr; continue; }
            void* rb = RigidBodyOf(r.iv);
            if (!rb) { r.iv = nullptr; continue; }
            float d = Dist(RB_GetVec(rb, RB_GetPosition), cam.pos);
            float on = OnScreenS(r.iv), off = OffScreenS(r.iv);
            bool go = r.ai ? (d >= NetUtil::HANDOVER_GONE_M || (on <= 0.f && off >= 3.f && d >= 120.f))
                           : NetUtil::OwnCarMayGo(on, off, d);
            if (go) { KillVehicle(r.iv); r.iv = nullptr; ++removed; }
        }
        return removed;
    }
    void ForgetRetired() { for (auto& r : g_retired) r.iv = nullptr; }   // the world is gone

    int g_handoverLogs = 0;   // per-car handover lines logged since the last switch (only the first few)
    int g_hoWaiting = 0;      // host cars waiting for a spot out of view (last MaintainMirrors)
    int g_hoKeptOnExpiry = 0; // host cars the host stopped sending that stayed here because they were on screen
    int g_hoRetiredGone = 0;  // let-go cars we removed once they were out of view
    int g_viewWaiting = 0;    // NoSpawnNearPlayers: copies not created yet because their spot is in our view
                              // within 60 m (last MaintainMirrors)

    int FillPlayerViews(NetUtil::PlayerView* views, int maxN) {
        int n = 0;
        if (!views || maxN <= 0) return 0;
        NetUtil::CamView cam = ReadCamera(nullptr);
        if (cam.valid) {
            memcpy(views[n].pos, cam.pos, 12);
            memcpy(views[n].dir, cam.dir, 12);
            views[n].valid = true; ++n;
        } else if (void* lrb = RigidBodyOf(LocalVehicle())) {
            memcpy(views[n].pos, RB_GetVec(lrb, RB_GetPosition), 12);
            float f[3] = {};
            VCall<void(__thiscall*)(void*, float*)>(lrb, 13 /*GetForwardVector*/)(lrb, f);
            memcpy(views[n].dir, f, 12);
            views[n].valid = true; ++n;
        }
        if (n >= maxN) return n;
        if (void* grb = RigidBodyOf(g_ghost)) {
            memcpy(views[n].pos, RB_GetVec(grb, RB_GetPosition), 12);
            float f[3] = {};
            VCall<void(__thiscall*)(void*, float*)>(grb, 13 /*GetForwardVector*/)(grb, f);
            memcpy(views[n].dir, f, 12);
            views[n].valid = true; ++n;
        } else {
            const Net::Snapshot& p = Net::Latest();
            if (p.valid && p.s.hasCar && p.s.gameFlowState == 6) {
                memcpy(views[n].pos, p.s.pos, 12);
                memcpy(views[n].dir, p.s.linVel, 12);
                views[n].valid = true; ++n;
            }
        }
        return n;
    }
    bool CarShouldStayAt(const float* pos) {
        if (!pos) return false;
        NetUtil::PlayerView views[2]{};
        int n = FillPlayerViews(views, 2);
        return n > 0 && NetUtil::CarProtectedFromEvict(views, n, pos);
    }
    bool CarShouldStay(void* iv) {
        if (!iv) return false;
        if (iv == LocalVehicle() || iv == g_ghost) return true;
        void* rb = RigidBodyOf(iv);
        return rb && CarShouldStayAt(RB_GetVec(rb, RB_GetPosition));
    }

}

static void OnCarHitImpl(uint16_t id, const float* dv);
static void OnCarHoldImpl(const CarHoldPacket& p);
static void SharedPursuitCopHit(uint16_t mirrorId);   // pursuit.inl

bool Game::InstallTrafficHooks() {
    // the game deleted one of our mirror cars to make room (and may build a new car in its memory): forget it now
    // (and wait 3 s before building it again: two copies over the car budget evicted each other every frame)
    // Any car either player can still see (close, front cone, or rear / mirrors) is kept when the
    // 20-car budget looks for something to evict - including the game's own traffic and cops.
    g_keepMirror = [](void* iv) -> bool {
        __try { return CarShouldStay(iv); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    };
    g_onMirrorRemoved = [](void* iv) {
        for (auto& m : g_mirrors) if (m.used && !m.bound && m.iv == iv) {
            m.iv = nullptr; m.noSpawnUntil = Net::LocalClockMs() + 3000.0;
            m.localSince = m.localUntil = 0; m.localTouched = false;
        }
    };
    uint8_t op = 0; int32_t rel = 0;
    if (!Mem::Read(DensityCallSite, op) || !Mem::Read(DensityCallSite + 1, rel) || op != 0xE8 ||
        DensityCallSite + 5 + rel != ComputeDensityFn) {
        LOG("Traffic: density call site doesn't match - traffic sync disabled");
        return false;
    }
    Mem::PatchCall(DensityCallSite, (void*)&hk_ComputeDensity);
    bool c1 = PatchCallChecked(PatrolCallSite, SpawnPatrolFn, (void*)&hk_SpawnPatrol);
    bool c2 = PatchCallChecked(PursuitCallSite1, SpawnPursuitFn, (void*)&hk_SpawnPursuit);
    bool c3 = PatchCallChecked(PursuitCallSite2, SpawnPursuitFn, (void*)&hk_SpawnPursuit);
    LOG("Cops: spawn hooks %s/%s/%s", c1 ? "ok" : "FAIL", c2 ? "ok" : "FAIL", c3 ? "ok" : "FAIL");
    {   // [Traffic] NoSpawnNearPlayers (checked at run time, so the hooks are always in place)
        bool t = PatchCallChecked(TrafficSpotCallSite, TrafficSpotBlockedFn, (void*)&hk_TrafficSpotBlocked);
        int cops = 0;
        for (uintptr_t site : CopSpotCallSites) cops += PatchCallChecked(site, CopSpotOkFn, (void*)&hk_CopSpotOk) ? 1 : 0;
        LOG("Traffic: new traffic / cops never right in front of the other player - spot checks hooked: traffic %s, cops %d/4 (NoSpawnNearPlayers=%d)",
            t ? "ok" : "FAIL", cops, (int)g_cfg.trafficNoSpawnNearPlayers);
    }
    Net::OnTraffic = [](const TrafficEntry* e, int count, double sendMs) { if (g_coopLinked) OnTrafficPacket(e, count, sendMs); };
    Net::OnTrailers = [](const TrailerEntry* e, int count, double sendMs) { if (g_coopLinked) OnTrailerPacket(e, count, sendMs); };
    Net::OnRacerName = &OnRacerName;
    Net::OnCarHit = [](uint16_t id, const float* dv) { if (!g_coopLinked) return; __try { OnCarHitImpl(id, dv); } __except (EXCEPTION_EXECUTE_HANDLER) {} };
    Net::OnCarHold = [](const CarHoldPacket& p) { if (!g_coopLinked) return; __try { OnCarHoldImpl(p); } __except (EXCEPTION_EXECUTE_HANDLER) {} };
    LOG("Traffic: hooked ComputeDensity call (client can suppress its own traffic)");
    return true;
}

// Which of our cars may show this host car: one with a trailer for a truck pulling one (the host sends
// its trailer), one without otherwise; anything when the host sends no trailer data (older build).
static int WantTrailer(const Mirror& m) {
    if (!HostSendsTrailers()) return -1;
    return (m.n && NetUtil::TrailerDataFresh(m.tn, m.tn ? m.ts[m.tn - 1].t : 0.0, m.s[m.n - 1].sendMs)) ? 1 : 0;
}

// The other player's own car (ghost, or the racer built as their model) must never be bound as a
// traffic or opponent mirror, and must not be sent as one of our own racers.
static bool IsPeerReservedCar(void* iv) {
    return iv && (iv == g_ghost || iv == g_replacedRacer);
}

// A host traffic car shown during a race (client, SharedInRaces): not a cop, not a racer.
static bool IsRaceTrafficMirror(const Mirror& m) {
    if (m.racer || (m.n && (m.s[m.n - 1].e.flags & 2))) return false;
    if (m.n && (m.s[m.n - 1].e.flags & 1)) return false;
    return m.bound || (SharedRaceTraffic() && m.n);
}

// Client, every 30 s: how much of the traffic around us is shared. Within 150 m: the host's cars shown
// here, the host's cars we know of but don't show (waiting for a car / a spot), and our own traffic cars
// that are on our screen only (the host doesn't see them). Reported: "some cars are synced, some only local".
static void TrafficSyncSummary(double now, const float* myPos) {
    if (g_cfg.isHost || !myPos) return;
    static double next = 0;
    if (next - now > 60000) next = now;              // the sim clock restarted (world reload)
    if (now < next) return;
    next = now + 30000;
    int shown = 0, notShown = 0, shownFar = 0, ownOnly = 0, cops = 0;
    for (auto& m : g_mirrors) {
        if (!m.used || m.racer) continue;
        if (m.n && (m.s[m.n - 1].e.flags & 1)) { ++cops; continue; }
        if (m.iv) {
            void* rb = RigidBodyOf(m.iv);
            if (rb && Dist(RB_GetVec(rb, RB_GetPosition), myPos) < 150.f) ++shown; else ++shownFar;
        } else ++notShown;
    }
    void** b = nullptr;
    for (int i = 0, cnt = ListSize(VL_AITRAFFIC, &b); i < cnt; ++i) {
        void* t = ListGet(VL_AITRAFFIC, i);
        if (!t || t == g_ghost || IsMirrorCar(t) || !IsVehicle(t)) continue;
        void* rb = RigidBodyOf(t);
        if (rb && Dist(RB_GetVec(rb, RB_GetPosition), myPos) < 150.f) ++ownOnly;
    }
    LOG("Traffic: sync - within 150 m: %d of the host's cars shown, %d of our own cars only on this screen; %d host cars further away, %d host cars not shown yet, %d host cops (%s)",
        shown, ownOnly, shownFar, notShown, cops, g_clientRacing ? "race" : "free roam");
}

static int g_raceBindLogs = 0;   // binding lines logged in this race (only the first few)
static int g_raceLastBound = -1, g_raceLastWaiting = -1;   // last logged status line

// Client in a race (SharedInRaces): give every host traffic car without a car here one of OUR OWN
// traffic cars to follow. Our own cars are never deleted and no AI is touched, so a car the race
// script owns stays a valid car (it is only moved); a host car with none of ours free gets a copy.
// A car of the same model first, then any model.
static void BindRaceTraffic() {
    bool want = false;   // only look at our cars when a host car is waiting for one
    for (auto& m : g_mirrors) if (m.used && !m.iv && m.n && IsRaceTrafficMirror(m)) { want = true; break; }
    if (!want) return;
    void* cars[64];
    NetUtil::TrafficCandidate cand[64];
    int nc = 0;
    void* me = LocalVehicle();
    void* lrb = RigidBodyOf(me);
    void* grb = RigidBodyOf(g_ghost);
    float lp[3] = {}, gp[3] = {};
    if (lrb) memcpy(lp, RB_GetVec(lrb, RB_GetPosition), 12);
    if (grb) memcpy(gp, RB_GetVec(grb, RB_GetPosition), 12);
    void** b = nullptr;
    int cnt = ListSize(VL_AITRAFFIC, &b);
    for (int i = 0; i < cnt && nc < 64; ++i) {
        void* t = ListGet(VL_AITRAFFIC, i);
        if (!t || t == me || !IsVehicle(t) || IsKeptCar(t) || IsPeerReservedCar(t)) continue;
        void* rb = RigidBodyOf(t);
        if (!rb) continue;
        cars[nc] = t;
        cand[nc].model = ModelKeyOf(t);
        memcpy(cand[nc].pos, RB_GetVec(rb, RB_GetPosition), 12);
        // already following a host car, or touching a player (moving it away would jolt them)
        cand[nc].skip = IsMirrorCar(t) || (lrb && Dist(lp, cand[nc].pos) < 6.f) || (grb && Dist(gp, cand[nc].pos) < 6.f);
        cand[nc].trailer = TrailerOf(t) != nullptr;
        ++nc;
    }
    if (!nc) return;
    // NoSpawnNearPlayers: a host car whose spot we are looking at (within 60 m) is only shown by one of
    // our cars that is already about there (it glides into place); else it waits until out of view
    bool viewRule = g_cfg.trafficNoSpawnNearPlayers;
    NetUtil::CamView cam = viewRule ? ReadCamera(lrb ? lp : nullptr) : NetUtil::CamView{};
    for (int pass = 0; pass < 2; ++pass) {           // same model for everyone first, then any model
        for (auto& m : g_mirrors) {
            if (!m.used || m.iv || !m.n || !IsRaceTrafficMirror(m)) continue;
            const TrafficEntry& e = m.s[m.n - 1].e;
            // never put a car inside a player (it would fling them): wait until it has moved on
            if ((lrb && Dist(lp, e.pos) < 6.f) || (grb && Dist(gp, e.pos) < 6.f)) continue;
            int k = NetUtil::PickTrafficPartner(m.model, e.pos, cand, nc, pass == 1, WantTrailer(m));
            if (k < 0) continue;
            bool inView = viewRule && !NetUtil::MirrorMayAppear(cam, e.pos);
            if (inView && !(Dist(cand[k].pos, e.pos) <= 10.f)) { if (pass == 1) ++g_viewWaiting; continue; }
            if (inView) m.blendPending = true;
            cand[k].skip = true;
            m.iv = cars[k];
            m.bound = true;
            if (g_raceBindLogs < 10) {
                ++g_raceBindLogs;
                LOG("Traffic: our traffic car %p (%s) now follows the host's car %u (%s model, %.0f m away)",
                    cars[k], VehName(cars[k]), (unsigned)m.id, cand[k].model == m.model ? "same" : "other", Dist(cand[k].pos, e.pos));
            }
        }
    }
}

// ---- smooth handover (client, free roam) ----------------------------------------------------------
// A host car copy we no longer need. With keepUnseen false only a car on screen is kept (an unseen one
// is removed right away as before). A kept traffic car drives on as our own traffic car (traffic AI);
// a cop, or a car that can't get the AI, coasts on and is removed once it is off screen.
static void LetGoOfMirror(Mirror& m, bool keepUnseen, int& handed, int& managed, int& coasting) {
    if (m.racer || m.bound || !m.iv || WasRemovedByGame(m.iv) || !ListContains(-1, m.iv) ||
        (!keepUnseen && !OnScreen(m.iv))) { RemoveMirror(m); return; }
    void* iv = m.iv;
    bool cop = m.n && (m.s[m.n - 1].e.flags & 1);
    int r = cop ? HAND_FAILED : HandToTrafficAI(iv);
    if (cop) {   // a cop copy the host dropped, kept on screen with no AI until out of view (reported twitching in place)
        static int copLetGoLogs = 0;
        if (copLetGoLogs < 20) {
            ++copLetGoLogs;
            void* rb = RigidBodyOf(iv); void* lrb = RigidBodyOf(LocalVehicle());
            const float* v = rb ? RB_GetVec(rb, RB_GetLinearVelocity) : nullptr;
            float sp = v ? sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]) : -1.f;
            float d = rb && lrb ? Dist(RB_GetVec(rb, RB_GetPosition), RB_GetVec(lrb, RB_GetPosition)) : -1.f;
            LOG("Cops: the other player dropped their cop %u (%s) - it stays here with no AI until out of view (%.0f km/h, %.0f m from us, %s)",
                (unsigned)m.id, VehName(iv), sp * 3.6f, d, keepUnseen ? "they left" : "on screen");
        }
    }
    if (r == HAND_FAILED) { Retire(iv, false); ++coasting; }
    else {
        ++handed;
        if (r == HAND_AI_MANAGED) ++managed; else Retire(iv, true);
        if (g_handoverLogs < 8) {
            ++g_handoverLogs;
            LOG("Traffic: the host's car %u (%s) drives on as our own traffic car %p (%s)", (unsigned)m.id, VehName(iv), iv,
                r == HAND_AI_MANAGED ? "traffic AI, the game culls it" : "traffic AI, we remove it once out of view");
        }
    }
    m = Mirror{};
}

// Leaving the host's traffic (or the host went away): none of the host's cars around us vanish.
static void ReleaseHostTraffic(const char* why) {
    int handed = 0, managed = 0, coasting = 0, freed = 0;
    for (auto& m : g_mirrors) {
        if (!m.used || m.racer) continue;
        if (m.bound) { ++freed; RemoveMirror(m); continue; }     // our own car: it just drives on
        LetGoOfMirror(m, true, handed, managed, coasting);
    }
    if (handed || coasting || freed)
        LOG("Traffic: %s - %d of the host's cars drive on as our traffic (%d taken over by the game's traffic manager), "
            "%d coast on until out of view, %d of our own cars stop following the host's", why, handed, managed, coasting, freed);
}

// Our own traffic cars a host car may take over (like BindRaceTraffic): not ours to keep, active,
// not already following a host car, not touching a player. Cars we handed over earlier count too.
struct OwnTraffic { void* car[64]; NetUtil::TrafficCandidate c[64]; int n; bool built; };
static void CollectOwnTraffic(OwnTraffic& o, const float* lp, const float* gp) {
    o.n = 0; o.built = true;
    void* me = LocalVehicle();
    void** b = nullptr;
    int cnt = ListSize(VL_AITRAFFIC, &b);
    for (int i = 0; i < cnt && o.n < 64; ++i) {
        void* t = ListGet(VL_AITRAFFIC, i);
        if (!t || t == g_ghost || t == me || !IsVehicle(t) || IsMirrorCar(t) || !IsActiveCar(t)) continue;
        if (IsKeptCar(t) && !IsRetired(t)) continue;
        void* rb = RigidBodyOf(t);
        if (!rb) continue;
        o.car[o.n] = t;
        o.c[o.n].model = ModelKeyOf(t);
        memcpy(o.c[o.n].pos, RB_GetVec(rb, RB_GetPosition), 12);
        o.c[o.n].skip = (lp && Dist(lp, o.c[o.n].pos) < 6.f) || (gp && Dist(gp, o.c[o.n].pos) < 6.f);
        o.c[o.n].trailer = TrailerOf(t) != nullptr;
        ++o.n;
    }
}
static bool AnyCarNear(const float* pos, float r) {
    void** b = nullptr;
    int cnt = ListSize(VL_ALL, &b);
    for (int i = 0; i < cnt; ++i) {
        void* t = ListGet(VL_ALL, i);
        void* rb = RigidBodyOf(t);
        if (rb && Dist(RB_GetVec(rb, RB_GetPosition), pos) < r) return true;
    }
    return false;
}

// A host traffic car without a car here: true = create it now (its spot is out of view or far, and
// free). Otherwise one of our own cars may become it: the one already sitting in its spot, or (spot in
// view) a nearby one, which then glides into place. Else it waits until its spot is out of view.
static bool PlaceHostCar(Mirror& m, const NetUtil::CamView& cam, const float* lp, const float* gp, OwnTraffic& own) {
    const TrafficEntry& e = m.s[m.n - 1].e;
    if (!own.built) CollectOwnTraffic(own, lp, gp);
    int wantTrailer = WantTrailer(m);
    int k = NetUtil::PickTakeoverPartner(m.model, e.pos, own.c, own.n, NetUtil::HANDOVER_OVERLAP_M, NetUtil::HANDOVER_OVERLAP_M, wantTrailer);
    const char* how = "in its spot";
    if (k < 0) {
        if (NetUtil::MaySpawnUnseen(cam, e.pos, lp)) return !AnyCarNear(e.pos, NetUtil::HANDOVER_OVERLAP_M);
        k = NetUtil::PickTakeoverPartner(m.model, e.pos, own.c, own.n, 25.f, 10.f, wantTrailer);
        how = "nearby, in view";
        if (k < 0) return false;
    }
    void* car = own.car[k];
    own.c[k].skip = true;
    for (auto& r : g_retired) if (r.iv == car) { r.iv = nullptr; ForgetCar(car); }   // one we handed over earlier
    m.iv = car;
    m.bound = true;
    m.blendPending = true;
    if (g_handoverLogs < 16) {
        ++g_handoverLogs;
        LOG("Traffic: our traffic car %p (%s, %s) now shows the host's car %u (%s model, %.0f m away)", car, VehName(car), how,
            (unsigned)m.id, own.c[k].model == m.model ? "same" : "other", Dist(own.c[k].pos, e.pos));
    }
    return false;
}

// Spawn / drive / expire the mirror cars (both sides: the client mirrors the host's traffic and
// cops, the host mirrors the client's own pursuit cops).
// The world is at the game's 20-car limit and a cop of the other player's roadblock waits for room:
// remove one of our own traffic cars that neither player can see (the farthest), at most every 0.5 s.
// Never in a race (race scripts own some traffic cars; removing one crashed the game, 0x5FD94A) -
// a Challenge Series pursuit counts as free roam here, as for the traffic sync.
int g_rbRoomMade = 0;
// a race (or one that just ended) may own some of our traffic cars: removing one crashed the game (0x5FD94A)
static bool RaceMayOwnCars() {
    void* rs = nullptr; int mode = 0; void* parms = nullptr; int type = -1;
    if (Mem::Read(0x91E000, rs) && rs && Mem::Read((uintptr_t)rs + 0x1960, mode) && mode == 1 &&
        Mem::Read((uintptr_t)rs + 0x1968, parms) && parms)
        type = ((int(__thiscall*)(void*))0x5FAA20)(parms);
    return CoopRace::ClientTrafficAsRace(mode, type) || g_clientRacing || (g_raceEndedTick && GetTickCount() - g_raceEndedTick < 8000);
}
static void MakeRoomForRoadblockCop() {
    static DWORD last = 0;
    if (GetTickCount() - last < 500) return;
    if (RaceMayOwnCars()) return;
    void* lrb = RigidBodyOf(LocalVehicle());
    if (!lrb) return;
    const float* me = RB_GetVec(lrb, RB_GetPosition);
    void* best = nullptr; float bestD = -1.f;
    void** b = nullptr;
    int cnt = ListSize(VL_AITRAFFIC, &b);
    for (int i = 0; i < cnt; ++i) {
        void* t = ListGet(VL_AITRAFFIC, i);
        if (!t || t == LocalVehicle() || t == g_ghost || IsMirrorCar(t) || IsPeerReservedCar(t) || !IsVehicle(t)) continue;
        if (ListContainsOne(VL_AIRACERS, t) || ListContainsOne(VL_AICOPS, t) || CarShouldStay(t)) continue;
        void* rb = RigidBodyOf(t);
        if (!rb) continue;
        float d = Dist(RB_GetVec(rb, RB_GetPosition), me);
        if (d > bestD) { bestD = d; best = t; }
    }
    if (!best) return;
    last = GetTickCount();
    KillVehicle(best);
    if (++g_rbRoomMade <= 5 || g_rbRoomMade % 20 == 0)
        LOG("Cops: removed one of our traffic cars out of sight (%.0f m) to make room for one of the other player's cops (chasing or in their roadblock, %d so far)", bestD, g_rbRoomMade);
}

static int MaintainMirrors(double now) {
    {   // how often copies were put on the ground (every 10 s while it happens)
        static DWORD last = 0; static int lastLearned = 0;
        if (GetTickCount() - last > 10000) {
            if (g_groundFixed) LOG("Traffic: grounded %d frame(s), max %.2f m (other model: %d, max %.2f m), %d ride sample(s)",
                                   g_groundFixed, g_groundMaxFix, g_groundFixedOther, g_groundMaxFixOther, g_groundLearned - lastLearned);
            if (g_modelSwaps || g_modelOtherFrames) LOG("Traffic: other model shown %d frame(s), %d swapped for the right one",
                                   g_modelOtherFrames, g_modelSwaps);
            last = GetTickCount(); lastLearned = g_groundLearned; g_groundFixed = 0; g_groundMaxFix = 0;
            g_groundFixedOther = 0; g_groundMaxFixOther = 0; g_modelSwaps = 0; g_modelOtherFrames = 0;
        }
    }
    {   // truck trailers (every 10 s while there are any)
        static DWORD last = 0;
        if (GetTickCount() - last > 10000) {
            if (g_trlDriven || g_trlOwnHitch || g_trlRehitched || g_trlPutBack)
                LOG("Traffic: trailers - %d frame(s) moved with the host's trailer data, %d frame(s) pulled by our own hitch, %d re-hitched, %d put back at the hitch (%d trailer states received, host sends trailers %d)",
                    g_trlDriven, g_trlOwnHitch, g_trlRehitched, g_trlPutBack, g_trailerKeys, (int)HostSendsTrailers());
            last = GetTickCount(); g_trlDriven = g_trlOwnHitch = g_trlRehitched = g_trlPutBack = 0; g_trailerKeys = 0;
        }
    }
    double renderSend = now - Net::PeerClockOffsetMs() - g_cfg.interpDelayMs;
    int spawned = 0, alive = 0, freed = 0;
    g_viewWaiting = 0;
    if (SharedRaceTraffic()) {
        for (auto& m : g_mirrors)   // the game removed a car we followed with: find another one
            if (m.used && m.bound && m.iv && (!ListContainsOne(VL_AITRAFFIC, m.iv) || m.iv == LocalVehicle())) { m.iv = nullptr; m.bound = false; }   // gone, or the address now holds another kind of car
        BindRaceTraffic();
    }
    // The other player's own car comes first: no mirror spawns until it exists, and when its spawn
    // failed (vehicle budget full) give back a couple of mirror cars.
    bool mayspawn = g_ghost && !g_ghostStarved;
    // No new copies while an event loads (its first 15 s, and during cutscenes): the race start deletes
    // them again a moment later, and a start camera that picked one of them crashed (0x44E9FD). A restart
    // (same event) is a race start too: the copies built during the race go first, and the 15 s start over.
    {
        static uint32_t lastEv = 0; static DWORD evAt = 0; static bool lastRestart = false, lastNis = false;
        uint32_t ev = Game::CurrentEventHash();
        uint8_t rs = 0; Mem::Read(0x91CF28, rs);
        bool restart = ev && rs, nis = ev && NisActive();
        if (ev != lastEv) { lastEv = ev; evAt = GetTickCount(); }
        if ((restart && !lastRestart) || (nis && !lastNis)) {
            evAt = GetTickCount();
            int gone = 0;
            if (restart && !lastRestart)
                for (auto& m : g_mirrors)
                    if (m.used && !m.racer && !m.bound && m.iv) { RemoveMirror(m); ++gone; }
            if (restart && !lastRestart) LOG("Traffic: race restart - %d copy car(s) removed, no new ones for 15 s", gone);
            // (a cutscene only re-arms the 15 s: a copy removed while one plays is deleted after it, RemoveMirror)
        }
        lastRestart = restart; lastNis = nis;
        if ((ev && GetTickCount() - evAt < 15000) || NisActive()) mayspawn = false;
    }
    // SmoothHandover (client, free roam, near the host): host cars appear only where it can't be seen
    bool smooth = SmoothHandover() && !g_clientRacing && g_trafficNear;
    NetUtil::CamView cam{};
    float lpos[3] = {}, gpos[3] = {};
    const float* lp = nullptr; const float* gp = nullptr;
    OwnTraffic own; own.n = 0; own.built = false;
    if (smooth) {
        if (void* lrb = RigidBodyOf(LocalVehicle())) { memcpy(lpos, RB_GetVec(lrb, RB_GetPosition), 12); lp = lpos; }
        if (void* grb = RigidBodyOf(g_ghost)) { memcpy(gpos, RB_GetVec(grb, RB_GetPosition), 12); gp = gpos; }
        cam = ReadCamera(lp);
    }
    g_hoWaiting = 0;
    // NoSpawnNearPlayers: every other copy we create (cops, cars in races, the client's cops on the host,
    // SmoothHandover=0) never appears inside our view within 60 m either - it waits until out of view
    NetUtil::CamView viewCam{}; bool viewCamRead = false;
    for (auto& m : g_mirrors) {
        if (!m.used) continue;
        // the owner keeps sending, just not this car: it's gone there. (If the owner sends nothing at all,
        // its game is paused - alt-tab, map, loading: keep every mirror frozen where it is.)
        // SmoothHandover: a car still on screen here drives on as our own traffic instead of vanishing.
        if (g_lastTrafficPkt - m.lastSeen > 1500) {
            // a cutscene's camera may have picked a copy (0x44E9FD): none is deleted while one plays
            if (NisActive() && !m.racer && !m.bound && m.iv) continue;
            if (smooth && !m.racer && !m.bound && m.iv) { int h = 0, mg = 0, co = 0; LetGoOfMirror(m, false, h, mg, co); g_hoKeptOnExpiry += h + co; }
            else RemoveMirror(m);
            continue;
        }
        if (m.iv && (!ListContains(-1, m.iv) || (!m.bound && !StillOurCar(m.iv)))) { m.iv = nullptr; m.bound = false; }   // the game removed it (or built another car there)
        // free roam: our own car that showed this host car was un-spawned by the traffic manager (or is gone)
        if (m.bound && m.iv && !SharedRaceTraffic() && (!ListContainsOne(VL_AITRAFFIC, m.iv) || m.iv == LocalVehicle() || !IsActiveCar(m.iv))) { m.iv = nullptr; m.bound = false; }
        // one of our own cars of ANOTHER model shows this host car (the only one in its spot, or nearby while the
        // spot was in view - so it didn't pop in): once nobody sees it, it goes and a copy of the host's model is
        // built in its place. (In a race our own cars stand in for good and are never removed.)
        // Only where a new car may appear unseen (MaySpawnUnseen: not right behind us in the mirror), with room
        // for the copy, never in or just after a race (0x5FD94A), and at most every few seconds per host car.
        if (m.bound && m.iv && m.n && !m.racer && ModelKeyOf(m.iv) != m.model) {
            ++g_modelOtherFrames;
            void* srb = RigidBodyOf(m.iv);
            void** lb = nullptr;
            if (smooth && srb && !SharedRaceTraffic() && !RaceMayOwnCars() && now >= m.swapNotBefore &&
                OffScreenS(m.iv) > NetUtil::HANDOVER_OFFSCREEN_S && NetUtil::MaySpawnUnseen(cam, RB_GetVec(srb, RB_GetPosition), lp) &&
                ListSize(VL_ALL, &lb) < CopyCarLimit()) {
                if (g_modelSwaps < 3) LOG("Traffic: host car %u (model %08X) was shown by our %s (%08X) - out of view now, building the right model",
                                          (unsigned)m.id, m.model, VehName(m.iv), ModelKeyOf(m.iv));
                KillVehicle(m.iv);
                m.iv = nullptr; m.bound = false; m.groundFix = 0; m.rideKnown = false; m.ownSampled = false;
                m.swapNotBefore = now + 5000.0;
                ++g_modelSwaps;
                continue;                           // built next frame (the removed car is not rebound meanwhile)
            }
        }
        // in a race a host car with no car of ours to follow it gets a copy, built below as in free roam
        // (out of view, with room); the copy is ours, so it may go again. Our own cars are never killed.
        if (IsRaceTrafficMirror(m) && (m.iv || m.bound)) {
            // one of our own traffic cars follows the host's car (bound above), or our copy of it
            if (m.iv && !NisActive()) {
                DriveMirror(m, renderSend);
                DriveTrailer(m, renderSend);
                GlareBits(m.iv) = m.s[m.n - 1].e.glare;
            }
            if (m.iv) ++alive;
            continue;
        }
        if (m.racer) {
            // the next race (a rival's second race: no world load) built a new racer at the same address
            if (m.iv && m.wid && WorldIdOf(m.iv) != m.wid) {
                LOG("Race: the racer that followed host opponent %u is a new car now - binding again", m.id);
                m.iv = nullptr; m.wid = 0; m.fails = 0;
            }
            // the host is in another event (the last race's results, or the next race while we're still on
            // ours): those are another race's racers - ours keep their own AI until it sends this race's
            {
                const Net::Snapshot& hp = Net::Latest();
                if (!CoopRace::HostRacersAreOurs(hp.valid, hp.valid ? hp.s.eventHash : 0u, Game::CurrentEventHash())) {
                    if (m.iv) {
                        if (!m.wid || WorldIdOf(m.iv) == m.wid) RestoreAI(m.iv);
                        LOG("Race: host opponent %u is from another event (host %08X, ours %08X) - our racer %p drives itself", m.id, hp.s.eventHash, Game::CurrentEventHash(), m.iv);
                        m.iv = nullptr; m.wid = 0; m.fails = 0;
                    }
                    m.unboundSince = 0;
                    continue;
                }
            }
            // never drive the other player's own car as an opponent / traffic slot
            if (m.iv && IsPeerReservedCar(m.iv)) {
                LOG("Race: opponent mirror %u was bound to the other player's car - letting go", m.id);
                m.iv = nullptr; m.wid = 0; m.bound = false; m.fails = 0;
            }
            // bind to one of our own AI racers - the same racer slot and role (1.8+ hosts send them), else the
            // same model, else (after a moment) any free one - switch its AI off and drive it. The peer ghost /
            // factory peer racer is never a candidate.
            if (!m.iv && m.n) {
                if (!m.unboundSince || m.unboundSince > now) m.unboundSince = now;
                void** b = nullptr;
                int cnt = ListSize(VL_AIRACERS, &b);
                if (cnt > 16) cnt = 16;
                CoopRace::RacerCand cand[16]; void* cars[16];
                void* me = LocalVehicle();
                for (int i = 0; i < cnt; ++i) {
                    void* r = ListGet(VL_AIRACERS, i);
                    cars[i] = r;
                    cand[i].free = r && !IsMirrorCar(r) && RigidBodyOf(r) &&
                                   CoopRace::MayBindAsRaceMirror(r == me, r == g_ghost, r == g_replacedRacer);
                    cand[i].model = cand[i].free ? ModelKeyOf(r) : 0;
                    cand[i].slot = cand[i].free ? RacerSlotOfInfo(RacerInfoOf(r)) : -1;
                    cand[i].role = RacerRoleOfSlot(cand[i].slot);
                }
                CoopRace::RacerWant want{ m.model };
                CoopRace::DecodeRacerSlot(m.s[m.n - 1].e.flags, want.slot, want.role);
                int k = CoopRace::PickRacer(cand, cnt, want, now - m.unboundSince > CoopRace::ANY_MODEL_AFTER_MS);
                if (k >= 0) {
                    void* r = cars[k];
                    m.iv = r; m.wid = WorldIdOf(r); m.unboundSince = 0;
                    DisableAI(r);
                    static const char* const kRoleName[] = { "normal", "rival", "helper" };
                    const char* wr = (want.role >= 0 && want.role <= 2) ? kRoleName[want.role] : "role unknown";
                    if (cand[k].model == m.model) LOG("Race: our racer %p (%s, slot %d) now follows the host's opponent (mirror id %u, host slot %d, %s)", r, VehName(r), cand[k].slot, m.id, want.slot, wr);
                    else LOG("Race: our racer %p (%s, model %08X, slot %d) now follows the host's opponent %u of model %08X (host slot %d, %s) - no free racer of that model in that slot here",
                             r, VehName(r), cand[k].model, cand[k].slot, m.id, m.model, want.slot, wr);
                }
                if (!m.iv && !m.fails) {   // log once why it found no partner
                    m.fails = 1;
                    char ours[200] = ""; size_t L = 0;
                    for (int i = 0; i < cnt && L < 180; ++i) { void* r = ListGet(VL_AIRACERS, i); if (r) L += sprintf_s(ours + L, sizeof(ours) - L, " %s=%08X%s", VehName(r), ModelKeyOf(r), r == g_ghost ? "(ghost)" : IsMirrorCar(r) ? "(bound)" : ""); }
                    LOG("Race: no racer of model %08X for host opponent %u; ours:%s", m.model, m.id, ours);
                }
            }
            if (m.iv && !NisActive()) DriveMirror(m, renderSend);
            if (m.iv && m.name[0]) if (void* ri = RacerInfoOf(m.iv)) { const char* nm = m.name; Mem::Write((uintptr_t)ri + 0x08, nm); }
            if (m.iv) ++alive;
            continue;
        }
        if (g_ghostStarved && m.iv && freed < 2 && !NisActive()) { KillVehicle(m.iv); m.iv = nullptr; ++freed; continue; }   // (room for the ghost: after a cutscene)
        if (!m.iv && m.n && mayspawn && spawned < 2 && m.fails < 3) {
            const TrafficEntry& e = m.s[m.n - 1].e;
            // never create a car inside a player (it would fling them): wait until it has moved on
            void* lrb = RigidBodyOf(LocalVehicle());
            void* grb = RigidBodyOf(g_ghost);
            if ((lrb && Dist(RB_GetVec(lrb, RB_GetPosition), e.pos) < 6.f) ||
                (grb && Dist(RB_GetVec(grb, RB_GetPosition), e.pos) < 6.f)) continue;
            if (Net::LocalClockMs() < m.noSpawnUntil) continue;   // the game just deleted it for room
            // the world is at the game's 20-car limit: wait for room instead of making the game delete one of
            // its cars for this copy (a car vanished in front of the player, then the game took the copy back)
            // (a cop of their roadblock: one of our own traffic cars nobody sees makes room for it, or the
            // roadblock would have a gap here that their game doesn't have)
            { void** lb = nullptr; if (ListSize(VL_ALL, &lb) >= CopyCarLimit()) {
                if (NetUtil::CopCopyNeedsRoom(e.flags)) { MakeRoomForRoadblockCop(); ++g_copRoomWaits; }
                ++g_budgetWaits;
                {   // no room: logged (at most every 10 s; the 30 s budget line counts every wait)
                    static DWORD lastAt = 0; DWORD now = GetTickCount();
                    if (now - lastAt > 10000) { lastAt = now; LOG("Traffic: no room for a copy of the other player's car - %d cars in the world, the limit for copies is %d ([Traffic] MaxCars); it waits", ListSize(VL_ALL, &lb), CopyCarLimit()); }
                }
                continue; } }
            // SmoothHandover: only where the player can't see it pop in (or one of our cars becomes it)
            bool placed = smooth && !(e.flags & 1);
            if (placed && !PlaceHostCar(m, cam, lp, gp, own)) { if (!m.iv) ++g_hoWaiting; continue; }
            if (!placed && g_cfg.trafficNoSpawnNearPlayers) {
                if (!viewCamRead) { viewCamRead = true; viewCam = ReadCamera(lrb ? RB_GetVec(lrb, RB_GetPosition) : nullptr); }
                if (!NetUtil::CopCopyMayAppearInView(e.flags) && !NetUtil::MirrorMayAppear(viewCam, e.pos)) {
                    ++g_viewWaiting; if (e.flags & 1) ++g_copViewWaits; continue;
                }
            }
            float dir[3] = { 1, 0, 0 };
            m.iv = SpawnVehicle(e.model, nullptr, e.pos, dir, g_cfg.trafficMirrorClass);
            ++spawned;
            if (!m.iv) { ++m.fails; LOG("Traffic: mirror spawn of %08X failed (%d)", e.model, m.fails); continue; }
            m.trailerSnap = true;   // a truck builds its trailer facing our spawn direction: put it at the hitch once the truck is in place
            if (e.flags & 1) LOG("Cops: showing the other player's police car (id %u, model %08X%s%s)", (unsigned)m.id, e.model,
                                 (e.flags & 4) ? ", chasing" : "", (e.flags & TE_ROADBLOCK) ? ", in their roadblock" : "");
        }
        if (m.iv) {
            DriveMirror(m, renderSend);
            DriveTrailer(m, renderSend);
            GlareBits(m.iv) = m.s[m.n - 1].e.glare;   // lights: brake/head/signals/cop light bar
            ++alive;
        }
    }
    if (freed) LOG("Traffic: removed %d mirror cars to make room for the other player's car", freed);
    {   // the other player's cops whose copy is not here yet (every 5 s while it happens)
        static DWORD nextCopLog = 0;
        if ((g_copRoomWaits || g_copViewWaits) && (int)(GetTickCount() - nextCopLog) >= 0) {
            nextCopLog = GetTickCount() + 5000;
            LOG("Cops: copies of the other player's cops waiting - %d frame(s) at the 20-car limit, %d frame(s) out of view (patrol)",
                g_copRoomWaits, g_copViewWaits);
            g_copRoomWaits = g_copViewWaits = 0;
        }
    }
    g_ghostStarved = false;
    return alive;
}

// A cop of ours standing in a roadblock: IVehicle vt+0xAC GetAIVehiclePtr -> IVehicleAI vt+0xA4 GetRoadBlock
// (AIVehicle::GetRoadBlock 0x431D80, the same slot in every AIVehicle* vtable, e.g. 0x891A44).
static bool CopInRoadblock(void* iv) {
    if (!IsVehicle(iv)) return false;
    void* ai = VCall<void* (__thiscall*)(void*)>(iv, 0xAC / 4)(iv);
    return ai && VCall<void* (__thiscall*)(void*)>(ai, 0xA4 / 4)(ai) != nullptr;
}

// Collect our own (non-mirror) cars of the given lists, nearest to `to` first, into a packet.
// Cops standing in one of our roadblocks go first (after race opponents), flagged TE_ROADBLOCK.
// One packet holds MAX_TRAFFIC_PER_PACKET (20) cars: 20 x 64 bytes keeps it in one UDP datagram (under the
// ~1400-byte MTU). With more cars than that (MaxCars), `rotate` (a cursor kept by the caller) splits it: the
// first kAlwaysSent of the ordered list (racers, roadblock cops, cops, then the nearest) go in every packet,
// the other slots take turns through the rest - each car is refreshed at least every few packets (40 cars:
// every 4th, 200 ms at 20 Hz), well inside the 1.5 s after which the other game drops a copy it didn't hear of.
static int g_sendTurn = 0;   // host: whose turn it is among the cars beyond the always-sent ones
static int CollectOwned(std::initializer_list<int> lists, const float* to, TrafficEntry* out, int maxOut, void** outIv = nullptr, int* rotate = nullptr) {
    constexpr int kCand = 128, kAlwaysSent = 12;
    struct C { void* iv; float d; uint8_t flags; } cand[kCand]; int nc = 0;
    void* me = LocalVehicle();
    for (int list : lists) {
        void** b = nullptr;
        int cnt = ListSize(list, &b);
        for (int i = 0; i < cnt && nc < kCand; ++i) {
            void* t = ListGet(list, i);
            if (!t || t == me || IsMirrorCar(t) || IsPeerReservedCar(t)) continue;
            void* rb = RigidBodyOf(t);
            if (!rb) continue;
            cand[nc].iv = t;
            // race opponents first, then cops, then traffic (nearest first within each)
            bool inRb = list == VL_AICOPS && CopInRoadblock(t);
            cand[nc].d = Dist(RB_GetVec(rb, RB_GetPosition), to) - (list == VL_AIRACERS ? 20000.f : inRb ? 15000.f : list == VL_AICOPS ? 10000.f : 0.f);
            cand[nc].flags = (uint8_t)((list == VL_AIRACERS ? 2 : list == VL_AICOPS ? 1 : 0) | (inRb ? TE_ROADBLOCK : 0));
            ++nc;
        }
    }
    // a racer often also sits in the traffic list: one entry, racer flags, or the client binds a
    // traffic car to that id and the real racer stays on the grid (stacked / unsynced)
    {
        NetUtil::OwnedCarCand dc[kCand];
        for (int i = 0; i < nc; ++i) { dc[i].key = (uintptr_t)cand[i].iv; dc[i].d = cand[i].d; dc[i].flags = cand[i].flags; }
        nc = NetUtil::DedupOwnedCars(dc, nc);
        for (int i = 0; i < nc; ++i) { cand[i].iv = (void*)dc[i].key; cand[i].d = dc[i].d; cand[i].flags = (uint8_t)dc[i].flags; }
    }
    for (int i = 0; i < nc; ++i)
        for (int j = i + 1; j < nc; ++j)
            if (cand[j].d < cand[i].d) { C x = cand[i]; cand[i] = cand[j]; cand[j] = x; }
    // more cars than one packet holds: the first kAlwaysSent stay, the rest take turns for the other slots
    if (rotate && nc > maxOut && maxOut > kAlwaysSent) {
        int rest = nc - kAlwaysSent, slots = maxOut - kAlwaysSent;
        int start = *rotate % rest;
        C pick[kCand]; int k = 0;
        for (int i = 0; i < kAlwaysSent; ++i) pick[k++] = cand[i];
        for (int j = 0; j < slots; ++j) pick[k++] = cand[kAlwaysSent + (start + j) % rest];
        *rotate = (start + slots) % rest;
        for (int i = 0; i < k; ++i) cand[i] = pick[i];
        nc = k;
        static DWORD lastLog = 0;
        if (GetTickCount() - lastLog > 30000) { lastLog = GetTickCount(); LOG("Traffic: %d cars to send, %d per packet - %d every packet, the other %d in turns (each at least every %d packets)", kAlwaysSent + rest, maxOut, kAlwaysSent, rest, (rest + slots - 1) / slots); }
    }
    int n = 0;
    for (int i = 0; i < nc && n < maxOut; ++i) {
        void* rb = RigidBodyOf(cand[i].iv);
        TrafficEntry& e = out[n];
        e.id = HostIdFor(cand[i].iv);
        if (!e.id) continue;
        e.flags = cand[i].flags;
        if (e.flags & 1) {   // a cop in a pursuit (IPursuitAI 0x4038E0, slot 5 GetInPursuit - what the minimap checks)
            void* pai = Find(cand[i].iv, 0x4038E0);
            if (pai && VCall<bool(__thiscall*)(void*)>(pai, 5)(pai)) e.flags |= 4;
        }
        if (EmulatedPhysics(cand[i].iv)) e.flags |= 8;     // floating here: the other game puts its copy on the ground
        // host: a racer's slot and role, so the client's racer in the same slot follows it (not just one of the same model)
        if (g_cfg.isHost && (e.flags & 2)) {
            int slot = RacerSlotOfInfo(RacerInfoOf(cand[i].iv));
            e.flags = CoopRace::EncodeRacerSlot(e.flags, slot, RacerRoleOfSlot(slot));
        }
        e.model = ModelKeyOf(cand[i].iv);
        e.glare = GlareBits(cand[i].iv);
        memcpy(e.pos, RB_GetVec(rb, RB_GetPosition), 12);
        memcpy(e.rot, RB_GetVec(rb, RB_GetOrientation), 16);
        memcpy(e.linVel, RB_GetVec(rb, RB_GetLinearVelocity), 12);
        memcpy(e.angVel, RB_GetVec(rb, RB_GetAngularVelocity), 12);
        if (outIv) outIv[n] = cand[i].iv;
        ++n;
    }
    return n;
}

// Host: the trailers of the trucks among the cars just collected (same order, same clock).
static int CollectTrailers(void* const* ivs, const TrafficEntry* ents, int n, TrailerEntry* out, int maxOut) {
    int k = 0;
    for (int i = 0; i < n && k < maxOut; ++i) {
        void* art = ArticulatedOf(ivs[i]);
        if (!art) continue;
        void* tr = TrailerOfArt(art);
        void* trb = RigidBodyOf(tr);
        if (!trb) continue;
        TrailerEntry& t = out[k];
        t = TrailerEntry{};
        t.id = ents[i].id;
        t.flags = (uint8_t)((TrailerHitched(art) ? TRAILER_HITCHED : 0) | (EmulatedPhysics(tr) ? TRAILER_EMULATED : 0));
        t.model = ModelKeyOf(tr);
        memcpy(t.pos, RB_GetVec(trb, RB_GetPosition), 12);
        memcpy(t.rot, RB_GetVec(trb, RB_GetOrientation), 16);
        memcpy(t.linVel, RB_GetVec(trb, RB_GetLinearVelocity), 12);
        memcpy(t.angVel, RB_GetVec(trb, RB_GetAngularVelocity), 12);
        if (NetUtil::ValidTrailerEntry(t)) ++k;    // the receiver drops a whole packet with one bad entry
    }
    return k;
}

static int g_sentCount = 0;

// NoSpawnNearPlayers status whenever the numbers change (at most every 5 s)
static void NoSpawnStatusTick(double now) {
    static int lastT = 0, lastC = 0, lastW = -1;
    static double nextLog = 0;
    if (nextLog - now > 10000) nextLog = now;   // the sim clock restarted (world reload)
    if (now < nextLog || (g_noSpawnTraffic == lastT && g_noSpawnCops == lastC && g_viewWaiting == lastW)) return;
    if (lastW < 0 && !g_noSpawnTraffic && !g_noSpawnCops && !g_viewWaiting) { lastW = 0; return; }
    lastT = g_noSpawnTraffic; lastC = g_noSpawnCops; lastW = g_viewWaiting; nextLog = now + 5000;
    LOG("Traffic: no-spawn - %d traffic / %d police spots refused in front of the other player so far, %d of their cars wait until out of our view",
        lastT, lastC, lastW);
}

static void TrafficTickImpl(bool connected, const Net::Snapshot& peer) {
    double now = Net::LocalClockMs();
    FlushNisDoomed(Game::GameFlowState() == 6);
    if (g_cfg.trafficNoSpawnNearPlayers) NoSpawnStatusTick(now);
    void* me = LocalVehicle();
    void* myRb = RigidBodyOf(me);
    {   // their pause menu sends "no car" for a while: keep everything as it is (like the other player's car)
        static double noCarSince = 0;
        if (peer.valid && !peer.s.hasCar && peer.s.gameFlowState == 6 && connected && myRb && Game::GameFlowState() == 6) {
            if (!noCarSince) noCarSince = now;
            if (now - noCarSince < 120000) return;
        } else noCarSince = 0;
    }
    if (Game::GameFlowState() != 6 || !myRb || !connected || !peer.valid || !peer.s.hasCar) {
        // not in a shared world: clean up and hand traffic back to the game
        if (g_suppressTraffic) LOG("Traffic: released (not connected / not in game)");
        g_suppressTraffic = false;
        g_raceDensityOff = false;
        g_trafficNear = false;
        bool inWorld = Game::GameFlowState() == 6 && myRb;
        // still driving (only the host is gone / has no car): the host's cars around us stay and drive on
        if (inWorld && SmoothHandover() && !g_clientRacing) ReleaseHostTraffic("the host's traffic is gone");
        for (auto& m : g_mirrors) if (m.used) RemoveMirror(m);
        if (inWorld) TickRetired(ReadCamera(RB_GetVec(myRb, RB_GetPosition))); else ForgetRetired();
        g_mirrorCount = 0;
        return;
    }
    const float* myPos = RB_GetVec(myRb, RB_GetPosition);
    float distToPeer = Dist(myPos, peer.s.pos);
    static double nextSend = 0;
    if (nextSend - now > 1000) nextSend = now;   // the sim clock restarted (world reload)
    bool sendNow = now >= nextSend;
    if (sendNow) nextSend = now + 50; // 20 Hz

    if (g_cfg.isHost) {
        // host: send its cops + traffic nearest to the client; mirror the client's own pursuit cops
        // host: in a race its AI opponents always go too (the client's own racers follow them)
        bool inRace = false;
        { void* rs = nullptr; int mode = 0; inRace = Mem::Read(0x91E000, rs) && rs && Mem::Read((uintptr_t)rs + 0x1960, mode) && mode == 1; }
        if (sendNow && (distToPeer <= 450.f || inRace)) {
            TrafficEntry out[MAX_TRAFFIC_PER_PACKET];
            { static int lastRacers = -1; void** rb_ = nullptr; int nr = inRace ? ListSize(VL_AIRACERS, &rb_) : 0;
              if (nr != lastRacers) { lastRacers = nr; LOG("Race: sending %d racer slot(s) to the client (in race %d)", nr, (int)inRace); } }
            void* outIv[MAX_TRAFFIC_PER_PACKET] = {};
            g_sentCount = distToPeer <= 450.f
                ? CollectOwned({ VL_AIRACERS, VL_AICOPS, VL_AITRAFFIC }, peer.s.pos, out, MAX_TRAFFIC_PER_PACKET, outIv, &g_sendTurn)
                : CollectOwned({ VL_AIRACERS }, peer.s.pos, out, MAX_TRAFFIC_PER_PACKET, outIv, &g_sendTurn);
            float simT = Game::SimTimeSec();
            Net::SendTraffic(out, g_sentCount, simT);
            // the trailers of the trucks in it (sent every time, also empty: tells the client we send them)
            TrailerEntry trl[MAX_TRAILERS_PER_PACKET];
            int nt = CollectTrailers(outIv, out, g_sentCount, trl, MAX_TRAILERS_PER_PACKET);
            Net::SendTrailers(trl, nt, simT);
            if (inRace) {   // our opponents' race progress (the client's copies of them have no AI to work it out)
                RaceProgEntry rp[MAX_RACEPROG]; int np = 0;
                for (int i = 0; i < g_sentCount && np < MAX_RACEPROG; ++i) {
                    if (!(out[i].flags & 2) || !outIv[i]) continue;
                    void* ri = RacerInfoOf(outIv[i]);
                    if (!ri) continue;
                    RaceProgEntry& e = rp[np];
                    e = RaceProgEntry{};
                    e.id = out[i].id;
                    int laps = 0, cps = 0; uint32_t outs = 0; uint8_t fin = 0;
                    Mem::Read((uintptr_t)ri + 0x18, e.pct); Mem::Read((uintptr_t)ri + 0x24, e.lapPct); Mem::Read((uintptr_t)ri + 0xF8, e.toNext);
                    Mem::Read((uintptr_t)ri + 0x28, laps); Mem::Read((uintptr_t)ri + 0x2C, cps);
                    Mem::Read((uintptr_t)ri + 0x1C, outs); Mem::Read((uintptr_t)ri + 0x21, fin);
                    e.laps = (int16_t)(laps < 0 ? 0 : laps > 200 ? 200 : laps); e.cps = (int16_t)(cps < 0 ? 0 : cps > 2000 ? 2000 : cps);
                    e.flags = (outs ? 1 : 0) | (fin ? 2 : 0);
                    ++np;
                }
                Net::SendRaceProg(rp, np);
            }
            static int lastTrailers = -1;
            if ((nt > 0) != (lastTrailers > 0) || lastTrailers < 0) {
                lastTrailers = nt;
                LOG("Traffic: %d truck trailer(s) sent with the traffic", nt);
            }
        }
        // opponents' names, every 2 s while racing (the client shows the same names)
        static double nextNames = 0;
        if (nextNames - now > 5000) nextNames = now;
        if (inRace && now >= nextNames) {
            nextNames = now + 2000;
            void** b = nullptr;
            int cnt = ListSize(VL_AIRACERS, &b);
            for (int i = 0; i < cnt; ++i) {
                void* r = ListGet(VL_AIRACERS, i);
                if (!r || IsMirrorCar(r) || IsPeerReservedCar(r)) continue;
                void* ri = RacerInfoOf(r);
                const char* nm = nullptr;
                if (!ri || !Mem::Read((uintptr_t)ri + 0x08, nm) || !nm || !Mem::IsReadable(nm, 2)) continue;
                char buf[31]; if (!Mem::SafeRead(nm, buf, 30)) continue; buf[30] = 0;
                if (uint16_t id = HostIdFor(r)) Net::SendRacerName(id, buf);
            }
        }
        g_mirrorCount = MaintainMirrors(now);
        return;
    }

    // ---- client ----
    bool isNear = g_trafficNear ? distToPeer < 450.f : distToPeer < 350.f;
    // not more than one switch every 15 s: each one removes and rebuilds all traffic around us (with
    // SmoothHandover it hands cars over instead, but a quick back-and-forth still churns them)
    static double lastFlip = -1e9;
    if (lastFlip > now) lastFlip = -1e9;                         // the sim clock restarted (world reload)
    if (isNear != g_trafficNear && now - lastFlip < 15000) isNear = g_trafficNear;
    if (isNear != g_trafficNear) {
        lastFlip = now;
        g_trafficNear = isNear;
        bool smoothNow = SmoothHandover() && !g_clientRacing;
        LOG("Traffic: %s the host (%.0f m) - %s", isNear ? "near" : "away from", distToPeer,
            isNear ? (smoothNow ? "mirroring host traffic (our cars stay until out of view, host cars appear out of view)" : "mirroring host traffic")
                   : (smoothNow ? "using own traffic (the host's cars around us drive on as ours)" : "using own traffic"));
        g_handoverLogs = 0;
        if (!isNear) {
            if (smoothNow) ReleaseHostTraffic("left the host's traffic");
            for (auto& m : g_mirrors) if (m.used && !m.racer) RemoveMirror(m);
        }
    }
    {
        void* rs = nullptr; int mode = 0; void* parms = nullptr; int type = -1;
        if (Mem::Read(0x91E000, rs) && rs && Mem::Read((uintptr_t)rs + 0x1960, mode) && mode == 1 &&
            Mem::Read((uintptr_t)rs + 0x1968, parms) && parms)
            type = ((int(__thiscall*)(void*))0x5FAA20)(parms);              // GRaceParameters type
        bool racing = CoopRace::ClientTrafficAsRace(mode, type);
        {
            static bool lastPc = false; bool pc = mode == 1 && type == CoopRace::kTypePursuitChallenge;
            if (pc != lastPc) { lastPc = pc; if (pc) LOG("Traffic: pursuit challenge - sharing the host's traffic as in free roam"); }
        }
        if (racing != g_clientRacing) {
            // cars bound to the host's are our own: just released. Our copies go at the race start (the event
            // reloads the world); at its end they stay and go on as free-roam copies (no cars vanish).
            int released = 0;
            for (auto& m : g_mirrors) {
                if (!m.used || m.racer || !m.n || (m.s[m.n - 1].e.flags & 1)) continue;
                if (m.bound) { ++released; RemoveMirror(m); }
                else if (racing) RemoveMirror(m);
            }
            g_clientRacing = racing;
            g_raceBindLogs = 0; g_raceLastBound = g_raceLastWaiting = -1;
            if (racing && g_cfg.trafficSharedInRaces)
                LOG("Traffic: in a race - mirroring the host's traffic with our own traffic cars, copies where none is free (SharedInRaces=1)");
            else
                LOG("Traffic: %s", racing ? "in a race - keeping our own traffic" : "race over - sharing the host's traffic again");
            if (released) LOG("Traffic: released %d of our traffic cars that followed the host's (they drive on by themselves)", released);
            if (!racing) g_raceEndedTick = GetTickCount();
        }
    }
    if (g_clientRacing) {
        g_suppressTraffic = false;
        // near the host no new traffic cars of our own: they'd be on this screen only (the host's cars are
        // shown by our cars or copies). The ones we have drive on; none is removed (race scripts own some).
        g_raceDensityOff = g_cfg.trafficSharedInRaces && g_trafficNear;
        if (sendNow) {
            TrafficEntry out[MAX_TRAFFIC_PER_PACKET];
            g_sentCount = CollectOwned({ VL_AICOPS }, peer.s.pos, out, 8);
            Net::SendTraffic(out, g_sentCount, Game::SimTimeSec());
        }
        g_mirrorCount = MaintainMirrors(now);
        if (g_cfg.trafficSharedInRaces) {
            // status line whenever the numbers change (at most every 5 s)
            int bound = 0, waiting = 0;
            for (auto& m : g_mirrors) if (m.used && IsRaceTrafficMirror(m)) { if (m.iv) ++bound; else ++waiting; }
            static double nextLog = 0;
            if (nextLog - now > 10000) nextLog = now;   // the sim clock restarted (world reload)
            if ((bound != g_raceLastBound || waiting != g_raceLastWaiting) && now >= nextLog) {
                g_raceLastBound = bound; g_raceLastWaiting = waiting; nextLog = now + 5000;
                LOG("Traffic: in a race - mirroring the host's traffic (%d cars, %d of the host's without a car here)", bound, waiting);
            }
        }
        TrafficSyncSummary(now, myPos);
        return;
    }
    g_raceDensityOff = false;
    g_suppressTraffic = isNear;   // no own traffic / patrol spawns near the host (pursuit spawns stay)
    bool smooth = SmoothHandover();
    NetUtil::CamView cam = smooth ? ReadCamera(myPos) : NetUtil::CamView{};
    if (smooth) g_hoRetiredGone += TickRetired(cam);
    if (!isNear) { g_sentCount = 0; g_mirrorCount = MaintainMirrors(now); return; }   // race opponents only

    // remove our own (non-mirror) traffic cars, a few per frame. Our own cops stay: they belong to
    // our pursuit and are sent to the host. SmoothHandover: only cars that are off screen (the rest
    // drive on until they leave the view; no new ones spawn here).
    int ownInView = 0;
    if (!g_raceEndedTick || GetTickCount() - g_raceEndedTick > 8000) {
        int killed = 0;
        void** b = nullptr;
        int cnt = ListSize(VL_AITRAFFIC, &b);
        for (int i = cnt - 1; i >= 0 && killed < 3; --i) {
            void* t = ListGet(VL_AITRAFFIC, i);
            if (!t || t == g_ghost || IsMirrorCar(t) || !IsVehicle(t)) continue;
            void* rb = RigidBodyOf(t);
            if (smooth) {
                float d = rb ? Dist(RB_GetVec(rb, RB_GetPosition), cam.pos) : 0.f;
                if (!NetUtil::OwnCarMayGo(OnScreenS(t), OffScreenS(t), d)) { ++ownInView; continue; }
            }
            // rear-view / the other player's view: the game's on-screen timer is the local camera only
            if (rb && CarShouldStayAt(RB_GetVec(rb, RB_GetPosition))) { ++ownInView; continue; }
            KillVehicle(t); ++killed; ++g_killedOwn;
        }
    }
    if (sendNow) {
        TrafficEntry out[MAX_TRAFFIC_PER_PACKET];
        g_sentCount = CollectOwned({ VL_AICOPS }, peer.s.pos, out, 8);
        Net::SendTraffic(out, g_sentCount, Game::SimTimeSec());   // also when empty: tells the host our cops are gone
    }
    g_mirrorCount = MaintainMirrors(now);
    if (smooth) {
        // status whenever the numbers change (at most every 5 s)
        int shownByOurs = 0;
        for (auto& m : g_mirrors) if (m.used && m.bound && m.iv) ++shownByOurs;
        static int lastInView = -1, lastWaiting = -1, lastOurs = -1;
        static double nextLog = 0;
        if (nextLog - now > 10000) nextLog = now;   // the sim clock restarted (world reload)
        if ((ownInView != lastInView || g_hoWaiting != lastWaiting || shownByOurs != lastOurs) && now >= nextLog) {
            lastInView = ownInView; lastWaiting = g_hoWaiting; lastOurs = shownByOurs; nextLog = now + 5000;
            LOG("Traffic: handover - %d of our cars still in view, %d host cars waiting for a spot out of view, %d shown by our own cars, %d host cars kept on screen after the host dropped them, %d let-go cars removed out of view",
                ownInView, g_hoWaiting, shownByOurs, g_hoKeptOnExpiry, g_hoRetiredGone);
        }
    }
    TrafficSyncSummary(now, myPos);
}

// About to hit a car the other game owns: our physics takes the copy over for a moment (HitUtil::TakeLocalOnApproach).
// Their race opponents are left to the network (they race on both screens).
static void TakeLocalHits(void* lrb, DWORD now) {
    if ((int)(g_noHitUntil - now) > 0) return;
    const float* lp = RB_GetVec(lrb, RB_GetPosition);
    const float* lv = RB_GetVec(lrb, RB_GetLinearVelocity);
    for (auto& m : g_mirrors) {
        // (not race opponents, and not our own cars that follow theirs in a race: the race script owns those)
        if (!m.used || !m.iv || m.racer || m.bound || m.n == 0) continue;
        if (!m.localSince && m.localAgainAt && (int)(now - m.localAgainAt) < 0) continue;   // just released: let it glide back first
        void* rb = RigidBodyOf(m.iv);
        if (!rb) continue;
        const float* cp = RB_GetVec(rb, RB_GetPosition);
        if (m.localSince) {   // ours already: a little longer while we still touch it
            bool touching = HitUtil::StillTouching(lp, cp);
            if (touching && HitUtil::CopyWasHit(m.localV0, RB_GetVec(rb, RB_GetLinearVelocity))) m.localTouched = true;
            if (touching && now - m.localSince < HitUtil::LOCAL_HIT_MAX_MS && (int)(m.localUntil - (now + 600)) < 0)
                m.localUntil = now + 600;
            continue;
        }
        if (!HitUtil::TakeLocalOnApproach(lp, lv, cp, RB_GetVec(rb, RB_GetLinearVelocity))) continue;
        m.localSince = now | 1; m.localUntil = now + HitUtil::LOCAL_HIT_MS; m.blendT0 = 0; m.blendPending = false;
        m.localTouched = false; m.localSentAt = 0; memcpy(m.localV0, RB_GetVec(rb, RB_GetLinearVelocity), 12);
        float dx = cp[0] - lp[0], dz = cp[2] - lp[2];
        LOG("Collision: about to hit the other player's car %u (%s, %.1f m, %.0f km/h) - our physics moves it for a moment",
            (unsigned)m.id, VehName(m.iv), sqrtf(dx * dx + dz * dz), sqrtf(lv[0] * lv[0] + lv[2] * lv[2]) * 3.6f);
    }
}

// Hitting a car the OTHER game owns (their traffic, cops, race opponents shown here as mirrors): the
// mirror is moved by the network, so only our car reacts. Measure what our car lost in the impact
// (250 ms from the first abrupt change) and send it to the owner, who pushes the real car.
static void MirrorHitTick() {
    void* lrb = RigidBodyOf(LocalVehicle());
    if (!lrb) return;
    static float prevV[3], v0[3]; static DWORD prevT = 0, t0 = 0, lastSent = 0; static uint16_t target = 0;
    const float* lv = RB_GetVec(lrb, RB_GetLinearVelocity);
    const float* lp = RB_GetVec(lrb, RB_GetPosition);
    DWORD now = GetTickCount();
    // a teleport (restart, reset to track, event start) or a cutscene is not an impact: no hits for 1 s
    {
        static float lastPos[3]; static bool have = false;
        float sp = sqrtf(lv[0] * lv[0] + lv[1] * lv[1] + lv[2] * lv[2]);
        float dt = prevT ? (now - prevT) / 1000.f : 0.f;
        uint8_t restartPending = 0; Mem::Read(0x91CF28, restartPending);
        bool jumped = have && Dist(lastPos, lp) > sp * dt + 3.f && (int)(g_testJumpOkUntil - now) <= 0;
        if (jumped || restartPending || NisActive() || Game::RaceStage() == 1) g_noHitUntil = now + 1000;
        memcpy(lastPos, lp, 12); have = true;
    }
    if ((int)(g_noHitUntil - now) > 0) t0 = 0;
    TakeLocalHits(lrb, now);
    void* grb0 = RigidBodyOf(g_ghost);
    bool playerNear = grb0 && Dist(RB_GetVec(grb0, RB_GetPosition), lp) < 6.f;    // then it is the player-to-player case
    if (!t0 && !playerNear && (int)(g_noHitUntil - now) <= 0 && prevT && now - prevT < 100 && now - lastSent > 600) {
        float d0[3] = { prevV[0] - lv[0], 0, prevV[2] - lv[2] };
        if (sqrtf(d0[0] * d0[0] + d0[2] * d0[2]) > 3.f) {
            float best = 5.5f; uint16_t id = 0;
            for (auto& m : g_mirrors) {
                if (!m.used || !m.iv) continue;
                void* rb = RigidBodyOf(m.iv);
                if (!rb) continue;
                float dd = Dist(RB_GetVec(rb, RB_GetPosition), lp);
                if (dd < best && HitPointsAt(d0, lp, RB_GetVec(rb, RB_GetPosition))) { best = dd; id = m.id; }
            }
            void* grb = RigidBodyOf(g_ghost);
            if (id && !(grb && Dist(RB_GetVec(grb, RB_GetPosition), lp) < best)) {
                t0 = now; target = id; memcpy(v0, prevV, 12);
                // the copy is a wall to our physics: keep the speed a hit on a real car would leave us
                // (not when our physics moves it right now - then the hit was a real one)
                Mirror* hm = FindMirror(id);
                void* hrb = (hm && !hm->racer && hm->iv && !hm->localSince) ? RigidBodyOf(hm->iv) : nullptr;
                if (hrb) {
                    const float* hp = RB_GetVec(hrb, RB_GetPosition);
                    float n[3] = { hp[0] - lp[0], 0.f, hp[2] - lp[2] }, nv[3];
                    if (HitUtil::MirrorHitKeepSpeed(prevV, lv, n, nv)) {
                        float was = sqrtf(prevV[0] * prevV[0] + prevV[2] * prevV[2]) * 3.6f;
                        float lost = sqrtf(lv[0] * lv[0] + lv[2] * lv[2]) * 3.6f;
                        float kept = sqrtf(nv[0] * nv[0] + nv[2] * nv[2]) * 3.6f;
                        RB_SetVec(lrb, RB_SetLinearVelocity, nv);
                        LOG("Collision: hit the other player's car %u at %.0f km/h - %.0f km/h left, keeping %.0f km/h (it is a copy: no wall)",
                            (unsigned)id, was, lost, kept);
                    }
                }
            }
        }
    }
    if (t0 && now - t0 >= 250) {
        float dv[3] = { v0[0] - lv[0], v0[1] - lv[1], v0[2] - lv[2] };
        float mag = sqrtf(dv[0] * dv[0] + dv[1] * dv[1] + dv[2] * dv[2]);
        t0 = 0; lastSent = now;
        if (mag > 4.f) {
            Net::SendCarHit(target, dv);
            LOG("Collision: hit the other player's car %u, lost %.0f km/h - sent to them", (unsigned)target, mag * 3.6f);
            Mirror* m = FindMirror(target);
            if (m && m->n && (m->s[m->n - 1].e.flags & 1)) SharedPursuitCopHit(target);   // one of their cops
        }
    }
    memcpy(prevV, lv, 12); prevT = now;
}

// owner side: push our real car (cops / traffic). A single velocity write is eaten by the AI
// in a frame or two, so the car felt like a wall; hold the push for 0.4 s like player hits.
struct CarPush { void* iv; uint16_t id; float push[3]; DWORD from; bool used; };
CarPush g_carPushes[16] = {};

static void StartCarPush(void* iv, uint16_t id, const float* push) {
    if (!iv || !push) return;
    CarPush* slot = nullptr;
    for (auto& p : g_carPushes) if (p.used && p.iv == iv) { slot = &p; break; }
    if (!slot) for (auto& p : g_carPushes) if (!p.used) { slot = &p; break; }
    if (!slot) slot = &g_carPushes[0];
    slot->iv = iv; slot->id = id; slot->push[0] = push[0]; slot->push[1] = 0; slot->push[2] = push[2];
    slot->from = GetTickCount(); slot->used = true;
}

// owner side: the other game's physics moves our car after its hit (PKT_CARHOLD): put our real car where
// it is there; on its last word our AI drives on from that spot (no glide back on their screen).
// A slot stays tied to its car for 1 s after the hold ends (at), so a late or repeated message is ignored.
struct CarHold { void* iv; uint16_t id; CarHoldPacket p; DWORD at, t0; uint32_t seq; float off[3]; bool fresh, used, refused; };
CarHold g_carHolds[8] = {};
static bool CarHeld(void* iv) {
    for (auto& h : g_carHolds) if (h.used && !h.refused && h.iv == iv) return true;
    return false;
}
static void EndHold(CarHold& h, DWORD now) { h.used = false; h.at = now; }

static void OnCarHoldImpl(const CarHoldPacket& in) {
    for (auto& h : g_hostIds) {
        if (!h.iv || NetUtil::TrafficWireId(h.id, g_cfg.isHost) != in.id) continue;
        if (!ListContains(-1, h.iv) || NisActive()) return;
        void* rb = RigidBodyOf(h.iv);
        if (!rb) return;
        CarHoldPacket p = in;
        const float* cp = RB_GetVec(rb, RB_GetPosition);
        if (!HitUtil::SaneHold(p.rot, p.linVel, p.angVel) || Dist(cp, p.pos) > HitUtil::HOLD_MAX_JUMP) return;
        DWORD now = GetTickCount();
        CarHold* slot = nullptr;
        for (auto& c : g_carHolds) if (c.iv == h.iv && (c.used || now - c.at < 1000)) { slot = &c; break; }
        if (slot && !HitUtil::NewerSeq(p.h.seq, slot->seq)) return;   // late, or a repeat
        if (slot && slot->used && slot->refused) {                     // our own player touches it: ours
            slot->seq = p.h.seq; slot->at = now;
            if (p.last) EndHold(*slot, now);
            return;
        }
        if (!slot || !slot->used) {
            if (p.last) return;   // the hold's other messages were lost: our AI drives it already
            if (!slot) for (auto& c : g_carHolds) if (!c.used && now - c.at >= 1000) { slot = &c; break; }
            if (!slot) for (auto& c : g_carHolds) if (!c.used) { slot = &c; break; }
            if (!slot) return;
            for (int i = 0; i < 3; ++i) slot->off[i] = cp[i] - p.pos[i];
            slot->iv = h.iv; slot->id = p.id; slot->t0 = now; slot->refused = false;
            LOG("Collision: the other player hit our car %u (%s) - it follows their physics for a moment (%.1f m apart)",
                (unsigned)p.id, VehName(h.iv), Dist(cp, p.pos));
        }
        for (auto& c : g_carPushes) if (c.used && c.iv == h.iv) c.used = false;   // their physics already has the push
        slot->p = p; slot->seq = p.h.seq; slot->at = now; slot->fresh = true; slot->used = true;
        return;
    }
}

static void ApplyCarHolds() {
    DWORD now = GetTickCount();
    void* lrb = RigidBodyOf(LocalVehicle());
    bool nis = NisActive();
    for (auto& h : g_carHolds) {
        if (!h.used) continue;
        void* rb = (h.iv && ListContains(-1, h.iv)) ? RigidBodyOf(h.iv) : nullptr;
        if (!rb || nis) { EndHold(h, now); continue; }
        if (h.refused) { if (now - h.at > 500) EndHold(h, now); continue; }
        if (lrb && HitUtil::StillTouching(RB_GetVec(lrb, RB_GetPosition), RB_GetVec(rb, RB_GetPosition))) {
            h.refused = true;   // both of us touch it: our physics keeps it (theirs glides back on their screen)
            LOG("Collision: we touch our car %u too - our physics keeps it", (unsigned)h.id);
            continue;
        }
        if (h.fresh) {
            h.fresh = false;
            float k = HitUtil::HoldBlendLeft(now - h.t0), pos[3];
            for (int i = 0; i < 3; ++i) pos[i] = h.p.pos[i] + h.off[i] * k;
            RB_SetVec(rb, RB_SetOrientationQuat, h.p.rot);
            RB_SetVec(rb, RB_SetPosition, pos);
            RB_SetVec(rb, RB_SetLinearVelocity, h.p.linVel);
            RB_SetVec(rb, RB_SetAngularVelocity, h.p.angVel);
            if (h.p.last) {
                EndHold(h, now);
                LOG("Collision: our car %u drives on from where the other player's hit left it", (unsigned)h.id);
                continue;
            }
        }
        if (now - h.at > 500) { EndHold(h, now); LOG("Collision: no word on our car %u for 0.5 s - it drives on", (unsigned)h.id); }
    }
}

static void ApplyCarPushes() {
    ApplyCarHolds();
    DWORD now = GetTickCount();
    for (auto& p : g_carPushes) {
        if (!p.used) continue;
        if (!p.iv || !ListContains(-1, p.iv) || now - p.from >= HitUtil::HIT_PUSH_MS) { p.used = false; continue; }
        void* rb = RigidBodyOf(p.iv);
        if (!rb) { p.used = false; continue; }
        const float* cv = RB_GetVec(rb, RB_GetLinearVelocity);
        float nv[3];
        if (HitUtil::SustainPush(cv, p.push, now - p.from, HitUtil::HIT_PUSH_MS, nv))
            RB_SetVec(rb, RB_SetLinearVelocity, nv);
    }
}

static void OnCarHitImpl(uint16_t id, const float* dv) {
    for (auto& h : g_hostIds) {
        if (!h.iv || NetUtil::TrafficWireId(h.id, g_cfg.isHost) != id) continue;
        if (!ListContains(-1, h.iv)) return;
        if (CarHeld(h.iv)) return;   // their physics moves it (PKT_CARHOLD): the push is in it
        void* rb = RigidBodyOf(h.iv);
        if (!rb) return;
        float mag = sqrtf(dv[0] * dv[0] + dv[1] * dv[1] + dv[2] * dv[2]);
        float k = HitUtil::HitPushScale(mag);
        const float* v = RB_GetVec(rb, RB_GetLinearVelocity);
        float push[3] = { dv[0] * k, 0, dv[2] * k };
        float nv[3] = { v[0] + push[0], v[1], v[2] + push[2] };
        RB_SetVec(rb, RB_SetLinearVelocity, nv);
        StartCarPush(h.iv, id, push);
        LOG("Collision: the other player hit our car %u (%s), pushed by %.0f km/h", (unsigned)id, VehName(h.iv), mag * k * 3.6f);
        return;
    }
}

void Game::TrafficTick(bool connected, const Net::Snapshot& peer) {
    if (!g_ok || !g_cfg.trafficSync) return;
    __try {
        TrafficTickImpl(connected, peer);
        if (connected && GameFlowState() == 6) { MirrorHitTick(); ApplyCarPushes(); }
        else if (GameFlowState() == 6) ApplyCarPushes();
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { LOG("Traffic: exception"); }
}

int  Game::TrafficCount() { return g_mirrorCount; }
bool Game::TrafficSuppressed() { return g_suppressTraffic; }

// Race restart: vanilla unloads the pursuit, but our mirrored remote cops (and leftover local
// cops if the unload missed them) stayed in the world with no radar blips. Drop them now.
static void RoadblockOnRestart();   // roadblock.inl
void Game::OnRaceRestart() {
    RoadblockOnRestart();
    int mirrors = 0, local = 0;
    for (auto& m : g_mirrors) {
        if (!m.used) continue;
        bool cop = m.n && (m.s[m.n - 1].e.flags & 1);
        if (cop && !m.racer) { RemoveMirror(m); ++mirrors; }
    }
    void** b = nullptr;
    for (int i = ListSize(VL_AICOPS, &b) - 1; i >= 0; --i) {
        void* c = ListGet(VL_AICOPS, i);
        if (c && IsVehicle(c) && c != g_ghost && !IsMirrorCar(c)) { KillVehicle(c); ++local; }
    }
    if (mirrors || local) LOG("Race: restart - removed %d mirrored cop(s) and %d of our police car(s)", mirrors, local);
    else LOG("Race: restart - no leftover cops to remove");
}

// Test: spawn an AI police car (copmidsize, DRIVER_COP) 25 m ahead of the local player.
// (AICopManager::SpawnPatrolCar refuses near the safe house, so we build one directly.)
static bool DebugSpawnCopImpl(char* out, size_t n) {
    __try {
        void* me = LocalVehicle();
        void* rb = RigidBodyOf(me);
        if (!rb) { strcpy_s(out, n, "no local car"); return false; }
        const float* p = RB_GetVec(rb, RB_GetPosition);
        const float* v = RB_GetVec(rb, RB_GetLinearVelocity);
        float dir[3] = { 1, 0, 0 };
        float sp = sqrtf(v[0] * v[0] + v[2] * v[2]);
        if (sp > 1.f) { dir[0] = v[0] / sp; dir[2] = v[2] / sp; }
        float pos[3] = { p[0] + dir[0] * 25.f, p[1] + 1.f, p[2] + dir[2] * 25.f };
        using HashFn = uint32_t(__cdecl*)(const char*);
        uint32_t key = ((HashFn)0x4519D0)("copmidsize");
        void* iv = SpawnVehicle(key, nullptr, pos, dir, DRV_COP);
        void** b = nullptr;
        sprintf_s(out, n, "police car %s (cops now %d)", iv ? "spawned" : "FAILED", ListSize(VL_AICOPS, &b));
        return iv != nullptr;
    } __except (Game::LogFault("DebugSpawnCop", GetExceptionInformation()), EXCEPTION_EXECUTE_HANDLER) { strcpy_s(out, n, "exception"); return false; }
}
std::string Game::DebugSpawnCop() {
    if (!g_ok || GameFlowState() != 6) return "not in game";
    char buf[96] = "";
    DebugSpawnCopImpl(buf, sizeof(buf));
    LOG("DebugSpawnCop: %s", buf);
    return buf;
}

// Test 'ground': our car and every AI car - physics mode, height above our ground (log), counts (reply).
static bool DebugGroundScan(int& cars, int& emulated, int& noGround) {
    __try {
        void* me = LocalVehicle();
        for (int list : { VL_PLAYERS, VL_AIRACERS, VL_AICOPS, VL_AITRAFFIC }) {
            void** b = nullptr;
            int cnt = ListSize(list, &b);
            for (int i = 0; i < cnt; ++i) {
                void* t = ListGet(list, i);
                void* rb = t ? RigidBodyOf(t) : nullptr;
                if (!rb) continue;
                const float* p = RB_GetVec(rb, RB_GetPosition);
                float gy = 0; bool have = GroundHeightAt(p, gy);
                int mode = VTable(t) == 0x8AA828 ? VCall<int(__thiscall*)(void*)>(t, 14)(t) : -1;
                ++cars; if (mode == 2) ++emulated; if (!have) ++noGround;
                LOG("Ground: %s%s list %d mode %d y %.2f, %s%.2f m above our ground%s", t == me ? "OUR CAR " : "", IsMirrorCar(t) ? "copy " : "",
                    list, mode, p[1], have ? "" : "(no ground) ", have ? p[1] - gy : 0.f, list == VL_PLAYERS && t != me ? " (the other player)" : "");
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}
std::string Game::DebugGround() {
    if (!g_ok || GameFlowState() != 6) return "not in game";
    int cars = 0, emulated = 0, noGround = 0;
    if (!DebugGroundScan(cars, emulated, noGround)) return "exception";
    char r[128];
    sprintf_s(r, "%d car(s): %d in emulated physics, %d without ground under them (details in the log)", cars, emulated, noGround);
    return r;
}

// Test 'bodies': every car in the world - name, model, driver class, the BODY part of its parts record
// and its render connection (FindCarRenderConn 0x744D40: +0x28 state, 2+ = drawn; +0x2C model type).
// For cars drawn without a body (cutscenes).
static void* CarPartByIndex(int16_t index);   // paint.inl
static int DebugBodiesScan() {
    int n = 0;
    __try {
        void* me = LocalVehicle();
        void** b = nullptr;
        int cnt = ListSize(VL_ALL, &b);
        for (int i = 0; i < cnt; ++i) {
            void* t = ListGet(VL_ALL, i);
            if (!t || !IsVehicle(t)) continue;
            int16_t body = -1; const void* rec = Customizations(t);
            if (rec) Mem::SafeRead((const uint8_t*)rec + 23 * 2, &body, sizeof(body));
            int st = -9, type = -9;
            void* sim = VCall<void* (__thiscall*)(void*)>(t, IV_GetSimable)(t);
            if (sim) {
                uint32_t wid = VCall<uint32_t(__thiscall*)(void*)>(sim, 28)(sim);
                char* conn = ((char*(__cdecl*)(uint32_t))0x744D40)(wid);
                if (conn) { st = *(int*)(conn + 0x28); type = *(int*)(conn + 0x2C); }
            }
            ++n;
            LOG("Bodies: %p %s%s model %08X class %d, parts %s, BODY %d (%s), render state %d type %d", t, t == me ? "OUR CAR " : t == g_ghost ? "THE OTHER PLAYER " : "",
                VehName(t), ModelKeyOf(t), DriverClass(t), rec ? "yes" : "NONE", body, body >= 0 ? (CarPartByIndex(body) ? "found" : "NOT FOUND") : "-", st, type);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    return n;
}
std::string Game::DebugBodies() {
    if (!g_ok || GameFlowState() != 6) return "not in game";
    int n = DebugBodiesScan();
    if (n < 0) return "exception";
    return std::to_string(n) + " car(s) (details in the log)";
}