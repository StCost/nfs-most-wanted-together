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

// ---- cooldown ------------------------------------------------------------------------------------
namespace {
    View V(bool in, bool seen, bool cooling, float left, bool ready = false, bool busted = false) {
        View v{};
        v.inPursuit = in; v.seen = seen; v.cooling = cooling; v.left = left; v.ready = ready; v.busted = busted;
        return v;
    }
}

TEST(pursuit_evade_bar_full_is_local) {
    CHECK(!EvadeBarFull(true, false, 3.f));                    // seen: the bar is not full
    CHECK(!EvadeBarFull(false, false, 1.0f));                  // hidden, not long enough
    CHECK(EvadeBarFull(false, false, 2.45f));                  // just under the 2.5 s threshold (slack)
    CHECK(EvadeBarFull(false, false, 2.5f));
    CHECK(EvadeBarFull(false, true, 0.f));                     // already in cooldown
    CHECK(!EvadeBarFull(true, true, 2.5f));                    // seen wins: the bar is not full
}

TEST(pursuit_cooldown_enters_immediately_and_freezes_until_both) {
    // A evaded and is in cooldown; B is still seen. A enters immediately; the meter is frozen.
    View a = V(true, false, true, 18, true), b = V(true, true, false, 20, false);
    CHECK(DecideCooldown(a, b, true, false).what == Cool::Freeze);
    CHECK(DecideCooldown(b, a, true, true).what == Cool::Free);    // B is not in cooldown: vanilla
    // the other way: B in cooldown, A still chasing
    CHECK(DecideCooldown(V(true, false, true, 18, true), V(true, true, false, 20, false), true, false).what == Cool::Freeze);
    CHECK(DecideCooldown(V(true, true, false, 20, false), V(true, false, true, 18, true), true, true).what == Cool::Free);
    // both in cooldown: meters run
    b.seen = false; b.cooling = true; b.ready = true; b.left = 15;
    CHECK(DecideCooldown(a, b).what == Cool::Free);
    CHECK(DecideCooldown(b, a).what == Cool::Free);
    // a player still hiding (not yet in cooldown) does not enter early - vanilla, Free
    View hiding = V(true, false, false, 20, true);
    CHECK(DecideCooldown(hiding, a).what == Cool::Free);
}

TEST(pursuit_cooldown_reset_when_peer_spotted_during_progress) {
    // both were in cooldown; B is spotted. A stays in cooldown, meter resets.
    View a = V(true, false, true, 8, true), b = V(true, true, false, 20, false);
    CHECK(DecideCooldown(a, b, true, true).what == Cool::Reset);
    // later ticks: B still out, already reset -> freeze
    CHECK(DecideCooldown(a, b, true, false).what == Cool::Freeze);
    // A was spotted: vanilla drops A; we must not freeze or reset A
    a.seen = true;
    CHECK(DecideCooldown(a, b, true, true).what == Cool::Free);
}

TEST(pursuit_cooldown_spot_during_frozen_wait) {
    // A is frozen waiting for B, who has not entered yet. Still Freeze, not Reset.
    View a = V(true, false, true, 18, true), b = V(true, true, false, 20, false);
    CHECK(DecideCooldown(a, b, true, false).what == Cool::Freeze);
    b.seen = false; b.ready = false;                           // B hiding, bar not full, not in cooldown
    CHECK(DecideCooldown(a, b, true, false).what == Cool::Freeze);
}

TEST(pursuit_escape_completes_only_when_both_finish) {
    View a = V(true, false, true, 0.1f, true), b = V(true, false, true, 8, true);
    CHECK(DecideCooldown(a, b).what == Cool::HoldEscape);
    CHECK(DecideCooldown(b, a).what == Cool::Free);            // B still has meter left: keep running
    b.left = 0.1f;                                             // both finished
    CHECK(DecideCooldown(a, b).what == Cool::Free);
    CHECK(DecideCooldown(b, a).what == Cool::Free);
}

TEST(pursuit_cooldown_disconnect_unfreezes) {
    View me = V(true, false, true, 10, true), peer = V(true, true, false, 20, false);
    CHECK(DecideCooldown(me, peer, true, false).what == Cool::Freeze);
    CHECK(DecideCooldown(me, peer, false, false).what == Cool::Free);  // disconnect: solo, unfreeze
    View busted = V(true, false, false, 0, false, true);
    CHECK(DecideCooldown(me, busted).what == Cool::Free);
    CHECK(DecideCooldown(me, V(false, false, false, 0)).what == Cool::Free);  // peer left / escaped
}

