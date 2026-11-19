#pragma once
// Pure helpers shared by the network and game layers. Nothing here touches the game or the socket,
// so the test suite (tests\) covers all of it without the game.
#include <cstdint>
#include <cstring>
#include <cmath>
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

    // ---- validation of what the network hands us ----------------------------------------------
    // These floats go straight into the game's rigid bodies: a NaN / infinity / absurd value from a
    // broken or hostile peer must never get that far.
    inline bool FiniteWithin(const float* v, int n, float limit) {
        for (int i = 0; i < n; ++i) if (!(fabsf(v[i]) <= limit)) return false;   // false for NaN too
        return true;
    }
    constexpr float MAX_POS = 1.0e6f, MAX_VEL = 1.0e4f, MAX_SIMTIME_S = 1.0e9f;
    inline bool ValidMotion(const float* pos, const float* rot, const float* linVel, const float* angVel) {
        return FiniteWithin(pos, 3, MAX_POS) && FiniteWithin(rot, 4, 2.0f) &&
               FiniteWithin(linVel, 3, MAX_VEL) && FiniteWithin(angVel, 3, MAX_VEL);
    }
    inline bool ValidCarState(const CarState& s) {
        return ValidMotion(s.pos, s.rot, s.linVel, s.angVel) &&
               FiniteWithin(&s.speedMps, 1, MAX_VEL) && FiniteWithin(&s.simTime, 1, MAX_SIMTIME_S);
    }
    inline bool ValidTrafficEntry(const TrafficEntry& e) {
        return ValidMotion(e.pos, e.rot, e.linVel, e.angVel);
    }
}
