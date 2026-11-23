#pragma once
// Shared pursuits ([Coop] SharedPursuits): the decisions that don't need the game, so the test suite
// covers them (tests\test_pursuit.cpp). pursuit.inl reads the game, asks these, and does what they say.
#include <cstdint>
#include "net.h"

namespace SharedPursuit {
    // The game's own limit: a pursuit sees its perpetrator while some cop saw them in the last 7 s
    // (AIPursuit::OnTask compares the time since seen with the float at 0x890858 = 7.0).
    constexpr float SEEN_LIMIT_S = 7.0f;
    constexpr float HEAT_EPS = 0.02f;          // heat travels x100: smaller differences are rounding
    constexpr float DELAY_SLACK_S = 0.25f;     // cooldowns this close count as "together"
    // A mirrored in-pursuit cop counts as seeing the local player when it is close enough in the
    // LOCAL world (the copy's interpolated body, not the owner's remote coordinates or a traffic id).
    constexpr float PEER_COP_SEES_PLAYER_M = 45.f;
    inline bool PeerCopSeesPlayer(bool isCop, bool inPursuit, const float* copPos, const float* playerPos,
                                  float maxDist = PEER_COP_SEES_PLAYER_M) {
        if (!isCop || !inPursuit || !copPos || !playerPos || !(maxDist > 0.f)) return false;
        float dx = copPos[0] - playerPos[0], dy = copPos[1] - playerPos[1], dz = copPos[2] - playerPos[2];
        float d2 = dx * dx + dy * dy + dz * dz;
        if (!(d2 == d2)) return false;
        return d2 <= maxDist * maxDist;
    }

    // ---- heat ------------------------------------------------------------------------------------
    // Called with every state from the other player. Returns the heat our car should take, or 0 to
    // leave ours alone. A value is copied only when it is new on their side (first one after a gap, or
    // higher than their last one) and hotter than ours. So when one of you gets heat the other gets the
    // same, the hotter of the two wins, and a heat that drops on one side (busted, another car) is
    // neither copied nor pushed back up by the other side's old value.
    struct HeatTracker { uint16_t lastPeer = 0; bool synced = false; };   // synced: had their first value this session
    inline float HeatToTake(HeatTracker& t, float mine, uint16_t peer100) {
        if (peer100 == 0 || peer100 > MAX_HEAT100) { t.lastPeer = 0; return 0.f; }   // unknown / not in free roam
        // the very first value of the session is taken (both start equal); after a gap (race, shop, menu)
        // the first value is only a baseline, so a heat we lost meanwhile (respray, busted) isn't pushed back
        bool fresh = t.lastPeer ? peer100 > t.lastPeer : !t.synced;
        t.synced = true;
        t.lastPeer = peer100;
        float peer = peer100 / 100.f;
        if (!fresh || !(mine == mine)) return 0.f;
        return peer > mine + HEAT_EPS ? peer : 0.f;
    }
    inline uint16_t HeatToWire(float heat) {
        if (!(heat > 0.f)) return 0;
        float h = heat * 100.f + 0.5f;
        return h >= (float)MAX_HEAT100 ? MAX_HEAT100 : (uint16_t)h;
    }

