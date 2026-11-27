#pragma once
// Pure helpers shared by the network and game layers. Nothing here touches the game or the socket,
// so the test suite (tests\) covers all of it without the game.
#include <cstdint>
#include <cstring>
#include <cmath>
#include <algorithm>
#include "net.h"

namespace NetUtil {
    // ---- names ------------------------------------------------------------------------------
    // The HUD standings copy racer names into 16-byte slots: 15 characters + NUL.
    constexpr int HUD_NAME_SIZE = 16;
    inline void CopyHudName(char* dst /*[HUD_NAME_SIZE] or larger*/, const char* src) {
        int i = 0;
        if (src) for (; i < HUD_NAME_SIZE - 1 && src[i]; ++i) dst[i] = src[i];
        dst[i] = 0;
    }

    // ---- traffic ids ------------------------------------------------------------------------
    // Each side numbers the cars it owns 1..0x7FFF. On the wire the client's ids carry bit 15, so
    // both sides can tell whose car an entry is (and never mirror their own cars back).
    constexpr uint16_t CLIENT_ID_BIT = 0x8000;
    inline uint16_t TrafficWireId(uint16_t localId, bool senderIsHost) {
        return senderIsHost ? localId : (uint16_t)(localId | CLIENT_ID_BIT);
    }
    inline bool TrafficIdIsClients(uint16_t wireId) { return (wireId & CLIENT_ID_BIT) != 0; }
    // the host accepts only the client's cars, the client only the host's
    inline bool TrafficIdAccepted(uint16_t wireId, bool weAreHost) { return weAreHost == TrafficIdIsClients(wireId); }
    // next local id after `id` (wraps to 1, never 0 and never with the client bit)
    inline uint16_t NextTrafficLocalId(uint16_t id) {
        ++id;
        return (id == 0 || id >= CLIENT_ID_BIT) ? (uint16_t)1 : id;
    }

    // CollectOwned flags: bit 0 = cop, bit 1 = racer. A racer can also still sit in the
    // traffic list; if the same car is sent twice (racer then traffic) the client used to
    // treat it as traffic, bind a traffic car to that id, and leave the real racer stacked.
    inline int TrafficRoleRank(uint16_t flags) {
        if (flags & 2) return 2;
        if (flags & 1) return 1;
        return 0;
    }
    inline uint16_t PreferTrafficFlags(uint16_t a, uint16_t b) {
        return TrafficRoleRank(b) > TrafficRoleRank(a) ? b : a;
    }
    struct OwnedCarCand { uintptr_t key; float d; uint16_t flags; };
    inline int DedupOwnedCars(OwnedCarCand* c, int n) {
        if (!c || n <= 0) return 0;
        int w = 0;
        for (int i = 0; i < n; ++i) {
            if (!c[i].key) continue;
            int found = -1;
            for (int j = 0; j < w; ++j) if (c[j].key == c[i].key) { found = j; break; }
            if (found < 0) { c[w++] = c[i]; continue; }
            c[found].flags = PreferTrafficFlags(c[found].flags, c[i].flags);
            if (c[i].d < c[found].d) c[found].d = c[i].d;
        }
        return w;
    }
    // Same id twice in one packet: keep the higher-rank role and that entry's motion.
    inline int DedupTrafficEntries(TrafficEntry* e, int count) {
        if (!e || count <= 0) return 0;
        int w = 0;
        for (int i = 0; i < count; ++i) {
            int found = -1;
            for (int j = 0; j < w; ++j) if (e[j].id == e[i].id) { found = j; break; }
            if (found < 0) { e[w++] = e[i]; continue; }
            if (TrafficRoleRank(e[i].flags) > TrafficRoleRank(e[found].flags)) e[found] = e[i];
            else e[found].flags = PreferTrafficFlags(e[found].flags, e[i].flags);
        }
        return w;
    }
    // The mirror must drop its old car when the role changes (a bound traffic car must
    // not be driven as a racer).
    inline bool TrafficRoleChanged(uint16_t oldFlags, bool hadBound, uint16_t newFlags) {
        if (TrafficRoleRank(oldFlags) != TrafficRoleRank(newFlags)) return true;
        if (hadBound && (newFlags & 2)) return true;
        return false;
    }

