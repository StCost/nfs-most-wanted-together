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

    // ---- the other player's patrol cops ----------------------------------------------------------
    // Near the host the client spawns no patrol cops of its own (traffic.inl), so the patrol cops around
    // both players belong to one game, and only that game's player can draw their attention (the game's
    // own spotting knows only its own player). The other game decides for its player with the copy of
    // that cop, by the game's own rule (AICopManager 0x429EF0): a perpetrator a cop sees starts a pursuit
    // when faster than 29.06 m/s (the float at 0x891694, 65 mph) or at heat level 4 and up (recognised).
    constexpr float PATROL_SEES_M = 40.f;            // a patrol cop notices a car this close in front of it
    constexpr float PATROL_SEES_ANY_DIR_M = 15.f;    // ...or this close in any direction
    constexpr float PATROL_SPEEDING_MS = 29.056772f; // 0x891694
    constexpr int   PATROL_RECOGNISE_HEAT = 4;       // heat level (int of the heat) from which no speeding is needed
    // facing: cos of the angle between the cop's forward and the direction from the cop to the player
    inline bool PatrolCopSpotsPlayer(float dist, float facing, float speedMs, float heat = 0.f) {
        if (!(dist >= 0.f) || dist > PATROL_SEES_M) return false;
        if (!(dist <= PATROL_SEES_ANY_DIR_M || facing >= 0.f)) return false;
        return speedMs > PATROL_SPEEDING_MS || (heat == heat && (int)heat >= PATROL_RECOGNISE_HEAT);
    }

    // ---- the other player's cops while we are already wanted (chase or cooldown) ----------------------
    // Our game's pursuit only counts its own cops as seeing us, and the other game's cops are only copies
    // here, so in a shared pursuit a cop of theirs right next to us never kept us in the chase or ended our
    // cooldown (and ours never did it for them). Their copy counts as seeing us by the same rules our game
    // uses for a patrol cop, without the speeding part (we are already wanted): a cop chasing there within
    // PEER_COP_SEES_PLAYER_M, a patrol one close in any direction or in front of it. Not in a hiding spot.
    inline bool PeerCopSpotsWanted(bool copInPursuit, float dist, float facing) {
        if (!(dist >= 0.f)) return false;
        if (copInPursuit) return dist <= PEER_COP_SEES_PLAYER_M;
        if (dist > PATROL_SEES_M) return false;
        return dist <= PATROL_SEES_ANY_DIR_M || facing >= 0.f;
    }
    // A ram of one of their cops counts as being seen for this long (ms).
    constexpr uint32_t PEER_COP_HIT_SEEN_MS = 1500;
    inline bool PeerCopKeepsUsSeen(bool inPursuit, bool hiding, bool engage, bool copSees, uint32_t sinceHitMs) {
        if (!inPursuit || hiding || !engage) return false;
        return copSees || sinceHitMs < PEER_COP_HIT_SEEN_MS;
    }

    // The minimap shows a cop of ours only while we are chased and once it is engaged with us (DrawCops 0x579E00:
    // IPursuitAI slot 35). The other player's cops (copies here, no IPursuitAI) follow the same rule: shown once
    // one of them has seen us (PeerCopSpotsWanted) in this pursuit, for PEER_COP_ENGAGED_MS after it last did -
    // not as soon as it exists or chases the other player. showAll: the game's own "all cops" switch (0x91CF00).
    constexpr uint32_t PEER_COP_ENGAGED_MS = 30000;
    inline bool PeerCopOnOurMap(bool showAll, bool chased, bool everSawUs, uint32_t sinceSawUsMs) {
        if (showAll) return true;
        return chased && everSawUs && sinceSawUsMs <= PEER_COP_ENGAGED_MS;
    }

    // ---- cops of our pursuit going after the other player ------------------------------------------
    // Each game's pursuit cops chase that game's player. When both are chased, a cop of ours that is
    // clearly closer to the other player's car goes after it (AITarget::Aquire), and comes back to us
    // once that player is far from it or we are clearly closer. At least one cop always stays on us.
    constexpr float RETARGET_RANGE_M = 60.f;    // the other player this close to the cop...
    constexpr float RETARGET_MARGIN_M = 10.f;   // ...and this much closer to it than we are
    constexpr float RELEASE_RANGE_M = 90.f;     // back on us when the other player is farther than this
    constexpr float RELEASE_MARGIN_M = 25.f;    // or when we are this much closer to the cop than they are
    inline bool CopChasesPeer(bool chasingPeerNow, float dMe, float dPeer) {
        if (!(dMe >= 0.f) || !(dPeer >= 0.f)) return false;   // unknown (NaN): stay on us
        if (chasingPeerNow) return dPeer <= RELEASE_RANGE_M && !(dMe + RELEASE_MARGIN_M < dPeer);
        return dPeer < RETARGET_RANGE_M && dPeer + RETARGET_MARGIN_M < dMe;
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
    // Design (1.8): evade bar and busted are personal (vanilla: cops see THAT player), the cooldown is
    // one for both. A player whose evade bar is full stays at the evade limit - the game is kept from
    // starting the cooldown - until the other player's bar is full too; then both enter the cooldown.
    // Nobody is ever in the cooldown alone: when one is spotted in the cooldown, the other leaves it as
    // well (back to the chase), even from a hiding spot. Escape only when both have finished: the first
    // stays in cooldown (bar full) until the other also finishes. Disconnect, a busted peer, or a peer
    // who left the pursuit: solo, vanilla.
    // As both are always in the cooldown together, the safe house works as in the game (it opens in the
    // cooldown); entering it ends the other player's pursuit as an escape too (PURSUIT_EVADED).
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
        bool hiding;    // in a hiding spot during its cooldown, by its own game (PURSUIT_HIDING)
    };
    inline View FromWire(uint8_t flags, uint16_t left10) {
        View v{};
        v.inPursuit = (flags & PURSUIT_ACTIVE) != 0;
        v.seen = (flags & PURSUIT_SEEN) != 0;
        v.cooling = (flags & PURSUIT_COOLDOWN) != 0;
        v.left = left10 / 10.f;
        v.ready = ((flags & PURSUIT_READY) != 0 || v.cooling) && !v.seen;   // seen in the cooldown: spotted
        v.busted = (flags & PURSUIT_BUSTED) != 0;
        v.evaded = (flags & PURSUIT_EVADED) != 0;
        v.hiding = (flags & PURSUIT_HIDING) != 0 && v.cooling;
        return v;
    }
    inline uint16_t LeftToWire(float left) {
        if (!(left > 0.f)) return 0;
        float l = left * 10.f + 0.5f;
        return l >= (float)MAX_COOL_LEFT10 ? MAX_COOL_LEFT10 : (uint16_t)l;
    }

    enum class Cool {
        Free,        // vanilla: enter cooldown, let the meter run, or escape
        HoldEvade,   // our evade bar is full, theirs not: stay at the evade limit (no cooldown yet)
        BackToChase, // we are in cooldown, they are not (spotted): leave it too
        HoldEscape,  // our meter is finished: do not escape until the peer has finished too
        ForceEscape, // the peer evaded (safehouse): end ours as an escape/win too
    };
    struct Decision { Cool what; float left; };   // left: remaining time to hold at for HoldEscape
    // The exe float at 0x890D34: not seen this long in the current state starts cooldown.
    constexpr float COOLDOWN_UNSEEN_S = 2.5f;
    constexpr float WAIT_UNSEEN_SLACK_S = 0.05f;
    // HoldEvade keeps the unseen time here: under the game's limit by a few frames, over EVADE_READY_S.
    constexpr float HOLD_UNSEEN_S = COOLDOWN_UNSEEN_S - 0.15f;
    constexpr float EVADE_READY_S = COOLDOWN_UNSEEN_S - 0.2f;   // the bar counts as full from here
    // Lowest +0x128 that does not look like "seen again" (that would cancel cooldown).
    constexpr float COOLDOWN_FLOOR_S = SEEN_LIMIT_S + 0.1f;
    constexpr float ESCAPE_HOLD_LEFT_S = 0.2f;   // keep this much left so evade level stays < 1
    // Hidden long enough that the cooldown may start (or already in it). Seen resets the bar.
    inline bool EvadeBarFull(bool seen, bool cooling, float unseenInState, float threshold = EVADE_READY_S) {
        if (seen) return false;
        if (cooling) return true;
        return threshold > 0.f && unseenInState + WAIT_UNSEEN_SLACK_S >= threshold;
    }
    // The pursuit bar as the HUD shows it, for the other player's screen: kind 0 not chased, 1 the chase
    // (-100 busted .. +100 evade), 2 the cooldown (share done).
    constexpr float BUSTED_AT_S = 5.0f;
    struct Bar { int8_t value; uint8_t kind; };

    // The partner bar on the game's own meter (peerbar.inl). The meter's v runs +1 busted .. -1 evaded, so a
    // chase value on the wire (-100 busted .. +100 evade) is -value / 100.
    inline float Clamp(float f, float lo, float hi) { return !(f > lo) ? lo : f > hi ? hi : f; }
    inline float MeterV(int value) { return -Clamp((float)value, -100.f, 100.f) / 100.f; }
    // a shown value easing toward the latest one (rate 1/s; 0 s = no step); jumps the last bit so it lands
    inline float FollowValue(float shown, float target, float dtS, float rate) {
        if (!(dtS > 0.f)) return shown;
        float k = dtS * rate; if (k > 1.f) k = 1.f;
        float v = shown + (target - shown) * k;
        return (v - target < 0.005f && target - v < 0.005f) ? target : v;
    }
    // PursuitBoard::Update's fill: each segment as a share of its full width; the far ones flash (ALERT) past
    // half, the middle mark pulses while |v| < 0.1
    struct ChaseFill { float bustFar, bustNear, evadeNear, evadeFar; bool bustAlert, evadeAlert, midPulse; };
    inline ChaseFill ChaseMeterFill(float v) {
        ChaseFill f;
        f.bustFar = v > 0.5f ? (v - 0.5f) * 2.f : 0.f;
        f.bustNear = v > 0.5f ? 0.9f : v > 0.1f ? v * 2.f - 0.1f : 0.f;
        f.evadeNear = v < -0.5f ? 1.f : v < -0.1f ? v * -2.f - 0.1f : 0.f;
        f.evadeFar = v < -0.5f ? (v + 0.5f) * -2.f : 0.f;
        f.bustAlert = v > 0.5f; f.evadeAlert = v < -0.5f;
        f.midPulse = v < 0.1f && v > -0.1f;
        return f;
    }

    // From the game's own meter value (TimeUntilBusted: +1 busted .. -1 evaded, <= -1 the cooldown):
    // on the wire as -100 busted .. +100 evade, so older builds read it as before.
    inline Bar BarFromMeter(bool active, bool cooling, bool busted, float timeUntilBusted, float left, float need) {
        if (!active) return { 0, 0 };
        float t = timeUntilBusted == timeUntilBusted ? timeUntilBusted : 0.f;
        float v = busted ? 1.f : (t < 0.99f ? t : 0.99f);
        if (cooling || !(v > -1.f)) {
            float done = need > 0.f ? 1.f - left / need : 0.f;
            done = !(done > 0.f) ? 0.f : done > 1.f ? 1.f : done;
            return { (int8_t)(done * 100.f + 0.5f), 2 };
        }
        float w = -v * 100.f;
        return { (int8_t)(w < 0.f ? w - 0.5f : w + 0.5f), 1 };
    }

    // The partner's "ready" (unseen with a full evade bar, or cooling) as one shared decision: a change counts
    // only once it has held READY_HOLD_MS. Their ready flickers (spotted for a moment, a cop copy passing), and
    // every flicker moved us into the cooldown and straight back out (10 bounces in 4 minutes in a live test).
    constexpr uint32_t READY_HOLD_MS = 700;
    struct ReadyDebounce { bool stable = false, last = false; uint32_t since = 0; bool init = false; };
    inline bool DebounceReady(ReadyDebounce& d, bool now, uint32_t nowMs, uint32_t holdMs = READY_HOLD_MS) {
        if (!d.init) { d.init = true; d.stable = d.last = now; d.since = nowMs; return now; }
        if (now != d.last) { d.last = now; d.since = nowMs; }
        if (now != d.stable && nowMs - d.since >= holdMs) d.stable = now;
        return d.stable;
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
    inline Decision DecideCooldown(const View& me, const View& peer, bool peerConnected = true) {
        if (!me.inPursuit) return { Cool::Free, 0.f };
        // Peer entered a safehouse (or otherwise evaded): end ours as an escape, even if we are
        // still in a chase. A disconnected peer is solo, not a force-escape.
        if (peerConnected && peer.evaded && !me.busted) return { Cool::ForceEscape, 0.f };
        if (PeerAllowsSolo(peer, peerConnected)) return { Cool::Free, 0.f };  // disconnect / busted / left: vanilla
        if (!me.cooling) {
            // our bar is full, theirs not: wait at the evade limit (both enter the cooldown together)
            if (me.ready && !me.seen && !peer.ready) return { Cool::HoldEvade, 0.f };
            return { Cool::Free, 0.f };
        }
        if (me.seen) return { Cool::Free, 0.f };       // we were spotted: vanilla drops us
        if (!peer.ready) return { Cool::BackToChase, 0.f };   // they were spotted (or never got there): both chase
        if (CooldownFinished(me) && !CooldownFinished(peer)) return { Cool::HoldEscape, ESCAPE_HOLD_LEFT_S };
        return { Cool::Free, 0.f };
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

    // ---- hiding spot and cooldown progress, shared while both are in cooldown ----------------------
    // When both players are in cooldown (neither seen) and one of them is in a hiding spot, the other
    // gets the hiding spot's faster cooldown too: their game's pursuit is told it is hidden, so it
    // fills the meter as vanilla does in a hiding spot. Only the cooldown speeds up: the cops' own sight
    // is not touched, so a player who is not really hidden is still spotted as usual (and a spot ends the
    // sharing, as the spotted player leaves cooldown).
    inline bool ShareHidingSpot(const View& me, const View& peer, bool peerConnected) {
        if (!SharedPursuitLive(me, peer, peerConnected)) return false;
        return me.cooling && peer.cooling && !me.seen && !peer.seen && peer.hiding && !me.hiding;
    }
    // While both are in cooldown (neither seen) the cooldown is one: the one further on wins. Returns the
    // progress (seconds) to add to ours so that we have no more time left than they do, or 0. We only ever
    // catch up (the other game does the same with ours), so the two meet at the larger progress.
    inline float CooldownCatchUp(const View& me, const View& peer, bool peerConnected, float slack = DELAY_SLACK_S) {
        if (!SharedPursuitLive(me, peer, peerConnected)) return 0.f;
        if (!me.cooling || !peer.cooling || me.seen || peer.seen) return 0.f;
        if (!(me.left >= 0.f) || !(peer.left >= 0.f)) return 0.f;
        float d = me.left - peer.left;
        return d > slack ? d : 0.f;
    }

    // ---- where the cooldown is shared --------------------------------------------------------------
    // Both in free roam (the full shared pursuit), or both in the same event (Challenge Series, a
    // career pursuit / milestone event, a race with cops): each game runs its own copy of that
    // event's pursuit; there the cooldown / escape, their cops engaging us (PeerCopsEngage) and a
    // bust (BustWithThem) are shared (heat and the safehouse stay free roam / pursuit challenge only). A player in another event, in a
    // menu or loading: solo.
    enum class Place { None, FreeRoam, SameEvent };
    struct Where { bool inGame; uint32_t eventHash; bool racing; };
    inline Place SharedPlace(const Where& me, const Where& peer) {
        if (!me.inGame || !peer.inGame) return Place::None;
        if (!me.eventHash && !peer.eventHash && !me.racing && !peer.racing) return Place::FreeRoam;
        if (me.eventHash && me.eventHash == peer.eventHash) return Place::SameEvent;
        return Place::None;
    }
    // Where the whole pursuit is shared as in free roam (heat, being pulled into their chase, their
    // cops spotting and chasing us): free roam, and a Challenge Series pursuit both of us are in
    // (open-world driving with cops and no opponents). Not a race with cops, not another event.
    inline bool OpenWorldShared(bool localFreeRoam, Place place, bool pursuitChallenge) {
        return localFreeRoam || (place == Place::SameEvent && pursuitChallenge);
    }
    // The other player's cops in an event both of us are in (a Challenge Series tollbooth or sprint, a
    // career race with cops): each game runs its own cops, and theirs are only mirrors here that the
    // game's own spotting never sees. So their cops engage us as in free roam: a chasing one close to
    // us or a patrol one we speed past starts a pursuit here, and so does ramming one. Never on the
    // results / win screens, in a cutscene or before the race has started (the start countdown).
    inline bool PeerCopsEngage(bool openWorld, Place place, bool resultsFreeze, bool cutscene, bool raceStarting) {
        if (resultsFreeze || cutscene) return false;
        if (openWorld) return true;
        return place == Place::SameEvent && !raceStarting;
    }
    // A pursuit that just started on the other player pulls us in: anywhere in free roam / a pursuit
    // challenge (one shared chase), in another event only while we are close to them (their cops are
    // around us too; a player far ahead on the route is not chased because the other one was).
    constexpr float EVENT_JOIN_M = 250.f;
    inline bool JoinTheirPursuit(bool openWorld, bool engage, float peerDist) {
        if (!engage) return false;
        if (openWorld) return true;
        return peerDist == peerDist && peerDist >= 0.f && peerDist <= EVENT_JOIN_M;
    }
    // On the results / win / Continue screens of an event (our racer finished or is out, or a
    // PostRace screen is up) nobody drives: the pursuit must not move on - no bust meter, no new
    // pursuit pulled in, no spotting. (1.7.4: a player was busted on the win screen.)
    inline bool PursuitFrozenForResults(bool inEvent, bool localDone, bool resultsScreen) {
        return resultsScreen || (inEvent && localDone);
    }
    // Their escape ends ours (ForceEscape) only if their pursuit was live while ours was (the sender
    // keeps EVADED on the wire after its pursuit is gone, so an escape from an earlier pursuit must
    // not end a later one of ours); in an event also only once their pursuit was live in this same
    // event, so an old escape signal can't win the event for us.
    inline bool PeerEscapeCounts(Place place, bool peerEvaded, bool peerChasedInEvent, bool peerLiveWithOurs) {
        if (!peerEvaded || !peerLiveWithOurs) return false;
        if (place == Place::SameEvent) return peerChasedInEvent;
        return true;
    }

    // ---- cost to state / bounty: both players get credit (1.7.4) ------------------------------------
    // Each game runs its own pursuit with its own cops, roadblocks and spike strips; the other
    // player's cops are only mirrors here (not in our pursuit) and their car is a ghost that never
    // counts as our hit. So every immobilized cop, dodged roadblock or smashed prop is counted
    // exactly once, in ONE of the two games. The joint total is therefore the SUM of the two
    // players' own counts (a max would drop whatever the other player did differently, and no event
    // is in both, so the sum can't count one twice).
    // Rule: each side sends what it earned itself in its current pursuit (never what it was
    // credited), cumulative, so a lost packet loses nothing. The other side credits only what they
    // gained while both pursuits were live together, and adds it to its own pursuit counters, so
    // the HUD Cost to State, the milestone / Challenge goals, the rap sheet and the bounty all see
    // the joint total. Infractions stay personal: they are tickets against your own car.
    enum TallyStat {
        T_COPS_DESTROYED, T_ROADBLOCKS, T_STAT158, T_COPS_DAMAGED, T_STAT168, T_SPIKES,
        T_PROPERTY, T_STAT17C, T_STAT180, T_STAT184, T_STAT188, T_BOUNTY28, T_BOUNTY24, T_COUNT
    };
    static_assert(T_COUNT == PSTATS_COUNT, "PKT_PSTATS carries one value per tally stat");
    constexpr int32_t TALLY_MAX = 100000000;   // $100M / 100M units: anything above is garbage
    struct TallyShare {
        bool     live = false;          // our pursuit is running
        int32_t  last[T_COUNT] = {};    // the game's values at our last look
        int32_t  own[T_COUNT] = {};     // what we earned ourselves in this pursuit (sent)
        int32_t  owed[T_COUNT] = {};    // their gains while both were live (to credit here)
        int32_t  given[T_COUNT] = {};   // what we already added to our counters
        uint32_t peerId = 0;            // their pursuit (a new id starts at 0)
        int32_t  peerPrev[T_COUNT] = {};
    };
    inline int32_t ClampTally(int64_t v) { return v < 0 ? 0 : v > TALLY_MAX ? TALLY_MAX : (int32_t)v; }
    // our pursuit started (a new pursuit object): count from the game's values now
    inline void TallyStart(TallyShare& t, const int32_t cur[T_COUNT]) {
        t.live = true;
        for (int i = 0; i < T_COUNT; ++i) { t.last[i] = cur[i]; t.own[i] = t.owed[i] = t.given[i] = 0; }
    }
    inline void TallyStop(TallyShare& t) { t.live = false; }
    // the game's values now: rises are ours. A drop (the game banked / reset it) is not a gain, and
    // what we gave stays given (nothing is credited twice).
    inline void TallyObserve(TallyShare& t, const int32_t cur[T_COUNT]) {
        if (!t.live) return;
        for (int i = 0; i < T_COUNT; ++i) {
            if (cur[i] > t.last[i]) t.own[i] = ClampTally((int64_t)t.own[i] + (cur[i] - t.last[i]));
            t.last[i] = cur[i];
        }
    }
    // their own counts (cumulative for their pursuit `id`). joint: both pursuits are live together
    // (and in the same place). Gains made while not joint are never credited later.
    inline void TallyPeer(TallyShare& t, uint32_t id, const int32_t theirs[T_COUNT], bool joint) {
        if (id != t.peerId) { t.peerId = id; for (int i = 0; i < T_COUNT; ++i) t.peerPrev[i] = 0; }
        for (int i = 0; i < T_COUNT; ++i) {
            int32_t v = ClampTally(theirs[i]);
            if (v <= t.peerPrev[i]) continue;           // old / reordered copy, or nothing new
            if (joint && t.live) t.owed[i] = ClampTally((int64_t)t.owed[i] + (v - t.peerPrev[i]));
            t.peerPrev[i] = v;
        }
    }
    // what to add to our counters now; call TallyGave with the same values once they are added
    inline bool TallyToAdd(const TallyShare& t, int32_t add[T_COUNT]) {
        bool any = false;
        for (int i = 0; i < T_COUNT; ++i) {
            add[i] = t.live && t.owed[i] > t.given[i] ? t.owed[i] - t.given[i] : 0;
            any = any || add[i] > 0;
        }
        return any;
    }
    // after adding: the game's values now (what changed meanwhile is the credit, not our own)
    inline void TallyGave(TallyShare& t, const int32_t add[T_COUNT], const int32_t curAfter[T_COUNT]) {
        for (int i = 0; i < T_COUNT; ++i) { t.given[i] = ClampTally((int64_t)t.given[i] + add[i]); t.last[i] = curAfter[i]; }
    }
    // the game's own Cost to State formula (IPursuit slot 56, 0x40AA00) over the counters we share,
    // for the log and the tests: cops immobilized $5000, damaged $250, property damage at cost ...
    inline int64_t TallyCost(const int32_t v[T_COUNT]) {
        int64_t a = 2 * (10LL * v[T_COPS_DESTROYED] + v[T_STAT168] + v[T_STAT158]) + v[T_STAT184] + v[T_STAT17C] + v[T_COPS_DAMAGED];
        return a * 250 + 225LL * (v[T_STAT180] + 2LL * v[T_STAT188]) + v[T_PROPERTY];
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
    // Busted is personal in free roam. In an event both of you are in (Challenge Series, a race with
    // cops, a pursuit challenge) a bust fails the event, and it fails it for both: the other game busts
    // its player too (the game's own arrest: busted cutscene, then the event's loss screen). Only while
    // we are still in that same event (seen together within SAME_EVENT_GRACE_MS: their state may leave
    // the event a moment after the bust), still driving (not finished / out / on a results screen),
    // and not busted already.
    constexpr uint32_t SAME_EVENT_GRACE_MS = 5000;
    constexpr uint32_t SHARED_BUST_TRY_MS = 12000;   // how long we keep trying (a pursuit may need starting first)
    struct EventBust {
        uint32_t lastSameEvent = 0;   // event hash we were last seen in together
        uint32_t lastSameAt = 0;      // when (ms)
    };
    inline void EventSeenTogether(EventBust& b, bool sameEvent, uint32_t eventHash, uint32_t now) {
        if (sameEvent && eventHash) { b.lastSameEvent = eventHash; b.lastSameAt = now; }
    }
    // Free roam: one shared pursuit (both chased, together in it): a bust of one busts the other too, as the
    // escape is shared. Not when we are no longer chased (we escaped first, or never were) or already busted.
    inline bool BustTogetherInFreeRoam(uint32_t eventHash, bool ourPursuitLive, bool peerWasLiveWithOurs, bool resultsFreeze, bool alreadyBusted) {
        return !eventHash && ourPursuitLive && peerWasLiveWithOurs && !resultsFreeze && !alreadyBusted;
    }
    inline bool BustWithThem(const EventBust& b, uint32_t eventHash, uint32_t now, bool resultsFreeze, bool alreadyBusted) {
        if (!eventHash || resultsFreeze || alreadyBusted) return false;
        if (b.lastSameEvent != eventHash) return false;
        return (uint32_t)(now - b.lastSameAt) <= SAME_EVENT_GRACE_MS;
    }
}
