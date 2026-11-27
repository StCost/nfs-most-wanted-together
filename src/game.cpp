#include "game.h"
#include <intrin.h>
#include "netutil.h"
#include "pursuitutil.h"
#include "milestoneutil.h"
#include "raceutil.h"
#include "joinutil.h"
#include "profileutil.h"
#include "prologueutil.h"
#include "rivalgateutil.h"
#include "groundutil.h"
#include "hitutil.h"
#include "noticeutil.h"
#include "paintutil.h"
#include "feutil.h"
#include "exitutil.h"
#include "storyutil.h"
#include "togetherutil.h"
#include "opponentsutil.h"
#include "mem.h"
#include "log.h"
#include "config.h"
#include "input.h"
#include "overlay.h"
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <shlobj.h>
#include <vector>
#include <deque>
#include <string>
#include <share.h>

// ---------------------------------------------------------------------------
// speed.exe v1.3 addresses. Each one was checked against the exe's own code:
//  * game flow:   `cmp dword ptr [0x925E90], 6` appears ~150 times
//  * UCOM Find:   0x5D59F0 (thiscall Object*, handle); handles are functions that
//                 `mov eax, <own address>; ret` (0x4039F0 / 0x4039E0 / 0x4040E0)
//  * vtables:     taken from the constructors that register each interface
//                 (RBVehicle ctor @0x6B7B1A, PVehicle ctor @0x689072)
//  * vtable slot meaning: disassembled (e.g. slot 23 copies 3 floats to +0x10)
// Reference: nya-common-nfsmw / MostWantedTimeTrialGhosts (MPL-2.0), ExOpts.
// ---------------------------------------------------------------------------
namespace Addr {
    constexpr uintptr_t EntryPoint     = 0x7C4040;
    constexpr uintptr_t GameFlowState  = 0x925E90; // 3 = front end, 4/5 = loading, 6 = in game
    constexpr uintptr_t VehicleLists   = 0x92CD18; // UTL::ListableSet<IVehicle,20,eVehicleList,10>
    constexpr uint32_t  VehicleBucket  = 0x60;     // FixedVector: vt, mBegin, mCap, mSize, slots[20]
    constexpr uintptr_t UCOM_Find      = 0x5D59F0;

    constexpr uint32_t  H_IRigidBody   = 0x4039F0;
    constexpr uint32_t  H_IRBVehicle   = 0x4039E0;

    // Expected vtables (runtime sanity checks - we refuse to touch anything else)
    constexpr uintptr_t VT_PVehicle_IVehicle  = 0x8AA828;
    constexpr uintptr_t VT_RBVehicle_IRigid   = 0x8AC880;
    constexpr uintptr_t VT_Simple_IRigid      = 0x8AC548; // SimpleRigidBody: same slots 8-10,17,23-25,29 (verified)
    constexpr uintptr_t VT_RBVehicle_IRBVeh   = 0x8AC990;

    // AIVehicleRacecar::Update pointer slot; swapping in AIVehicleEmpty's update stops racer AI.
    constexpr uintptr_t RacerAIUpdateSlot = 0x892748;
    constexpr uintptr_t RacerAIUpdate     = 0x42AA80;
    constexpr uintptr_t EmptyAIUpdate     = 0x43CE60;

    // Vehicle creation. The game registers PVehicle::Construct (0x689820) as the "PVehicle" factory
    // (writes it to 0x92C534 at startup). Construct takes a Sim::Param by value and checks:
    //   param.mType == *VehicleParamsType(&out)  (UCrc32 of "VehicleParams", from 0x4040F0)
    //   param.mName == 0xA6B47FAC                ("BASE")
    // then reads VehicleParams at param.mData (+0x10 class, +0x14 model key, +0x18 dir*, +0x1C pos*,
    // +0x20 customization*, +0x24 cache, +0x28 matched perf, +0x2C flags).
    constexpr uintptr_t PVehicleConstruct = 0x689820;
    constexpr uintptr_t VehicleParamsType = 0x4040F0;
    constexpr uint32_t  ParamNameBase     = 0xA6B47FAC;
    constexpr uintptr_t VT_PVehicle_ISimable = 0x8AA940; // slot 2 = Kill (0x6851D0, deferred delete)
}

enum VehicleList { VL_ALL = 0, VL_PLAYERS = 1, VL_AI = 2, VL_AIRACERS = 3, VL_AICOPS = 4, VL_AITRAFFIC = 5 };

// IRigidBody vtable slots (verified)
enum RB {
    RB_GetPosition = 8, RB_GetLinearVelocity = 9, RB_GetAngularVelocity = 10,
    RB_GetOrientation = 17, // quaternion, Vector4
    RB_GetDimension = 18,   // half extents, local X/Y/Z (IRigidBody::GetDimension)
    RB_SetPosition = 23, RB_SetLinearVelocity = 24, RB_SetAngularVelocity = 25,
    RB_SetOrientationQuat = 29,
};
// IVehicle slots
enum IV { IV_GetSimable = 1, IV_IsSpooled = 16, IV_GetVehicleName = 19, IV_GetVehicleKey = 20,
          IV_SetDriverClass = 21, IV_GetDriverClass = 22, IV_IsLoading = 23, IV_GetCustomizations = 50 };
enum DriverClassE { DRV_HUMAN = 0, DRV_TRAFFIC = 1, DRV_COP = 2, DRV_RACER = 3, DRV_NONE = 4, DRV_NIS = 5, DRV_REMOTE = 6 };
// IRBVehicle slots
enum IRBV { IRBV_EnableObjectCollisions = 3 };

namespace {
    bool g_ok = false;
    bool g_coopLinked = true;   // co-op is active right now: connected, and both players are past the prologue

    struct Vec3 { float x, y, z; };
    struct Quat { float x, y, z, w; };

    // ---- raw helpers ----
    template <typename T> T VCall(void* obj, int slot) {
        auto fn = (*(T**)obj)[slot];
        return fn;
    }
    uintptr_t VTable(void* obj) { uintptr_t vt = 0; Mem::Read((uintptr_t)obj, vt); return vt; }

    void* Find(void* iface, uint32_t handle) {
        // iface is a UCOM::IUnknown: [0]=vtable, [4]=Object*
        void* object = nullptr;
        if (!Mem::Read((uintptr_t)iface + 4, object) || !object) return nullptr;
        using Fn = void* (__thiscall*)(void*, uint32_t);
        return ((Fn)Addr::UCOM_Find)(object, handle);
    }

    int ListSize(int list, void*** begin) {
        uintptr_t bucket = Addr::VehicleLists + list * Addr::VehicleBucket;
        void** b = nullptr; uint32_t size = 0;
        if (!Mem::Read(bucket + 4, b) || !Mem::Read(bucket + 0xC, size)) return 0;
        if (!b || size > 64) return 0;
        *begin = b;
        return (int)size;
    }

    void* ListGet(int list, int i) {
        void** b = nullptr;
        int n = ListSize(list, &b);
        if (i < 0 || i >= n) return nullptr;
        void* v = nullptr;
        Mem::Read((uintptr_t)&b[i], v);
        return v;
    }

    bool ListContainsOne(int list, void* v);
    // list == -1: search every vehicle list (used for cars we spawned ourselves)
    bool ListContains(int list, void* v) {
        if (list < 0) {
            for (int l = 0; l < 10; ++l) if (ListContainsOne(l, v)) return true;
            return false;
        }
        return ListContainsOne(list, v);
    }
    bool ListContainsOne(int list, void* v) {
        void** b = nullptr;
        int n = ListSize(list, &b);
        for (int i = 0; i < n; ++i) {
            void* x = nullptr;
            if (Mem::Read((uintptr_t)&b[i], x) && x == v) return true;
        }
        return false;
    }

    bool IsVehicle(void* iv) { return iv && VTable(iv) == Addr::VT_PVehicle_IVehicle; }

    // Accept any IRigidBody whose slots we use point to the exact functions verified in the exe
    // (RBVehicle 0x8AC880 and the remote-player body 0x8AD178 share them).
    bool RigidVtableMatches(uintptr_t vt) {
        static const struct { int slot; uintptr_t fn; } k[] = {
            {RB_GetPosition, 0x671180}, {RB_GetLinearVelocity, 0x671190}, {RB_GetAngularVelocity, 0x6711A0},
            {RB_GetOrientation, 0x671320}, {RB_SetPosition, 0x696FB0}, {RB_SetLinearVelocity, 0x696FD0},
            {RB_SetAngularVelocity, 0x696FF0}, {RB_SetOrientationQuat, 0x696E00} };
        for (auto& e : k) {
            uintptr_t f = 0;
            if (!Mem::Read(vt + e.slot * 4, f) || f != e.fn) return false;
        }
        return true;
    }

    void* RigidBodyOf(void* iv) {
        if (!IsVehicle(iv)) return nullptr;
        void* rb = Find(iv, Addr::H_IRigidBody);
        if (!rb) return nullptr;
        uintptr_t vt = VTable(rb);
        if (vt != Addr::VT_RBVehicle_IRigid && vt != Addr::VT_Simple_IRigid && !RigidVtableMatches(vt)) return nullptr;
        return rb;
    }

    void* RBVehicleOf(void* iv) {
        if (!IsVehicle(iv)) return nullptr;
        void* r = Find(iv, Addr::H_IRBVehicle);
        if (!r) return nullptr;
        // accept any IRBVehicle whose EnableObjectCollisions slot is the verified function (0x6B7CA0)
        uintptr_t f = 0;
        if (VTable(r) != Addr::VT_RBVehicle_IRBVeh && (!Mem::Read(VTable(r) + IRBV_EnableObjectCollisions * 4, f) || f != 0x6B7CA0)) return nullptr;
        return r;
    }

    const float* RB_GetVec(void* rb, int slot) {
        using Fn = const float* (__thiscall*)(void*);
        return VCall<Fn>(rb, slot)(rb);
    }
    void RB_SetVec(void* rb, int slot, const void* v) {
        using Fn = void(__thiscall*)(void*, const void*);
        VCall<Fn>(rb, slot)(rb, v);
    }
    const char* VehName(void* iv) {
        using Fn = const char* (__thiscall*)(void*);
        const char* n = VCall<Fn>(iv, IV_GetVehicleName)(iv);
        return (n && Mem::IsReadable(n, 1)) ? n : "?";
    }
    int DriverClass(void* iv) {
        using Fn = int(__thiscall*)(void*);
        return VCall<Fn>(iv, IV_GetDriverClass)(iv);
    }
    void SetCollisions(void* iv, bool on) {
        void* r = RBVehicleOf(iv);
        if (!r) return;
        using Fn = void(__thiscall*)(void*, bool);
        VCall<Fn>(r, IRBV_EnableObjectCollisions)(r, on);
    }

