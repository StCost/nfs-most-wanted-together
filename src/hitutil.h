#pragma once
// Collision impulse between players and with cars the other game owns. Decisions
// that do not need the game, so the test suite covers them (tests\test_util.cpp).
// game.cpp applies player hits (PKT_HIT); traffic.inl applies car hits (PKT_CARHIT).
#include <cmath>
#include <cstdint>

namespace HitUtil {
    // Applying a received player hit: do not require collisions to be on on our
    // copy of their car (the impact already happened on their machine). The old
    // 10 m check on the *current* ghost pose dropped host->client hits when the
    // host's snapshot was a few metres late (interp delay at speed). Use an
    // extrapolated pose, a longer window, or recent local contact instead.
    constexpr float HIT_APPLY_NEAR_M = 40.f;
    constexpr float HIT_EXTRAP_MAX_S = 0.5f;
    constexpr float HIT_STALE_S = 1.0f;
    constexpr unsigned HIT_PUSH_MS = 400;     // sustain, same as the player-car push
    constexpr float HIT_PUSH_SHARE = 0.6f;
    constexpr float HIT_PUSH_CAP_MPS = 45.f;  // ~160 km/h

    inline bool Finite3(const float* p) {
        return p && p[0] == p[0] && p[1] == p[1] && p[2] == p[2];
    }

    inline void ExtrapolatePos(const float* pos, const float* vel, float ageS, float* out) {
        float t = ageS;
        if (!(t >= 0.f)) t = 0.f;
        if (t > HIT_EXTRAP_MAX_S) t = HIT_EXTRAP_MAX_S;
        for (int i = 0; i < 3; ++i)
            out[i] = pos[i] + (vel && Finite3(vel) ? vel[i] * t : 0.f);
    }

    inline bool Near3(const float* a, const float* b, float maxDist) {
        if (!Finite3(a) || !Finite3(b) || !(maxDist > 0.f)) return false;
        float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
        float d2 = dx * dx + dy * dy + dz * dz;
        return d2 == d2 && d2 <= maxDist * maxDist;
    }

    // inWorld: in free roam / a race (flow 6). protectHold: a teleport / new ghost.
    // recentContact: we were next to them on THIS machine a moment ago (local contact).
    // peerPos/peerVel: their last known pose (ghost or snapshot); ageS is how old it is.
    inline bool ShouldApplyPeerHit(bool inWorld, bool protectHold, bool recentContact,
                                   const float* peerPos, const float* peerVel, float ageS,
                                   const float* localPos, float maxDist = HIT_APPLY_NEAR_M) {
        if (!inWorld || protectHold) return false;
        if (recentContact) return true;
        if (!peerPos || !localPos) return false;
        if (ageS > HIT_STALE_S) return false;
        float p[3];
        ExtrapolatePos(peerPos, peerVel, ageS, p);
        return Near3(p, localPos, maxDist);
    }

    inline float HitPushScale(float mag, float share = HIT_PUSH_SHARE, float cap = HIT_PUSH_CAP_MPS) {
        if (!(mag > 0.f) || !(share > 0.f)) return 0.f;
        return share * (mag > cap ? cap / mag : 1.f);
    }

    // Hold at least the fading push speed along the push for durationMs (player and remote-cop hits).
    // outVel is written only when a write is needed (current speed along the push is too low).
    inline bool SustainPush(const float* curVel, const float* push, unsigned elapsedMs,
                            unsigned durationMs, float* outVel) {
        if (!curVel || !push || !outVel || elapsedMs >= durationMs) return false;
        float f = 1.f - (float)elapsedMs / (float)durationMs;
        float plen = sqrtf(push[0] * push[0] + push[2] * push[2]);
        if (!(plen > 0.1f)) return false;
        float ux = push[0] / plen, uz = push[2] / plen;
        float along = curVel[0] * ux + curVel[2] * uz, want = plen * f;
        if (along >= want) return false;
        outVel[0] = curVel[0] + ux * (want - along);
        outVel[1] = curVel[1];
        outVel[2] = curVel[2] + uz * (want - along);
        return Finite3(outVel);
    }
}
