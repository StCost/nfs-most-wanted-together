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

    // Player-vs-partner collisions stay off at a race start, after a teleport, and during a
    // network hitch, and come back on only once the two collision boxes no longer overlap.
    // Each box is scaled by this factor on every axis (25% of its half-extents on each side).
    constexpr float COLLIDE_EXTENT_SCALE = 1.25f;
    constexpr unsigned STATE_HITCH_MS = 400;   // no state packet for this long: a hitch (same as a stale NOS pose)
    // Fallback if IRigidBody::GetDimension fails: a typical car, half-extents in metres (X width, Y height, Z length).
    constexpr float kDefaultHalfExtent[3] = { 1.05f, 0.70f, 2.20f };

    inline bool ValidHalfExtents(const float* h) {
        if (!h) return false;
        for (int i = 0; i < 3; ++i)
            if (!(h[i] == h[i]) || !(h[i] > 0.05f) || !(h[i] < 15.f)) return false;
        return true;
    }

    inline bool StateHitch(bool stateValid, unsigned ageMs, unsigned hitchMs = STATE_HITCH_MS) {
        return !stateValid || ageMs >= hitchMs;
    }

    // The overlap test only decides when collisions come back on. Once on, they stay on when the boxes
    // overlap again (the cars are about to touch): turning them off then let a car drive into the other.
    inline bool WantPlayerCollide(bool collisionsEnabled, bool hitch, bool teleportHold, bool boxesTooClose, bool collidingNow) {
        if (!collisionsEnabled || hitch || teleportHold) return false;
        return collidingNow || !boxesTooClose;
    }

    // Oriented box: pos is the centre, axis[i] is the unit local axis i, half[i] is the half-extent on it.
    struct Obb {
        float pos[3];
        float axis[3][3];
        float half[3];
    };

    inline void Norm3(float* v) {
        float len = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (len > 1e-6f) { v[0] /= len; v[1] /= len; v[2] /= len; }
        else { v[0] = 1.f; v[1] = 0.f; v[2] = 0.f; }
    }

    inline float Dot3h(const float* a, const float* b) {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    }

    // quat is (x,y,z,w). Local X/Y/Z become the three world axes. A broken quat becomes identity.
    inline void ObbFromPose(const float* pos, const float* quat, const float* half, Obb& out) {
        for (int i = 0; i < 3; ++i) {
            out.pos[i] = pos ? pos[i] : 0.f;
            out.half[i] = (half && half[i] == half[i] && half[i] > 0.f) ? half[i] : kDefaultHalfExtent[i];
        }
        float x = 0, y = 0, z = 0, w = 1.f;
        if (quat) { x = quat[0]; y = quat[1]; z = quat[2]; w = quat[3]; }
        float n = x * x + y * y + z * z + w * w;
        if (!(n > 0.25f) || !(n < 4.f)) { x = y = z = 0; w = 1.f; n = 1.f; }
        float s = 2.f / n;
        out.axis[0][0] = 1.f - s * (y * y + z * z);
        out.axis[0][1] = s * (x * y + w * z);
        out.axis[0][2] = s * (x * z - w * y);
        out.axis[1][0] = s * (x * y - w * z);
        out.axis[1][1] = 1.f - s * (x * x + z * z);
        out.axis[1][2] = s * (y * z + w * x);
        out.axis[2][0] = s * (x * z + w * y);
        out.axis[2][1] = s * (y * z - w * x);
        out.axis[2][2] = 1.f - s * (x * x + y * y);
        for (int i = 0; i < 3; ++i) Norm3(out.axis[i]);
    }

    inline bool AxisSeparated(const float* n, const Obb& a, const Obb& b) {
        float n2 = Dot3h(n, n);
        if (!(n2 > 1e-12f)) return false;
        float d[3] = { b.pos[0] - a.pos[0], b.pos[1] - a.pos[1], b.pos[2] - a.pos[2] };
        float dist = fabsf(Dot3h(d, n));
        float ra = fabsf(Dot3h(a.axis[0], n)) * a.half[0] + fabsf(Dot3h(a.axis[1], n)) * a.half[1] + fabsf(Dot3h(a.axis[2], n)) * a.half[2];
        float rb = fabsf(Dot3h(b.axis[0], n)) * b.half[0] + fabsf(Dot3h(b.axis[1], n)) * b.half[1] + fabsf(Dot3h(b.axis[2], n)) * b.half[2];
        return dist > ra + rb;
    }

    // True if the two boxes overlap or touch (no clearance). Touching faces count as overlap.
    inline bool ObbsOverlap(const Obb& a, const Obb& b) {
        for (int i = 0; i < 3; ++i) {
            if (AxisSeparated(a.axis[i], a, b)) return false;
            if (AxisSeparated(b.axis[i], a, b)) return false;
        }
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                float n[3] = {
                    a.axis[i][1] * b.axis[j][2] - a.axis[i][2] * b.axis[j][1],
                    a.axis[i][2] * b.axis[j][0] - a.axis[i][0] * b.axis[j][2],
                    a.axis[i][0] * b.axis[j][1] - a.axis[i][1] * b.axis[j][0]
                };
                if (AxisSeparated(n, a, b)) return false;
            }
        }
        return true;
    }

    // Hitting a copy of a car the other game owns (one of its cops, traffic cars, roadblock cars): the copy
    // is moved by the owner's snapshots, so to our physics it is an unmovable wall and our car stopped dead.
    // Give back what a hit on a real car of about our weight leaves us: all of the speed along the contact
    // (sliding past) and half of the speed into it (we shove it and go on with it). n: from our car to the
    // other one. False when the hit took nothing back to give (or the data is bad).
    constexpr float MIRROR_HIT_KEEP_INTO = 0.5f;
    constexpr float MIRROR_HIT_MIN_LOSS_MPS = 3.f;
    inline bool MirrorHitKeepSpeed(const float* before, const float* after, const float* n, float* out) {
        if (!Finite3(before) || !Finite3(after) || !Finite3(n)) return false;
        float nx = n[0], nz = n[2], nl = std::sqrt(nx * nx + nz * nz);
        if (!(nl > 1e-3f)) return false;
        nx /= nl; nz /= nl;
        float lx = before[0] - after[0], lz = before[2] - after[2];
        if (!(std::sqrt(lx * lx + lz * lz) >= MIRROR_HIT_MIN_LOSS_MPS)) return false;
        float into = before[0] * nx + before[2] * nz;           // speed towards the other car before the hit
        float bt[2] = { before[0] - into * nx, before[2] - into * nz };   // along the contact
        float keepInto = into > 0.f ? into * MIRROR_HIT_KEEP_INTO : into;
        out[0] = bt[0] + keepInto * nx;
        out[1] = after[1];
        out[2] = bt[1] + keepInto * nz;
        float bs = std::sqrt(before[0] * before[0] + before[2] * before[2]);
        float os = std::sqrt(out[0] * out[0] + out[2] * out[2]);
        float as = std::sqrt(after[0] * after[0] + after[2] * after[2]);
        if (os > bs && os > 0.f) { out[0] *= bs / os; out[2] *= bs / os; os = bs; }
        return os > as + 0.5f;                                   // only ever gives speed back
    }

    // Overlap test used to re-enable collisions: both boxes are enlarged by COLLIDE_EXTENT_SCALE.
    inline bool ObbsOverlapWithClearance(const Obb& a, const Obb& b, float scale = COLLIDE_EXTENT_SCALE) {
        Obb aa = a, bb = b;
        if (!(scale > 0.f)) scale = 1.f;
        for (int i = 0; i < 3; ++i) { aa.half[i] *= scale; bb.half[i] *= scale; }
        return ObbsOverlap(aa, bb);
    }
    // A parked remote car: the received pose still wobbles by a fraction of a millimetre (sensor-level
    // physics noise on their machine plus float rounding), which shows as a visible shiver. While the car
    // is stopped (target speed and spin ~0) a pose within 1 mm / ~0.06 deg of the one we hold is not
    // applied - the car stays put. Any real movement (speed, spin, or a bigger step) takes the new pose.
    constexpr float STILL_SPEED_MPS = 0.05f;      // 0.18 km/h
    constexpr float STILL_SPIN_RPS  = 0.02f;
    constexpr float STILL_DEADBAND_M = 0.001f;    // 1 mm
    constexpr float STILL_ROT_DOT = 0.9999995f;   // |q . held| above this: under ~0.06 deg apart
    struct StillHold { bool have = false; float pos[3] = { 0, 0, 0 }; float rot[4] = { 0, 0, 0, 1 }; };
    // true: keep the held pose (write it into pos / rot); false: the new pose is taken and held from now
    inline bool HoldStill(StillHold& h, float pos[3], float rot[4], const float lin[3], const float ang[3]) {
        float v2 = lin[0] * lin[0] + lin[1] * lin[1] + lin[2] * lin[2];
        float w2 = ang[0] * ang[0] + ang[1] * ang[1] + ang[2] * ang[2];
        bool still = v2 < STILL_SPEED_MPS * STILL_SPEED_MPS && w2 < STILL_SPIN_RPS * STILL_SPIN_RPS;
        if (still && h.have) {
            float dx = pos[0] - h.pos[0], dy = pos[1] - h.pos[1], dz = pos[2] - h.pos[2];
            float dot = std::fabs(rot[0] * h.rot[0] + rot[1] * h.rot[1] + rot[2] * h.rot[2] + rot[3] * h.rot[3]);
            if (dx * dx + dy * dy + dz * dz < STILL_DEADBAND_M * STILL_DEADBAND_M && dot > STILL_ROT_DOT) {
                for (int i = 0; i < 3; ++i) pos[i] = h.pos[i];
                for (int i = 0; i < 4; ++i) rot[i] = h.rot[i];
                return true;
            }
        }
        for (int i = 0; i < 3; ++i) h.pos[i] = pos[i];
        for (int i = 0; i < 4; ++i) h.rot[i] = rot[i];
        h.have = true;
        return false;
    }
}