    // ---- shared traffic inside races -----------------------------------------------------------
    // In a race the client never deletes its own traffic (the race scripts own some of those cars).
    // Instead each host traffic car is bound to one of the client's own traffic cars, which then
    // follows the host's car. A candidate is one of the client's own traffic cars; `skip` marks the
    // ones that may not be taken (already bound, too close to a player, ...).
    struct TrafficCandidate { uint32_t model; float pos[3]; bool skip; bool trailer = false; };   // trailer: pulls one (semi truck)
    // A truck with a trailer is only shown by a car with a trailer and the other way round.
    // want: 1 = with, 0 = without, -1 = not known (anything goes).
    inline bool TrailerMatches(int want, bool has) { return want < 0 || (want != 0) == has; }
    // Index of the best partner for a host car of `model` at `pos`: a car of the same model first
    // (nearest of those), otherwise the nearest car of any model when `anyModel`. -1 if none.
    // wantTrailer: see TrailerMatches.
    inline int PickTrafficPartner(uint32_t model, const float* pos, const TrafficCandidate* c, int n, bool anyModel, int wantTrailer = -1) {
        int best = -1, bestAny = -1;
        float bestD = 0, bestAnyD = 0;
        for (int i = 0; i < n; ++i) {
            if (c[i].skip || !TrailerMatches(wantTrailer, c[i].trailer)) continue;
            float dx = c[i].pos[0] - pos[0], dy = c[i].pos[1] - pos[1], dz = c[i].pos[2] - pos[2];
            float d = dx * dx + dy * dy + dz * dz;
            if (!(d == d) || d > 150.f * 150.f) continue;                      // NaN, or too far to jump in unnoticed
            if (c[i].model == model && (best < 0 || d < bestD)) { best = i; bestD = d; }
            if (bestAny < 0 || d < bestAnyD) { bestAny = i; bestAnyD = d; }
        }
        return best >= 0 ? best : (anyModel ? bestAny : -1);
    }

    // ---- smooth traffic handover (client, free roam) ------------------------------------------
    // When the client comes near the host, the host's cars may only appear where the player can't see
    // it happen, and the client's own cars only go once they are off screen. The camera is a view
    // cone in game coordinates (y up); `valid` false = the camera is unknown (then pos is the player).
    struct CamView { float pos[3]; float dir[3]; bool valid; };
    constexpr float HANDOVER_VIEW_COS   = 0.342f;  // cos 70 deg: the cone counted as "in view" (wider than the game's FOV)
    constexpr float HANDOVER_FAR_M      = 180.f;   // farther from the camera than this a car may appear even in view
    constexpr float HANDOVER_NEAR_M     = 30.f;    // never appear closer to the player than this (look-back, mirror)
    constexpr float HANDOVER_CAR_M      = 3.5f;    // a car's radius: a car half inside the cone is in view
    constexpr float HANDOVER_OVERLAP_M  = 6.f;     // two cars closer than this would sit inside each other
    constexpr float HANDOVER_GONE_M     = 500.f;   // beyond this our own car may go even if (a few pixels) on screen
    constexpr float HANDOVER_OFFSCREEN_S = 0.3f;   // off screen at least this long before a car is removed

