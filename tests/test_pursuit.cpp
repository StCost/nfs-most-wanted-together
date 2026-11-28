// Shared pursuits (src/pursuitutil.h): heat sharing, personal evade bars, frozen cooldown until
// both are in, escape when both finish, safehouse hidden until then, and the pursuit fields of the state packet.
#include "net_helpers.h"
#include "../src/pursuitutil.h"
#include <limits>

using namespace SharedPursuit;

// ---- heat ----------------------------------------------------------------------------------------
TEST(pursuit_heat_takes_the_hotter_one) {
    HeatTracker t;
    CHECK_NEAR(HeatToTake(t, 1.0f, 250), 2.5f, 1e-4);   // first value from them, hotter: take it
    HeatTracker u;
    CHECK_EQ(HeatToTake(u, 3.0f, 250), 0);               // ours is hotter: keep ours (they take ours)
}

TEST(pursuit_heat_only_rises_are_copied) {
    HeatTracker t;
    CHECK_EQ(HeatToTake(t, 3.0f, 300), 0);               // same heat: nothing to do
    CHECK_EQ(HeatToTake(t, 3.0f, 300), 0);
    CHECK_NEAR(HeatToTake(t, 3.0f, 350), 3.5f, 1e-4);   // they got heat: so do we
    // they were busted / changed car: their heat drops - ours stays
    CHECK_EQ(HeatToTake(t, 3.5f, 100), 0);
    // and their old value never comes back to push ours: a later rise below ours changes nothing
    CHECK_EQ(HeatToTake(t, 3.5f, 150), 0);
    // a rise above ours is copied again
    CHECK_NEAR(HeatToTake(t, 3.5f, 400), 4.0f, 1e-4);
}

TEST(pursuit_heat_dropped_on_our_side_is_not_pushed_back) {
    // we had 4, got busted (heat 1). Their 4 is not new, so it is not copied back to us.
    HeatTracker t;
    CHECK_EQ(HeatToTake(t, 4.0f, 400), 0);
    CHECK_EQ(HeatToTake(t, 1.0f, 400), 0);
    CHECK_EQ(HeatToTake(t, 1.0f, 400), 0);
}

TEST(pursuit_heat_gap_starts_over) {
    HeatTracker t;
    CHECK_EQ(HeatToTake(t, 2.0f, 200), 0);
    CHECK_EQ(HeatToTake(t, 2.0f, 0), 0);                 // they went into a race / menu: unknown
    CHECK_EQ(t.lastPeer, 0);
    CHECK_EQ(HeatToTake(t, 1.0f, 200), 0);               // back after a gap: only a baseline (we may have lost ours on purpose)
    CHECK_NEAR(HeatToTake(t, 1.0f, 250), 2.5f, 1e-4);   // a new rise there is copied again
}

TEST(pursuit_heat_rounding_never_ratchets) {
    // both sides rise slowly; copying each other's rounded value must not run away
    HeatTracker a, b;
    float ha = 2.34f, hb = 2.31f;
    for (int i = 0; i < 200; ++i) {
        float ta = HeatToTake(a, ha, HeatToWire(hb)); if (ta > 0) ha = ta;
        float tb = HeatToTake(b, hb, HeatToWire(ha)); if (tb > 0) hb = tb;
    }
    CHECK(ha <= 2.35f && hb <= 2.35f);
    CHECK_NEAR(ha, hb, 0.02);
}

TEST(pursuit_heat_bad_values) {
    HeatTracker t;
    CHECK_EQ(HeatToTake(t, 1.0f, MAX_HEAT100 + 1), 0);   // out of range: ignored
    CHECK_EQ(t.lastPeer, 0);
    float nan = std::numeric_limits<float>::quiet_NaN();
    CHECK_EQ(HeatToTake(t, nan, 300), 0);                // our own heat unreadable: leave it
    CHECK_EQ(HeatToWire(0.f), 0);
    CHECK_EQ(HeatToWire(-3.f), 0);
    CHECK_EQ(HeatToWire(nan), 0);
    CHECK_EQ(HeatToWire(2.345f), 235);
    CHECK_EQ(HeatToWire(1.0e9f), MAX_HEAT100);
}

TEST(pursuit_peer_cop_sees_player_uses_local_positions) {
    const float me[3] = { 0, 0, 0 };
    const float nearCop[3] = { 20, 0, 20 };          // ~28 m
    const float farCop[3] = { 80, 0, 0 };
    const float nanPos[3] = { std::numeric_limits<float>::quiet_NaN(), 0, 0 };
    CHECK(PeerCopSeesPlayer(true, true, nearCop, me));
    CHECK(!PeerCopSeesPlayer(true, true, farCop, me));
    CHECK(!PeerCopSeesPlayer(false, true, nearCop, me));     // a traffic car, not a cop
    CHECK(!PeerCopSeesPlayer(true, false, nearCop, me));    // not in a pursuit
    CHECK(!PeerCopSeesPlayer(true, true, nullptr, me));
    CHECK(!PeerCopSeesPlayer(true, true, nearCop, nullptr));
    CHECK(!PeerCopSeesPlayer(true, true, nanPos, me));
    CHECK(PeerCopSeesPlayer(true, true, farCop, me, 100.f));
}

// the other player's cops see us while we are already wanted: chase and cooldown, both directions
TEST(pursuit_peer_cop_spots_wanted_player) {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    CHECK(PeerCopSpotsWanted(true, 30.f, -1.f));               // chasing there, 30 m away, any direction
    CHECK(PeerCopSpotsWanted(true, PEER_COP_SEES_PLAYER_M, 0.f));
    CHECK(!PeerCopSpotsWanted(true, 60.f, 1.f));               // too far
    CHECK(PeerCopSpotsWanted(false, 30.f, 0.5f));              // patrol cop, we are in front: no speeding needed
    CHECK(!PeerCopSpotsWanted(false, 30.f, -0.5f));            // patrol cop, behind it
    CHECK(PeerCopSpotsWanted(false, 10.f, -0.9f));             // right next to it
    CHECK(!PeerCopSpotsWanted(false, 41.f, 1.f));
    CHECK(!PeerCopSpotsWanted(true, nan, 1.f));
    CHECK(!PeerCopSpotsWanted(false, -1.f, 1.f));
    CHECK(!PeerCopSpotsWanted(false, 30.f, nan));              // unknown facing: not seen unless close
}