TEST(pursuit_cooldown_walkthrough_both_roles) {
    View a = V(true, true, false, 20, false), b = V(true, true, false, 20, false);
    CHECK(DecideCooldown(a, b).what == Cool::Free);
    CHECK(DecideCooldown(b, a).what == Cool::Free);
    a.seen = false; a.ready = true; a.cooling = true; a.left = 20;      // A evades, enters cooldown
    CHECK(DecideCooldown(a, b, true, false).what == Cool::Freeze);
    CHECK(DecideCooldown(b, a, true, true).what == Cool::Free);
    b.seen = false; b.ready = true; b.cooling = true; b.left = 20;      // B evades too: both run
    CHECK(DecideCooldown(a, b, true, false).what == Cool::Free);
    CHECK(DecideCooldown(b, a, true, true).what == Cool::Free);
    a.left = 8; b.left = 8;
    b.seen = true; b.ready = false; b.cooling = false;                 // B spotted during progress
    CHECK(DecideCooldown(a, b, true, true).what == Cool::Reset);
    CHECK(DecideCooldown(b, a, true, true).what == Cool::Free);        // B: vanilla, back to pursuit
    CHECK(DecideCooldown(a, b, true, false).what == Cool::Freeze);     // A stays frozen at 0
    b.seen = false; b.ready = true; b.cooling = true; b.left = 20;     // B evades again
    CHECK(DecideCooldown(a, b, true, false).what == Cool::Free);
    CHECK(DecideCooldown(b, a, true, true).what == Cool::Free);
}

TEST(pursuit_delay_progress) {
    // needed 30 s, progress 20 (search 5 + since seen 15) -> 10 left; the other one has 14 left
    float search = 5, since = 15;
    DelayProgress(30, 14, search, since);
    CHECK_NEAR(search, 1, 1e-4);                         // taken from the search time first
    CHECK_NEAR(since, 15, 1e-4);
    search = 1; since = 15;
    DelayProgress(30, 20, search, since);                // 10 more: search to 0, then since seen
    CHECK_NEAR(search, 0, 1e-4);
    CHECK_NEAR(since, 10, 1e-4);
    // never back down to the "seen" limit: that would end the cooldown
    search = 0; since = 9;
    DelayProgress(30, 29, search, since);
    CHECK_NEAR(since, SEEN_LIMIT_S + 0.1f, 1e-4);
    // already behind: nothing changes
    search = 2; since = 8;
    DelayProgress(30, 5, search, since);
    CHECK_NEAR(search, 2, 1e-4); CHECK_NEAR(since, 8, 1e-4);
    // garbage in: untouched
    float nan = std::numeric_limits<float>::quiet_NaN();
    search = 2; since = 8;
    DelayProgress(nan, 5, search, since);
    DelayProgress(30, nan, search, since);
    CHECK_NEAR(search, 2, 1e-4); CHECK_NEAR(since, 8, 1e-4);
}

TEST(pursuit_reset_and_freeze_progress) {
    float search = 5, since = 15;
    ResetCooldownProgress(search, since);
    CHECK_NEAR(search, 0, 1e-4);
    CHECK_NEAR(since, COOLDOWN_FLOOR_S, 1e-4);
    // freeze holds the snapshot (and will not go below the "seen" floor)
    float acc = 4, sn = 12;
    FreezeProgress(1, 8, acc, sn);
    CHECK_NEAR(acc, 1, 1e-4);
    CHECK_NEAR(sn, 8, 1e-4);
    acc = 3; sn = 2;                                       // 2 s would look "seen" and cancel cooldown
    FreezeProgress(0, 2, acc, sn);
    CHECK_NEAR(sn, COOLDOWN_FLOOR_S, 1e-4);
    float acc2 = 2, sn2 = 8, need = 20;
    CompleteCooldown(need, acc2, sn2);
    CHECK_NEAR(acc2 + sn2, need, 1e-4);
    CHECK(sn2 >= COOLDOWN_FLOOR_S);
}