    inline float Dist3(const float* a, const float* b) {
        float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
        return sqrtf(dx * dx + dy * dy + dz * dz);
    }
    // Could the camera see a car at p? Only within farM; an unknown camera sees everything within farM.
    inline bool InCameraView(const CamView& c, const float* p, float farM) {
        float d[3] = { p[0] - c.pos[0], p[1] - c.pos[1], p[2] - c.pos[2] };
        float dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (!(dist == dist)) return true;                       // NaN: assume seen
        if (dist > farM) return false;
        if (!c.valid || dist <= HANDOVER_CAR_M) return true;
        float dl = sqrtf(c.dir[0] * c.dir[0] + c.dir[1] * c.dir[1] + c.dir[2] * c.dir[2]);
        if (!(dl > 1e-3f)) return true;                         // no direction: assume seen
        float cosA = (d[0] * c.dir[0] + d[1] * c.dir[1] + d[2] * c.dir[2]) / (dist * dl);
        cosA = cosA > 1.f ? 1.f : cosA < -1.f ? -1.f : cosA;
        float margin = asinf(HANDOVER_CAR_M / dist);           // the car's own size widens the cone
        return acosf(cosA) <= acosf(HANDOVER_VIEW_COS) + margin;
    }

    // ---- do not evict / recycle a car either player can still see --------------------------------
    // The 20-car budget (and our own cull when the client comes near the host) used to take cars
    // that were right in front of someone, or only visible in the rear-view / mirrors. A car stays
    // if it is close to a player, in their forward cone, or in the rear cone. `dir` is the camera
    // heading, or the car heading when the camera is unknown (the other player's camera is not here).
    constexpr float EVICT_CLOSE_M     = 40.f;
    constexpr float EVICT_FRONT_FAR_M = 180.f;
    constexpr float EVICT_REAR_FAR_M  = 80.f;
    constexpr float EVICT_FRONT_COS   = 0.342f;  // cos 70 deg (same half-cone as handover)
    constexpr float EVICT_REAR_COS    = 0.5f;    // cos 60 deg behind (mirrors / look-back)
    struct PlayerView { float pos[3]; float dir[3]; bool valid; };

    inline bool InHeadingCone(const float* origin, const float* dir, const float* p, float farM, float minCos) {
        if (!origin || !dir || !p || !(farM > 0.f)) return false;
        float d[3] = { p[0] - origin[0], p[1] - origin[1], p[2] - origin[2] };
        float dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (!(dist == dist) || dist > farM) return false;
        if (dist <= HANDOVER_CAR_M) return true;
        float dl = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
        if (!(dl > 1e-3f)) return false;
        float cosA = (d[0] * dir[0] + d[1] * dir[1] + d[2] * dir[2]) / (dist * dl);
        return cosA >= minCos;
    }
    inline bool CarVisibleToPlayer(const PlayerView& v, const float* carPos) {
        if (!v.valid || !carPos) return false;
        float dist = Dist3(carPos, v.pos);
        if (!(dist == dist)) return true;                 // NaN: keep the car
        if (dist <= EVICT_CLOSE_M) return true;
        if (InHeadingCone(v.pos, v.dir, carPos, EVICT_FRONT_FAR_M, EVICT_FRONT_COS)) return true;
        float back[3] = { -v.dir[0], -v.dir[1], -v.dir[2] };
        if (InHeadingCone(v.pos, back, carPos, EVICT_REAR_FAR_M, EVICT_REAR_COS)) return true;
        return false;
    }
    inline bool CarProtectedFromEvict(const PlayerView* views, int nViews, const float* carPos) {
        if (!views || nViews <= 0 || !carPos) return false;
        for (int i = 0; i < nViews; ++i)
            if (CarVisibleToPlayer(views[i], carPos)) return true;
        return false;
    }
    // May a host car be created at p now without the player seeing it pop in: not right next to the
    // player, and outside the camera's view or far away.
    inline bool MaySpawnUnseen(const CamView& c, const float* p, const float* player) {
        if (player && !(Dist3(p, player) >= HANDOVER_NEAR_M)) return false;   // (NaN too)
        return !InCameraView(c, p, HANDOVER_FAR_M);
    }
    // May one of the client's own traffic cars be removed? The game's per-car screen timers
    // (IVehicle::GetOnScreenTime / GetOffScreenTime): never while on screen, only once it has been off
    // screen for a moment (both timers 0 = not drawn at all), or when it is far beyond drawing range.
    inline bool OwnCarMayGo(float onScreenS, float offScreenS, float distToCamM) {
        if (distToCamM >= HANDOVER_GONE_M) return true;
        if (!(onScreenS <= 0.f)) return false;                  // on screen (or garbage): keep
        return offScreenS <= 0.f || offScreenS >= HANDOVER_OFFSCREEN_S;
    }
    // A host car whose spot is in view can take over one of our own cars instead: one of the same model
    // within sameM, else any model within anyM (the car jumps that far: keep it short). -1 if none.
    inline int PickTakeoverPartner(uint32_t model, const float* pos, const TrafficCandidate* c, int n, float sameM, float anyM, int wantTrailer = -1) {
        int best = -1, bestAny = -1;
        float bestD = 0, bestAnyD = 0;
        for (int i = 0; i < n; ++i) {
            if (c[i].skip || !TrailerMatches(wantTrailer, c[i].trailer)) continue;
            float d = Dist3(c[i].pos, pos);
            if (!(d == d)) continue;
            if (c[i].model == model && d <= sameM && (best < 0 || d < bestD)) { best = i; bestD = d; }
            if (d <= anyM && (bestAny < 0 || d < bestAnyD)) { bestAny = i; bestAnyD = d; }
        }
        return best >= 0 ? best : bestAny;
    }