TEST(pursuit_peer_cop_keeps_us_seen) {
    const uint32_t never = 0xFFFFFFFFu;
    CHECK(PeerCopKeepsUsSeen(true, false, true, true, never));     // their cop next to us: seen (cooldown ends)
    CHECK(!PeerCopKeepsUsSeen(true, false, true, false, never));   // none near: our own cops decide
    CHECK(!PeerCopKeepsUsSeen(false, false, true, true, never));   // not chased: the start-a-pursuit path handles it
    CHECK(!PeerCopKeepsUsSeen(true, true, true, true, never));     // in a hiding spot: not seen
    CHECK(!PeerCopKeepsUsSeen(true, false, false, true, never));   // their cops don't count here (other event, results)
    CHECK(PeerCopKeepsUsSeen(true, false, true, false, 200));      // we rammed one of their cops just now
    CHECK(!PeerCopKeepsUsSeen(true, false, true, false, PEER_COP_HIT_SEEN_MS));
}

TEST(pursuit_patrol_cop_spots_speeding_player) {
    CHECK(PatrolCopSpotsPlayer(30.f, 0.8f, 33.f));            // in front, 120 km/h
    CHECK(!PatrolCopSpotsPlayer(30.f, 0.8f, 20.f));           // 72 km/h: not speeding
    CHECK(!PatrolCopSpotsPlayer(30.f, -0.5f, 33.f));          // behind the cop, not close
    CHECK(PatrolCopSpotsPlayer(10.f, -0.9f, 33.f));           // right next to it: any direction
    CHECK(!PatrolCopSpotsPlayer(45.f, 1.f, 50.f));            // too far
    float nan = std::numeric_limits<float>::quiet_NaN();
    CHECK(!PatrolCopSpotsPlayer(nan, 1.f, 50.f));
    CHECK(!PatrolCopSpotsPlayer(10.f, 1.f, nan));
    CHECK(!PatrolCopSpotsPlayer(30.f, nan, 33.f));
    CHECK(!PatrolCopSpotsPlayer(30.f, 0.8f, 28.f));           // 101 km/h: under the game's 65 mph
    CHECK(PatrolCopSpotsPlayer(30.f, 0.8f, 5.f, 4.2f));       // heat 4: recognised, no speeding needed
    CHECK(!PatrolCopSpotsPlayer(30.f, 0.8f, 5.f, 3.9f));
    CHECK(!PatrolCopSpotsPlayer(30.f, -0.8f, 5.f, 5.f));      // behind it: not seen
}

TEST(pursuit_cop_goes_after_the_closer_player) {
    CHECK(CopChasesPeer(false, 50.f, 20.f));                  // the other player is clearly closer
    CHECK(!CopChasesPeer(false, 25.f, 20.f));                 // only a little closer: stays on us
    CHECK(!CopChasesPeer(false, 200.f, 70.f));                // the other player too far from the cop
    CHECK(CopChasesPeer(true, 40.f, 50.f));                   // chasing them: keeps at it (hysteresis)
    CHECK(!CopChasesPeer(true, 20.f, 50.f));                  // we are clearly closer now: back on us
    CHECK(!CopChasesPeer(true, 200.f, 95.f));                 // they got away from it
    float nan = std::numeric_limits<float>::quiet_NaN();
    CHECK(!CopChasesPeer(false, nan, 10.f));
    CHECK(!CopChasesPeer(true, 10.f, nan));
}

// ---- cooldown ------------------------------------------------------------------------------------
namespace {
    View V(bool in, bool seen, bool cooling, float left, bool ready = false, bool busted = false) {
        View v{};
        v.inPursuit = in; v.seen = seen; v.cooling = cooling; v.left = left; v.ready = ready; v.busted = busted;
        return v;
    }
}

TEST(pursuit_evade_bar_full_is_local) {
    CHECK(!EvadeBarFull(true, false, 3.f, 9.f));               // seen: the bar is not full
    CHECK(!EvadeBarFull(false, false, 1.0f, 9.f));             // hidden, not long enough
    CHECK(EvadeBarFull(false, false, HOLD_UNSEEN_S, 9.f));     // held at the evade limit: still full
    CHECK(EvadeBarFull(false, false, 2.5f, 9.f));
    CHECK(!EvadeBarFull(false, false, 2.5f, SEEN_LIMIT_S));    // not past the seen limit: no cooldown yet
    CHECK(!EvadeBarFull(false, false, 2.5f, 1.f));
    CHECK(EvadeBarFull(false, true, 0.f, 0.f));                // already in cooldown
    CHECK(!EvadeBarFull(true, true, 2.5f, 9.f));               // seen wins: the bar is not full
    CHECK(HOLD_UNSEEN_S < COOLDOWN_UNSEEN_S);                  // the hold keeps the game from starting the cooldown
    CHECK(HOLD_UNSEEN_S + WAIT_UNSEEN_SLACK_S >= EVADE_READY_S);
}

TEST(pursuit_full_evade_bar_waits_for_the_other_player) {
    // A's bar is full, B is still seen: A waits at the evade limit, B is vanilla
    View a = V(true, false, false, 0, true), b = V(true, true, false, 0, false);
    CHECK(DecideCooldown(a, b).what == Cool::HoldEvade);
    CHECK(DecideCooldown(b, a).what == Cool::Free);
    // B's bar fills too: both may enter the cooldown
    b.seen = false; b.ready = true;
    CHECK(DecideCooldown(a, b).what == Cool::Free);
    CHECK(DecideCooldown(b, a).what == Cool::Free);
    // B entered first, A a frame later: B stays in it (A is ready), A enters
    b.cooling = true; b.left = 20;
    CHECK(DecideCooldown(b, a).what == Cool::Free);
    CHECK(DecideCooldown(a, b).what == Cool::Free);
    // our bar not full yet, theirs not either: the hold is already on (the game can step past "full" into the
    // cooldown in one go - from backup it did, every time)
    View notFull = V(true, false, false, 0, false);
    CHECK(DecideCooldown(notFull, V(true, true, false, 0, false)).what == Cool::HoldEvade);
    // we are seen ourselves: vanilla (nothing to hold)
    View seenMe = V(true, true, false, 0, false);
    CHECK(DecideCooldown(seenMe, V(true, true, false, 0, false)).what == Cool::Free);
}

