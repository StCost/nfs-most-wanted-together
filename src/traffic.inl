// Included from game.cpp (uses its vehicle helpers).
// ---------------------------------------------------------------------------
// Traffic sync
//  Host: owns traffic. ~20x/s sends its traffic cars nearest to the client (when the client is near).
//  Client: near the host (enter 350 m / leave 450 m) it zeroes its own traffic density (hook on the
//  ComputeDensity call at 0x439BBD), removes its own traffic cars and spawns "mirror" cars that copy
//  the host's traffic (no AI: hard-set, interpolated transforms). Far from the host it keeps its own.
// ---------------------------------------------------------------------------
namespace {
    constexpr uintptr_t DensityCallSite  = 0x439BBD;   // call AITrafficManager::ComputeDensity (0x41EC20)
    constexpr uintptr_t ComputeDensityFn = 0x41EC20;   // returns the traffic density (float, ST0)
    bool  g_suppressTraffic = false;   // also suppresses the client's own cop spawns

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

    float __fastcall hk_ComputeDensity(void* self, void* edx) {
        using Fn = float(__fastcall*)(void*, void*);
        float d = ((Fn)ComputeDensityFn)(self, edx);
        return g_suppressTraffic ? 0.0f : d;
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
        bool     racer = false; // a race opponent of the host: drives one of our own racers (no spawn)
        char     name[31] = {};  // its display name on the host (race opponents)
    };
    Mirror g_mirrors[48];
    bool   g_trafficNear = false;
    int    g_mirrorCount = 0, g_killedOwn = 0;

    Mirror* FindMirror(uint16_t id) {
        for (auto& m : g_mirrors) if (m.used && m.id == id) return &m;
        return nullptr;
    }
    bool IsMirrorCar(void* iv) {
        for (auto& m : g_mirrors) if (m.used && m.iv == iv) return true;
        return false;
    }

    constexpr uint16_t CLIENT_ID_BIT = 0x8000;   // ids of cars owned by the client (its pursuit cops)

    double g_lastTrafficPkt = 0;   // local ms of the owner's last traffic packet

