// Shared pursuits (src/pursuitutil.h): heat sharing, personal evade bars, frozen cooldown until
// both are in, escape when both finish, and the pursuit fields of the state packet.
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
    CHECK(v.busted); CHECK(!v.inPursuit);
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