TEST(pursuit_spotted_in_cooldown_takes_both_back_to_the_chase) {
    View a = V(true, false, true, 12, true), b = V(true, false, true, 12, true);
    CHECK(DecideCooldown(a, b).what == Cool::Free);            // both in cooldown: meters run
    b.seen = true; b.ready = false;                            // B spotted (its game drops it)
    CHECK(DecideCooldown(b, a).what == Cool::Free);            // B: vanilla
    CHECK(DecideCooldown(a, b).what == Cool::BackToChase);     // A leaves the cooldown too
    a.hiding = true;                                           // even from a hiding spot
    CHECK(DecideCooldown(a, b).what == Cool::BackToChase);
    // A spotted itself: vanilla drops A
    a.hiding = false; a.seen = true;
    CHECK(DecideCooldown(a, b).what == Cool::Free);
}

static_assert(ESCAPE_HOLD_LEFT_S > PURSUIT_TICK_S, "the escape hold must outlast one pursuit tick");

TEST(pursuit_escape_hold_outlasts_a_tick) {
    // held at the margin, then one tick of progress (plain, hiding spot ~3x, a 1 s hitch): still some left
    const float rates[] = { PURSUIT_TICK_S, PURSUIT_TICK_S * 3.f, 1.0f };
    for (float perTick : rates) {
        float need = 13.f, accum = 2.f, since = 11.f;           // finished: progress 13 of 13
        float hold = EscapeHoldLeft(perTick);
        DelayProgress(need, hold, accum, since);
        float left = need - (accum + since);
        CHECK(left > hold - 0.01f);
        CHECK(left - perTick > 0.f);                             // the game did not escape before the next hold
    }
    CHECK(EscapeHoldLeft(0.f) == ESCAPE_HOLD_LEFT_S);
    CHECK(EscapeHoldLeft(100.f) == ESCAPE_HOLD_MAX_S);
}

TEST(pursuit_held_escape_stays_finished) {
    // held back above the finished line, ours still counts as finished (no Free/HoldEscape flapping)
    View a = V(true, false, true, 3.f, true), b = V(true, false, true, 8, true);
    a.held = true;
    CHECK(DecideCooldown(a, b).what == Cool::HoldEscape);
    b.left = 0.f;                                                // the partner finished (a held one sends 0)
    CHECK(DecideCooldown(a, b).what == Cool::Free);              // the caller then completes ours
}

TEST(pursuit_evade_wait_does_not_count_toward_the_cooldown) {
    float accum = 3.f, since = 6.f;                            // 9 s of progress after a wait (2 s when it began)
    UndoEvadeWait(13.f, 2.f, accum, since);
    CHECK(accum == 0.f && since == 6.f);                       // the accumulated part goes back first
    accum = 10.f; since = 40.f;                                // a long wait: sinceSeen grew too
    UndoEvadeWait(13.f, 2.f, accum, since);
    CHECK(accum == 0.f && since > SEEN_LIMIT_S);               // never back under the seen limit
    CHECK(13.f - (accum + since) > 5.f);                       // most of the cooldown still ahead
    accum = 1.f; since = 0.5f;                                 // less progress than when the wait began: untouched
    UndoEvadeWait(13.f, 2.f, accum, since);
    CHECK(accum == 1.f && since == 0.5f);
}

TEST(pursuit_escape_completes_only_when_both_finish) {
    View a = V(true, false, true, 0.1f, true), b = V(true, false, true, 8, true);
    CHECK(DecideCooldown(a, b).what == Cool::HoldEscape);
    CHECK(DecideCooldown(b, a).what == Cool::Free);            // B still has meter left: keep running
    b.left = 0.1f;                                             // both finished
    CHECK(DecideCooldown(a, b).what == Cool::Free);
    CHECK(DecideCooldown(b, a).what == Cool::Free);
}

TEST(pursuit_cooldown_solo_without_the_other_player) {
    View me = V(true, false, true, 10, true), peer = V(true, true, false, 20, false);
    CHECK(DecideCooldown(me, peer).what == Cool::BackToChase);
    CHECK(DecideCooldown(me, peer, false).what == Cool::Free);              // disconnect: solo
    View busted = V(true, false, false, 0, false, true);
    CHECK(DecideCooldown(me, busted).what == Cool::Free);
    CHECK(DecideCooldown(me, V(false, false, false, 0)).what == Cool::Free);  // peer left / escaped
    View full = V(true, false, false, 0, true);
    CHECK(DecideCooldown(full, peer, false).what == Cool::Free);           // no waiting when alone
    CHECK(DecideCooldown(full, busted).what == Cool::Free);
}

TEST(pursuit_cooldown_walkthrough_both_roles) {
    View a = V(true, true, false, 20, false), b = V(true, true, false, 20, false);
    CHECK(DecideCooldown(a, b).what == Cool::Free);
    CHECK(DecideCooldown(b, a).what == Cool::Free);
    a.seen = false; a.ready = true;                                     // A's bar fills
    CHECK(DecideCooldown(a, b).what == Cool::HoldEvade);
    b.seen = false; b.ready = true;                                     // B's too
    CHECK(DecideCooldown(a, b).what == Cool::Free);
    a.cooling = true; b.cooling = true;                                 // both in cooldown
    CHECK(DecideCooldown(a, b).what == Cool::Free);
    CHECK(DecideCooldown(b, a).what == Cool::Free);
    b.seen = true; b.ready = false; b.cooling = false;                 // B spotted
    CHECK(DecideCooldown(a, b).what == Cool::BackToChase);
    a.cooling = false; a.ready = false; a.seen = true;                 // A back in the chase
    CHECK(DecideCooldown(a, b).what == Cool::Free);
    a.seen = false; a.ready = true;                                     // A loses them again first
    CHECK(DecideCooldown(a, b).what == Cool::HoldEvade);
}

