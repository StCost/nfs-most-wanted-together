// Blacklist rival race in co-op: both players ready (src/rivalgateutil.h).
#include "mwt_test.h"
#include "../src/rivalgateutil.h"

using namespace RivalGate;

TEST(rivalgate_encode_decode) {
    CHECK_EQ(Encode(false, true, true, 2), 0);            // not allowed: nothing else is sent
    CHECK_EQ(Encode(true, false, true, 2), 1);            // unknown: as older builds
    CHECK_EQ(Encode(true, true, false, 0), 3);
    Peer p = Decode(true, Encode(true, true, true, 2));
    CHECK(p.known); CHECK(p.ready); CHECK_EQ(p.idx, 2);
    p = Decode(true, Encode(true, true, false, 5));
    CHECK(p.known); CHECK(!p.ready); CHECK_EQ(p.idx, 0);
    CHECK_EQ(Decode(true, Encode(true, true, true, 99)).idx, 7);
    CHECK_EQ(Decode(true, Encode(true, true, true, -3)).idx, 0);
    CHECK(!Decode(true, 1).known);                        // an older build sends 1
    CHECK(!Decode(false, Encode(true, true, true, 0)).known);   // no state yet
    CHECK(!Decode(true, kKnown | kReady).known);          // bit 0 clear: nothing to read
}

TEST(rivalgate_older_builds_read_allowed) {
    // older builds test pad[1] != 0: whatever we send must keep that meaning
    for (int known = 0; known < 2; ++known)
        for (int ready = 0; ready < 2; ++ready) {
            CHECK(Encode(true, known != 0, ready != 0, 3) != 0);
            CHECK(Encode(false, known != 0, ready != 0, 3) == 0);
            CHECK(PeerAllowed(true, Encode(true, known != 0, ready != 0, 3)));
            CHECK(!PeerAllowed(true, Encode(false, known != 0, ready != 0, 3)));
        }
    CHECK(PeerAllowed(false, 0));                         // no state yet: allowed
    CHECK(PeerAllowed(true, 1));
}

TEST(rivalgate_gate) {
    Peer notReady = Decode(true, Encode(true, true, false, 0));
    Peer ready0 = Decode(true, Encode(true, true, true, 0));
    Peer ready1 = Decode(true, Encode(true, true, true, 1));
    Peer old = Decode(true, 1);
    // same rival: waits until they have it open too
    CHECK(Gated(true, 14, true, 0, 14, notReady));
    CHECK(!Gated(true, 14, true, 0, 14, ready0));
    CHECK(!Gated(true, 14, true, 0, 14, ready1));         // they are on a later race of the rival
    CHECK(Gated(true, 14, true, 1, 14, ready0));          // they are on an earlier one
    // not ready ourselves, not linked, no / old partner state: nothing changes
    CHECK(!Gated(true, 14, false, 0, 14, notReady));
    CHECK(!Gated(false, 14, true, 0, 14, notReady));
    CHECK(!Gated(true, 14, true, 0, 14, old));
    CHECK(!Gated(true, 14, true, 0, 14, Peer{}));
    // different rivals: wait for a partner behind (higher number), not for one ahead
    CHECK(Gated(true, 14, true, 0, 15, notReady));
    CHECK(Gated(true, 14, true, 0, 15, ready0));
    CHECK(!Gated(true, 14, true, 0, 13, notReady));
    // prologue (16) / no profile (0): never
    CHECK(!Gated(true, 16, true, 0, 16, notReady));
    CHECK(!Gated(true, 15, true, 0, 16, notReady));
    CHECK(!Gated(true, 15, true, 0, 0, notReady));
    CHECK(!Gated(true, 0, true, 0, 15, notReady));
}

TEST(rivalgate_partner_waits_mirrors_gate) {
    Peer ready0 = Decode(true, Encode(true, true, true, 0));
    // they have it open, we don't: they wait for us
    CHECK(PartnerWaits(true, 14, false, 0, 14, ready0));
    CHECK(!PartnerWaits(true, 14, true, 0, 14, ready0));
    CHECK(PartnerWaits(true, 15, false, 0, 14, ready0));  // we are behind them
    CHECK(!PartnerWaits(true, 13, false, 0, 14, ready0)); // we are past their rival
    CHECK(!PartnerWaits(false, 14, false, 0, 14, ready0));
    CHECK(!PartnerWaits(true, 14, false, 0, 14, Decode(true, 1)));
    // never both waiting for each other
    for (int a = 0; a < 3; ++a) for (int b = 0; b < 3; ++b)
        for (int ra = 0; ra < 2; ++ra) for (int rb = 0; rb < 2; ++rb)
            for (int bin = 13; bin <= 15; ++bin) {
                Peer pb = Decode(true, Encode(true, true, rb != 0, b));
                Peer pa = Decode(true, Encode(true, true, ra != 0, a));
                bool aWaits = Gated(true, 14, ra != 0, pa.idx, bin, pb);
                bool bWaits = Gated(true, bin, rb != 0, pb.idx, 14, pa);
                CHECK(!(aWaits && bWaits));
            }
}