    // ---- cooldown ----------------------------------------------------------------------------------
    // Design (1.7.1, refined): evade bar and busted are personal (vanilla: cops see THAT player).
    // When a player evades they ENTER cooldown immediately. Their cooldown meter does not progress
    // (frozen) until BOTH are in cooldown; then both meters run as usual. Escape is applied only
    // when both have finished: the first stays in cooldown (bar full) until the other also finishes.
    // If a player is spotted during cooldown, ONLY that player drops back to pursuit (vanilla).
    // The other stays in cooldown; their progress is reset to the start of the meter and frozen
    // until the spotted player evades and re-enters cooldown. A hidden player is never pulled back
    // into pursuit because the other was spotted. Disconnect, a busted peer, or a peer who left
    // the pursuit: solo — unfreeze and escape on your own.
    //
    // Safehouse (1.7.1): during a live shared pursuit (both connected, both in a pursuit, neither
    // busted) the safehouse world marker and map/radar icon stay hidden, and entry is blocked,
    // until BOTH are in cooldown (meters running or waiting to escape together). Then either
    // player may enter a safehouse. The game ends that player's pursuit as evaded, and a
    // PURSUIT_EVADED flag tells the other game to end theirs the same way (escape/win). Disconnect
    // or a busted / departed peer: solo — markers and entry as vanilla.
    //
    // One side of a pursuit as the other game needs it (CarState::pursuitFlags / coolLeft10).
    struct View {
        bool inPursuit;
        bool seen;      // its own cops see it (PURSUIT_SEEN)
        bool cooling;   // already in PS_COOLDOWN
        float left;
        bool ready;     // evade bar full, or already cooling (PURSUIT_READY)
        bool busted;
        bool evaded;    // pursuit ended as an escape (PURSUIT_EVADED)
    };
    inline View FromWire(uint8_t flags, uint16_t left10) {
        View v{};
        v.inPursuit = (flags & PURSUIT_ACTIVE) != 0;
        v.seen = (flags & PURSUIT_SEEN) != 0;
        v.cooling = (flags & PURSUIT_COOLDOWN) != 0;
        v.left = left10 / 10.f;
        v.ready = (flags & PURSUIT_READY) != 0 || v.cooling;
        v.busted = (flags & PURSUIT_BUSTED) != 0;
        v.evaded = (flags & PURSUIT_EVADED) != 0;
        return v;
    }
    inline uint16_t LeftToWire(float left) {
        if (!(left > 0.f)) return 0;
        float l = left * 10.f + 0.5f;
        return l >= (float)MAX_COOL_LEFT10 ? MAX_COOL_LEFT10 : (uint16_t)l;
    }

    enum class Cool {
        Free,        // vanilla: enter cooldown, let the meter run, or escape
        Freeze,      // we are in cooldown: do not let the meter advance (peer is not in cooldown)
        Reset,       // we are in cooldown: set the meter back to the start (peer was in cooldown and got spotted)
        HoldEscape,  // our meter is finished: do not escape until the peer has finished too
        ForceEscape, // the peer evaded (safehouse): end ours as an escape/win too
    };
    struct Decision { Cool what; float left; };   // left: remaining time to hold at for HoldEscape
    // The exe float at 0x890D34: not seen this long in the current state starts cooldown.
    constexpr float COOLDOWN_UNSEEN_S = 2.5f;
    constexpr float WAIT_UNSEEN_SLACK_S = 0.05f;
    // Lowest +0x128 that does not look like "seen again" (that would cancel cooldown).
    constexpr float COOLDOWN_FLOOR_S = SEEN_LIMIT_S + 0.1f;
    constexpr float ESCAPE_HOLD_LEFT_S = 0.2f;   // keep this much left so evade level stays < 1
    // Hidden long enough that vanilla would enter cooldown (or already in it). Seen resets the bar.
    inline bool EvadeBarFull(bool seen, bool cooling, float unseenInState, float threshold = COOLDOWN_UNSEEN_S) {
        if (seen) return false;
        if (cooling) return true;
        return threshold > 0.f && unseenInState + WAIT_UNSEEN_SLACK_S >= threshold;
    }
    inline bool CooldownFinished(const View& v, float eps = ESCAPE_HOLD_LEFT_S) {
        return v.cooling && v.left <= eps;
    }
    inline bool PeerAllowsSolo(const View& peer, bool peerConnected) {
        if (!peerConnected) return true;
        if (!peer.inPursuit || peer.busted) return true;
        return false;
    }
    // peerWasCooling: the peer's previous tick (so a drop out of cooldown is a spot, not "never entered").
    // A shared pursuit is live only while both sides are in one and neither is busted.
    inline bool SharedPursuitLive(const View& me, const View& peer, bool peerConnected) {
        return peerConnected && me.inPursuit && peer.inPursuit && !me.busted && !peer.busted;
    }
    // Vanilla markers / entry, except during a live shared pursuit until both are in cooldown.
    inline bool SafehouseAllowed(const View& me, const View& peer, bool peerConnected) {
        if (!SharedPursuitLive(me, peer, peerConnected)) return true;
        return me.cooling && peer.cooling;
    }
    inline bool HideSafehouse(const View& me, const View& peer, bool peerConnected) {
        return !SafehouseAllowed(me, peer, peerConnected);
    }
    inline Decision DecideCooldown(const View& me, const View& peer, bool peerConnected = true,
                                   bool peerWasCooling = false) {
        if (!me.inPursuit) return { Cool::Free, 0.f };
        // Peer entered a safehouse (or otherwise evaded): end ours as an escape, even if we are
        // still in a chase or a frozen cooldown. A disconnected peer is solo, not a force-escape.
        if (peerConnected && peer.evaded && !me.busted) return { Cool::ForceEscape, 0.f };
        if (!me.cooling) return { Cool::Free, 0.f };   // enter immediately when vanilla wants
        if (me.seen) return { Cool::Free, 0.f };       // we were spotted: vanilla drops us; do not freeze ourselves
        if (PeerAllowsSolo(peer, peerConnected)) return { Cool::Free, 0.f };  // disconnect / busted / left: unfreeze
        if (!peer.cooling) return { peerWasCooling ? Cool::Reset : Cool::Freeze, 0.f };
        if (CooldownFinished(me) && !CooldownFinished(peer)) return { Cool::HoldEscape, ESCAPE_HOLD_LEFT_S };
        return { Cool::Free, 0.f };
    }
    inline void ResetCooldownProgress(float& accum, float& sinceSeen) {
        accum = 0.f;
        sinceSeen = COOLDOWN_FLOOR_S;
    }
    inline void FreezeProgress(float frozenAccum, float frozenSince, float& accum, float& sinceSeen) {
        accum = frozenAccum;
        sinceSeen = frozenSince < COOLDOWN_FLOOR_S ? COOLDOWN_FLOOR_S : frozenSince;
    }
    // Fill the cooldown meter so evade level reaches 1 (vanilla then ends the pursuit as evaded).
    inline void CompleteCooldown(float required, float& accum, float& sinceSeen) {
        sinceSeen = sinceSeen < COOLDOWN_FLOOR_S ? COOLDOWN_FLOOR_S : sinceSeen;
        if (!(required > 0.f)) { accum = 0.f; return; }
        float need = required - sinceSeen;
        accum = need > 0.f ? need : 0.f;
    }