TEST(pursuit_wire_ready_needs_unseen) {
    View v = FromWire(PURSUIT_ACTIVE | PURSUIT_COOLDOWN | PURSUIT_SEEN, 50);
    CHECK(v.cooling); CHECK(!v.ready);                          // seen in the cooldown: spotted
    v = FromWire(PURSUIT_ACTIVE | PURSUIT_READY | PURSUIT_SEEN, 0);
    CHECK(!v.ready);
}

TEST(pursuit_bar_from_the_game_meter_nan) {
    float nan = std::numeric_limits<float>::quiet_NaN();
    Bar b = BarFromMeter(true, false, false, nan, 0.f, 0.f);
    CHECK_EQ((int)b.kind, 1); CHECK_EQ((int)b.value, 0);
    b = BarFromMeter(true, true, false, -1.f, nan, 60.f);
    CHECK_EQ((int)b.kind, 2); CHECK_EQ((int)b.value, 0);
}

TEST(pursuit_peer_safehouse_forces_our_escape) {
    View me = V(true, false, true, 8, true), peer = V(false, false, false, 0);
    peer.evaded = true;
    CHECK(DecideCooldown(me, peer, true).what == Cool::ForceEscape);
    // even if we were still in a chase
    View chasing = V(true, true, false, 20, false);
    CHECK(DecideCooldown(chasing, peer, true).what == Cool::ForceEscape);
    // disconnect is solo, not a force-escape
    CHECK(DecideCooldown(me, peer, false).what == Cool::Free);
}

TEST(pursuit_shared_hiding_spot) {
    View me = V(true, false, true, 20, true), peer = V(true, false, true, 25, true);
    CHECK(!ShareHidingSpot(me, peer, true));                   // nobody hides
    peer.hiding = true;
    CHECK(ShareHidingSpot(me, peer, true));                    // both in cooldown, they hide: ours runs hidden too
    CHECK(!ShareHidingSpot(me, peer, false));                  // disconnected: solo
    me.hiding = true;
    CHECK(!ShareHidingSpot(me, peer, true));                   // we hide ourselves: vanilla already
    me.hiding = false;
    View chased = V(true, false, false, 0, false);
    CHECK(!ShareHidingSpot(chased, peer, true));               // we are not in cooldown yet
    View seen = V(true, true, true, 20, true);
    CHECK(!ShareHidingSpot(seen, peer, true));                 // we were spotted
    View peerSeen = peer; peerSeen.seen = true;
    CHECK(!ShareHidingSpot(me, peerSeen, true));               // they were spotted
    View peerOut = V(true, false, false, 0, true); peerOut.hiding = true;
    CHECK(!ShareHidingSpot(me, peerOut, true));                // they are not in cooldown
    View peerBusted = peer; peerBusted.busted = true;
    CHECK(!ShareHidingSpot(me, peerBusted, true));
    // the wire: hiding only counts in cooldown
    View w = FromWire(PURSUIT_ACTIVE | PURSUIT_COOLDOWN | PURSUIT_HIDING, 100);
    CHECK(w.hiding);
    w = FromWire(PURSUIT_ACTIVE | PURSUIT_HIDING, 100);
    CHECK(!w.hiding);
    CHECK((PURSUIT_HIDING & PURSUIT_KNOWN_BITS) == PURSUIT_HIDING);
}

TEST(pursuit_shared_cooldown_progress) {
    View me = V(true, false, true, 20, true), peer = V(true, false, true, 12, true);
    CHECK_NEAR(CooldownCatchUp(me, peer, true), 8.0, 1e-4);   // they are further on: we take their progress
    CHECK_NEAR(CooldownCatchUp(peer, me, true), 0.0, 1e-6);   // never back: they keep theirs
    View close = V(true, false, true, 12.2f, true);
    CHECK_NEAR(CooldownCatchUp(close, peer, true), 0.0, 1e-6);   // within the slack (wire is 0.1 s)
    CHECK_NEAR(CooldownCatchUp(me, peer, false), 0.0, 1e-6);  // disconnected
    View seen = V(true, true, true, 20, true);
    CHECK_NEAR(CooldownCatchUp(seen, peer, true), 0.0, 1e-6); // we were spotted: vanilla
    View peerSeen = peer; peerSeen.seen = true;
    CHECK_NEAR(CooldownCatchUp(me, peerSeen, true), 0.0, 1e-6);
    View peerChased = V(true, false, false, 0, false);
    CHECK_NEAR(CooldownCatchUp(me, peerChased, true), 0.0, 1e-6);   // only both in cooldown
    View chased = V(true, false, false, 0, false);
    CHECK_NEAR(CooldownCatchUp(chased, peer, true), 0.0, 1e-6);
    View peerBusted = peer; peerBusted.busted = true;
    CHECK_NEAR(CooldownCatchUp(me, peerBusted, true), 0.0, 1e-6);
    View nanLeft = me; nanLeft.left = std::numeric_limits<float>::quiet_NaN();
    CHECK_NEAR(CooldownCatchUp(nanLeft, peer, true), 0.0, 1e-6);
    // after catching up, the two meet: both see the same time left and hold / escape together
    float search = 3.f;
    search += CooldownCatchUp(me, peer, true);
    CHECK_NEAR(search, 11.0, 1e-4);
    View done = V(true, false, true, 0.1f, true), peerDone = V(true, false, true, 0.1f, true);
    CHECK(DecideCooldown(done, peerDone, true).what == Cool::Free);
}

TEST(pursuit_wire_view) {
    View v = FromWire(PURSUIT_ACTIVE | PURSUIT_COOLDOWN, 123);
    CHECK(v.inPursuit); CHECK(!v.seen); CHECK(v.cooling); CHECK(v.ready); CHECK(!v.busted);
    CHECK_NEAR(v.left, 12.3, 1e-4);
    v = FromWire(PURSUIT_ACTIVE | PURSUIT_SEEN, 0);
    CHECK(v.seen); CHECK(!v.cooling); CHECK(!v.ready);
    v = FromWire(PURSUIT_ACTIVE | PURSUIT_READY, 0);
    CHECK(v.ready); CHECK(!v.seen); CHECK(!v.cooling);
    v = FromWire(PURSUIT_BUSTED, 0);
    CHECK(v.busted); CHECK(!v.inPursuit); CHECK(!v.evaded);
    v = FromWire(PURSUIT_EVADED, 0);
    CHECK(v.evaded); CHECK(!v.inPursuit); CHECK(!v.busted);
    CHECK_EQ(LeftToWire(12.34f), 123);
    CHECK_EQ(LeftToWire(-1.f), 0);
    CHECK_EQ(LeftToWire(1.0e9f), MAX_COOL_LEFT10);
}