    // IRigidBody::GetDimension (slot 18): the collision box's half-extents in local X/Y/Z.
    // If the call fails, a typical car-sized box is used (HitUtil::kDefaultHalfExtent).
    void ReadHalfExtents(void* rb, float out[3]) {
        out[0] = HitUtil::kDefaultHalfExtent[0];
        out[1] = HitUtil::kDefaultHalfExtent[1];
        out[2] = HitUtil::kDefaultHalfExtent[2];
        if (!rb) return;
        __try {
            float d[3] = {};
            VCall<void(__thiscall*)(void*, float*)>(rb, RB_GetDimension)(rb, d);
            if (HitUtil::ValidHalfExtents(d)) { out[0] = d[0]; out[1] = d[1]; out[2] = d[2]; }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    // IRBVehicle slot 4 SetInvulnerability(type, seconds) (0x6B7CB0 on RBVehicle: stores +0x44/+0x48).
    // INVULNERABLE_FROM_RESET (2) is the game's post-reset state: the car blinks and ghosts through
    // other cars. On the remote-player body class this slot is an empty stub (harmless).
    void SetInvulnerable(void* iv, int type, float seconds) {
        void* r = RBVehicleOf(iv);
        if (!r) return;
        using Fn = void(__thiscall*)(void*, int, float);
        VCall<Fn>(r, 4)(r, type, seconds);
    }

    void* LocalVehicle() {
        // The players list also holds remote-player cars we spawned (DRIVER_REMOTE): pick the human one.
        void** b = nullptr;
        int n = ListSize(VL_PLAYERS, &b);
        for (int i = 0; i < n; ++i) {
            void* iv = ListGet(VL_PLAYERS, i);
            if (IsVehicle(iv) && DriverClass(iv) == 0 /*DRIVER_HUMAN*/) return iv;
        }
        return nullptr;
    }

    // ---- ghost state ----
    void*  g_ghost = nullptr;       // IVehicle* we are puppeting (an AI racer, or our spawned peer car)
    float  g_push[3] = { 0, 0, 0 };   // velocity we got from being hit by the other player
    DWORD  g_pushFrom = 0;
    DWORD  g_lastHitSent = 0, g_hitT0 = 0;   // our own impact being measured / last one sent
    DWORD  g_noHitUntil = 0;                 // teleport / restart / cutscene: a velocity jump is not an impact
    DWORD  g_testJumpOkUntil = 0;            // the `ram` test command teleports on purpose: don't treat that jump as one
    // lost velocity must point at the car we supposedly hit (horizontal plane)
    bool HitPointsAt(const float* dv, const float* from, const float* to) {
        float dx = to[0] - from[0], dz = to[2] - from[2];
        float dl = sqrtf(dx * dx + dz * dz), vl = sqrtf(dv[0] * dv[0] + dv[2] * dv[2]);
        return dl > 0.01f && vl > 0.01f && (dv[0] * dx + dv[2] * dz) > 0.5f * dl * vl;
    }
    bool   g_ghostStarved = false;
    DWORD  g_noSpawnUntil = 0;      // an event is starting: the race builds the peer's car, don't spawn one  // spawning the peer car failed: traffic mirrors give up budget
    int    g_ghostList = VL_AIRACERS; // which vehicle list g_ghost came from (VL_ALL for our spawned car)
    enum GhostKind { GK_NONE, GK_RACER, GK_SPAWNED, GK_TRAFFIC };
    GhostKind g_ghostKind = GK_NONE;
    uint32_t g_spawnedKey = 0, g_spawnedHash = 0; // which peer car g_ghost was spawned as
    int      g_spawnFails = 0;
    DWORD    g_nextSpawnTry = 0;            // backoff between spawn attempts
    void*    g_replacedRacer = nullptr;            // racer built as the peer's car by the factory hook
    bool     g_racerReplacedThisLoad = false;
    DWORD    g_ghostYieldUntil = 0;            // event start: our copy of their car may be removed for room
    float    g_peerSlot[3] = {};                    // where the race built that racer (its grid slot)
    bool     g_peerSlotValid = false;
    float    g_clientSlot[3] = {};                  // host: where the client starts in this event (HostGridSlotTick)
    uint32_t g_clientSlotEvent = 0;                 //   ... for this event (0 = not decided)
    bool     g_clientSlotRacer = false;             //   ... the slot of the client's racer (else a free one)
    uint32_t g_origRacer[1] = {};
    void*    g_origRacerCustom = nullptr;
    void*    g_origRacerMatched = nullptr;
    uint8_t  g_peerCustomForRace[0x198];
    // Race rosters: each game picks its AI opponents at random, so the game that starts an event sends
    // the opponents it built (model + tuning, in construction order) and the other game builds the same.
    // role: CoopRace::RacerRole (rival / helper / normal) the sender marked (kRoleUnknown from 1.7.9 and older)
    struct RosterEntry { uint32_t key; bool hasCustom; char name[24]; uint8_t custom[0x198]; int role; };
    RosterEntry g_localRoster[8];  int g_localRosterN = 0;     // AI racers built here since the last event
    RosterEntry g_peerRoster[8];   int g_peerRosterN = 0;      // received from the peer
    DWORD    g_peerRosterAt = 0;
    bool     g_usePeerRoster = false;  int g_peerRosterNext = 0;
    bool     PeerRosterFresh() { return g_peerRosterN > 0 && GetTickCount() - g_peerRosterAt < 5000; }
    uint32_t g_joiningEvent = 0;             // event hash we're joining because the peer started it (event.inl)
    DWORD    g_joinStartedAt = 0;
    bool   g_aiPatched = false;
    bool   g_ghostCollide = false;  // collisions currently enabled on the ghost
    DWORD  g_hitContactAt = 0;      // last time the ghost was next to us (local contact for applying a received hit)
    char   g_ghostName[48] = "";
    float  g_jitterMm = 0, g_extrapMs = 0;
    bool   g_protectHoldReset = false;  // set when a new ghost is acquired, a cutscene runs, or a race starts: start a 3 s hold
    bool   g_protectHolding = false;    // collisions stay off during the hold so a late start-teleport can still land
    LARGE_INTEGER g_qpf{}, g_lastQpc{};

    // Switch off the racing AI of ONE car (the ones we drive from the network), leaving every other
    // racer alone: give that car's AI object a private copy of its vtable with the update slot
    // (0x892748 in the shared vtable) pointing to an empty function.
    struct AIOverride { void* iv; void* ai; uint32_t* field; uint32_t origVt; uint32_t copy[0x200]; };
    AIOverride g_aiOv[12];

    bool DisableAI(void* iv) {
        if (!IsVehicle(iv)) return false;
        for (auto& o : g_aiOv) if (o.iv && !ListContains(-1, o.iv)) o.iv = nullptr;   // cars that are gone
        void* ai = VCall<void* (__thiscall*)(void*)>(iv, 0xAC / 4)(iv);   // GetAIVehiclePtr
        if (!ai) return false;
        for (auto& o : g_aiOv) {
            if (o.iv != iv) continue;
            if (o.ai == ai && *o.field == (uint32_t)&o.copy[1]) return true;
            // the game built a new car at this address (next race without a world load): its AI is new
            LOG("AI: %p is a new car with a new AI - switching that one off", iv);
            o.iv = nullptr;
        }
        // find the field holding the racer vtable (a pointer a little below the update slot)
        uint32_t* field = nullptr; uint32_t vt = 0;
        for (int off = -0x100; off <= 0x100; off += 4) {
            uint32_t v = 0;
            if (!Mem::Read((uintptr_t)ai + off, v)) continue;
            if (v > Addr::RacerAIUpdateSlot || v + 0x600 < Addr::RacerAIUpdateSlot || (v & 3)) continue;
            uint32_t first = 0, slot = 0;
            if (!Mem::Read(v, first) || first < 0x401000 || first > 0x880000) continue;
            if (!Mem::Read(Addr::RacerAIUpdateSlot, slot) || slot != Addr::RacerAIUpdate) continue;
            if (v > vt) { vt = v; field = (uint32_t*)((uintptr_t)ai + off); }
        }
        if (!field) { LOG("AI: no racer vtable found on %p", iv); return false; }
        for (auto& o : g_aiOv) {
            if (o.iv) continue;
            o.iv = iv; o.ai = ai; o.field = field; o.origVt = vt;
            Mem::SafeRead((void*)(vt - 4), o.copy, sizeof(o.copy));        // RTTI pointer + entries
            o.copy[1 + (Addr::RacerAIUpdateSlot - vt) / 4] = Addr::EmptyAIUpdate;
            *field = (uint32_t)&o.copy[1];
            LOG("AI: disabled racing AI of %p (vtable %08X)", iv, vt);
            return true;
        }
        return false;
    }
    void RestoreAI(void* iv) {
        for (auto& o : g_aiOv) {
            if (!o.iv || (iv && o.iv != iv)) continue;
            if (Game::GameFlowState() == 6 && ListContains(-1, o.iv)) __try { if (*o.field == (uint32_t)&o.copy[1]) *o.field = o.origVt; } __except (EXCEPTION_EXECUTE_HANDLER) {}
            o.iv = nullptr;
        }
    }
    // A cutscene (NIS: race intro, arrest, story scene) is running: the game animates the cars itself.
    // INIS::mInstance 0x9885C8, state dword +0x2AC (3 loaded, 4/6 pre/post movie, 5 playing, 7 skipped)
    bool NisActive() {
        void* nis = nullptr; int st = 0;
        if (!Mem::Read(0x9885C8, nis) || !nis || !Mem::Read((uintptr_t)nis + 0x2AC, st)) return false;
        return st >= 3 && st <= 6;
    }    void PatchRacerAI(bool disable) {   // the ghost racer only
        if (disable) { if (g_ghost) DisableAI(g_ghost); }
        else if (g_ghost) RestoreAI(g_ghost);
        g_aiPatched = disable;
    }

    Quat Nlerp(Quat a, Quat b, float t) {
        float dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
        if (dot < 0) { b.x = -b.x; b.y = -b.y; b.z = -b.z; b.w = -b.w; }
        Quat r{ a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t };
        float len = sqrtf(r.x * r.x + r.y * r.y + r.z * r.z + r.w * r.w);
        if (len < 1e-6f) return b;
        r.x /= len; r.y /= len; r.z /= len; r.w /= len;
        return r;
    }

    float Dist(const float* a, const float* b) {
        float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
        return sqrtf(dx * dx + dy * dy + dz * dz);
    }
}

static double SimClockMs();

bool Game::Init() {
    uintptr_t base = (uintptr_t)GetModuleHandleA(nullptr);
    uint32_t peOff = 0, ep = 0;
    if (!Mem::Read(base + 0x3C, peOff) || !Mem::Read(base + peOff + 0x28, ep) || base + ep != Addr::EntryPoint) {
        LOG("Game::Init: unexpected speed.exe (entry %08X) - game features disabled", (unsigned)(base + ep));
        return false;
    }
    uintptr_t slot = 0;
    Mem::Read(Addr::RacerAIUpdateSlot, slot);
    if (slot != Addr::RacerAIUpdate) {
        LOG("Game::Init: racer AI slot holds %08X, expected %08X (another mod?) - ghost disabled", (unsigned)slot, (unsigned)Addr::RacerAIUpdate);
        return false;
    }
    QueryPerformanceFrequency(&g_qpf);
    QueryPerformanceCounter(&g_lastQpc);
    g_ok = true;
    LOG("Game::Init: speed.exe v1.3 detected");
    InstallVehicleFactoryHook();
    Net::LocalClockMs = &SimClockMs;
    return true;
}

int Game::GameFlowState() {
    int s = -1;
    if (!Mem::Read(Addr::GameFlowState, s)) return -1;
    return s;
}

static double SimClockMs() {
    float t = 0;
    if (!Mem::Read(0x9885D8, t)) return Net::NowMs();   // Sim::GetTime() reads this float (seconds)
    return (double)t * 1000.0;
}
float Game::SimTimeSec() { float t = 0; Mem::Read(0x9885D8, t); return t; }

static void* RacerInfoOfLocal() {
    void* rs = nullptr;
    if (!Mem::Read(0x91E000, rs) || !rs) return nullptr;
    void* iv = LocalVehicle();
    if (!IsVehicle(iv)) return nullptr;
    void* sim = VCall<void* (__thiscall*)(void*)>(iv, IV_GetSimable)(iv);
    return sim ? ((void*(__thiscall*)(void*, void*))0x5E8B20)(rs, sim) : nullptr;
}

// Test: hand the local car to the game's racer AI and back (IHumanAI::SetAiControl, the same call the
// game uses after the finish line). In a race it drives the route, in free roam it cruises the roads.
static bool g_apByUs = false;      // the autopilot was switched on by the mod (F5 / AutoPilot), not by the game
static bool g_apUserOff = false;   // the player switched the automatic autopilot off (until the next world/race)
static bool AutopilotIsOn(void* hai) { return VCall<bool(__thiscall*)(void*)>(hai, 3)(hai); }

static const char* ToggleAutopilotSafe() {
    __try {
        void* iv = LocalVehicle();
        void* hai = IsVehicle(iv) ? Find(iv, 0x403770 /*IHumanAI*/) : nullptr;
        if (!hai) return "no human AI on the local car";
        bool on = VCall<bool(__thiscall*)(void*)>(hai, 3)(hai);
        VCall<void(__thiscall*)(void*, bool)>(hai, 4)(hai, !on);
        if (!on) {
            void* vai = VCall<void* (__thiscall*)(void*)>(iv, 0xAC / 4)(iv);
            if (vai) VCall<void(__thiscall*)(void*, int)>(vai, 0x50 / 4)(vai, 1);   // ResetDriveToNav(current lane)
        } else if (void* rb = Find(iv, Addr::H_IRBVehicle)) {
            VCall<void(__thiscall*)(void*, int, float)>(rb, 4)(rb, 3, 1.0f);       // 1 s invulnerability
        }
        LOG("Autopilot %s", on ? "OFF" : "ON");
        g_apUserOff = on; g_apByUs = !on;
        return on ? "autopilot off" : "autopilot ON (AI drives your car)";
    } __except (EXCEPTION_EXECUTE_HANDLER) { return "exception"; }
}
// Nitrous for the autopilot: the racer AI that drives the local car never presses NOS, so while it
// drives we press it on straights. IInput (handle 0x403F90): slot 2 GetControls, slot 8
// SetControlNOS(bool); the controls block starts at IInput+8 (steering +0x0C, gas +0x1C, brake +0x20,
// handbrake +0x24, NOS byte +0x29 - all relative to the IInput pointer). The engine only lets the
// press through while IEngine (handle 0x404020) slot 7 - nitrous left, 0..1 - is above 0.
static void AutopilotNos(void* iv, void* hai) {
    static bool pressing = false;
    static DWORD holdUntil = 0, coolUntil = 0, lastLog = 0;
    void* in = Find(iv, 0x403F90 /*IInput*/);
    if (!in) return;
    auto setNos = [&](bool on) { VCall<void(__thiscall*)(void*, bool)>(in, 8)(in, on); pressing = on; };
    if (!g_cfg.autoPilotNos || !g_apByUs || !hai || !AutopilotIsOn(hai)) { if (pressing) setNos(false); return; }   // never for the game's own AI control (after the finish line)
    const uint8_t* c = (const uint8_t*)in;
    float steer = *(const float*)(c + 0x0C), gas = *(const float*)(c + 0x1C), brake = *(const float*)(c + 0x20);
    float speed = 0;
    if (void* rb = RigidBodyOf(iv)) { const float* v = RB_GetVec(rb, RB_GetLinearVelocity); speed = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }
    DWORD now = GetTickCount();
    // Like a racer would: only in a race, on a real straight (steady for 1.5 s, i.e. out of the corner),
    // with at least half a tank, and not when already near top speed. A burst runs while the straight
    // lasts and stops at 10 % so there's always some left; then the tank refills for the next straight.
    void* eng = Find(iv, 0x404020 /*IEngine*/);
    float tank = eng ? VCall<float(__thiscall*)(void*)>(eng, 7)(eng) : 0.f;
    int mode = 0; void* rs = nullptr;
    if (Mem::Read(0x91E000, rs) && rs) Mem::Read((uintptr_t)rs + 0x1960, mode);
    bool straight = fabsf(steer) < 0.12f && gas > 0.8f && brake < 0.05f && speed > 22.0f;   // ~80 km/h
    static DWORD straightSince = 0;
    if (!straight) straightSince = 0; else if (!straightSince) straightSince = now;
    if (pressing) {
        if (!straight || tank < 0.10f || mode != 1) { setNos(false); coolUntil = now + 2000; }
        else setNos(true);                     // keep it pressed in case the AI cleared the controls
    } else if (mode == 1 && straight && now - straightSince > 1500 && tank >= 0.5f && speed < 72.0f /*~260 km/h*/ && (int)(now - coolUntil) > 0) {
        setNos(true);
        if (now - lastLog > 30000) { lastLog = now; LOG("Autopilot: nitrous on a straight at %.0f km/h (%.0f%% in the tank)", speed * 3.6f, tank * 100.0f); }
    }
    (void)holdUntil;
}
// After our finish (or knockout) the car went on at full speed under the player's hands while the finish
// camera / results screen was up. The game's own AI drives it then (the autopilot), until the event is
// over or the race restarts; then the player has it back.
static void FinishAutopilotTickImpl() {
    static bool ours = false;            // we switched the autopilot on for the finish
    void* rs = nullptr; int mode = 0;
    if (Mem::Read(0x91E000, rs) && rs) Mem::Read((uintptr_t)rs + 0x1960, mode);
    bool inEvent = Game::GameFlowState() == 6 && Game::CurrentEventHash() != 0;
    bool done = false;
    if (inEvent) {
        if (void* ri = RacerInfoOfLocal()) {
            uint8_t fin = 0; uint32_t outs = 0;
            Mem::Read((uintptr_t)ri + 0x21, fin); Mem::Read((uintptr_t)ri + 0x1C, outs);
            done = fin || outs;
        }
    }
    void* iv = LocalVehicle();
    void* hai = IsVehicle(iv) ? Find(iv, 0x403770) : nullptr;
    if (!hai) { if (!inEvent) ours = false; return; }
    if (done && !ours && !NisActive() && !AutopilotIsOn(hai)) {
        ToggleAutopilotSafe(); ours = true;
        LOG("Race: we're over the line - the AI drives our car until the results are closed");
    } else if (ours && !done) {
        if (AutopilotIsOn(hai)) ToggleAutopilotSafe();
        ours = false;
        LOG("Race: results closed - our car is ours again");
    }
}
// [Debug] AutoPilot=1: switch the autopilot on whenever we're driving in free roam or a race.
void Game::AutopilotTick() {
    if (g_ok && !g_cfg.autoPilot) { __try { FinishAutopilotTickImpl(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
    if (g_ok && GameFlowState() == 6 && !NisActive()) {
        __try {
            void* iv = LocalVehicle();
            if (IsVehicle(iv)) AutopilotNos(iv, Find(iv, 0x403770));
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (!g_ok || !g_cfg.autoPilot) return;
    static int lastFlow = -1, lastMode = -1;
    int flow = GameFlowState(), mode = 0;
    void* rs = nullptr;
    if (Mem::Read(0x91E000, rs) && rs) Mem::Read((uintptr_t)rs + 0x1960, mode);
    if (flow != lastFlow || mode != lastMode) g_apUserOff = false;   // new world or race: auto again
    lastFlow = flow; lastMode = mode;
    static DWORD next = 0;
    if (flow != 6 || g_apUserOff || NisActive() || GetTickCount() < next) return;
    next = GetTickCount() + 1000;
    __try {
        void* iv = LocalVehicle();
        void* hai = IsVehicle(iv) ? Find(iv, 0x403770) : nullptr;
        if (!hai || AutopilotIsOn(hai)) return;
        ToggleAutopilotSafe();
        g_apUserOff = false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

std::string Game::ToggleAutopilot() {
    if (!g_ok || GameFlowState() != 6) return "not in game";
    return ToggleAutopilotSafe();
}

// Visual state of a car: its IInput controls (handle 0x403F90; controls block at IInput+8: steering
// +0x0C, gas +0x1C, brake +0x20, handbrake +0x24, NOS byte +0x29 - relative to the IInput pointer;
// setters slot 5 steering, 6 gas, 7 brake, 8 NOS(bool), 9 handbrake) and its light bits (IVehicle+0x9C).
static uint8_t Unit8(float v) { return (uint8_t)(v <= 0.f ? 0 : v >= 1.f ? 255 : (int)(v * 255.f + 0.5f)); }
static void ReadCarControls(void* iv, CarState& out) {
    const uint8_t* in = (const uint8_t*)Find(iv, 0x403F90 /*IInput*/);
    if (in) {
        out.steer = *(const float*)(in + 0x0C);
        if (!(fabsf(out.steer) <= 1.f)) out.steer = 0.f;
        out.gas = Unit8(*(const float*)(in + 0x1C));
        out.brake = Unit8(*(const float*)(in + 0x20));
        out.handbrake = Unit8(*(const float*)(in + 0x24));
        out.nos = in[0x29] ? 1 : 0;
    }
    out.glare = *(const uint32_t*)((const uint8_t*)iv + 0x9C);
    // damage: IDamageable (handle 0x405000) slot 8 GetZoneDamage(out) - 10 zones x 3 bits, the same
    // value it keeps at IDamageable+0x4C; broken lights: IDamageableVehicle (0x668440) +0x5C bit mask
    if (void* dmg = Find(iv, 0x405000)) { uint32_t z = 0; VCall<void*(__thiscall*)(void*, uint32_t*)>(dmg, 8)(dmg, &z); out.damageZones = z; }
    if (void* dv = Find(iv, 0x668440)) out.brokenLights = *(const uint32_t*)((const uint8_t*)dv + 0x5C);
}
// Show the other player's controls on their car here: front-wheel angle (steering), nitrous flames (NOS)
// and the lights (brake / head / reverse / signals from the glare bits). The motion itself is still
// hard-set from the network every frame.
// Dents / broken parts and lights of the other player's car (written only when they change).
static void ApplyCarDamage(void* iv, const CarState& s) {
    if (!g_cfg.syncDamage) return;
    if (uint8_t* dmg = (uint8_t*)Find(iv, 0x405000)) {
        uint32_t& z = *(uint32_t*)(dmg + 0x4C);
        if (z != (s.damageZones & 0x3FFFFFFF)) {
            z = s.damageZones & 0x3FFFFFFF;
            static uint32_t logged = 0xFFFFFFFF;                      // the game may reset it: log only new values
            if (z != logged) { logged = z; LOG("Ghost: damage zones now %08X", z); }
        }
    }
    if (uint8_t* dv = (uint8_t*)Find(iv, 0x668440)) {
        uint32_t& l = *(uint32_t*)(dv + 0x5C);
        if (l != s.brokenLights) { l = s.brokenLights; LOG("Ghost: broken lights now %08X", l); }
    }
}
// The flames (and NOS sound) follow IEngine slot 8 "nitrous engaged", which the engine works out from its
// own throttle and speed - the copy of their car here has no throttle fed in (it fought the hard-set motion),
// so it never engaged. Slot 8 of the three engine classes (vtables 0x8AB6E0 / 0x8AB7C0 / 0x8ABF88) answers
// "engaged" for their car while they hold NOS; every other car gets the game's own answer.
static void* g_ghostEngine = nullptr;
static bool  g_ghostNos = false;
static uintptr_t g_nosOrig[3] = {};
template <int I> static bool __fastcall hk_NosEngaged(void* eng, void*) {
    if (eng && eng == g_ghostEngine && g_ghostNos) return true;
    return ((bool(__thiscall*)(void*))g_nosOrig[I])(eng);
}
static void InstallNosHooks() {
    static const uintptr_t slots[3] = { 0x8AB700, 0x8AB7E0, 0x8ABFA8 };
    void* hooks[3] = { (void*)&hk_NosEngaged<0>, (void*)&hk_NosEngaged<1>, (void*)&hk_NosEngaged<2> };
    int n = 0;
    for (int i = 0; i < 3; ++i) {
        uintptr_t cur = 0;
        if (Mem::Read(slots[i], cur) && (cur == 0x6A0410 || cur == 0x6A1180)) { g_nosOrig[i] = cur; Mem::PatchPointer((void*)slots[i], hooks[i]); ++n; }
    }
    LOG("Ghost: nitrous flames for the other player's car hooked %d/3", n);
}
static void ApplyCarControls(void* iv, const CarState& s) {
    // nitrous only while their state is fresh and their car moves: after a race their game stops sending
    // input changes (results screen, the AI has the car) and the last "NOS held" kept the flames on a parked car
    bool nos;
    {
        const Net::Snapshot& L = Net::Latest();
        bool fresh = L.valid && GetTickCount() - L.recvTick < 400;
        nos = s.nos != 0 && fresh && s.speedMps > 3.f && s.finished == 0;
    }
    void* in = Find(iv, 0x403F90 /*IInput*/);
    if (in) {
        // gas / brake / handbrake are not fed in: they act on the physics and fought the hard-set motion
        // (50-80 mm jitter spikes on a car driving fast). The brake lights come from the glare bits.
        float steer = s.steer < -1.f ? -1.f : s.steer > 1.f ? 1.f : s.steer;
        VCall<void(__thiscall*)(void*, float)>(in, 5)(in, steer);
        VCall<void(__thiscall*)(void*, bool)>(in, 8)(in, nos);
    }
    // Nitrous flames: the press alone shows nothing - the engine only engages NOS (and draws the flames)
    // while its tank has some left, and the copy of their car here never refills. IEngine (handle 0x404020):
    // slot 7 nitrous left (0..1), slot 8 engaged, slot 11 has nitrous, slot 12 ChargeNOS(amount). While they
    // hold NOS, keep the tank of their car here topped up.
    g_ghostEngine = Find(iv, 0x404020);
    g_ghostNos = nos;
    if (nos) {
        if (void* eng = g_ghostEngine) {
            bool has = VCall<bool(__thiscall*)(void*)>(eng, 11)(eng);
            static void* loggedFor = nullptr;
            if (!has) { if (loggedFor != iv) { loggedFor = iv; LOG("Ghost: the other player uses nitrous, but their car here has no nitrous installed - no flames"); } }
            else if (VCall<float(__thiscall*)(void*)>(eng, 7)(eng) < 0.5f) VCall<void(__thiscall*)(void*, float)>(eng, 12)(eng, 1.0f);
        }
    }
    *(uint32_t*)((uint8_t*)iv + 0x9C) = s.glare;
}
// While the remote car is held during a cutscene it must not keep the last steering / NOS.
static void ClearCarControls(void* iv) {
    void* in = Find(iv, 0x403F90 /*IInput*/);
    if (!in) return;
    VCall<void(__thiscall*)(void*, float)>(in, 5)(in, 0.f);
    VCall<void(__thiscall*)(void*, bool)>(in, 8)(in, false);
    g_ghostNos = false;   // no flames on a held car
}

bool Game::ReadLocalCar(CarState& out) {
    if (!g_ok || GameFlowState() != 6) return false;
    __try {
        void* iv = LocalVehicle();
        void* rb = RigidBodyOf(iv);
        if (!rb) return false;
        const float* p = RB_GetVec(rb, RB_GetPosition);
        const float* q = RB_GetVec(rb, RB_GetOrientation);
        const float* v = RB_GetVec(rb, RB_GetLinearVelocity);
        const float* w = RB_GetVec(rb, RB_GetAngularVelocity);
        memcpy(out.pos, p, 12);
        memcpy(out.rot, q, 16);
        memcpy(out.linVel, v, 12);
        memcpy(out.angVel, w, 12);
        out.speedMps = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        Mem::Read(0x9885D8, out.simTime);                 // when (in simulation time) this state is from
        // race progress of my racer (GRacerInfo: +0x18 pct race, +0x21 finished, +0x24 pct lap,
        // +0x28 laps, +0x2C checkpoints this lap), looked up with GRaceStatus::GetRacerInfo(ISimable*)
        ReadCarControls(iv, out);
        out.inRace = 0;
        void* rs = nullptr;
        int mode = 0;
        if (Mem::Read(0x91E000, rs) && rs && Mem::Read((uintptr_t)rs + 0x1960, mode) && mode == 1) {
            void* sim = VCall<void* (__thiscall*)(void*)>(iv, IV_GetSimable)(iv);
            void* ri = sim ? ((void*(__thiscall*)(void*, void*))0x5E8B20)(rs, sim) : nullptr;
            if (ri) {
                int laps = 0, cps = 0; uint8_t fin = 0;
                Mem::Read((uintptr_t)ri + 0x18, out.racePct);
                Mem::Read((uintptr_t)ri + 0x24, out.lapPct);
                Mem::Read((uintptr_t)ri + 0x28, laps);
                Mem::Read((uintptr_t)ri + 0x2C, cps);
                Mem::Read((uintptr_t)ri + 0x21, fin);
                auto clampf = [](float v) { return (v == v && v > 0.f) ? (v < 100.f ? v : 100.f) : 0.f; };   // the receiver rejects values outside 0..100
                out.racePct = clampf(out.racePct); out.lapPct = clampf(out.lapPct);
                laps = laps < 0 ? 0 : laps > 200 ? 200 : laps; cps = cps < 0 ? 0 : cps > 2000 ? 2000 : cps;
                out.laps = (int16_t)laps; out.checkpoints = (int16_t)cps; out.finished = fin ? Game::LocalFinishRank(ri) : 0;   // our rank at the finish
                { static uint8_t lastFin = 0; if (out.finished && !lastFin) LOG("Race: we finished (rank %d)", (int)out.finished); lastFin = out.finished; }
                // our own name in the standings: the co-op player name (both games may share one
                // save profile, so the profile name would be the same on both)
                static char myName[32];
                if (!g_cfg.playerName.empty()) {
                    NetUtil::CopyHudName(myName, g_cfg.playerName.c_str());       // HUD slots are 16 bytes
                    const char* nm = myName; Mem::Write((uintptr_t)ri + 0x08, nm);
                }
                out.inRace = 1;
            }
        }
        out.pursuitHeat = Game::LocalPursuitHeat();
        out.nisHash = Game::NisSceneHash();
        out.nisVote = Game::NisVoteHash();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG("ReadLocalCar: exception");
        return false;
    }
}

// ---------------------------------------------------------------------------
// Spawning our own vehicles
// ---------------------------------------------------------------------------
namespace {
    struct SimParam { uint32_t mType, mName; void* mData; uint32_t pad; };
    struct VehicleParams {
        SimParam base;
        int32_t  carClass;
        uint32_t carType;
        const float* initialVec;
        const float* initialPos;
        void*    customization;
        void*    vehicleCache;
        void*    matched;
        uint32_t flags;
    };
    static_assert(sizeof(VehicleParams) == 0x30, "VehicleParams layout");

    // ---- our IVehicleCache ----
    // PVehicle::Construct runs a car-memory budget check (0x687710). A new car model needs room in the
    // CarLoader pool; when over budget, the game asks the IVehicleCache passed in VehicleParams+0x24 to
    // agree to evicting other cars (traffic/cops). With no cache it simply fails. Interface (UCOM::IUnknown
    // layout: vtable, mCOMObject): [1] GetCacheName, [2] OnQueryVehicleCache(IVehicle*, IVehicleCache*)
    // -> 0 VCR_WANT / 1 VCR_DONTCARE, [3] OnRemovedVehicleCache(IVehicle*).
    // The cache must also be in the game's list of caches (0x425300 on cache+8; array 0x92FDA4): the
    // budget check (0x687710) asks only listed caches before it DELETES a vehicle to make room for a new
    // traffic car / cop, and builds the new one in the same memory. Unlisted, the other player's car was
    // deleted and our pointer then drove a cop or traffic car (seen as "his car turned into a taxi").
    constexpr int kKeepMax = 128;
    void* g_keepCars[kKeepMax] = {};                // every car we spawned (the other player's car, mirrors)
    uint32_t g_keepWid[kKeepMax] = {};              // their world ids: the same address may later hold another car
    // A free slot of the grid around this car (CoopRace::GridSlotFor - behind it, beside that, beside it),
    // at its height, facing the same way; with no cars around, one car length (6 m) straight behind it.
    // False when every slot it knows is taken (out: behind it all the same).
    bool FreeGridSlotNear(void* rb, float out[3], float& along, float& side, int& n) {
        void* exclude = nullptr;
        along = -CoopRace::kGridDefaultRow; side = 0.f; n = 0;
        float f[3];
        VCall<void(__thiscall*)(void*, float*)>(rb, 13 /*GetForwardVector*/)(rb, f);
        const float* p = RB_GetVec(rb, RB_GetPosition);
        float fl = sqrtf(f[0] * f[0] + f[2] * f[2]);
        if (!(fl > 0.1f)) { out[0] = p[0] - f[0] * 6.f; out[1] = p[1]; out[2] = p[2] - f[2] * 6.f; return true; }
        float fx = f[0] / fl, fz = f[2] / fl, rx = fz, rz = -fx;        // forward and right on the ground
        CoopRace::GridCar cars[24];
        void* self = LocalVehicle();
        void** b = nullptr;
        int total = ListSize(VL_ALL, &b);
        for (int i = 0; i < total && n < 24; ++i) {
            void* iv = ListGet(VL_ALL, i);
            if (!iv || iv == self || iv == g_ghost || iv == exclude || !IsVehicle(iv)) continue;
            void* orb = RigidBodyOf(iv);
            if (!orb) continue;
            const float* q = RB_GetVec(orb, RB_GetPosition);
            if (!q) continue;
            float dx = q[0] - p[0], dy = q[1] - p[1], dz = q[2] - p[2];
            if (fabsf(dy) > 4.f || dx * dx + dz * dz > 40.f * 40.f) continue;
            cars[n].along = dx * fx + dz * fz; cars[n].side = dx * rx + dz * rz; ++n;
        }
        bool found = CoopRace::GridSlotFor(cars, n, along, side);
        out[0] = p[0] + fx * along + rx * side; out[1] = p[1]; out[2] = p[2] + fz * along + rz * side;
        return found;
    }
    // The cars around this one for the log: "ahead/right" in metres per car (negative: behind / left),
    // so a log shows the grid a start slot was picked from.
    void GridCarsText(void* rb, char* out, size_t cap) {
        out[0] = 0;
        if (!rb || cap < 8) return;
        float f[3];
        VCall<void(__thiscall*)(void*, float*)>(rb, 13 /*GetForwardVector*/)(rb, f);
        const float* p = RB_GetVec(rb, RB_GetPosition);
        float fl = sqrtf(f[0] * f[0] + f[2] * f[2]);
        if (!p || !(fl > 0.1f)) return;
        float fx = f[0] / fl, fz = f[2] / fl, rx = fz, rz = -fx;
        void* self = LocalVehicle();
        void** b = nullptr;
        size_t len = 0;
        for (int i = 0, total = ListSize(VL_ALL, &b); i < total; ++i) {
            void* iv = ListGet(VL_ALL, i);
            if (!iv || iv == self || !IsVehicle(iv)) continue;
            void* orb = RigidBodyOf(iv);
            const float* q = orb ? RB_GetVec(orb, RB_GetPosition) : nullptr;
            if (!q) continue;
            float dx = q[0] - p[0], dy = q[1] - p[1], dz = q[2] - p[2];
            if (fabsf(dy) > 4.f || dx * dx + dz * dz > 40.f * 40.f) continue;
            int w = _snprintf_s(out + len, cap - len, _TRUNCATE, "%s%.1f/%.1f%s", len ? " " : "",
                                dx * fx + dz * fz, dx * rx + dz * rz, iv == g_ghost ? "(them)" : "");
            if (w < 0) break;
            len += (size_t)w;
        }
        if (!len) _snprintf_s(out, cap, _TRUNCATE, "none");
    }
    uint32_t WorldIdOf(void* iv) {            // ISimable slot 28 (0x686AF0): unique per car, never reused
        // only a live car: calling into a deleted one ran whatever its freed memory held (an event start
        // deletes most cars at once) and crashed past the __try below
        if (!IsVehicle(iv) || !ListContains(-1, iv)) return 0;
        __try {
            void* sim = VCall<void* (__thiscall*)(void*)>(iv, 1 /*IV_GetSimable*/)(iv);
            return sim ? VCall<uint32_t(__thiscall*)(void*)>(sim, 28)(sim) : 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }
    // false if this address now holds a different car than the one we spawned there
    bool StillOurCar(void* iv) {
        for (int i = 0; i < kKeepMax; ++i) if (g_keepCars[i] == iv) return !g_keepWid[i] || WorldIdOf(iv) == g_keepWid[i];
        return true;
    }
    void* g_removedCars[16] = {}; int g_removedNext = 0;   // ours the game deleted: the address may hold another car now
    void (*g_onCarRemoved)(void*) = nullptr;
    void (*g_onMirrorRemoved)(void*) = nullptr;     // traffic.inl: drop any mirror that used this car
    bool (*g_keepMirror)(void*) = nullptr;          // traffic.inl: a copy of the other player's car (traffic / cop) the game must not take for room

    bool IsKeptCar(void* iv) { for (void* k : g_keepCars) if (k && k == iv) return true; return false; }
    void KeepCar(void* iv) {
        for (void*& r : g_removedCars) if (r == iv) r = nullptr;     // this address is ours again
        // the same address again (the game freed a car of ours without asking the cache - a race's racers -
        // and built this one there): renew that entry. A second entry would leave the old one first, and its
        // stale world id made StillOurCar() call the new car "not ours" (the ghost then was respawned every frame)
        for (int i = 0; i < kKeepMax; ++i) if (g_keepCars[i] == iv) { g_keepWid[i] = WorldIdOf(iv); return; }
        for (int i = 0; i < kKeepMax; ++i) if (!g_keepCars[i]) { g_keepCars[i] = iv; g_keepWid[i] = WorldIdOf(iv); return; }
    }
    void ForgetCar(void* iv) { for (void*& k : g_keepCars) if (k == iv) k = nullptr; }
    bool WasRemovedByGame(void* iv) { if (!iv) return false; for (void* r : g_removedCars) if (r == iv) return true; return false; }

    // __fastcall(ecx = this, edx = unused, stack args...) matches MSVC __thiscall with callee cleanup.
    void        __fastcall Cache_Dtor(void*, void*, int) {}
    const char* __fastcall Cache_GetName(void*, void*) { return "MWTogetherRemotePlayers"; }
    // keep the other player's car (and the test clone); mirror cars may go to make room - they come back
    int         __fastcall Cache_OnQuery(void*, void*, void* iv, void* /*asking*/);
    void        __fastcall Cache_OnRemoved(void*, void*, void* iv) {
        if (IsKeptCar(iv)) {
            LOG("Vehicle cache: game removed our car %p", iv);
            ForgetCar(iv);
            g_removedCars[g_removedNext] = iv; g_removedNext = (g_removedNext + 1) % 16;
            if (g_onMirrorRemoved) g_onMirrorRemoved(iv);
            if (g_onCarRemoved) g_onCarRemoved(iv);
        }
    }
    void* g_cacheVtbl[4] = { (void*)&Cache_Dtor, (void*)&Cache_GetName, (void*)&Cache_OnQuery, (void*)&Cache_OnRemoved };
    struct VCache { void** vt; void* com; void* node; };
    VCache g_cache = { g_cacheVtbl, (void*)0x4D575443 /* unique tag, compared only */, nullptr };
    bool g_cacheListed = false;
    void ListOurCache() {
        if (g_cacheListed) return;
        __try { ((void(__thiscall*)(void*))0x425300)((char*)&g_cache + 8); g_cacheListed = true; LOG("Vehicle cache: registered with the game"); }
        __except (EXCEPTION_EXECUTE_HANDLER) { LOG("Vehicle cache: registering failed"); }
    }

    int __fastcall Cache_OnQuery(void*, void*, void* iv, void* /*asking*/) {
        // while an event we join builds its racers, the game may take our copy of their car for room
        bool yield = g_ghostYieldUntil && (int)(GetTickCount() - g_ghostYieldUntil) < 0;
        if (iv && iv == g_ghost && !yield) return 0;   // want
        // a copy of the other player's traffic / cop close to us stays: taking it for room made cops vanish
        // in front of the player (and come back 3 s later, taking another car - over and over in pursuits)
        if (iv && g_keepMirror && g_keepMirror(iv)) return 0;
        return 1;                                      // don't care
    }

    uint32_t VehicleKey(void* iv) {
        using Fn = uint32_t(__thiscall*)(void*);
        return VCall<Fn>(iv, IV_GetVehicleKey)(iv);
    }
    const void* Customizations(void* iv) {
        using Fn = const void* (__thiscall*)(void*);
        return VCall<Fn>(iv, IV_GetCustomizations)(iv);
    }

    void* g_killed[32] = {}; int g_killedNext = 0;   // cars we destroyed (see KillVehicle)
    // Room for MaxCars cars ([Traffic] MaxCars, 40 by default - budget.inl): the game's car tables grown to it, and the other player's
    // copies (built by SpawnVehicle) allowed up to 40; the game's own cars keep its 20.
    constexpr int kGameCarLimit = 20;
    int CopyCarCap() { return g_cfg.maxCars; }   // [Traffic] MaxCars
    bool g_tablesBig = false;       // every car table has its big buffer now
    bool g_copySpawn = false;       // SpawnVehicle is building one of the other player's cars
    int  CopyCarLimit() { return g_tablesBig ? CopyCarCap() : kGameCarLimit; }
    void* SpawnVehicle(uint32_t carKey, const void* custom, const float pos[3], const float dir[3], int driverClass) {
        uint32_t typeHash = 0;
        ((uint32_t* (__cdecl*)(uint32_t*))Addr::VehicleParamsType)(&typeHash);
        static uint8_t customCopy[0x198];           // the game copies it during construction
        if (custom) memcpy(customCopy, custom, sizeof(customCopy));
        VehicleParams vp{};
        vp.base.mType = typeHash;
        vp.base.mName = Addr::ParamNameBase;
        vp.base.mData = &vp;
        vp.carClass = driverClass;
        vp.carType = carKey;
        vp.initialVec = dir;
        vp.initialPos = pos;
        vp.customization = custom ? customCopy : nullptr;
        vp.vehicleCache = &g_cache;   // lets the budget check evict traffic/cops to make room for this model
        using Fn = void* (__cdecl*)(SimParam);
        g_copySpawn = true;
        void* simable = ((Fn)Addr::PVehicleConstruct)(vp.base);
        g_copySpawn = false;
        if (!simable) return nullptr;
        // find its IVehicle interface
        void* iv = nullptr;
        void* object = nullptr;
        if (Mem::Read((uintptr_t)simable + 4, object) && object) {
            using FindFn = void* (__thiscall*)(void*, uint32_t);
            iv = ((FindFn)Addr::UCOM_Find)(object, 0x4040E0 /*IVehicle*/);
        }
        LOG("SpawnVehicle key %08X class %d -> simable %p ivehicle %p", carKey, driverClass, simable, iv);
        if (iv) { for (void*& k : g_killed) if (k == iv) k = nullptr; KeepCar(iv); }
        return IsVehicle(iv) ? iv : nullptr;
    }

    // a car we already destroyed must never be destroyed again (seen at a race start: the same car killed
    // twice in one frame, then the game called into the freed object - "pure virtual function call")
    // (for a few seconds only: later the game may build a new car at that address - one of our own traffic
    //  cars then could never be removed, "skipped" every frame)
    DWORD g_killedAt[32] = {};
    bool AlreadyKilled(void* iv) { for (int i = 0; i < 32; ++i) if (g_killed[i] == iv && GetTickCount() - g_killedAt[i] < 3000) return true; return false; }
    void KillVehicle(void* iv) {
        if (!iv) return;
        if (AlreadyKilled(iv)) { LOG("KillVehicle %p skipped - already destroyed", iv); return; }
        if (!StillOurCar(iv)) { LOG("KillVehicle %p skipped - the game built another car there", iv); ForgetCar(iv); return; }
        ForgetCar(iv);
        if (!IsVehicle(iv)) return;
        using GetSim = void* (__thiscall*)(void*);
        void* sim = VCall<GetSim>(iv, IV_GetSimable)(iv);
        if (!sim || VTable(sim) != Addr::VT_PVehicle_ISimable) { LOG("KillVehicle: unexpected ISimable %p", sim); return; }
        using KillFn = void(__thiscall*)(void*);
        VCall<KillFn>(sim, 2)(sim);
        g_killed[g_killedNext] = iv; g_killedAt[g_killedNext] = GetTickCount(); g_killedNext = (g_killedNext + 1) % 32;
        LOG("KillVehicle %p", iv);
    }

    void* g_clone = nullptr;
}

// ---------------------------------------------------------------------------
// Races: the game builds every car through the "PVehicle" factory pointer (0x92C534). While a race
// loads, build the first AI racer as the peer's car (model + tuning) so they race in their own car
// and count in positions / results. (Same technique as MostWantedTimeTrialGhosts.)
// ---------------------------------------------------------------------------
// A Blacklist rival (boss) race has a single opponent: the rival. There the other player must not
// take that racer's place, so their car is spawned next to the race instead (not in the standings).
// The event being started is known either from the race status or from the invitation we act on.
static uint32_t g_startingEventHash = 0;
// A prologue event (16.x) loading: its opponents are scripted characters (16.2.1: Razor, whose win the
// story needs), so the other player doesn't take an AI racer's place there either.
static bool PrologueRaceStarting() {
    __try {
        void* rs = nullptr; void* parms = nullptr;
        if (Mem::Read(0x91E000, rs) && rs) Mem::Read((uintptr_t)rs + 0x1968, parms);
        const char* id = parms ? ((const char*(__thiscall*)(void*))0x5FBA70)(parms) : nullptr;
        return id && Mem::IsReadable(id, 4) && Prologue::IsPrologueEvent(id);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool BossRaceStarting() {
    __try {
        void* rs = nullptr; void* parms = nullptr; void* db = nullptr;
        if (Mem::Read(0x91E000, rs) && rs) Mem::Read((uintptr_t)rs + 0x1968, parms);
        if (!parms && g_startingEventHash && Mem::Read(0x91E004, db) && db)
            parms = ((void*(__thiscall*)(void*, uint32_t))0x5FB710)(db, g_startingEventHash);
        return parms && ((bool(__thiscall*)(void*))0x5FBD90)(parms);     // GRaceParameters::IsBossRace
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Rival races together: one more AI racer on the rival's side (bosshelper.inl). Set when the race
// builds its racer list, read by the car factory below.
namespace {
    bool     g_helperOn = false;        // this race load added the helper
    int      g_helperOpp = -1;          // its opponent index (= the event's own opponent count)
    int      g_helperSlot = -1;         // its GRacerInfo slot in GRaceStatus
    int      g_bossRacerBuilds = 0;     // AI racer cars built for this rival race so far
    bool     g_helperPeerRoster = false;// the client joined with the host's roster: its helper car is entry g_helperOpp
    bool     g_partnerOn = false;       // this rival race load added a racer slot for the other player
    int      g_partnerOpp = -1;         // its opponent index (after the helper)
    int      g_partnerSlot = -1;        // its GRacerInfo slot
    bool     g_bossRaceOn = false;      // this race load is a Blacklist rival race (racer roles: traffic.inl)
    uint32_t g_bossRivalKey = 0;        // the rival's car as built in this load (its record: for the helper)
    bool     g_bossRivalHasRec = false;
    uint8_t  g_bossRivalRec[0x198];
    uint8_t  g_helperRec[0x198];        // the helper's record (the game copies it while building the car)
    // GRacerInfo slot of a racer info (GRaceStatus+0x18 + n*0x194), -1 = none
    int RacerSlotOfInfo(void* ri) {
        char* rs = nullptr;
        if (!ri || !Mem::Read(0x91E000, rs) || !rs) return -1;
        intptr_t off = (char*)ri - (rs + 0x18);
        if (off < 0 || off % 0x194 || off / 0x194 > 15) return -1;
        return (int)(off / 0x194);
    }
    int RacerRoleOfSlot(int slot) {
        return CoopRace::RacerRoleOf(slot, g_bossRaceOn, g_helperOn ? g_helperSlot : -1, g_partnerOn ? g_partnerSlot : -1);
    }
}
static void InstallBossHelper();
static void InstallRivalGate();
static void InstallRoadblockSync();
static void InstallBudgetLog();
// The helper's car per Blacklist rival: the partner the rival's race intro shows, where it shows one
// (BossHelperCar, raceutil.h); otherwise the game's own pick. (The cars the intro cutscene itself builds
// are traffic and police props - traftaxi, copmidsize_nis ... - never the partner, so they aren't used.)
// Body-shell check for the log: the car's BODY part (car slot 23 - the slot table at 0x9044D8: BASE 0,
// ..., BODY 23, ..., BASE_PAINT 76) in the record it is built with. With no record the game gives the
// car its model's stock parts (RideInfo 0x7594A0) and only then applies a record (0x56F2B0).
static void* CarPartByIndex(int16_t index);   // paint.inl
static void LogCarBody(const char* who, uint32_t model, const void* rec) {
    int16_t body = -1, paint = -1; void* part = nullptr;
    if (rec) {
        Mem::SafeRead((const uint8_t*)rec + 23 * 2, &body, sizeof(body));
        Mem::SafeRead((const uint8_t*)rec + Paint::kBasePaintOff, &paint, sizeof(paint));
        if (body >= 0) part = CarPartByIndex(body);
    }
    if (!rec) LOG("Body: %s %08X - NO parts record (the model's stock parts)", who, model);
    else LOG("Body: %s %08X - BODY part %d (%s), paint %d", who, model, body, part ? "found" : body < 0 ? "none" : "NOT FOUND", paint);
}
static uint32_t PartnerCarKey() {
    __try {
        void* rs = nullptr; void* parms = nullptr;
        if (Mem::Read(0x91E000, rs) && rs) Mem::Read((uintptr_t)rs + 0x1968, parms);
        const char* id = parms ? ((const char*(__thiscall*)(void*))0x5FBA70)(parms) : nullptr;
        const char* model = (id && Mem::IsReadable(id, 2)) ? CoopRace::BossHelperCar(atoi(id)) : nullptr;
        return model ? ((uint32_t(__cdecl*)(const char*))0x4519D0)(model) : 0;   // model key = Attrib hash of the name
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// ---- fixed AI opponents ([Coop] FixedOpponents, opponentsutil.h) ---------------------------------------
namespace {
    Opponents::Book g_oppBook;
    bool     g_oppBookLoaded = false;
    uint32_t g_oppHash = 0;          // the event whose racers are being built
    DWORD    g_oppLastBuild = 0;
    int      g_oppNext = 0;          // its next opponent (AI racers the game picked, in build order)
    uint8_t  g_oppRec[Opponents::kRecordBytes];
    std::string OppBookPath() {
        char path[MAX_PATH] = {};
        HMODULE m = nullptr;
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&OppBookPath, &m) ||
            !GetModuleFileNameA(m, path, MAX_PATH)) return "MWTogether-opponents.bin";
        std::string s = path;
        size_t slash = s.find_last_of("\\/");
        return (slash == std::string::npos ? std::string() : s.substr(0, slash + 1)) + "MWTogether-opponents.bin";
    }
    void OppBookLoad() {
        if (g_oppBookLoaded) return;
        g_oppBookLoaded = true;
        std::vector<uint8_t> buf;
        if (FILE* f = fopen(OppBookPath().c_str(), "rb")) {
            uint8_t chunk[4096]; size_t n;
            while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) buf.insert(buf.end(), chunk, chunk + n);
            fclose(f);
        }
        if (buf.empty()) LOG("Race: fixed opponents - no book yet (each event's first opponents are kept)");
        else if (g_oppBook.Load(buf.data(), buf.size())) LOG("Race: fixed opponents - book of %u event(s) loaded", (unsigned)g_oppBook.Events());
        else LOG("Race: fixed opponents - the book is damaged, starting a new one");
    }
    void OppBookSave() {
        std::vector<uint8_t> out = g_oppBook.Save();
        std::string path = OppBookPath(), tmp = path + ".tmp";
        FILE* f = fopen(tmp.c_str(), "wb");
        if (!f) { LOG("Race: fixed opponents - can't write %s", tmp.c_str()); return; }
        bool ok = fwrite(out.data(), 1, out.size(), f) == out.size();
        ok = fclose(f) == 0 && ok;
        if (ok) ok = MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
        if (!ok) LOG("Race: fixed opponents - saving the book failed");
    }
    uint32_t OppEventNow() {
        void* rs = nullptr; void* parms = nullptr;
        if (!Mem::Read(0x91E000, rs) || !rs || !Mem::Read((uintptr_t)rs + 0x1968, parms) || !parms) return 0;
        return ((uint32_t(__thiscall*)(void*))0x5FAB40)(parms);   // GRaceParameters::GetEventHash
    }
    // GRacerInfo::ChooseRandomName 0x5DC800 (thiscall, from GRaceStatus::AddRacer at 0x602001): bRandom
    // (seed bDefaultSeed 0x8EC4E0) picks RACERNAME_000..149 until no other racer has it. For that call
    // the seed comes from the event and the racer's slot; the generator then goes on where it was.
    constexpr uintptr_t ChooseNameFn = 0x5DC800, ChooseNameCall = 0x602001, RandomSeed = 0x8EC4E0;
    void __fastcall hk_ChooseRandomName(void* ri, void*) {
        uint32_t hash = 0; int slot = -1;
        if (g_cfg.fixedOpponents) { __try { hash = OppEventNow(); slot = RacerSlotOfInfo(ri); } __except (EXCEPTION_EXECUTE_HANDLER) { hash = 0; } }
        uint32_t saved = 0;
        bool seeded = hash && slot >= 0 && Mem::Read(RandomSeed, saved);
        if (seeded) { uint32_t s = Opponents::NameSeed(hash, slot); Mem::Write(RandomSeed, s); }
        ((void(__thiscall*)(void*))ChooseNameFn)(ri);
        if (seeded) Mem::Write(RandomSeed, saved);
    }
}

static void* __cdecl hk_VehicleConstruct(SimParam p) {
    using Fn = void* (__cdecl*)(SimParam);
    VehicleParams* vp = (VehicleParams*)p.mData;
    bool replaced = false;
    if (vp && vp->carClass == DRV_NIS && vp->carType) { LOG("Race: cutscene car built, model %08X", vp->carType); LogCarBody("cutscene car", vp->carType, vp->customization); }
    if (vp && vp->carClass == DRV_RACER && !g_racerReplacedThisLoad && g_cfg.ghostEnabled && Net::Stats().connected && g_coopLinked && !BossRaceStarting() && !PrologueRaceStarting()) {
        if (const CarInfo* pc = Net::PeerCar()) {
            g_origRacer[0] = vp->carType; g_origRacerCustom = vp->customization; g_origRacerMatched = vp->matched;
            vp->carType = pc->carKey;
            if (pc->hasCustom) { memcpy(g_peerCustomForRace, pc->custom, sizeof(g_peerCustomForRace)); vp->customization = g_peerCustomForRace; }
            else vp->customization = nullptr;
            vp->matched = nullptr;
            replaced = true;
            g_racerReplacedThisLoad = true;
        }
    }

    // the other AI racers: build the same opponents as the game that started the event
    bool fromRoster = false;
    uint32_t rKey = 0; void* rCustom = nullptr; void* rMatched = nullptr;
    // (not in a rival race: the event itself names the rival's car, so both games build the same one; a
    // roster could only swap it for another car and the client's rival would then match no host racer)
    bool boss = vp && vp->carClass == DRV_RACER && BossRaceStarting();
    if (boss && g_usePeerRoster) {
        g_usePeerRoster = false; LOG("Race: rival race - the event's own rival car, not the partner's roster");
        if (g_helperOn) g_helperPeerRoster = true;      // ... but the helper is theirs (the game that started it)
    }
    // the helper (the racer after the event's own opponents): the host's choice on the client, else the
    // rival's partner from the list when that model is loaded, else the car the game picked
    int bossIdx = boss ? g_bossRacerBuilds++ : -1;
    bool helper = boss && g_helperOn && bossIdx == g_helperOpp;
    // the other player's racer slot in a rival race together: built as their car, like the replaced AI racer
    // of other races (their name, progress and finish then show in the standings)
    if (boss && g_partnerOn && bossIdx == g_partnerOpp && !g_racerReplacedThisLoad && g_cfg.ghostEnabled && Net::Stats().connected && g_coopLinked) {
        if (const CarInfo* pc = Net::PeerCar()) {
            g_origRacer[0] = vp->carType; g_origRacerCustom = vp->customization; g_origRacerMatched = vp->matched;
            vp->carType = pc->carKey;
            if (pc->hasCustom) { memcpy(g_peerCustomForRace, pc->custom, sizeof(g_peerCustomForRace)); vp->customization = g_peerCustomForRace; }
            else vp->customization = nullptr;
            vp->matched = nullptr;
            replaced = true;
            g_racerReplacedThisLoad = true;
            LOG("Race: rival race - the other player's racer built as their car %08X", pc->carKey);
            LogCarBody("the other player's racer", pc->carKey, vp->customization);
        }
    }
    uint32_t hKey = 0; void* hCustom = nullptr; void* hMatched = nullptr; const char* hFrom = "";
    if (helper) {
        hKey = vp->carType; hCustom = vp->customization; hMatched = vp->matched;
        // the other game's helper when we join its race: from the roster we joined with, or one that came
        // in for this event since (the rival's next race starts by itself, the roster may come late)
        const RosterEntry* pe = nullptr;
        if (g_helperPeerRoster || (g_joiningEvent && PeerRosterFresh())) {
            int roles[8]; int n = g_peerRosterN < 8 ? g_peerRosterN : 8;
            for (int i = 0; i < n; ++i) roles[i] = g_peerRoster[i].role;
            int k = CoopRace::RosterHelperEntry(roles, n, g_helperOpp);
            if (k >= 0) pe = &g_peerRoster[k];
        }
        uint32_t listKey = PartnerCarKey();
        CoopRace::HelperIn in{ listKey, vp->carType, g_bossRivalKey, g_bossRivalHasRec, pe ? pe->key : 0u, pe && pe->hasCustom };
        CoopRace::HelperOut o = CoopRace::ChooseHelperCar(in);
        if (o.record == CoopRace::kHelperPeerRecord && pe) {
            memcpy(g_helperRec, pe->custom, sizeof(g_helperRec));
            hFrom = "the other player's helper, its parts";
        } else if (o.record == CoopRace::kHelperRivalRecord) {
            memcpy(g_helperRec, g_bossRivalRec, sizeof(g_helperRec));
            hFrom = pe ? "the other player's helper model, the rival's parts" : "the rival's partner from the list, the rival's parts";
        } else {
            hKey = 0;
            uint32_t wanted = pe ? pe->key : listKey;
            hFrom = (wanted && wanted != vp->carType) ? "the game's pick (no parts for the wanted model here)" : "the game's pick, its parts";
            if (wanted && wanted != vp->carType) LOG("Race: rival race - helper model %08X has no parts here (rival %08X%s) - keeping the game's pick", wanted, g_bossRivalKey, g_bossRivalHasRec ? "" : " without a record");
        }
        if (hKey) { vp->carType = o.model; vp->customization = g_helperRec; vp->matched = nullptr; }
    }
    if (vp && vp->carClass == DRV_RACER && !replaced && g_coopLinked && g_usePeerRoster && g_peerRosterNext < g_peerRosterN) {
        RosterEntry& r = g_peerRoster[g_peerRosterNext++];
        if (r.hasCustom) {
            rKey = vp->carType; rCustom = vp->customization; rMatched = vp->matched;
            vp->carType = r.key;
            vp->customization = (void*)r.custom;
            vp->matched = nullptr;
            fromRoster = true;
        } else if (r.key != vp->carType) {
            // built without a record it would have no body: our own car (the mirror binds it by its slot)
            LOG("Race: the peer's opponent %d (model %08X) came without its parts - keeping our own %08X", g_peerRosterNext, r.key, vp->carType);
        }
    }

    // fixed opponents: the ones this event was first built with here (not over the other player's roster,
    // their car, or a rival race's own racers)
    bool fromBook = false; int oppIdx = -1; uint32_t oppHash = 0;
    uint32_t bKey = 0; void* bCustom = nullptr; void* bMatched = nullptr;
    if (vp && vp->carClass == DRV_RACER && !replaced && !boss && g_cfg.fixedOpponents) {
        __try {
            OppBookLoad();
            oppHash = OppEventNow();
            DWORD nowB = GetTickCount();
            if (Opponents::NewBuild(g_oppHash, oppHash, nowB - g_oppLastBuild)) { g_oppHash = oppHash; g_oppNext = 0; }
            g_oppLastBuild = nowB;
            oppIdx = oppHash ? g_oppNext++ : -1;
            const Opponents::Entry* be = (oppIdx >= 0 && !fromRoster) ? g_oppBook.Get(oppHash, oppIdx) : nullptr;
            if (be && be->hasCustom && !(g_coopLinked && g_usePeerRoster)) {
                bKey = vp->carType; bCustom = vp->customization; bMatched = vp->matched;
                memcpy(g_oppRec, be->custom, sizeof(g_oppRec));
                vp->carType = be->key; vp->customization = g_oppRec; vp->matched = nullptr;
                fromBook = true;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { oppIdx = -1; }
    }

    void* simable = ((Fn)Addr::PVehicleConstruct)(p);
    if (fromBook && !simable) {
        LOG("Race: fixed opponent %d (model %08X) not loaded here - the game's pick instead", oppIdx, vp->carType);
        vp->carType = bKey; vp->customization = bCustom; vp->matched = bMatched; fromBook = false;
        simable = ((Fn)Addr::PVehicleConstruct)(p);
    } else if (fromBook) LOG("Race: opponent %d built as this event's fixed opponent (model %08X)", oppIdx, vp->carType);
    if (simable && oppIdx >= 0 && !fromBook && vp) {
        uint8_t rec[Opponents::kRecordBytes];
        bool hasRec = vp->customization && Mem::SafeRead(vp->customization, rec, sizeof(rec));
        if (g_oppBook.Put(oppHash, oppIdx, vp->carType, hasRec ? rec : nullptr)) {
            LOG("Race: fixed opponents - event %08X opponent %d is model %08X from now on", oppHash, oppIdx, vp->carType);
            OppBookSave();
        }
    }
    if (helper && hKey && !simable) {
        LOG("Race: rival race - helper model %08X (%s) not loaded here - the game's pick %08X instead", vp->carType, hFrom, hKey);
        vp->carType = hKey; vp->customization = hCustom; vp->matched = hMatched; hFrom = "the game's pick";
        simable = ((Fn)Addr::PVehicleConstruct)(p);
    }
    if (helper) {
        LOG("Race: rival race - helper racer built as model %08X, %s, parts %s (%s)", vp->carType, hFrom, vp->customization ? "yes" : "NONE", simable ? "ok" : "FAILED");
        LogCarBody("helper racer", vp->carType, vp->customization);
    }
    else if (boss && !replaced) {
        LOG("Race: rival race - rival built as model %08X, parts %s (%s)", vp->carType, vp->customization ? "yes" : "NONE", simable ? "ok" : "FAILED");
        LogCarBody("rival", vp->carType, vp->customization);
        if (simable && !g_bossRivalKey) {   // the first one: its record can dress a helper of the same model
            g_bossRivalKey = vp->carType;
            g_bossRivalHasRec = vp->customization && Mem::SafeRead(vp->customization, g_bossRivalRec, sizeof(g_bossRivalRec));
        }
    }
    if (fromRoster) {
        if (simable) LOG("Race: opponent %d built as the peer's opponent (key %08X)", g_peerRosterNext, vp->carType);
        else {
            LOG("Race: peer's opponent model %08X not loaded - keeping our own", vp->carType);
            vp->carType = rKey; vp->customization = rCustom; vp->matched = rMatched;
            simable = ((Fn)Addr::PVehicleConstruct)(p);
        }
    }
    // remember what we built, to send it when this game starts the event
    if (simable && vp && vp->carClass == DRV_RACER && !replaced && g_localRosterN < 8) {
        RosterEntry& r = g_localRoster[g_localRosterN++];
        r.key = vp->carType;
        r.hasCustom = vp->customization != nullptr;
        if (r.hasCustom) Mem::SafeRead(vp->customization, r.custom, sizeof(r.custom));
        r.name[0] = 0;
        r.role = !boss ? CoopRace::kRoleNormal : helper ? CoopRace::kRoleHelper : CoopRace::kRoleRival;
    }
    if (replaced && !simable) {
        // Their model isn't loaded here (they started the event far away). Do not keep this AI racer's
        // body as "their" car: that put the wrong model at their position while the standings still
        // named them. Build the opponent as the game wanted, and let UpdateGhost spawn their real car.
        uint32_t want = vp->carType;
        vp->carType = g_origRacer[0];
        vp->customization = g_origRacerCustom;
        vp->matched = g_origRacerMatched;
        simable = ((Fn)Addr::PVehicleConstruct)(p);
        replaced = false;
        if (simable) LOG("Race: the other player's car model %08X isn't loaded here - this racer stays an AI opponent (%08X); their car will be spawned", want, vp->carType);
        else { g_racerReplacedThisLoad = false; LOG("Race: racer for the other player could not be built (model %08X)", want); }
    }
    if (replaced && simable && CoopRace::AcceptReplacedRacer(true)) {
        void* object = nullptr;
        if (Mem::Read((uintptr_t)simable + 4, object) && object) {
            using FindFn = void* (__thiscall*)(void*, uint32_t);
            g_replacedRacer = ((FindFn)Addr::UCOM_Find)(object, 0x4040E0);
        }
        if (g_replacedRacer) KeepCar(g_replacedRacer);
        LOG("Race: AI racer built as peer car (key %08X) -> %p", vp->carType, g_replacedRacer);
        g_peerSlotValid = vp->initialPos && Mem::SafeRead(vp->initialPos, g_peerSlot, sizeof(g_peerSlot)) &&
                          NetUtil::FiniteWithin(g_peerSlot, 3, NetUtil::MAX_POS) &&
                          (fabsf(g_peerSlot[0]) + fabsf(g_peerSlot[1]) + fabsf(g_peerSlot[2])) > 1.f;   // 0,0,0: not placed yet (read later)
        if (g_peerSlotValid) LOG("Race: the other player's grid slot %.1f %.1f %.1f", g_peerSlot[0], g_peerSlot[1], g_peerSlot[2]);
    }
    return simable;
}

bool Game::InstallVehicleFactoryHook() {
    ListOurCache();
    g_onCarRemoved = [](void* iv) {
        if (iv == g_ghost) { g_ghost = nullptr; g_ghostKind = GK_NONE; g_spawnedKey = g_spawnedHash = 0; }
    };
    uintptr_t cur = 0;
    if (!Mem::Read(0x92C534, cur) || cur != Addr::PVehicleConstruct) {
        LOG("Vehicle factory slot holds %08X, expected %08X - race car replacement disabled", (unsigned)cur, (unsigned)Addr::PVehicleConstruct);
        return false;
    }
    Mem::PatchPointer((void*)0x92C534, (void*)&hk_VehicleConstruct);
    {
        uint8_t op = 0; int32_t rel = 0;
        bool names = Mem::Read(ChooseNameCall, op) && Mem::Read(ChooseNameCall + 1, rel) && op == 0xE8 && ChooseNameCall + 5 + rel == ChooseNameFn;
        if (names) Mem::PatchCall(ChooseNameCall, (void*)&hk_ChooseRandomName);
        LOG("Race: fixed opponents %s (racer names %s)", g_cfg.fixedOpponents ? "on" : "off ([Coop] FixedOpponents=0)", names ? "hooked" : "NOT hooked");
    }
    InstallTrafficHooks();
    InstallBudgetLog();
    InstallNosHooks();
    InstallEventSync();
    InstallRestartSync();
    InstallStreamHook();
    InstallNisHook();
    InstallStoryHooks();
    InstallBossHelper();
    InstallRivalGate();
    InstallRoadblockSync();
    Net::OnHit = [](const float* dv) {
        if (!g_coopLinked) return;
        __try {
            void* lrb = RigidBodyOf(LocalVehicle());
            if (!lrb || Game::GameFlowState() != 6) return;
            if ((int)(g_noHitUntil - GetTickCount()) > 0) return;   // teleport / restart / cutscene
            const float* lp = RB_GetVec(lrb, RB_GetPosition);
            // Prefer the interpolated ghost (already at "now"); fall back to their last snapshot,
            // extrapolated by how long ago we got it. Do not require g_ghostCollide: the impact
            // already happened on their machine (host->client used to drop here when the ghost
            // was just over 10 m late, or collisions were briefly off).
            const float* peerPos = nullptr; const float* peerVel = nullptr; float ageS = 0.f;
            void* grb = RigidBodyOf(g_ghost);
            if (grb) { peerPos = RB_GetVec(grb, RB_GetPosition); peerVel = RB_GetVec(grb, RB_GetLinearVelocity); }
            if (!peerPos) {
                const Net::Snapshot& p = Net::Latest();
                if (p.valid && p.s.hasCar) {
                    peerPos = p.s.pos; peerVel = p.s.linVel;
                    ageS = (GetTickCount() - p.recvTick) / 1000.f;
                }
            }
            bool recent = g_hitContactAt && (GetTickCount() - g_hitContactAt) < 600;
            if (!HitUtil::ShouldApplyPeerHit(true, g_protectHolding, recent, peerPos, peerVel, ageS, lp)) return;
            float mag = sqrtf(dv[0] * dv[0] + dv[1] * dv[1] + dv[2] * dv[2]);
            float k = HitUtil::HitPushScale(mag);
            // A standing player car brakes itself, so one velocity write is gone within a frame or two:
            // remember the push and keep the car moving along it for 0.4 s (fading out), see ApplyPush.
            g_push[0] = dv[0] * k; g_push[1] = 0; g_push[2] = dv[2] * k;
            g_pushFrom = GetTickCount();
            g_hitT0 = 0; g_lastHitSent = g_pushFrom;      // this is THEIR impact: don't report the push back as ours
            LOG("Collision: hit by the other player, pushed by %.0f km/h", mag * k * 3.6f);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    };
    InstallCoopWin();
    InstallSharedMilestones();
    InstallCatchUp();
    InstallNoSlowMotion();
    InstallWeatherSync();
    InstallFollow();
    InstallMinimapHook();
    InstallWorldMapHook();
    InstallPursuitHooks();
    LOG("Hooked vehicle factory (races: AI racer -> peer car)");
    return true;
}

void Game::OnFlow(int flow) {
    if (flow != 6) {
        for (auto& o : g_aiOv) o.iv = nullptr;   // world unloading: the cars (and their AI) are gone
        for (void*& k : g_keepCars) k = nullptr;
        for (void*& r : g_removedCars) r = nullptr;
    }
    static int last = -1;
    if (flow != last) {
        if (flow == 4) { g_racerReplacedThisLoad = false; g_replacedRacer = nullptr; g_spawnFails = 0; } // new world loading
        last = flow;
    }
}

bool Game::ReadLocalCarInfo(CarInfo& out) {
    if (!g_ok || GameFlowState() != 6) return false;
    __try {
        void* iv = LocalVehicle();
        if (!iv) return false;
        memset(&out, 0, sizeof(out));
        // Send the MODEL key (hash of the car's model name, e.g. "eclipsegt"), not GetVehicleKey():
        // career cars can carry a per-save key (seen: 401337DE for a career eclipsegt) that doesn't
        // exist in the other player's game, so Construct fails there.
        out.carKey = VehicleKey(iv);
        const char* nm = VehName(iv);
        if (nm && nm[0] && nm[0] != '?') {
            using HashFn = uint32_t(__cdecl*)(const char*);
            uint32_t modelKey = ((HashFn)0x4519D0)(nm);   // Attrib::StringHash32
            if (modelKey) out.carKey = modelKey;
        }
        static uint32_t lastLoggedKey = 0;
        if (out.carKey != lastLoggedKey) {
            lastLoggedKey = out.carKey;
            LOG("Local car name: %s (key %08X, driver class %d)", VehName(iv), out.carKey, DriverClass(iv));
        }
        if (const void* c = Customizations(iv)) {
            memcpy(out.custom, c, CUSTOM_RECORD_SIZE);
            out.hasCustom = 1;
        }
        uint32_t h = 2166136261u; // FNV-1a
        for (int i = 0; i < CUSTOM_RECORD_SIZE; ++i) { h ^= out.custom[i]; h *= 16777619u; }
        out.customHash = h ^ out.carKey;
        return out.carKey != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

#include "traffic.inl"
#include "budget.inl"
#include "event.inl"
#include "paint.inl"
#include "minimap.inl"
#include "worldmap.inl"
#include "hud.inl"
#include "pursuit.inl"
#include "peerbar.inl"
#include "marker.inl"
#include "stream.inl"
static char g_dataDir[MAX_PATH] = "";
void Game::SetDataDir(const char* dir) { strncpy_s(g_dataDir, dir, _TRUNCATE); }
#include "boot.inl"
#include "exitwatch.inl"
#include "nis.inl"
#include "story.inl"
#include "profile.inl"
#include "coopwin.inl"
#include "weather.inl"
#include "follow.inl"
#include "milestone.inl"
#include "bosshelper.inl"
#include "rivalgate.inl"
#include "roadblock.inl"

void Game::ReleaseGhost() {
    g_streamPeer = false;
    g_ghostEngine = nullptr; g_ghostNos = false;
    if (g_ghost) {
        __try {
            // Only touch the car if the game still owns it (it may have been destroyed after a race).
            if (GameFlowState() == 6 && ListContains(g_ghostList, g_ghost)) {
                if (g_ghostKind == GK_SPAWNED) KillVehicle(g_ghost); // our own car: remove it
                else SetCollisions(g_ghost, true);                 // borrowed car: give it back
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        __try {
            if (g_ghostKind == GK_RACER) RestoreAI(g_ghost);          // give the racer its AI back
            ReleaseRetargets(PursuitOf(LocalVehicle()));               // no cop may keep chasing a dead car
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        g_ghostKind = GK_NONE;
        g_spawnedKey = g_spawnedHash = 0;
        LOG("Ghost released");
        g_ghost = nullptr;
        g_ghostName[0] = 0;
    }
    PatchRacerAI(false);
    g_ghostCollide = false;
}

// Race standings: the other player's name (GRacerInfo::mName, +0x08) and progress on their racer.
// Every frame from the moment their racer is ours - also during the intro cutscene and while spawn
// protection holds their car - so the standings show them right from the countdown (the progress that
// decides the order is put in before the game sorts, see hk_RacerUpdate in coopwin.inl).
static void PeerRacerInfoTick() {
    static char peerName[32];
    const std::string& pn = Net::Stats().peerName;
    if (pn.empty() || !g_ghost) return;
    NetUtil::CopyHudName(peerName, pn.c_str());       // HUD slots are 16 bytes
    void* rs = nullptr;
    Mem::Read(0x91E000, rs);
    void* sim = VCall<void* (__thiscall*)(void*)>(g_ghost, IV_GetSimable)(g_ghost);
    if (!rs || !sim) return;
    void* ri = ((void*(__thiscall*)(void*, void*))0x5E8B20)(rs, sim);  // GRaceStatus::GetRacerInfo(ISimable*)
    static void* loggedRi = nullptr;
    if (ri != loggedRi) { loggedRi = ri; LOG("Race: peer racer info %p (ghost %p, kind %d) name %s", ri, g_ghost, (int)g_ghostKind, peerName); }
    if (!ri) return;
    const char* nm = peerName; Mem::Write((uintptr_t)ri + 0x08, nm);
    // (+0x21 is not written: it isn't reliably "finished" and ranked a parked peer 1st)
    float used = 0;
    if (!WritePeerProgress(ri, &used)) return;
    static DWORD nextLog = 0;
    if (GetTickCount() >= nextLog) {
        nextLog = GetTickCount() + 5000;
        const CarState& ls = Net::Latest().s;
        int rank = 0; Mem::Read((uintptr_t)ri + 0x10, rank);
        void* mri = RacerInfoOfLocal();
        int myRank = 0; float myPct = 0;
        if (mri) { Mem::Read((uintptr_t)mri + 0x10, myRank); Mem::Read((uintptr_t)mri + 0x18, myPct); }
        LOG("Race: peer pct %.3f (sent %.3f) lap %.3f laps %d cps %d fin %d -> rank %d | me pct %.3f rank %d",
            used, ls.racePct, ls.lapPct, (int)ls.laps, (int)ls.checkpoints, (int)ls.finished, rank, myPct, myRank);
    }
}

// Race start: both games put their own player on the player's grid slot, so on each screen the other
// player's car would stand inside ours. The client moves off it. Collisions stay off until the two
// cars' collision boxes (enlarged by 25%) no longer overlap; the remote car is still shown at its real position.
// Where to: the client's AI racers follow the host's (traffic.inl), so the host's grid is the one both
// screens show - the client belongs on the slot the HOST's race gave the client's racer (the replaced AI
// racer: where the host's intro cutscene shows them). Up to 1.7.8 the client took the slot ITS OWN race
// built the host's racer on, as if both races put the two swapped racers on the same slot; they don't
// always, and then the client stood on a slot the host's grid has an AI racer on (often further back)
// while the slot meant for them stayed empty.
// Host: decides where the client starts and tells them (PKT_GRIDSLOT, for a minute from the start or a
// restart): the slot of the client's racer, or - no racer for them in this race - a free slot read from
// its own grid once it stands there (intro over).
static void HostGridSlotTick(const Net::Snapshot& latest) {
    static uint32_t forEvent = 0; static uint8_t lastRestart = 0; static DWORD sendUntil = 0, nextSend = 0;
    if (!g_cfg.isHost || !g_cfg.separateGridSlots) return;
    uint32_t ev = Game::CurrentEventHash();
    uint8_t restart = 0; Mem::Read(0x91CF28, restart);
    DWORD now = GetTickCount();
    if (ev != forEvent) { forEvent = ev; g_clientSlotEvent = 0; sendUntil = 0; }
    if (restart && !lastRestart && g_clientSlotEvent && g_clientSlotEvent == ev) { sendUntil = now + 60000; nextSend = 0; }
    lastRestart = restart;
    if (!ev) return;
    if (!g_clientSlotEvent) {
        bool fromRacer = g_ghostKind == GK_RACER && g_ghost == g_replacedRacer && g_peerSlotValid;
        float along = 0.f, side = 0.f; int n = 0;
        // decided once we stand on the grid (intro over) with them on our spot. The slot read when the race
        // built their racer is not always where the grid has room for them (seen 24 m behind the grid with an
        // empty slot next to us, 10.3.2): it is taken only if no car stands on it and no free slot of our
        // grid is nearer to us; else that free slot.
        if (Game::RaceStage() != 2 || !latest.valid || !latest.s.inRace || latest.s.eventHash != ev) return;
        void* lrb = RigidBodyOf(LocalVehicle());
        if (!lrb) return;
        const float* lp = RB_GetVec(lrb, RB_GetPosition);
        const float* lv = RB_GetVec(lrb, RB_GetLinearVelocity);
        float mySpeed = sqrtf(lv[0] * lv[0] + lv[1] * lv[1] + lv[2] * lv[2]);
        if (!NetUtil::SharingStartSpot(lp, mySpeed, latest.s.pos, latest.s.speedMps)) return;
        float freeSlot[3] = {};
        bool haveFree = FreeGridSlotNear(lrb, freeSlot, along, side, n);
        if (fromRacer) {
            bool taken = false;
            void** lb = nullptr;
            for (int i = 0, total = ListSize(VL_ALL, &lb); i < total && !taken; ++i) {
                void* iv = ListGet(VL_ALL, i);
                if (!iv || iv == LocalVehicle() || iv == g_ghost || !IsVehicle(iv)) continue;
                void* orb = RigidBodyOf(iv);
                const float* q = orb ? RB_GetVec(orb, RB_GetPosition) : nullptr;
                if (q && NetUtil::Dist3(q, g_peerSlot) < 2.5f) taken = true;
            }
            float racerD = NetUtil::Dist3(lp, g_peerSlot), freeD = haveFree ? NetUtil::Dist3(lp, freeSlot) : 1e9f;
            if (taken || (haveFree && freeD + 1.f < racerD) || !(racerD < 40.f)) {
                static uint32_t rejectLoggedFor = 0;   // this runs every frame until a slot is decided
                if (rejectLoggedFor != ev) {
                    rejectLoggedFor = ev;
                    LOG("Start: the slot our race gave the client's racer is %.1f m from us%s - %s", racerD,
                        taken ? " with a car on it" : "", haveFree ? "a nearer free slot of our grid instead" : "no free slot either");
                }
                fromRacer = false;
            }
        }
        if (fromRacer) memcpy(g_clientSlot, g_peerSlot, sizeof(g_clientSlot));
        else {
            if (!haveFree) {
                static uint32_t loggedFor = 0;
                if (loggedFor != ev) { loggedFor = ev; LOG("Start: no free slot on our grid for the client (%d car(s) around) - their game decides", n); }
                return;
            }
            memcpy(g_clientSlot, freeSlot, sizeof(g_clientSlot));
        }
        g_clientSlotEvent = ev; g_clientSlotRacer = fromRacer; sendUntil = now + 60000; nextSend = 0;
        { char cars[512]; GridCarsText(lrb, cars, sizeof(cars)); LOG("Start: cars around us (m ahead/right): %s", cars); }
        if (fromRacer) LOG("Start: the client starts on the slot our race gave their racer %.1f %.1f %.1f - telling them", g_clientSlot[0], g_clientSlot[1], g_clientSlot[2]);
        else LOG("Start: the client starts on a free slot of our grid, %.1f m %s, %.1f m %s of us (%d car(s) around) - telling them",
                 fabsf(along), along < 0 ? "behind" : "ahead", fabsf(side), side < 0 ? "left" : "right", n);
    }
    if (g_clientSlotEvent == ev && sendUntil && (int)(now - sendUntil) < 0 && (int)(now - nextSend) >= 0) {
        Net::SendGridSlot(ev, g_clientSlot, g_clientSlotRacer);
        nextSend = now + 500;
    }
}

// Client: moves there once both stand still on the player's slot (NetUtil::PickStartSlot); waits a moment
// for the host's word, and without it (an older host) takes the slot of the host's racer here as before.
static void TakePeerSlotTick(const Net::Snapshot& latest, void* ghostRb) {
    static uint32_t winEvent = 0; static void* slotOf = nullptr; static uint8_t lastRestart = 0;
    static DWORD windowUntil = 0, sharingSince = 0;
    if (g_cfg.isHost || !g_cfg.separateGridSlots) return;
    uint32_t ev = Game::CurrentEventHash();
    uint8_t restart = 0; Mem::Read(0x91CF28, restart);
    DWORD now = GetTickCount();
    // a new event, a new racer for them or a restart (the game puts everyone back on the grid) opens the window
    if (ev != winEvent || slotOf != g_replacedRacer || (restart && !lastRestart)) {
        winEvent = ev; slotOf = g_replacedRacer; windowUntil = ev ? now + 60000 : 0; sharingSince = 0;
    }
    lastRestart = restart;
    if (!windowUntil || (int)(now - windowUntil) > 0) return;
    void* rs = nullptr; int mode = 0;
    if (!Mem::Read(0x91E000, rs) || !rs || !Mem::Read((uintptr_t)rs + 0x1960, mode) || mode != 1) return;
    if (!latest.valid || !latest.s.hasCar || !latest.s.inRace || latest.s.eventHash != ev) return;
    void* lrb = RigidBodyOf(LocalVehicle());
    if (!lrb) return;
    const float* lp = RB_GetVec(lrb, RB_GetPosition);
    const float* lv = RB_GetVec(lrb, RB_GetLinearVelocity);
    float mySpeed = sqrtf(lv[0] * lv[0] + lv[1] * lv[1] + lv[2] * lv[2]);
    if (!NetUtil::SharingStartSpot(lp, mySpeed, latest.s.pos, latest.s.speedMps)) { sharingSince = 0; return; }
    if (!sharingSince) sharingSince = now ? now : 1;
    float hostSlot[3] = {}; bool hostRacer = false;
    bool haveHost = Net::PeerGridSlot(ev, hostSlot, &hostRacer) && NetUtil::ShouldTakePeerSlot(lp, mySpeed, latest.s.pos, latest.s.speedMps, hostSlot);
    bool haveOwn = g_ghostKind == GK_RACER && g_ghost == g_replacedRacer && g_peerSlotValid &&
                   NetUtil::ShouldTakePeerSlot(lp, mySpeed, latest.s.pos, latest.s.speedMps, g_peerSlot);
    NetUtil::StartSlot pick = NetUtil::PickStartSlot(haveHost, haveOwn, (int)(now - sharingSince));
    if (pick == NetUtil::StartSlot::Wait) return;
    windowUntil = 0;
    if (pick == NetUtil::StartSlot::None) {
        LOG("Start: no grid slot from the host and none of our own - the other player is shown at their real position");
        return;
    }
    const float* slot = pick == NetUtil::StartSlot::Host ? hostSlot : g_peerSlot;
    float from[3] = { lp[0], lp[1], lp[2] };
    // the slot's height was read when the race placed that racer, before its suspension settled: on the
    // same grid (level within 1 m) keep our own settled height, else the car dropped onto the road at the start
    float slotY = fabsf(slot[1] - lp[1]) < 1.f ? lp[1] : slot[1] + 0.05f;
    float to[3] = { slot[0], slotY, slot[2] }, zero[3] = { 0, 0, 0 };
    {   // the grid as we see it (from the player's spot, before the move) and where the slot is on it
        char cars[512]; GridCarsText(lrb, cars, sizeof(cars));
        float f[3]; VCall<void(__thiscall*)(void*, float*)>(lrb, 13 /*GetForwardVector*/)(lrb, f);
        float fl = sqrtf(f[0] * f[0] + f[2] * f[2]), dx = to[0] - from[0], dz = to[2] - from[2];
        if (fl > 0.1f) LOG("Start: grid around the player's spot (m ahead/right): %s; our slot %.1f/%.1f", cars,
                           (dx * f[0] + dz * f[2]) / fl, (dx * f[2] - dz * f[0]) / fl);
    }
    RB_SetVec(lrb, RB_SetPosition, to);                 // grid slots face the same way: keep our heading
    RB_SetVec(lrb, RB_SetLinearVelocity, zero); RB_SetVec(lrb, RB_SetAngularVelocity, zero);
    if (ghostRb) {                                      // their car onto the slot we left (where they are)
        RB_SetVec(ghostRb, RB_SetOrientationQuat, latest.s.rot);
        RB_SetVec(ghostRb, RB_SetPosition, latest.s.pos);
        RB_SetVec(ghostRb, RB_SetLinearVelocity, zero); RB_SetVec(ghostRb, RB_SetAngularVelocity, zero);
    }
    {   // reported: the host saw us drop from above at the start. How high over the road we were and are now.
        float gFrom = 0, gTo = 0;
        bool hf = GroundHeightAt(from, gFrom), ht = GroundHeightAt(to, gTo);
        LOG("Start: height over the road %s m before the move, %s m on the slot (our ride height %.2f m)",
            hf ? std::to_string(from[1] - gFrom).substr(0, 5).c_str() : "?", ht ? std::to_string(to[1] - gTo).substr(0, 5).c_str() : "?", g_myRide);
    }
    if (pick == NetUtil::StartSlot::Host)
        LOG("Start: we stood on the host's grid slot - moved to %s (%.1f m)",
            hostRacer ? "the slot the host's race gave our racer" : "the free slot the host gave us", NetUtil::Dist3(from, to));
    else
        LOG("Start: we stood on the host's grid slot - moved to the slot of the host's racer here (%.1f m, no word from the host)", NetUtil::Dist3(from, to));
    SnapToGroundSafe("on our grid slot");   // the slot's height can be the host car's, above our own road height
}

static void UpdateGhostImpl(const Net::Snapshot& latest, const Net::Snapshot& prev, bool enabled, char* out, size_t outLen) {
#define RET(msg) do { strncpy_s(out, outLen, msg, _TRUNCATE); return; } while (0)
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    float dt = (float)(now.QuadPart - g_lastQpc.QuadPart) / (float)g_qpf.QuadPart;
    g_lastQpc = now;
    if (dt > 0.1f) dt = 0.1f;

    if (!enabled) { Game::ReleaseGhost(); RET("disabled (F9)"); }
    if (Game::GameFlowState() != 6) { g_ghost = nullptr; Game::ReleaseGhost(); for (auto& o : g_aiOv) o.iv = nullptr; RET("waiting (not in game)"); }
    // The game may delete our copy of their car (to build a race car) and build another car in its memory:
    // then the pointer is someone else's car (seen: the rival's GTI). Let go of it, never touch it again.
    // Also drop a racer whose address was reused, or whose model is no longer the other player's.
    if (g_ghost && !StillOurCar(g_ghost)) {
        LOG("Ghost: the game replaced our copy of their car with another car - letting go of it");
        ForgetCar(g_ghost);
        g_ghost = nullptr; g_ghostKind = GK_NONE; g_spawnedKey = g_spawnedHash = 0;
    }
    // Drop the ghost if the game removed that car.
    if (g_ghost && !ListContains(g_ghostList, g_ghost)) {
        LOG("Ghost car left its vehicle list (despawned)");
        g_ghost = nullptr;
        Game::ReleaseGhost();
    }
    {   // their pause menu / a short gap reports "no car" while they're still in the world: keep the car
        // standing where it was for a while instead of removing it and spawning it again (each new spawn
        // can come out wrong, and it reappeared next to us)
        static DWORD noCarSince = 0;
        bool noCar = !latest.valid || !latest.s.hasCar;
        if (!noCar) noCarSince = 0;
        else {
            if (!noCarSince) noCarSince = GetTickCount();
            bool stillInWorld = latest.valid && latest.s.gameFlowState == 6;
            if (g_ghost && stillInWorld && GetTickCount() - noCarSince < 120000) {
                if (void* rb = RigidBodyOf(g_ghost)) { float z[3] = { 0, 0, 0 }; RB_SetVec(rb, RB_SetLinearVelocity, z); RB_SetVec(rb, RB_SetAngularVelocity, z); }
                RET("paused there - car kept in place");
            }
            Game::ReleaseGhost(); RET("waiting (peer not driving)");
        }
    }



    // Using a borrowed traffic car but a spawn retry is due: give it back and try spawning the real car.
    if (g_ghost && g_ghostKind == GK_TRAFFIC && Net::PeerCar() && GetTickCount() >= g_nextSpawnTry) Game::ReleaseGhost();

    // An event started (from free roam): the race built a racer as the peer's car. Use that one and
    // drop our free-roam copy, or the race would have the peer twice (one of them AI-driven).
    if (g_ghost && g_ghostKind != GK_RACER && g_replacedRacer == g_ghost && ListContains(VL_AIRACERS, g_ghost)) {
        g_ghostKind = GK_RACER; g_ghostList = VL_AIRACERS; DisableAI(g_ghost);   // same object reused by the race
        LOG("Race: the ghost is now the racer built as the peer's car");
    }
    // (only a racer the code below would take: otherwise it lets go of our copy and spawns it again, every frame)
    if (g_ghost && g_ghostKind != GK_RACER && g_replacedRacer && g_replacedRacer != g_ghost &&
        ListContains(VL_AIRACERS, g_replacedRacer) && RigidBodyOf(g_replacedRacer) && StillOurCar(g_replacedRacer) &&
        CoopRace::MayAcquireGhostRacer(true, ModelKeyOf(g_replacedRacer), Net::PeerCar() ? Net::PeerCar()->carKey : 0, Net::PeerCar() != nullptr)) {
        LOG("Race: switching the ghost to the racer built as the peer's car");
        Game::ReleaseGhost();
    }

    // The peer switched cars: respawn ours as their new model / tuning.
    const CarInfo* pc = Net::PeerCar();
    if (g_ghost && pc && !CoopRace::GhostModelIsPeers(ModelKeyOf(g_ghost), pc->carKey, true)) {
        LOG("Ghost: this car is model %08X, the other player's is %08X - letting go (will spawn their car)",
            ModelKeyOf(g_ghost), pc->carKey);
        Game::ReleaseGhost();
    }
    if (g_ghost && g_ghostKind == GK_RACER && g_replacedRacer != g_ghost) {
        LOG("Ghost: racer %p is not the factory-built peer racer - letting go", g_ghost);
        Game::ReleaseGhost();
    }
    {   // their car info never came (seen once over a VPN): show them in a car like ours meanwhile
        static CarInfo fallback{}; static DWORD waitSince = 0; static bool told = false;
        if (pc) { waitSince = 0; told = false; }
        else if (!waitSince) waitSince = GetTickCount();
        else if (GetTickCount() - waitSince > 6000) {
            CarInfo me{};
            if (Game::ReadLocalCarInfo(me)) {
                fallback = me; fallback.hasCustom = 0; memset(fallback.custom, 0, sizeof(fallback.custom)); fallback.customHash = me.carKey;
                pc = &fallback;
                if (!told) { told = true; LOG("Ghost: no car info from the other player after 6 s - showing them in a stock car like ours (key %08X)", me.carKey); }
            }
        }
    }
    if (g_ghost && g_ghostKind == GK_SPAWNED && pc && (pc->carKey != g_spawnedKey || pc->customHash != g_spawnedHash)) {
        LOG("Peer changed car - respawning");
        Game::ReleaseGhost();
    }

    if (!g_ghost) {
        void* iv = nullptr;
        // 1) in a race: only the racer the factory hook built as the peer's own model. Never an
        // ordinary AI racer or traffic car (that showed the wrong model at their position).
        if (g_replacedRacer && ListContains(VL_AIRACERS, g_replacedRacer) && RigidBodyOf(g_replacedRacer) &&
            CoopRace::MayAcquireGhostRacer(true, ModelKeyOf(g_replacedRacer), pc ? pc->carKey : 0, pc != nullptr) &&
            StillOurCar(g_replacedRacer))
            iv = g_replacedRacer;
        if (iv) {
            g_ghostList = VL_AIRACERS;
            g_ghostKind = GK_RACER;
        } else if (GetTickCount() < g_noSpawnUntil) {
            RET("event starting...");
        } else if (pc && GetTickCount() >= g_nextSpawnTry) {
            // 2) free roam: spawn the peer's own car (their model + tuning) where they are
            float dir[3] = { 1, 0, 0 };
            float vlen = sqrtf(latest.s.linVel[0] * latest.s.linVel[0] + latest.s.linVel[2] * latest.s.linVel[2]);
            if (vlen > 1.f) { dir[0] = latest.s.linVel[0] / vlen; dir[2] = latest.s.linVel[2] / vlen; }
            // where they really are, also on our own spot (an event with no start slot for them, e.g. a toll
            // booth): the new car's collisions are switched off below in the same frame, before any physics
            // step, and stay off until the two collision boxes (enlarged by 25%) no longer overlap
            float spawnPos[3] = { latest.s.pos[0], latest.s.pos[1], latest.s.pos[2] };
            iv = SpawnVehicle(pc->carKey, pc->hasCustom ? pc->custom : nullptr, spawnPos, dir, DRV_REMOTE);
            if (!iv && pc->hasCustom) {
                // Diagnose: is it the customization record that Construct rejects, or the model itself?
                iv = SpawnVehicle(pc->carKey, nullptr, spawnPos, dir, DRV_REMOTE);
                LOG("Spawn of %08X with customization failed; stock retry %s", pc->carKey, iv ? "SUCCEEDED (custom record rejected)" : "failed too");
            }
            if (!iv) {
                // Usually: that car model isn't loaded in this world. Retry later; borrow traffic meanwhile.
                ++g_spawnFails;
                g_ghostStarved = true;
                g_nextSpawnTry = GetTickCount() + 1500;
                LOG("Spawning peer car %08X failed (attempt %d) - freeing room, retry in 1.5 s", pc->carKey, g_spawnFails);
                RET("peer car model not loaded yet - retrying");
            }
            {   // face their real heading (the spawn direction alone points north when they stand still)
                void* srb = RigidBodyOf(iv);
                float q[4] = { latest.s.rot[0], latest.s.rot[1], latest.s.rot[2], latest.s.rot[3] };
                if (srb && (q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]) > 0.5f)
                    VCall<void(__thiscall*)(void*, const float*)>(srb, RB_SetOrientationQuat)(srb, q);
            }
            g_ghostList = -1; // any list
            g_ghostKind = GK_SPAWNED;
            g_spawnedKey = pc->carKey;
            g_spawnedHash = pc->customHash;
        } else if (!pc) {
            RET("waiting for peer car info");
        } else {
            // No borrowing of AI traffic cars any more: their AI keeps driving them away.
            RET(g_spawnFails ? "peer car spawn failed - retrying" : "spawning peer car...");
        }
        g_ghost = iv;
        if (g_ghostKind == GK_RACER && iv == g_replacedRacer && !g_peerSlotValid) {
            // the race places its racers after building them: read the slot now, before we move this car
            if (void* rb0 = RigidBodyOf(iv)) {
                const float* p0 = RB_GetVec(rb0, RB_GetPosition);
                if (p0 && (fabsf(p0[0]) + fabsf(p0[1]) + fabsf(p0[2])) > 1.f) {
                    memcpy(g_peerSlot, p0, 12); g_peerSlotValid = true;
                    LOG("Race: the other player's grid slot %.1f %.1f %.1f (where the race placed their racer)", p0[0], p0[1], p0[2]);
                }
            }
        }
        if (g_ghostKind == GK_RACER) DisableAI(iv);
        strncpy_s(g_ghostName, VehName(iv), _TRUNCATE);
        SetCollisions(iv, false);
        g_ghostCollide = false;
        g_protectHoldReset = true;
        LOG("Ghost acquired: %s %p (%s)", g_ghostKind == GK_RACER ? (iv == g_replacedRacer ? "racer built as peer car" : "AI racer")
                                          : g_ghostKind == GK_SPAWNED ? "spawned peer car" : "traffic car", iv, g_ghostName);
    }
    PeerRacerInfoTick();

    void* rb = RigidBodyOf(g_ghost);
    if (!rb) {
        // A car we just spawned gets its physics body a moment later: wait instead of giving up.
        if (g_ghostKind == GK_SPAWNED) {
            static void* loggedFor = nullptr;
            if (loggedFor != g_ghost) {
                loggedFor = g_ghost;
                void* r = Find(g_ghost, Addr::H_IRigidBody);
                LOG("Spawned car %p: IRigidBody %p vtable %08X (accepted: %08X / %08X)", g_ghost, r,
                    r ? (unsigned)VTable(r) : 0u, (unsigned)Addr::VT_RBVehicle_IRigid, (unsigned)Addr::VT_Simple_IRigid);
            }
            RET("peer car loading...");
        }
        Game::ReleaseGhost(); RET("ghost car invalid");
    }

    SetStreamPeer(rb);   // keep the world (and full physics) loaded around the other player too
    HostGridSlotTick(latest);                           // (the client's slot is sent during the intro too)
    if (NisActive()) { g_protectHoldReset = true; ClearCarControls(g_ghost); sprintf_s(out, outLen, "%s  (cutscene)", g_ghostName); return; }
    TakePeerSlotTick(latest, rb);

    // --- target state: snapshot interpolation ~InterpDelayMs in the past (smooth, no guessing) ---
    CarState s = latest.s;
    double extrapMs = 0;
    if (!Net::SampleRemote((double)g_cfg.interpDelayMs, s, &extrapMs)) s = latest.s;

    const float* cur = RB_GetVec(rb, RB_GetPosition);
    float err = Dist(cur, s.pos);

    // Collisions stay off at a race start, after a teleport, and during a network hitch, so a
    // remote car landing on ours is not flung. The car is always synced to its real (interpolated)
    // pose; only EnableObjectCollisions is switched. They come back on once packets are flowing,
    // the 3 s start-teleport hold has ended, and the two collision boxes (enlarged by 25%) no
    // longer overlap.
    void* lrbP = RigidBodyOf(LocalVehicle());
    // Re-arm the 3 s hold whenever either car teleports (event start, reset, respawn: a jump of
    // more than 30 m), a new ghost is acquired, a cutscene runs, or the event / restart changes.
    // The two games reach the grid ~0.4 s apart: without the hold, overlap would see the partner
    // still far away and turn collisions back on before their start teleport lands.
    {
        static float lastTarget[3], lastLocal[3]; static bool haveLast = false;
        static DWORD holdUntil = 0;
        static uint32_t lastEv = 0; static uint8_t lastRestart = 0;
        const float* lpos = lrbP ? RB_GetVec(lrbP, RB_GetPosition) : nullptr;
        bool peerJump  = haveLast && Dist(lastTarget, s.pos) > 30.f;
        bool localJump = haveLast && lpos && Dist(lastLocal, lpos) > 30.f;
        uint32_t ev = Game::CurrentEventHash();
        uint8_t restart = 0; Mem::Read(0x91CF28, restart);
        bool eventStart = ev && ev != lastEv;
        bool raceRestart = restart && !lastRestart;
        lastEv = ev; lastRestart = restart;
        // a long jump of the other player (teleported to an event, a milestone pursuit, the safe house):
        // build their car again at the new place once the world there has streamed in, instead of moving
        // the old one there (it came out without a model)
        if (haveLast && Dist(lastTarget, s.pos) > 300.f && g_ghostKind == GK_SPAWNED) {
            LOG("Ghost: the other player jumped %.0f m - building their car again there", Dist(lastTarget, s.pos));
            memcpy(lastTarget, s.pos, 12);
            Game::ReleaseGhost();
            g_nextSpawnTry = GetTickCount() + 1500;
            return;
        }
        if (peerJump || localJump || g_protectHoldReset || eventStart || raceRestart) {
            holdUntil = GetTickCount() + 3000;
            g_protectHoldReset = false;
            const char* why = peerJump ? "peer jumped" : localJump ? "local car jumped"
                             : eventStart ? "event start" : raceRestart ? "restart" : "new ghost / cutscene";
            LOG("Collisions held off for 3 s: %s", why);
        }
        memcpy(lastTarget, s.pos, 12);
        if (lpos) memcpy(lastLocal, lpos, 12);
        haveLast = true;
        g_protectHolding = GetTickCount() < holdUntil;
    }

    // Hard-set the exact interpolated transform + velocities every frame. Because consecutive targets
    // come from one smooth curve, the car moves smoothly; setting velocities makes the physics step
    // between our frames carry it along the same path instead of fighting us.
    RB_SetVec(rb, RB_SetOrientationQuat, s.rot);
    RB_SetVec(rb, RB_SetPosition, s.pos);
    RB_SetVec(rb, RB_SetLinearVelocity, s.linVel);
    RB_SetVec(rb, RB_SetAngularVelocity, s.angVel);
    float* newPos = s.pos;
    if (g_cfg.syncControls) ApplyCarControls(g_ghost, s);
    ApplyCarDamage(g_ghost, s);

    // jitter meter: how far each frame's position is from a straight-line prediction made from the
    // previous two frames, scaled by their frame times (so uneven frame rates don't count). RMS, mm.
    {
        static float p1[3], p2[3]; static double t1 = 0, t2 = 0; static int have = 0;
        static double acc = 0; static int n = 0; static float rms = 0;
        double t0 = Net::LocalClockMs();               // simulation time of this frame
        if (have >= 2 && t1 > t2 && t0 > t1) {
            double r = (t0 - t1) / (t1 - t2);
            float d2 = 0;
            for (int i = 0; i < 3; ++i) { float pred = p1[i] + (float)((p1[i] - p2[i]) * r); float e = newPos[i] - pred; d2 += e * e; }
            if (d2 < 1.0f) { acc += d2; ++n; }   // ignore teleports
        }
        memcpy(p2, p1, 12); memcpy(p1, newPos, 12); t2 = t1; t1 = t0; if (have < 2) ++have;
        if (n >= 60) { rms = (float)sqrt(acc / n) * 1000.f; acc = 0; n = 0; }
        g_jitterMm = rms;
        g_extrapMs = (float)extrapMs;
    }

    // --- collisions: off at start / teleport / hitch; on once the enlarged boxes no longer overlap ---
    DWORD nowTick = GetTickCount();
    bool hitch = HitUtil::StateHitch(latest.valid && latest.s.hasCar, nowTick - latest.recvTick);
    float sep = -1.f;
    bool tooClose = false;
    if (void* lrb = RigidBodyOf(LocalVehicle())) {
        const float* lp = RB_GetVec(lrb, RB_GetPosition);
        sep = Dist(lp, newPos);
        const float* lq = RB_GetVec(lrb, RB_GetOrientation);
        const float* gq = RB_GetVec(rb, RB_GetOrientation);
        if (lp && newPos) {
            float lh[3], gh[3];
            ReadHalfExtents(lrb, lh);
            ReadHalfExtents(rb, gh);
            HitUtil::Obb a, b;
            HitUtil::ObbFromPose(lp, lq, lh, a);
            HitUtil::ObbFromPose(newPos, gq, gh, b);
            tooClose = HitUtil::ObbsOverlapWithClearance(a, b);
        }
        if (sep >= 0.f && sep < 8.f) g_hitContactAt = GetTickCount();   // local contact: apply a received hit even if the ghost later lags
        // Momentum for the car we hit: their car is moved by the network here, so our impact only
        // stops US. When our velocity changes abruptly right next to their car (far more than braking
        // can do in one frame), send them what we lost; their game adds part of it to their own car.
        if (g_pushFrom) {        // being pushed: hold at least the fading push speed for 0.4 s
            const float* cv = RB_GetVec(lrb, RB_GetLinearVelocity);
            float nv[3];
            if (HitUtil::SustainPush(cv, g_push, GetTickCount() - g_pushFrom, HitUtil::HIT_PUSH_MS, nv))
                RB_SetVec(lrb, RB_SetLinearVelocity, nv);
            else if (GetTickCount() - g_pushFrom >= HitUtil::HIT_PUSH_MS) g_pushFrom = 0;
        }
        static float prevV[3], hitV0[3]; static DWORD prevT = 0;
        DWORD& lastHitSent = g_lastHitSent; DWORD& hitT0 = g_hitT0;
        const float* lv = RB_GetVec(lrb, RB_GetLinearVelocity);
        DWORD tnow = GetTickCount();
        // an impact takes several frames: note the velocity when it begins, send the total loss 250 ms later
        if ((int)(g_noHitUntil - tnow) > 0) hitT0 = 0;
        if (!hitT0 && (int)(g_noHitUntil - tnow) <= 0 && !g_pushFrom && g_ghostCollide && prevT && tnow - prevT < 100 && sep < 6.f && tnow - lastHitSent > 600) {
            float d0[3] = { prevV[0] - lv[0], 0, prevV[2] - lv[2] };
            if (sqrtf(d0[0] * d0[0] + d0[2] * d0[2]) > 3.f && HitPointsAt(d0, RB_GetVec(lrb, RB_GetPosition), newPos)) { hitT0 = tnow; memcpy(hitV0, prevV, 12); }
        }
        if (hitT0 && tnow - hitT0 >= 250) {
            float dv[3] = { hitV0[0] - lv[0], hitV0[1] - lv[1], hitV0[2] - lv[2] };
            float mag = sqrtf(dv[0] * dv[0] + dv[1] * dv[1] + dv[2] * dv[2]);
            hitT0 = 0; lastHitSent = tnow;
            if (mag > 4.f) { Net::SendHit(dv); LOG("Collision: hit the other player, lost %.0f km/h - sent to them", mag * 3.6f); }
        }
        memcpy(prevV, lv, 12); prevT = tnow;
    }
    bool wantCollide = HitUtil::WantPlayerCollide(g_cfg.ghostCollisions, hitch, g_protectHolding, tooClose, g_ghostCollide);
    if (wantCollide != g_ghostCollide) {
        SetCollisions(g_ghost, wantCollide);
        g_ghostCollide = wantCollide;
        const char* why = wantCollide ? "" : hitch ? " (hitch)" : g_protectHolding ? " (teleport)" : tooClose ? " (overlap)" : "";
        LOG("Ghost collisions %s%s", wantCollide ? "ON" : "OFF", why);
    } else if (!g_ghostCollide) {
        // keep them off: a race's start (grid placement, reset to the start positions, the countdown)
        // resets its racers' bodies and turns their object collisions back on - the two cars on one grid
        // spot then pushed each other up into a pile. Said again every 200 ms until they may come on.
        static DWORD lastOff = 0;
        if (GetTickCount() - lastOff > 200) { lastOff = GetTickCount(); SetCollisions(g_ghost, false); }
    }

    const char* offWhy = !g_cfg.ghostCollisions ? "" : hitch ? " (hitch)" : g_protectHolding ? " (teleport)" : tooClose ? " (overlap)" : "";
    sprintf_s(out, outLen, "%s  jitter %.1fmm  extrap %.0fms  dist %.0fm  err %.1fm  coll %s%s", g_ghostName, g_jitterMm, g_extrapMs, sep, err,
              g_ghostCollide ? "on" : "off",
              g_ghostCollide ? "" : offWhy);
    (void)prev;
#undef RET
}

static bool UpdateGhostSafe(const Net::Snapshot& latest, const Net::Snapshot& prev, bool enabled, char* out, size_t outLen) {
    __try {
        UpdateGhostImpl(latest, prev, enabled, out, outLen);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The other player's car sometimes shows a different 3D model after a while (seen: a traffic station
// wagon) while the game still calls it "cobaltss". Check its render connection once a second:
// FindCarRenderConn(worldId) 0x744D40, worldId = ISimable slot 28 (0x686AF0), conn+0x2C model type,
// conn+0x28 state (2+ = drawn). A changed or missing model -> respawn it. If the model is unchanged, the
// wrong car on screen is another vehicle on the same spot: log what is there.
static int GhostRenderTypeSafe(void* iv) {
    __try {
        void* sim = VCall<void* (__thiscall*)(void*)>(iv, IV_GetSimable)(iv);
        if (!sim) return -2;
        uint32_t wid = VCall<uint32_t(__thiscall*)(void*)>(sim, 28)(sim);
        char* conn = ((char*(__cdecl*)(uint32_t))0x744D40)(wid);
        if (!conn) return -1;
        if (*(int*)(conn + 0x28) < 2) return -3;                 // still loading
        return *(int*)(conn + 0x2C);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -2; }
}
static void LogCarsOnGhostSafe() {
    __try {
        void* grb = RigidBodyOf(g_ghost);
        if (!grb) return;
        const float* gp = RB_GetVec(grb, RB_GetPosition);
        void** b = nullptr;
        int n = ListSize(-1, &b);
        for (int i = 0; i < n; ++i) {
            void* v = ListGet(-1, i);
            if (!v || v == g_ghost || v == LocalVehicle()) continue;
            void* rb = RigidBodyOf(v);
            if (rb && Dist(RB_GetVec(rb, RB_GetPosition), gp) < 4.f)
                LOG("Ghost: another car on the other player's spot: %s %p (mirror %d)", VehName(v), v, IsMirrorCar(v) ? 1 : 0);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
static void GhostModelCheck() {
    static void* checked = nullptr; static int firstType = -9; static DWORD since = 0, next = 0, nextNear = 0;
    if (!g_ghost || (g_ghostKind != GK_SPAWNED && g_ghostKind != GK_RACER)) { checked = nullptr; return; }   // (racers too: a body kit lost in a race)
    DWORD now = GetTickCount();
    if (checked != g_ghost) { checked = g_ghost; firstType = -9; since = now; next = now + 1500; }
    if (now >= nextNear) { nextNear = now + 5000; LogCarsOnGhostSafe(); }
    if (now < next) return;
    next = now + 1000;
    int type = GhostRenderTypeSafe(g_ghost);
    if (type == -3 || type == -2) return;
    if (firstType == -9) { if (type >= 0) { firstType = type; LOG("Ghost: render model type %d", type); } return; }
    if (type != firstType && now - since > 3000) {
        // respawning doesn't help (the new car can get a wrong model at once): only report it, once per change
        LOG("Ghost: render model changed (%d -> %d)", firstType, type);
        firstType = type;
    }
}

std::string Game::UpdateGhost(const Net::Snapshot& latest, const Net::Snapshot& prev, bool enabled) {
    if (!g_ok) return "game not recognised";
    {   // an event is over: the next one (started from free roam, no world load) builds their racer again.
        // (Only reset on a world load, the next event kept "already built" and raced a plain AI in their
        //  slot - an extra opponent - while their car was spawned beside the race.)
        static uint32_t lastEv = 0;
        uint32_t ev = CurrentEventHash();
        if (lastEv && !ev) g_racerReplacedThisLoad = false;
        lastEv = ev;
    }
    char buf[256] = "";
    if (!UpdateGhostSafe(latest, prev, enabled, buf, sizeof(buf))) {
        LOG("UpdateGhost: exception - releasing ghost");
        g_ghost = nullptr;
        PatchRacerAI(false);
        return "error (see log)";
    }
    GhostModelCheck();
    // Write the ghost line to the log every 2 s so tests don't depend on screenshots.
    static DWORD lastStats = 0;
    if (g_ghost && GetTickCount() - lastStats > 2000) {
        lastStats = GetTickCount();
        LOG("Ghost stats: %s | ping %d ms", buf, Net::Stats().pingMs);
    }
    return buf;
}

static bool DebugSpawnCloneImpl(char* out, size_t n) {
    __try {
        void* me = LocalVehicle();
        void* rb = RigidBodyOf(me);
        if (!rb) { strcpy_s(out, n, "no local car"); return false; }
        const float* p = RB_GetVec(rb, RB_GetPosition);
        const float* q = RB_GetVec(rb, RB_GetOrientation);
        // forward = rotate (0,0,1)? MW world uses +X forward for car bodies in rigid body space;
        // use linear velocity direction if moving, else a fixed offset along X.
        float fwd[3] = { 1, 0, 0 };
        (void)q;
        float pos[3] = { p[0] + fwd[0] * 8.f, p[1] + fwd[1] * 8.f, p[2] + 1.0f };
        // Prefer the peer's (remembered) car: usually a model that isn't loaded here, which exercises
        // the vehicle-cache / budget path. Fall back to a clone of my own car.
        uint32_t key = VehicleKey(me);
        const void* cust = Customizations(me);
        if (const CarInfo* pc = Net::PeerCar()) { key = pc->carKey; cust = pc->hasCustom ? pc->custom : nullptr; }
        void* iv = SpawnVehicle(key, cust, pos, fwd, DRV_REMOTE);
        if (!iv) { sprintf_s(out, n, "spawn failed (key %08X)", key); return false; }
        g_clone = iv;
        sprintf_s(out, n, "spawned %s (key %08X, custom %s)", VehName(iv), key, cust ? "yes" : "no");
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        strcpy_s(out, n, "exception while spawning");
        return false;
    }
}

std::string Game::DebugSpawnClone() {
    if (!g_ok || GameFlowState() != 6) return "not in game";
    char buf[160];
    DebugSpawnCloneImpl(buf, sizeof(buf));
    LOG("DebugSpawnClone: %s", buf);
    return buf;
}

static void SafeKill(void* iv) {
    __try { KillVehicle(iv); } __except (EXCEPTION_EXECUTE_HANDLER) { LOG("KillVehicle exception"); }
}

std::string Game::DebugKillClone() {
    if (!g_clone) return "no clone";
    SafeKill(g_clone);
    g_clone = nullptr;
    return "clone removed";
}

std::string Game::DebugInfo() {
    if (!g_ok || GameFlowState() != 6) return "";
    void** b;
    char buf[160];
    sprintf_s(buf, "Cars: players %d  racers %d  cops %d  traffic %d\n",
              ListSize(VL_PLAYERS, &b), ListSize(VL_AIRACERS, &b), ListSize(VL_AICOPS, &b), ListSize(VL_AITRAFFIC, &b));
    return buf;
}