    // ---- no new cars in front of the other player ([Traffic] NoSpawnNearPlayers) ---------------
    // The game picks spots for new traffic cars and cops around its OWN player only, so it happily puts
    // one right in front of the other player, who then crashes into it. A spot is refused when it is
    // close to that player's car and in front of it (their camera looks along the car), in any
    // direction when very close. The car's heading comes as up to two directions (the way the body
    // points, the way it moves); y is up, the cone is measured flat (x, z).
    constexpr float NOSPAWN_RADIUS_M   = 60.f;   // farther than this a new car is fine anywhere
    constexpr float NOSPAWN_ANY_M      = 15.f;   // closer than this: never, whatever the direction
    constexpr float NOSPAWN_VIEW_COS   = 0.5f;   // cos 60 deg: the half-cone counted as "in front"
    constexpr float NOSPAWN_CAM_BACK_M = 6.f;    // the chase camera sits about this far behind the car
    inline bool SpawnSpotBlocked(const float* p, const float* car, const float* dirs, int nDirs) {
        float d = Dist3(p, car);
        if (!(d == d)) return false;                            // NaN spot: not ours to judge
        if (d >= NOSPAWN_RADIUS_M) return false;
        if (d < NOSPAWN_ANY_M) return true;
        bool anyDir = false;
        for (int k = 0; k < nDirs; ++k) {
            const float* f = dirs + 3 * k;
            float fl = sqrtf(f[0] * f[0] + f[2] * f[2]);
            if (!(fl > 1e-3f)) continue;                        // no usable direction (or NaN)
            anyDir = true;
            float fx = f[0] / fl, fz = f[2] / fl;
            // seen from where the camera is (behind the car): the cone also covers the car's flanks
            float vx = p[0] - (car[0] - fx * NOSPAWN_CAM_BACK_M), vz = p[2] - (car[2] - fz * NOSPAWN_CAM_BACK_M);
            float vl = sqrtf(vx * vx + vz * vz);
            if (!(vl > 1e-3f)) return true;
            if ((vx * fx + vz * fz) / vl >= NOSPAWN_VIEW_COS) return true;
        }
        return !anyDir;                                          // heading unknown: the whole circle counts
    }
    // A host car (or the other player's cop) we haven't created here yet may appear now: not inside
    // our camera's view within NOSPAWN_RADIUS_M (else it waits until it is out of view or farther).
    // A copy of one of the other player's cops (TrafficEntry flags: 1 cop, 4 in a pursuit, 0x10 in a roadblock).
    // One chasing in a pursuit or standing in their roadblock must be here: otherwise a player hits a car the
    // other one can't see (host side: the client's cops were never built while the host's world was full or the
    // spot was in view, and the client seemed to crash into air there). Such a copy gets room at the 20-car
    // limit (one of our unseen traffic cars goes), and a chasing one may appear in view.
    inline bool CopCopyNeedsRoom(uint16_t flags) { return (flags & 1) && (flags & (4 | 0x10)); }
    inline bool CopCopyMayAppearInView(uint16_t flags) { return (flags & 1) && (flags & 4); }
    inline bool MirrorMayAppear(const CamView& c, const float* p) {
        return !InCameraView(c, p, NOSPAWN_RADIUS_M);
    }