// ---- busted --------------------------------------------------------------------------------------
TEST(pursuit_busted_follows_changes_only) {
    BustTracker t;
    CHECK(!PeerJustBusted(t, 7));      // first value: only where they are (no bust on connect)
    CHECK(!PeerJustBusted(t, 7));
    CHECK(PeerJustBusted(t, 8));       // they were busted
    CHECK(!PeerJustBusted(t, 8));      // once
    CHECK(PeerJustBusted(t, 9));
    CHECK(!PeerJustBusted(t, 9));
    BustTracker w;                     // the counter wraps
    CHECK(!PeerJustBusted(w, 255));
    CHECK(PeerJustBusted(w, 0));
}

// ---- the state packet ---------------------------------------------------------------------------
TEST(pursuit_state_fields_round_trip) {
    th::NetFixture f(true);
    th::Connect();
    StatePacket p = th::MakeState(5, 1.0f, 10, 0);
    p.s.heat100 = 345; p.s.pursuitFlags = PURSUIT_ACTIVE | PURSUIT_SEEN; p.s.bustSeq = 3; p.s.coolLeft10 = 150;
    th::Inject(p);
    const Net::Snapshot& l = Net::Latest();
    REQUIRE(l.valid);
    CHECK_EQ(l.s.heat100, 345);
    CHECK_EQ(l.s.pursuitFlags, PURSUIT_ACTIVE | PURSUIT_SEEN);
    CHECK_EQ(l.s.bustSeq, 3);
    CHECK_EQ(l.s.coolLeft10, 150);
}

TEST(pursuit_state_bad_fields_are_dropped) {
    th::NetFixture f(true);
    th::Connect();
    StatePacket good = th::MakeState(5, 1.0f, 10, 0);
    good.s.heat100 = 200;
    th::Inject(good);
    StatePacket bad = th::MakeState(6, 1.1f, 11, 0);
    bad.s.heat100 = 60000;                                // absurd heat: the whole state is refused
    th::Inject(bad);
    bad = th::MakeState(7, 1.2f, 12, 0);
    bad.s.pursuitFlags = 0xF0;
    th::Inject(bad);
    CHECK_EQ(Net::Latest().seq, 5);
    CHECK_EQ(Net::Latest().s.heat100, 200);
}

// ---- where the cooldown is shared (1.7.4: Challenge Series and career events too) ---------------
TEST(pursuit_shared_place) {
    using SharedPursuit::Where; using SharedPursuit::Place; using SharedPursuit::SharedPlace;
    Where roam{ true, 0, false }, ev{ true, 0x7E8EDB81u, true }, other{ true, 0x12345678u, true };
    Where menu{ false, 0, false }, racingNoEvent{ true, 0, true };
    CHECK(SharedPlace(roam, roam) == Place::FreeRoam);
    CHECK(SharedPlace(ev, ev) == Place::SameEvent);                // the same Challenge Series / career event
    CHECK(SharedPlace(ev, other) == Place::None);                  // different events: solo
    CHECK(SharedPlace(ev, roam) == Place::None);                   // one in an event, one roaming
    CHECK(SharedPlace(roam, ev) == Place::None);
    CHECK(SharedPlace(ev, menu) == Place::None);                   // the other one is in a menu / loading
    CHECK(SharedPlace(menu, menu) == Place::None);
    CHECK(SharedPlace(roam, racingNoEvent) == Place::None);
    Where evLoading{ false, 0x7E8EDB81u, true };
    CHECK(SharedPlace(ev, evLoading) == Place::None);              // same event, but not in game yet
}

TEST(pursuit_event_escape_signal_needs_their_pursuit_in_this_event) {
    using SharedPursuit::Place; using SharedPursuit::PeerEscapeCounts;
    CHECK(PeerEscapeCounts(Place::FreeRoam, true, false, true));   // free roam: as before (safehouse)
    CHECK(PeerEscapeCounts(Place::None, true, false, true));
    CHECK(!PeerEscapeCounts(Place::SameEvent, true, false, true)); // an escape from before this event
    CHECK(PeerEscapeCounts(Place::SameEvent, true, true, true));   // they escaped in this event: ours too
    CHECK(!PeerEscapeCounts(Place::SameEvent, false, true, true));
}

TEST(pursuit_old_escape_signal_does_not_end_a_later_pursuit) {
    using SharedPursuit::Place; using SharedPursuit::PeerEscapeCounts;
    // they escaped earlier and still send EVADED; our pursuit started after theirs had ended
    CHECK(!PeerEscapeCounts(Place::FreeRoam, true, false, false));
    CHECK(!PeerEscapeCounts(Place::None, true, false, false));
    CHECK(!PeerEscapeCounts(Place::SameEvent, true, true, false)); // e.g. after a Restart of the same event
}

// The reported case: in a Challenge Series pursuit the client lost the cops and its meter ran out
// while the host was still chased. Now: the client waits at the evade limit until the host's bar is
// full too; then both run, and the one who finishes first waits for the other.
TEST(pursuit_event_cooldown_waits_for_both_then_escapes_together) {
    View client = V(true, false, false, 0, true), host = V(true, true, false, 30, false);
    CHECK(DecideCooldown(client, host, true).what == Cool::HoldEvade);      // no cooldown alone
    CHECK(DecideCooldown(host, client, true).what == Cool::Free);           // host still chased: vanilla
    host.seen = false; host.ready = true;
    client.cooling = true; client.left = 30; host.cooling = true; host.left = 30;
    CHECK(DecideCooldown(client, host, true).what == Cool::Free);           // both cooling: both run
    CHECK(DecideCooldown(host, client, true).what == Cool::Free);
    client.left = 0.1f; host.left = 3.f;
    Decision d = DecideCooldown(client, host, true);
    CHECK(d.what == Cool::HoldEscape);                                       // client waits at the end
    host.left = 0.1f;
    CHECK(DecideCooldown(client, host, true).what == Cool::Free);           // both done: both escape
    CHECK(DecideCooldown(host, client, true).what == Cool::Free);
    host.evaded = true;                                                      // host's game ended it first
    CHECK(DecideCooldown(client, host, true).what == Cool::ForceEscape);
}