    // The game's escape progress is accum + sinceSeen (AIPursuit +0x124 / +0x128 from IPursuit*) and the
    // cooldown left is required - progress. Lower the progress so that `wantLeft` is left, taking it from
    // accum first; sinceSeen never goes down to the "seen" limit (that would end the cooldown).
    inline void DelayProgress(float required, float wantLeft, float& accum, float& sinceSeen) {
        if (!(required > 0.f) || !(wantLeft >= 0.f) || !(accum >= 0.f) || !(sinceSeen >= 0.f)) return;
        float want = required - wantLeft;
        if (want < 0.f) want = 0.f;
        float excess = accum + sinceSeen - want;
        if (excess <= 0.f) return;
        float a = excess < accum ? excess : accum;
        accum -= a; excess -= a;
        float room = sinceSeen - (SEEN_LIMIT_S + 0.1f);
        if (excess > 0.f && room > 0.f) sinceSeen -= excess < room ? excess : room;
    }

    // ---- where the cooldown is shared --------------------------------------------------------------
    // Both in free roam (the full shared pursuit), or both in the same event (Challenge Series, a
    // career pursuit / milestone event, a race with cops): each game runs its own copy of that
    // event's pursuit, and only the cooldown / escape is shared there (heat, joining a chase, being
    // spotted by their cops and the safehouse stay free roam only). A player in another event, in a
    // menu or loading: solo.
    enum class Place { None, FreeRoam, SameEvent };
    struct Where { bool inGame; uint32_t eventHash; bool racing; };
    inline Place SharedPlace(const Where& me, const Where& peer) {
        if (!me.inGame || !peer.inGame) return Place::None;
        if (!me.eventHash && !peer.eventHash && !me.racing && !peer.racing) return Place::FreeRoam;
        if (me.eventHash && me.eventHash == peer.eventHash) return Place::SameEvent;
        return Place::None;
    }
    // Their escape ends ours (ForceEscape) in free roam as before; in an event only once their
    // pursuit was live in this same event, so an old escape signal can't win the event for us.
    inline bool PeerEscapeCounts(Place place, bool peerEvaded, bool peerChasedInEvent) {
        if (!peerEvaded) return false;
        if (place == Place::SameEvent) return peerChasedInEvent;
        return true;
    }

    // ---- busted ------------------------------------------------------------------------------------
    // CarState::bustSeq counts the sender's own busts. True once for every change after the first value
    // we saw (the first one only tells us where they are).
    struct BustTracker { bool have = false; uint8_t last = 0; };
    inline bool PeerJustBusted(BustTracker& t, uint8_t peerSeq) {
        if (!t.have) { t.have = true; t.last = peerSeq; return false; }
        if (peerSeq == t.last) return false;
        t.last = peerSeq;
        return true;
    }
}