    void OnTrafficPacket(const TrafficEntry* e, int count, double sendMs) {
        double now = Net::LocalClockMs();
        g_lastTrafficPkt = now;
        for (int i = 0; i < count; ++i) {
            // the host accepts only the client's own cops; the client accepts the host's cars
            if (g_cfg.isHost != ((e[i].id & CLIENT_ID_BIT) != 0)) continue;
            bool racer = (e[i].flags & 2) != 0;
            if (!g_cfg.isHost && !racer && !g_trafficNear) continue;   // far from the host: own traffic
            Mirror* m = FindMirror(e[i].id);
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

    // GRacerInfo of a car in the current race (GRaceStatus::GetRacerInfo(ISimable*)), or null
    void* RacerInfoOf(void* iv) {
        void* rs = nullptr;
        if (!IsVehicle(iv) || !Mem::Read(0x91E000, rs) || !rs) return nullptr;
        void* sim = VCall<void* (__thiscall*)(void*)>(iv, IV_GetSimable)(iv);
        return sim ? ((void*(__thiscall*)(void*, void*))0x5E8B20)(rs, sim) : nullptr;
    }
    void OnRacerName(uint16_t id, const char* name) {
        if (Mirror* m = FindMirror(id)) strncpy_s(m->name, 16, name, _TRUNCATE);   // HUD slots are 16 bytes
    }

    void RemoveMirror(Mirror& m) {
        if (m.racer) { if (m.iv) RestoreAI(m.iv); }          // our own racer: give it its AI back
        else if (m.iv && ListContains(-1, m.iv)) KillVehicle(m.iv);
        m = Mirror{};
    }

    void DriveMirror(Mirror& m, double renderSendMs) {
        if (!m.iv || m.n == 0) return;
        void* rb = RigidBodyOf(m.iv);
        if (!rb) return;
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
        RB_SetVec(rb, RB_SetOrientationQuat, &q);
        RB_SetVec(rb, RB_SetPosition, pos);
        RB_SetVec(rb, RB_SetLinearVelocity, vel);
        RB_SetVec(rb, RB_SetAngularVelocity, av);
    }

    // host: stable ids for its traffic cars
    struct HostId { void* iv; uint16_t id; };
    HostId   g_hostIds[64];
    uint16_t g_nextHostId = 1;
    uint16_t HostIdFor(void* iv) {
        for (auto& h : g_hostIds) if (h.iv == iv) return g_cfg.isHost ? h.id : (uint16_t)(h.id | CLIENT_ID_BIT);
        for (auto& h : g_hostIds) {
            if (!h.iv || !ListContains(-1, h.iv)) {
                h.iv = iv; h.id = g_nextHostId++;
                if (g_nextHostId >= CLIENT_ID_BIT) g_nextHostId = 1;
                return g_cfg.isHost ? h.id : (uint16_t)(h.id | CLIENT_ID_BIT);
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
}

bool Game::InstallTrafficHooks() {
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
    Net::OnTraffic = &OnTrafficPacket;
    Net::OnRacerName = &OnRacerName;
    LOG("Traffic: hooked ComputeDensity call (client can suppress its own traffic)");
    return true;
}

// Spawn / drive / expire the mirror cars (both sides: the client mirrors the host's traffic and
// cops, the host mirrors the client's own pursuit cops).
static int MaintainMirrors(double now) {
    double renderSend = now - Net::PeerClockOffsetMs() - g_cfg.interpDelayMs;
    int spawned = 0, alive = 0, freed = 0;
    // The other player's own car comes first: no mirror spawns until it exists, and when its spawn
    // failed (vehicle budget full) give back a couple of mirror cars.
    bool mayspawn = g_ghost && !g_ghostStarved;
    for (auto& m : g_mirrors) {
        if (!m.used) continue;
        // the owner keeps sending, just not this car: it's gone there. (If the owner sends nothing at all,
        // its game is paused - alt-tab, map, loading: keep every mirror frozen where it is.)
        if (g_lastTrafficPkt - m.lastSeen > 1500) { RemoveMirror(m); continue; }
        if (m.iv && !ListContains(-1, m.iv)) m.iv = nullptr;             // the game removed it
        if (m.racer) {
            // bind to one of our own AI racers of the same model, switch its AI off and drive it
            if (!m.iv && m.n) {
                void** b = nullptr;
                int cnt = ListSize(VL_AIRACERS, &b);
                for (int i = 0; i < cnt; ++i) {
                    void* r = ListGet(VL_AIRACERS, i);
                    if (!r || r == g_ghost || IsMirrorCar(r) || !RigidBodyOf(r) || ModelKeyOf(r) != m.model) continue;
                    m.iv = r;
                    DisableAI(r);
                    LOG("Race: our racer %p (%s) now follows the host's opponent %u", r, VehName(r), m.id);
                    break;
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
        if (g_ghostStarved && m.iv && freed < 2) { KillVehicle(m.iv); m.iv = nullptr; ++freed; continue; }
        if (!m.iv && m.n && mayspawn && spawned < 2 && m.fails < 3) {
            const TrafficEntry& e = m.s[m.n - 1].e;
            // never create a car inside a player (it would fling them): wait until it has moved on
            void* lrb = RigidBodyOf(LocalVehicle());
            void* grb = RigidBodyOf(g_ghost);
            if ((lrb && Dist(RB_GetVec(lrb, RB_GetPosition), e.pos) < 6.f) ||
                (grb && Dist(RB_GetVec(grb, RB_GetPosition), e.pos) < 6.f)) continue;
            float dir[3] = { 1, 0, 0 };
            m.iv = SpawnVehicle(e.model, nullptr, e.pos, dir, g_cfg.trafficMirrorClass);
            ++spawned;
            if (!m.iv) { ++m.fails; LOG("Traffic: mirror spawn of %08X failed (%d)", e.model, m.fails); continue; }
        }
        if (m.iv) {
            DriveMirror(m, renderSend);
            GlareBits(m.iv) = m.s[m.n - 1].e.glare;   // lights: brake/head/signals/cop light bar
            ++alive;
        }
    }
    if (freed) LOG("Traffic: removed %d mirror cars to make room for the other player's car", freed);
    g_ghostStarved = false;
    return alive;
}

// Collect our own (non-mirror) cars of the given lists, nearest to `to` first, into a packet.
static int CollectOwned(std::initializer_list<int> lists, const float* to, TrafficEntry* out, int maxOut) {
    struct C { void* iv; float d; uint8_t flags; } cand[64]; int nc = 0;
    void* me = LocalVehicle();
    for (int list : lists) {
        void** b = nullptr;
        int cnt = ListSize(list, &b);
        for (int i = 0; i < cnt && nc < 64; ++i) {
            void* t = ListGet(list, i);
            if (!t || t == g_ghost || t == me || IsMirrorCar(t)) continue;
            void* rb = RigidBodyOf(t);
            if (!rb) continue;
            cand[nc].iv = t;
            // race opponents first, then cops, then traffic (nearest first within each)
            cand[nc].d = Dist(RB_GetVec(rb, RB_GetPosition), to) - (list == VL_AIRACERS ? 20000.f : list == VL_AICOPS ? 10000.f : 0.f);
            cand[nc].flags = list == VL_AIRACERS ? 2 : list == VL_AICOPS ? 1 : 0;
            ++nc;
        }
    }
    for (int i = 0; i < nc; ++i)
        for (int j = i + 1; j < nc; ++j)
            if (cand[j].d < cand[i].d) { C x = cand[i]; cand[i] = cand[j]; cand[j] = x; }
    int n = 0;
    for (int i = 0; i < nc && n < maxOut; ++i) {
        void* rb = RigidBodyOf(cand[i].iv);
        TrafficEntry& e = out[n];
        e.id = HostIdFor(cand[i].iv);
        if (!e.id) continue;
        e.flags = cand[i].flags;
        e.model = ModelKeyOf(cand[i].iv);
        e.glare = GlareBits(cand[i].iv);
        memcpy(e.pos, RB_GetVec(rb, RB_GetPosition), 12);
        memcpy(e.rot, RB_GetVec(rb, RB_GetOrientation), 16);
        memcpy(e.linVel, RB_GetVec(rb, RB_GetLinearVelocity), 12);
        memcpy(e.angVel, RB_GetVec(rb, RB_GetAngularVelocity), 12);
        ++n;
    }
    return n;
}

static int g_sentCount = 0;

static void TrafficTickImpl(bool connected, const Net::Snapshot& peer) {
    double now = Net::LocalClockMs();
    void* me = LocalVehicle();
    void* myRb = RigidBodyOf(me);
    if (Game::GameFlowState() != 6 || !myRb || !connected || !peer.valid || !peer.s.hasCar) {
        // not in a shared world: clean up and hand traffic back to the game
        if (g_suppressTraffic) LOG("Traffic: released (not connected / not in game)");
        g_suppressTraffic = false;
        g_trafficNear = false;
        for (auto& m : g_mirrors) if (m.used) RemoveMirror(m);
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
            g_sentCount = distToPeer <= 450.f
                ? CollectOwned({ VL_AIRACERS, VL_AICOPS, VL_AITRAFFIC }, peer.s.pos, out, MAX_TRAFFIC_PER_PACKET)
                : CollectOwned({ VL_AIRACERS }, peer.s.pos, out, MAX_TRAFFIC_PER_PACKET);
            Net::SendTraffic(out, g_sentCount, Game::SimTimeSec());
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
                if (!r || r == g_ghost || IsMirrorCar(r)) continue;
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
    if (isNear != g_trafficNear) {
        g_trafficNear = isNear;
        LOG("Traffic: %s the host (%.0f m) - %s", isNear ? "near" : "away from", distToPeer,
            isNear ? "mirroring host traffic" : "using own traffic");
        if (!isNear) for (auto& m : g_mirrors) if (m.used && !m.racer) RemoveMirror(m);
    }
    g_suppressTraffic = isNear;   // no own traffic / patrol spawns near the host (pursuit spawns stay)
    if (!isNear) { g_sentCount = 0; g_mirrorCount = MaintainMirrors(now); return; }   // race opponents only

    // remove our own (non-mirror) traffic cars, a few per frame. Our own cops stay: they belong to
    // our pursuit and are sent to the host.
    {
        int killed = 0;
        void** b = nullptr;
        int cnt = ListSize(VL_AITRAFFIC, &b);
        for (int i = cnt - 1; i >= 0 && killed < 3; --i) {
            void* t = ListGet(VL_AITRAFFIC, i);
            if (t && t != g_ghost && !IsMirrorCar(t) && IsVehicle(t)) { KillVehicle(t); ++killed; ++g_killedOwn; }
        }
    }
    if (sendNow) {
        TrafficEntry out[MAX_TRAFFIC_PER_PACKET];
        g_sentCount = CollectOwned({ VL_AICOPS }, peer.s.pos, out, 8);
        Net::SendTraffic(out, g_sentCount, Game::SimTimeSec());   // also when empty: tells the host our cops are gone
    }
    g_mirrorCount = MaintainMirrors(now);
}

void Game::TrafficTick(bool connected, const Net::Snapshot& peer) {
    if (!g_ok || !g_cfg.trafficSync) return;
    __try { TrafficTickImpl(connected, peer); }
    __except (EXCEPTION_EXECUTE_HANDLER) { LOG("Traffic: exception"); }
}

int  Game::TrafficCount() { return g_mirrorCount; }
bool Game::TrafficSuppressed() { return g_suppressTraffic; }

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
    } __except (EXCEPTION_EXECUTE_HANDLER) { strcpy_s(out, n, "exception"); return false; }
}
std::string Game::DebugSpawnCop() {
    if (!g_ok || GameFlowState() != 6) return "not in game";
    char buf[96] = "";
    DebugSpawnCopImpl(buf, sizeof(buf));
    LOG("DebugSpawnCop: %s", buf);
    return buf;
}