// ---- cost to state / bounty: both get credit (1.7.4) ------------------------------------------------
namespace {
    struct Tal { int32_t v[T_COUNT] = {}; };
}
TEST(pursuit_tally_sums_both_players_once) {
    TallyShare a, b;
    Tal ga, gb;                                              // the two games' counters
    TallyStart(a, ga.v); TallyStart(b, gb.v);
    // A immobilizes 2 of A's cops, B dodges a roadblock and smashes $1200 of property
    ga.v[T_COPS_DESTROYED] = 2; gb.v[T_ROADBLOCKS] = 1; gb.v[T_PROPERTY] = 1200;
    TallyObserve(a, ga.v); TallyObserve(b, gb.v);
    TallyPeer(a, 1, b.own, true); TallyPeer(b, 1, a.own, true);
    int32_t add[T_COUNT];
    REQUIRE(TallyToAdd(a, add)); for (int i = 0; i < T_COUNT; ++i) ga.v[i] += add[i]; TallyGave(a, add, ga.v);
    REQUIRE(TallyToAdd(b, add)); for (int i = 0; i < T_COUNT; ++i) gb.v[i] += add[i]; TallyGave(b, add, gb.v);
    for (int i = 0; i < T_COUNT; ++i) CHECK_EQ(ga.v[i], gb.v[i]);   // both show the joint total
    CHECK_EQ(ga.v[T_COPS_DESTROYED], 2);
    CHECK_EQ(ga.v[T_PROPERTY], 1200);
    CHECK_EQ(TallyCost(ga.v), 2 * 5000 + 1200);
    // what was credited is never sent back as our own (no ping-pong growth)
    TallyObserve(a, ga.v); TallyObserve(b, gb.v);
    CHECK_EQ(a.own[T_ROADBLOCKS], 0);
    CHECK_EQ(b.own[T_COPS_DESTROYED], 0);
    TallyPeer(a, 1, b.own, true); TallyPeer(b, 1, a.own, true);
    CHECK(!TallyToAdd(a, add));
    CHECK(!TallyToAdd(b, add));
    // the same packet again (UDP repeat) adds nothing
    TallyPeer(a, 1, b.own, true);
    CHECK(!TallyToAdd(a, add));
}
TEST(pursuit_tally_lost_packets_and_reorder) {
    TallyShare a; Tal g; TallyStart(a, g.v);
    Tal t1, t2, t3; t1.v[T_COPS_DAMAGED] = 1; t2.v[T_COPS_DAMAGED] = 3; t3.v[T_COPS_DAMAGED] = 4;
    TallyPeer(a, 7, t1.v, true);
    TallyPeer(a, 7, t3.v, true);                             // t2 was lost: cumulative, nothing is missed
    TallyPeer(a, 7, t2.v, true);                             // a late copy of t2: ignored
    int32_t add[T_COUNT]; TallyToAdd(a, add);
    CHECK_EQ(add[T_COPS_DAMAGED], 4);
}
TEST(pursuit_tally_only_while_both_live) {
    TallyShare a; Tal g;
    Tal t; t.v[T_SPIKES] = 2;
    TallyPeer(a, 5, t.v, true);                              // we have no pursuit yet: not ours
    TallyStart(a, g.v);
    int32_t add[T_COUNT];
    CHECK(!TallyToAdd(a, add));
    t.v[T_SPIKES] = 3;
    TallyPeer(a, 5, t.v, false);                             // not joint (other event, a menu): not yet
    CHECK(!TallyToAdd(a, add));
    t.v[T_SPIKES] = 5;
    TallyPeer(a, 5, t.v, true);                              // joint: their whole count (1.9.3)
    TallyToAdd(a, add);
    CHECK_EQ(add[T_SPIKES], 5);
    TallyGave(a, add, g.v);
    TallyStop(a);                                            // our pursuit ended (escaped / busted)
    t.v[T_SPIKES] = 9; TallyPeer(a, 5, t.v, true);
    CHECK(!TallyToAdd(a, add));
    TallyStart(a, g.v);                                      // our next pursuit, theirs goes on
    t.v[T_SPIKES] = 10; TallyPeer(a, 5, t.v, true);
    TallyToAdd(a, add);
    CHECK_EQ(add[T_SPIKES], 5);                              // 6..10 only: 1..5 went to our last pursuit
}
TEST(pursuit_tally_new_pursuit_and_bank_reset) {
    TallyShare a; Tal g; g.v[T_BOUNTY28] = 5000;             // pending bounty from before
    TallyStart(a, g.v);
    TallyObserve(a, g.v);
    CHECK_EQ(a.own[T_BOUNTY28], 0);                          // only rises in this pursuit are ours
    g.v[T_BOUNTY28] = 5600; TallyObserve(a, g.v);
    CHECK_EQ(a.own[T_BOUNTY28], 600);
    g.v[T_BOUNTY28] = 0; TallyObserve(a, g.v);               // the game banked it
    CHECK_EQ(a.own[T_BOUNTY28], 600);
    g.v[T_BOUNTY28] = 100; TallyObserve(a, g.v);
    CHECK_EQ(a.own[T_BOUNTY28], 700);
    // their new pursuit (new id) counts from 0 again
    Tal t; t.v[T_BOUNTY24] = 800; TallyPeer(a, 1, t.v, true);
    t.v[T_BOUNTY24] = 300; TallyPeer(a, 2, t.v, true);
    int32_t add[T_COUNT]; TallyToAdd(a, add);
    CHECK_EQ(add[T_BOUNTY24], 1100);
    // garbage is clamped, never negative
    t.v[T_PROPERTY] = -50; TallyPeer(a, 2, t.v, true);
    TallyToAdd(a, add);
    CHECK_EQ(add[T_PROPERTY], 0);
}
TEST(pursuit_tally_cost_formula) {
    Tal t;
    t.v[T_COPS_DESTROYED] = 1; CHECK_EQ(TallyCost(t.v), 5000);
    t = Tal{}; t.v[T_COPS_DAMAGED] = 4; CHECK_EQ(TallyCost(t.v), 1000);
    t = Tal{}; t.v[T_STAT188] = 1; t.v[T_STAT180] = 1; CHECK_EQ(TallyCost(t.v), 675);
    t = Tal{}; t.v[T_BOUNTY28] = 999; CHECK_EQ(TallyCost(t.v), 0);   // bounty is not a cost
}