    // ---- AI catch-up in co-op ----------------------------------------------------------------
    // The race % the AI catch-up measures itself against: the leading player. ourPct < 0 = we have no
    // racer (leave the game alone: false). The partner counts while `peerOk`.
    inline bool CatchUpLeaderPct(float ourPct, bool peerOk, float peerPct, float& out) {
        if (!(ourPct >= 0.f)) return false;
        out = ourPct;
        if (peerOk && peerPct == peerPct && peerPct > out) out = peerPct > 100.f ? 100.f : peerPct;
        return true;
    }

    // ---- interpolation math -----------------------------------------------------------------
    inline void Lerp3(const float* a, const float* b, float t, float* o) {
        for (int i = 0; i < 3; ++i) o[i] = a[i] + (b[i] - a[i]) * t;
    }

    // Normalized lerp of two quaternions (x,y,z,w), taking the short way round.
    inline void NlerpQ(const float* a, const float* b0, float t, float* o) {
        float b[4] = { b0[0], b0[1], b0[2], b0[3] };
        if (a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3] < 0) for (float& x : b) x = -x;
        float len = 0;
        for (int i = 0; i < 4; ++i) { o[i] = a[i] + (b[i] - a[i]) * t; len += o[i] * o[i]; }
        len = sqrtf(len);
        if (len > 1e-6f) for (int i = 0; i < 4; ++i) o[i] /= len;
    }

    // Cubic Hermite between (p0, v0) and (p1, v1), `dt` seconds apart, at u in [0,1].
    inline float Hermite(float p0, float v0, float p1, float v1, float dt, float u) {
        float u2 = u * u, u3 = u2 * u;
        float h00 = 2 * u3 - 3 * u2 + 1, h10 = u3 - 2 * u2 + u, h01 = -2 * u3 + 3 * u2, h11 = u3 - u2;
        return h00 * p0 + h10 * dt * v0 + h01 * p1 + h11 * dt * v1;
    }

    // ---- race standings ------------------------------------------------------------------------
    // The other player's race % (GRacerInfo +0x18, 0..100 over the whole race) as it is at sender time
    // `now`, from two of their samples (pctOld at tOld, pctNew at tNew, ms in their clock). The newest
    // value is moved on at the rate between the two samples, for at most `maxAheadMs` (late packets),
    // never backwards (a restart drops the % to 0: no negative rate) and never past 100.
    inline float RacePctAt(float pctOld, double tOld, float pctNew, double tNew, double now, double maxAheadMs = 250.0) {
        if (!(pctNew == pctNew)) return 0.f;
        float out = pctNew;
        double span = tNew - tOld, ahead = now - tNew;
        if (span >= 20.0 && ahead > 0.0 && pctOld == pctOld) {
            double rate = (pctNew - pctOld) / span;              // % per ms
            if (rate > 0.0 && rate < 0.05) {                     // 0.05 %/ms = a whole race in 2 s: anything faster is a jump
                if (ahead > maxAheadMs) ahead = maxAheadMs;
                out = (float)(pctNew + rate * ahead);
            }
        }
        if (out > 100.f) out = 100.f;
        if (out < 0.f) out = 0.f;
        return out;
    }

    // Grid slot sharing at a race start. Both games put their own player on the player's grid slot,
    // so the two players stand on the same spot; the client moves its car to the slot its game built
    // the host's racer on (where the host's game has the client's racer), so the two cars are apart
    // on both screens from the first frame. True when that move is due: both cars stand still on the
    // same spot (< 3 m) and the other player's slot is a nearby, different one (2..40 m away).
    inline bool Finite3(const float* v) { for (int i = 0; i < 3; ++i) if (!(fabsf(v[i]) <= 1.0e6f)) return false; return true; }
    // both players stand still on one spot (< 3 m): the player's slot both games gave their own player
    inline bool SharingStartSpot(const float* myPos, float mySpeed, const float* peerPos, float peerSpeed) {
        if (!Finite3(myPos) || !Finite3(peerPos)) return false;
        if (!(mySpeed < 1.f) || !(peerSpeed < 1.f)) return false;      // the countdown: nobody is moving yet
        return Dist3(myPos, peerPos) < 3.f;
    }
    inline bool ShouldTakePeerSlot(const float* myPos, float mySpeed, const float* peerPos, float peerSpeed, const float* slotPos) {
        if (!Finite3(slotPos) || !SharingStartSpot(myPos, mySpeed, peerPos, peerSpeed)) return false;
        float slot = Dist3(myPos, slotPos);
        return slot > 2.f && slot < 40.f;
    }
    // Which slot the client moves to while both stand on the player's slot. The client's AI racers follow
    // the host's (traffic.inl), so the host's grid is the one both screens show:
    //  Host - the slot the host's game gives us (PKT_GRIDSLOT: its race built our racer there, or a free
    //         slot of its grid) - where the host and its intro cutscene have us
    //  Own  - no word from the host for a moment (an older host): the slot our game built the host's racer
    //         on, as before - right only when both races put the two swapped racers on the same slot
    //  None - neither: the other player is shown at their real position (collisions stay off until they are apart)
    enum class StartSlot { Wait, Host, Own, None };
    constexpr int kHostSlotWaitMs = 2500;
    inline StartSlot PickStartSlot(bool hostSlot, bool ownSlot, int sharingMs) {
        if (hostSlot) return StartSlot::Host;
        if (sharingMs < kHostSlotWaitMs) return StartSlot::Wait;
        return ownSlot ? StartSlot::Own : StartSlot::None;
    }

    // ---- validation of what the network hands us ----------------------------------------------
    // These floats go straight into the game's rigid bodies: a NaN / infinity / absurd value from a
    // broken or hostile peer must never get that far.
    inline bool FiniteWithin(const float* v, int n, float limit) {
        for (int i = 0; i < n; ++i) if (!(fabsf(v[i]) <= limit)) return false;   // false for NaN too
        return true;
    }
    constexpr float MAX_POS = 1.0e6f, MAX_VEL = 1.0e4f, MAX_SIMTIME_S = 1.0e9f;
    inline bool ValidMotion(const float* pos, const float* rot, const float* linVel, const float* angVel, bool realRotation = true) {
        if (!FiniteWithin(pos, 3, MAX_POS) || !FiniteWithin(rot, 4, 2.0f) ||
            !FiniteWithin(linVel, 3, MAX_VEL) || !FiniteWithin(angVel, 3, MAX_VEL)) return false;
        float q = rot[0] * rot[0] + rot[1] * rot[1] + rot[2] * rot[2] + rot[3] * rot[3];
        return !realRotation || (q > 0.5f && q < 1.5f);          // a real rotation (a zero quaternion would stay zero after nlerp)
    }
    inline bool ValidCarState(const CarState& s) {
        return ValidMotion(s.pos, s.rot, s.linVel, s.angVel, s.hasCar != 0) &&   // no car: all zeros is fine
               FiniteWithin(&s.speedMps, 1, MAX_VEL) && FiniteWithin(&s.simTime, 1, MAX_SIMTIME_S) &&
               FiniteWithin(&s.steer, 1, 1.5f) &&
               FiniteWithin(&s.racePct, 1, 101.f) && s.racePct >= -1.f &&
               FiniteWithin(&s.lapPct, 1, 101.f) && s.lapPct >= -1.f &&
               s.laps >= 0 && s.laps <= 200 && s.checkpoints >= 0 && s.checkpoints <= 2000 &&
               s.heat100 <= MAX_HEAT100 && s.coolLeft10 <= MAX_COOL_LEFT10 && !(s.pursuitFlags & ~PURSUIT_KNOWN_BITS) &&
               s.pursuitBar >= -100 && s.pursuitBar <= 100 && s.pursuitBarKind <= 2;
    }
    inline bool ValidTrafficEntry(const TrafficEntry& e) {
        return ValidMotion(e.pos, e.rot, e.linVel, e.angVel);
    }
    inline bool ValidTrailerEntry(const TrailerEntry& e) {
        return !(e.flags & ~TRAILER_KNOWN_BITS) && ValidMotion(e.pos, e.rot, e.linVel, e.angVel);
    }

    // ---- semi truck trailers ------------------------------------------------------------------
    // A trailer is a car of its own that its truck builds and hitches (RBTractor). Our copy of the
    // other game's truck builds its own trailer; we move that trailer with the owner's trailer states
    // (PKT_TRAILER), interpolated exactly like the truck itself.
    struct MotionKey { double t; float pos[3], rot[4], linVel[3], angVel[3]; };
    // Append a key (time t in the sender's clock) to a ring of `cap`, oldest dropped when full. A key
    // not newer than the newest one is a late / duplicate packet: false, nothing changes.
    inline bool PushMotionKey(MotionKey* ring, int& n, int cap, const MotionKey& k) {
        if (n < 0 || n > cap || cap <= 0) n = 0;
        if (n && !(k.t > ring[n - 1].t)) return false;
        if (n == cap) { memmove(&ring[0], &ring[1], (cap - 1) * sizeof(MotionKey)); n = cap - 1; }
        ring[n++] = k;
        return true;
    }
    // The motion at `renderMs` (sender clock), the way traffic copies are driven: Hermite between the
    // two keys around it, else the newest key moved on by its velocity for at most 250 ms (older than
    // that: held still, velocities 0 - the owner's game is paused). False when there are no keys.
    inline bool SampleMotion(const MotionKey* k, int n, double renderMs, float* pos, float* rot, float* vel, float* av) {
        if (!k || n <= 0) return false;
        const MotionKey* a = nullptr; const MotionKey* b = nullptr;
        for (int i = n - 1; i >= 0; --i)
            if (k[i].t <= renderMs) { a = &k[i]; b = (i + 1 < n) ? &k[i + 1] : nullptr; break; }
        if (a && b) {
            double span = b->t - a->t;
            float u = span > 0 ? (float)((renderMs - a->t) / span) : 1.f;
            float dt = (float)(span / 1000.0);
            for (int i = 0; i < 3; ++i) {
                pos[i] = Hermite(a->pos[i], a->linVel[i], b->pos[i], b->linVel[i], dt, u);
                vel[i] = a->linVel[i] + (b->linVel[i] - a->linVel[i]) * u;
                av[i]  = a->angVel[i] + (b->angVel[i] - a->angVel[i]) * u;
            }
            NlerpQ(a->rot, b->rot, u, rot);
            return true;
        }
        const MotionKey& l = k[n - 1];
        float ex = (float)((renderMs - l.t) / 1000.0);
        if (ex < 0) ex = 0;
        bool stale = ex > 0.25f;
        if (ex > 0.25f) ex = 0.25f;
        for (int i = 0; i < 3; ++i) {
            pos[i] = l.pos[i] + l.linVel[i] * ex;
            vel[i] = stale ? 0.f : l.linVel[i];
            av[i]  = stale ? 0.f : l.angVel[i];
        }
        memcpy(rot, l.rot, 16);
        return true;
    }
    // The owner's trailer states for one truck count while the newest is younger than this (sender
    // clock vs the newest traffic packet of that truck); else the truck's own hitch pulls our trailer.
    constexpr double TRAILER_FRESH_MS = 1000.0;
    inline bool TrailerDataFresh(int nKeys, double newestTrailerMs, double newestTruckMs) {
        return nKeys > 0 && newestTruckMs - newestTrailerMs <= TRAILER_FRESH_MS;
    }
    // A hitched trailer's centre is at most ~10 m from its truck's centre (trailer ~13 m long, hitched
    // over the truck's rear axle). Farther (or NaN) = it came off / was left behind: put it back.
    constexpr float TRAILER_LOST_M = 16.f;
    inline bool TrailerLost(const float* truckPos, const float* trailerPos) {
        return !(Dist3(truckPos, trailerPos) <= TRAILER_LOST_M);
    }

    // ---- the partner marker (marker.inl projects, overlay.cpp draws) ----
    // Where a point in game space lands with the game's view-projection matrix M (row vectors; render space is
    // (z, -x, y)). On screen: x / y as screen fractions (y down). Off screen: dirX / dirY, the way to it from
    // the centre in screen fractions (y down) - (c0, c1) keeps the true side even behind the camera, where only
    // the divide by w flips it; behind is biased down.
    struct MarkerPlace { bool onScreen; float x, y, dirX, dirY; };
    inline MarkerPlace ProjectMarker(const float* M, const float* gamePos, float up) {
        const float r[4] = { gamePos[2], -gamePos[0], gamePos[1] + up, 1.f };
        float c[4];
        for (int j = 0; j < 4; ++j) c[j] = r[0] * M[j] + r[1] * M[4 + j] + r[2] * M[8 + j] + r[3] * M[12 + j];
        MarkerPlace p{ false, 0.f, 0.f, 0.f, 0.f };
        if (c[3] > 0.05f) {
            float sx = (c[0] / c[3] + 1) * 0.5f, sy = (1 - c[1] / c[3]) * 0.5f;
            if (sx >= 0.f && sx <= 1.f && sy >= 0.f && sy <= 1.f) { p.onScreen = true; p.x = sx; p.y = sy; return p; }
        }
        p.dirX = c[0]; p.dirY = -c[1];
        if (c[3] <= 0.05f) {
            float len = std::sqrt(p.dirX * p.dirX + p.dirY * p.dirY);
            p.dirY = std::fabs(p.dirY) + 0.35f * (len > 1e-3f ? len : 1.f);
        }
        return p;
    }
    // full size up to 60 m, 70 % from 400 m
    inline float MarkerScale(float distM) {
        return distM <= 60.f ? 1.f : distM >= 400.f ? 0.7f : 1.f - 0.3f * (distM - 60.f) / 340.f;
    }
    // The box the marker stays in (pixels): clear of the screen sides, below the rear-view mirror, above the
    // pursuit bars. On screen the place is clamped into it (so leaving the screen doesn't jump); off screen it
    // is where the line from the centre along (dirX, dirY) meets the box. Out: the centre and the unit
    // direction the tip points (down over a car). False: no direction.
    inline bool MarkerOnBox(const MarkerPlace& p, float W, float H, float* cx, float* cy, float* fx, float* fy) {
        const float left = 0.06f * H, right = W - 0.06f * H, top = 0.22f * H, bottom = 0.66f * H;
        if (p.onScreen) {
            *cx = p.x * W; *cy = p.y * H; *fx = 0.f; *fy = 1.f;
            *cx = *cx < left ? left : *cx > right ? right : *cx;
            *cy = *cy < top ? top : *cy > bottom ? bottom : *cy;
            return true;
        }
        float dx = p.dirX * W, dy = p.dirY * H, len = std::sqrt(dx * dx + dy * dy);
        if (!(len > 1e-3f)) return false;
        dx /= len; dy /= len;
        float t = 1e9f;
        if (std::fabs(dx) > 1e-4f) t = (std::min)(t, (dx > 0 ? right - W / 2 : W / 2 - left) / std::fabs(dx));
        if (std::fabs(dy) > 1e-4f) t = (std::min)(t, (dy > 0 ? bottom - H / 2 : H / 2 - top) / std::fabs(dy));
        *cx = W / 2 + dx * t; *cy = H / 2 + dy * t; *fx = dx; *fy = dy;
        return true;
    }
}
