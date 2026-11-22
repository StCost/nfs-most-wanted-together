// Shared pursuits (src/pursuitutil.h): heat sharing, personal evade bars with a shared cooldown
// entry, and the pursuit fields of the state packet through the real handler.
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
    CHECK(!EvadeBarFull(true, true, 2.5f));                    // seen wins: bar is not full
    CHECK_NEAR(WaitUnseenInState(), 2.45f, 1e-4);
}

TEST(pursuit_cooldown_waits_until_peer_is_ready_both_roles) {
    // A is hidden with a full bar; B is still seen. A must not enter cooldown. B's bar is vanilla.
    View a = V(true, false, false, 20, true), b = V(true, true, false, 20, false);
    CHECK(DecideCooldown(a, b).what == Cool::Wait);
    CHECK(DecideCooldown(b, a).what == Cool::Free);            // B is seen: the game resets B's bar
    // B is hidden but their bar is not full yet: A still waits
    b.seen = false; b.ready = false;
    CHECK(DecideCooldown(a, b).what == Cool::Wait);
    CHECK(DecideCooldown(b, a).what == Cool::Free);            // B is still filling
    // same the other way: B ready, A still seen
    View aSeen = V(true, true, false, 20, false), bReady = V(true, false, false, 20, true);
    CHECK(DecideCooldown(bReady, aSeen).what == Cool::Wait);
    CHECK(DecideCooldown(aSeen, bReady).what == Cool::Free);
}

TEST(pursuit_cooldown_enters_when_both_ready) {
    View a = V(true, false, false, 20, true), b = V(true, false, false, 20, true);
    CHECK(DecideCooldown(a, b).what == Cool::Free);
    CHECK(DecideCooldown(b, a).what == Cool::Free);
    // one already in cooldown counts as ready: the other may enter
    a.cooling = true; a.ready = true;
    b.cooling = false; b.ready = true;
    CHECK(DecideCooldown(a, b).what == Cool::Free);
    CHECK(DecideCooldown(b, a).what == Cool::Free);
    // escape timers are personal: a longer time left there does not hold us back
    CHECK(DecideCooldown(V(true, false, true, 4, true), V(true, false, true, 9, true)).what == Cool::Free);
}

TEST(pursuit_cooldown_does_not_cancel_when_peer_is_spotted) {
    // A is already in cooldown; B gets seen. A's cooldown keeps running (personal).
    View a = V(true, false, true, 8, true), b = V(true, true, false, 20, false);
    CHECK(DecideCooldown(a, b).what == Cool::Free);
    CHECK(DecideCooldown(b, a).what == Cool::Free);            // B is seen: vanilla
    // B hidden again, bar not full: still does not pull A out of cooldown
    b.seen = false; b.ready = false;
    CHECK(DecideCooldown(a, b).what == Cool::Free);
}

TEST(pursuit_cooldown_disconnect_and_busted_are_solo) {
    View me = V(true, false, false, 20, true), peer = V(true, true, false, 20, false);
    CHECK(DecideCooldown(me, peer, true).what == Cool::Wait);
    CHECK(DecideCooldown(me, peer, false).what == Cool::Free); // disconnect: enter when our bar is full
    // a busted peer does not block us (busts are personal)
    View busted = V(true, false, false, 0, false, true);
    CHECK(DecideCooldown(me, busted).what == Cool::Free);
    // a peer who already escaped / is not chased
    CHECK(DecideCooldown(me, V(false, false, false, 0)).what == Cool::Free);
    // we are not in a pursuit
    CHECK(DecideCooldown(V(false, false, false, 0), peer).what == Cool::Free);
    // we are seen: vanilla resets our bar, no gate
    CHECK(DecideCooldown(V(true, true, false, 20, false), peer).what == Cool::Free);
    // our bar is not full yet: vanilla fills it, no gate
    CHECK(DecideCooldown(V(true, false, false, 20, false), peer).what == Cool::Free);
}

TEST(pursuit_cooldown_walkthrough_both_roles) {
    // both chased and seen, then A hides (bar fills), then is ready, then B hides and is ready
    View a = V(true, true, false, 20, false), b = V(true, true, false, 20, false);
    CHECK(DecideCooldown(a, b).what == Cool::Free);
    CHECK(DecideCooldown(b, a).what == Cool::Free);
    a.seen = false;                                            // A hiding, bar not full
    CHECK(DecideCooldown(a, b).what == Cool::Free);
    a.ready = true;                                            // A bar full; B still seen
    CHECK(DecideCooldown(a, b).what == Cool::Wait);
    CHECK(DecideCooldown(b, a).what == Cool::Free);
    b.seen = false;                                            // B hiding, bar not full
    CHECK(DecideCooldown(a, b).what == Cool::Wait);
    b.ready = true;                                            // both ready: both enter
    CHECK(DecideCooldown(a, b).what == Cool::Free);
    CHECK(DecideCooldown(b, a).what == Cool::Free);
    a.cooling = true; b.cooling = true;                        // both in cooldown; B is spotted
    b.seen = true; b.ready = false; b.cooling = false;
    CHECK(DecideCooldown(a, b).what == Cool::Free);            // A stays in cooldown
    CHECK(DecideCooldown(b, a).what == Cool::Free);            // B: seen, vanilla
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