// ---- remote cops in a Challenge Series pursuit / the results screen (1.7.4) ---------------------
TEST(pursuit_remote_cops_chase_us_in_free_roam_and_pursuit_challenges) {
    using SharedPursuit::Place; using SharedPursuit::OpenWorldShared;
    CHECK(OpenWorldShared(true, Place::FreeRoam, false));
    CHECK(OpenWorldShared(true, Place::None, false));                 // free roam here (as before)
    CHECK(OpenWorldShared(false, Place::SameEvent, true));            // both in the same pursuit challenge
    CHECK(!OpenWorldShared(false, Place::SameEvent, false));          // a race with cops: no shared heat (their cops: PeerCopsEngage)
    CHECK(!OpenWorldShared(false, Place::None, true));                // the other one isn't in it
}
TEST(pursuit_frozen_on_the_results_screen) {
    using SharedPursuit::PursuitFrozenForResults;
    CHECK(PursuitFrozenForResults(true, true, false));                // won / finished: our car is the AI's
    CHECK(PursuitFrozenForResults(true, false, true));                // a PostRace screen is up
    CHECK(PursuitFrozenForResults(false, false, true));
    CHECK(!PursuitFrozenForResults(true, false, false));              // still racing / chased
    CHECK(!PursuitFrozenForResults(false, true, false));              // free roam: a stale flag means nothing
}

// ---- the other player's cops in any event both are in; busted together in events --------------
TEST(pursuit_peer_cops_engage_us_in_the_same_event) {
    using SharedPursuit::Place; using SharedPursuit::PeerCopsEngage;
    CHECK(PeerCopsEngage(true, Place::FreeRoam, false, false, false));     // free roam as before
    CHECK(PeerCopsEngage(true, Place::SameEvent, false, false, false));    // a pursuit challenge as before
    CHECK(PeerCopsEngage(false, Place::SameEvent, false, false, false));   // a tollbooth with cops: now too
    CHECK(!PeerCopsEngage(false, Place::None, false, false, false));       // they are in another event / a menu
    CHECK(!PeerCopsEngage(false, Place::SameEvent, true, false, false));   // results screen
    CHECK(!PeerCopsEngage(true, Place::FreeRoam, false, true, false));     // a cutscene
    CHECK(!PeerCopsEngage(false, Place::SameEvent, false, false, true));   // the event is still loading / its intro
    CHECK(PeerCopsEngage(true, Place::FreeRoam, false, false, true));      // (a stale race stage means nothing in free roam)
}
TEST(pursuit_their_pursuit_pulls_us_in_only_close_by_in_a_race) {
    using SharedPursuit::JoinTheirPursuit; using SharedPursuit::EVENT_JOIN_M;
    CHECK(JoinTheirPursuit(true, true, -1.f));          // free roam: anywhere (one shared chase)
    CHECK(JoinTheirPursuit(true, true, 5000.f));
    CHECK(JoinTheirPursuit(false, true, 40.f));         // a race: they are chased right next to us
    CHECK(JoinTheirPursuit(false, true, EVENT_JOIN_M));
    CHECK(!JoinTheirPursuit(false, true, EVENT_JOIN_M + 1.f));   // far ahead / behind on the route
    CHECK(!JoinTheirPursuit(false, true, -1.f));        // their car unknown here
    CHECK(!JoinTheirPursuit(false, true, std::numeric_limits<float>::quiet_NaN()));
    CHECK(!JoinTheirPursuit(true, false, 10.f));        // their cops don't engage us now
}
TEST(pursuit_busted_together_only_in_the_same_event) {
    using namespace SharedPursuit;
    EventBust b;
    const uint32_t EV = 0x1234ABCD;
    CHECK(!BustWithThem(b, EV, 1000, false, false));            // never seen together in it
    EventSeenTogether(b, true, EV, 1000);
    CHECK(BustWithThem(b, EV, 1000, false, false));             // both in it: busted with them
    CHECK(BustWithThem(b, EV, 1000 + SAME_EVENT_GRACE_MS, false, false));   // their state left the event just after the bust
    CHECK(!BustWithThem(b, EV, 1001 + SAME_EVENT_GRACE_MS, false, false));
    CHECK(!BustWithThem(b, 0, 1000, false, false));             // free roam: busts are personal
    CHECK(!BustWithThem(b, 0x99, 1000, false, false));          // we are in another event now
    CHECK(!BustWithThem(b, EV, 1000, true, false));             // we already finished / results screen
    CHECK(!BustWithThem(b, EV, 1000, false, true));             // already busted ourselves
    EventSeenTogether(b, false, EV, 9000);                      // not together: nothing refreshed
    CHECK(!BustWithThem(b, EV, 9000, false, false));
    EventSeenTogether(b, true, 0, 9000);                        // no event: nothing refreshed
    CHECK(!BustWithThem(b, EV, 9000, false, false));
    // the tick counter wraps
    EventBust w; EventSeenTogether(w, true, EV, 0xFFFFFF00u);
    CHECK(BustWithThem(w, EV, 0x100u, false, false));
}