TEST(pursuit_safehouse_hidden_until_both_cooling) {
    // live shared pursuit, one still chasing: hide
    View a = V(true, false, true, 18, true), b = V(true, true, false, 20, false);
    CHECK(SharedPursuitLive(a, b, true));
    CHECK(HideSafehouse(a, b, true));
    CHECK(!SafehouseAllowed(a, b, true));
    // both in cooldown (progressing or hold-escape): show / allow entry
    b.seen = false; b.cooling = true; b.ready = true; b.left = 15;
    CHECK(SafehouseAllowed(a, b, true));
    CHECK(!HideSafehouse(a, b, true));
    CHECK(SafehouseAllowed(b, a, true));
    // peer spotted during a progressing cooldown: hide again (we freeze/reset)
    b.seen = true; b.cooling = false; b.ready = false;
    CHECK(HideSafehouse(a, b, true));
    // disconnect / busted / peer left: solo, markers normal
    CHECK(!HideSafehouse(a, b, false));
    CHECK(!HideSafehouse(a, V(true, false, false, 0, false, true), true));
    CHECK(!HideSafehouse(a, V(false, false, false, 0), true));
    // neither in a pursuit
    CHECK(!HideSafehouse(V(false, false, false, 0), V(false, false, false, 0), true));
}

TEST(pursuit_peer_safehouse_forces_our_escape) {
    View me = V(true, false, true, 8, true), peer = V(false, false, false, 0);
    peer.evaded = true;
    CHECK(DecideCooldown(me, peer, true, true).what == Cool::ForceEscape);
    // even if we were still in a chase, or frozen waiting
    View chasing = V(true, true, false, 20, false);
    CHECK(DecideCooldown(chasing, peer, true).what == Cool::ForceEscape);
    // disconnect is solo, not a force-escape
    CHECK(DecideCooldown(me, peer, false, true).what == Cool::Free);
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
    CHECK(PeerEscapeCounts(Place::FreeRoam, true, false));         // free roam: as before (safehouse)
    CHECK(PeerEscapeCounts(Place::None, true, false));
    CHECK(!PeerEscapeCounts(Place::SameEvent, true, false));       // an escape from before this event
    CHECK(PeerEscapeCounts(Place::SameEvent, true, true));         // they escaped in this event: ours too
    CHECK(!PeerEscapeCounts(Place::SameEvent, false, true));
}

// The reported case: in a Challenge Series pursuit the client lost the cops and its meter ran out
// while the host was still chased. The rule: cooldown state yes, progress no, until both are in it;
// then both run, and the one who finishes first waits for the other.
TEST(pursuit_event_cooldown_waits_for_both_then_escapes_together) {
    View client = V(true, false, true, 30, true), host = V(true, true, false, 30, false);
    CHECK(DecideCooldown(client, host, true, false).what == Cool::Freeze);   // no progress alone
    float acc = 0.f, since = 8.f;
    FreezeProgress(0.f, 8.f, acc, since);
    CHECK(acc == 0.f); CHECK(since == 8.f);                                   // held where it started
    CHECK(DecideCooldown(host, client, true, true).what == Cool::Free);      // host still chased: vanilla
    host.seen = false; host.cooling = true; host.ready = true; host.left = 30;
    CHECK(DecideCooldown(client, host, true, false).what == Cool::Free);     // both cooling: both run
    CHECK(DecideCooldown(host, client, true, true).what == Cool::Free);
    client.left = 0.1f; host.left = 3.f;
    Decision d = DecideCooldown(client, host, true, true);
    CHECK(d.what == Cool::HoldEscape);                                        // client waits at the end
    CHECK(d.left > 0.f);
    host.left = 0.1f;
    CHECK(DecideCooldown(client, host, true, true).what == Cool::Free);      // both done: both escape
    CHECK(DecideCooldown(host, client, true, true).what == Cool::Free);
    host.evaded = true;                                                       // host's game ended it first
    CHECK(DecideCooldown(client, host, true, true).what == Cool::ForceEscape);
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
    TallyPeer(a, 5, t.v, false);                             // not joint (other event, a menu): not credited later
    t.v[T_SPIKES] = 5;
    TallyPeer(a, 5, t.v, true);                              // joint again: only what came now
    TallyToAdd(a, add);
    CHECK_EQ(add[T_SPIKES], 2);
    TallyStop(a);                                            // our pursuit ended (escaped / busted)
    t.v[T_SPIKES] = 9; TallyPeer(a, 5, t.v, true);
    CHECK(!TallyToAdd(a, add));
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