TEST(pursuit_peer_cops_on_map_like_ours) {
    using namespace SharedPursuit;
    CHECK(!PeerCopOnOurMap(false, false, true, 0));               // not chased: none (they stayed after an escape)
    CHECK(!PeerCopOnOurMap(false, true, false, 0));               // chased, but this cop never saw us
    CHECK(PeerCopOnOurMap(false, true, true, 1000));              // engaged
    CHECK(!PeerCopOnOurMap(false, true, true, PEER_COP_ENGAGED_MS + 1));   // lost us long ago
    CHECK(PeerCopOnOurMap(true, false, false, 0));                // the game shows every cop (debug)
}

TEST(pursuit_partner_meter_fill_matches_the_game) {
    using namespace SharedPursuit;
    CHECK_NEAR(MeterV(-100), 1.f, 1e-6f);                 // busted end of the bar: the meter's +1
    CHECK_NEAR(MeterV(100), -1.f, 1e-6f);
    CHECK_NEAR(MeterV(500), -1.f, 1e-6f);                 // clamped
    ChaseFill z = ChaseMeterFill(0.f);
    CHECK(z.midPulse); CHECK(!z.bustAlert); CHECK(!z.evadeAlert);
    CHECK_NEAR(z.bustFar + z.bustNear + z.evadeNear + z.evadeFar, 0.f, 1e-6f);
    CHECK(!ChaseMeterFill(0.1f).midPulse);                // the game: pulse only while |v| < 0.1
    CHECK(!ChaseMeterFill(-0.1f).midPulse);
    ChaseFill b = ChaseMeterFill(0.75f);
    CHECK_NEAR(b.bustFar, 0.5f, 1e-6f); CHECK_NEAR(b.bustNear, 0.9f, 1e-6f); CHECK(b.bustAlert);
    CHECK_NEAR(b.evadeNear, 0.f, 1e-6f);
    ChaseFill e = ChaseMeterFill(-0.3f);
    CHECK_NEAR(e.evadeNear, 0.5f, 1e-5f); CHECK_NEAR(e.evadeFar, 0.f, 1e-6f); CHECK(!e.evadeAlert);
    ChaseFill f = ChaseMeterFill(-1.f);
    CHECK_NEAR(f.evadeNear, 1.f, 1e-6f); CHECK_NEAR(f.evadeFar, 1.f, 1e-6f); CHECK(f.evadeAlert);
}

TEST(pursuit_partner_meter_follows_smoothly) {
    using namespace SharedPursuit;
    CHECK_NEAR(FollowValue(0.f, 1.f, 0.f, 12.f), 0.f, 1e-6f);        // no time: no step
    float v = FollowValue(0.f, 1.f, 0.016f, 12.f);
    CHECK(v > 0.15f && v < 0.25f);                                    // one frame: part of the way
    CHECK_NEAR(FollowValue(0.f, 1.f, 1.f, 12.f), 1.f, 1e-6f);        // a long gap: there at once
    CHECK_NEAR(FollowValue(0.998f, 1.f, 0.016f, 12.f), 1.f, 1e-6f);  // lands exactly
}

TEST(pursuit_bar_from_the_game_meter) {
    using namespace SharedPursuit;
    Bar a = BarFromMeter(false, false, false, 0.5f, 0.f, 0.f); CHECK_EQ((int)a.kind, 0);
    Bar b = BarFromMeter(true, false, false, 0.3f, 0.f, 0.f);  CHECK_EQ((int)b.kind, 1); CHECK_EQ((int)b.value, -30);   // towards busted
    Bar c = BarFromMeter(true, false, true, 0.2f, 0.f, 0.f);   CHECK_EQ((int)c.value, -100);                         // busted: full
    Bar d = BarFromMeter(true, false, false, -0.6f, 0.f, 0.f); CHECK_EQ((int)d.value, 60);                           // towards evade
    Bar e = BarFromMeter(true, false, false, 1.4f, 0.f, 0.f);  CHECK_EQ((int)e.value, -99);                          // capped at 0.99
    Bar f = BarFromMeter(true, true, false, -1.f, 30.f, 60.f); CHECK_EQ((int)f.kind, 2); CHECK_EQ((int)f.value, 50); // cooldown
    Bar g = BarFromMeter(true, false, false, -1.f, 0.f, 0.f);  CHECK_EQ((int)g.kind, 1); CHECK_EQ((int)g.value, 100);  // -1 in the chase: full evade (not yet cooling)
    CHECK_NEAR(MeterV(b.value), 0.3f, 1e-6f);
}

TEST(pursuit_busted_together_in_a_shared_free_roam_pursuit) {
    using namespace SharedPursuit;
    CHECK(BustTogetherInFreeRoam(0, true, true, false, false));      // both in it: busted too
    CHECK(!BustTogetherInFreeRoam(0, false, true, false, false));    // we already escaped: nothing
    CHECK(!BustTogetherInFreeRoam(0, true, false, false, false));    // never shared with them
    CHECK(!BustTogetherInFreeRoam(0x1234, true, true, false, false)); // an event: the event rule decides
    CHECK(!BustTogetherInFreeRoam(0, true, true, true, false));      // results screen
    CHECK(!BustTogetherInFreeRoam(0, true, true, false, true));      // already busted
}

TEST(pursuit_partner_ready_debounced) {
    using namespace SharedPursuit;
    ReadyDebounce d;
    CHECK(!DebounceReady(d, false, 1000));
    CHECK(!DebounceReady(d, true, 1100));                 // just became ready: not yet
    CHECK(!DebounceReady(d, false, 1300));                // a flicker: never counted
    CHECK(!DebounceReady(d, true, 1400));
    CHECK(DebounceReady(d, true, 1400 + READY_HOLD_MS));  // held: ready
    CHECK(DebounceReady(d, false, 2200));                 // spotted for a moment: still ready
    CHECK(DebounceReady(d, true, 2400));
    CHECK(DebounceReady(d, false, 3000));
    CHECK(!DebounceReady(d, false, 3000 + READY_HOLD_MS)); // spotted for good: not ready
}

TEST(pursuit_cooldown_grace) {
    using namespace SharedPursuit;
    CHECK(InCooldownGrace(true, 0));
    CHECK(InCooldownGrace(true, COOLDOWN_GRACE_MS - 1));
    CHECK(!InCooldownGrace(true, COOLDOWN_GRACE_MS));
    CHECK(!InCooldownGrace(false, 10));
}
