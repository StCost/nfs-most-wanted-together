// Shared police roadblocks: checks and the other player's roadblock table (src/roadblockutil.h).
#include "mwt_test.h"
#include "../src/roadblockutil.h"
#include <limits>

using namespace Roadblock;

namespace {
    Desc Make(uint32_t id, float x, float z, int pieces = 1) {
        Desc d{};
        d.id = id; d.setup = 0; d.nCars = 2; d.nPieces = (uint8_t)pieces;
        d.centre[0] = x; d.centre[1] = 10.f; d.centre[2] = z;
        d.dir[0] = 0.f; d.dir[2] = 1.f;
        for (int i = 0; i < pieces; ++i) {
            Piece& p = d.p[i];
            p.kind = PIECE_SAWHORSE;
            p.m[0] = 1; p.m[5] = 1; p.m[10] = 1; p.m[15] = 1;
            p.m[12] = x + 2.f * i; p.m[13] = 10.f; p.m[14] = z;
        }
        return d;
    }
}

TEST(roadblock_valid_desc) {
    CHECK(ValidDesc(Make(1, 100, 200, 3)));
    CHECK(ValidDesc(Make(1, 100, 200, 0)));                  // cars only (pieces knocked off the list): still a roadblock
    Desc d = Make(1, 100, 200, 0); d.nCars = 0;
    CHECK(!ValidDesc(d));                                    // nothing in it
    d = Make(0, 100, 200); CHECK(!ValidDesc(d));             // id 0 is never sent
    d = Make(1, 100, 200); d.flags = 0x80; CHECK(!ValidDesc(d));
    d = Make(1, 100, 200); d.setup = SETUPS_NORMAL; CHECK(!ValidDesc(d));
    d = Make(1, 100, 200); d.flags = RB_SPIKES; d.setup = SETUPS_SPIKES - 1; CHECK(ValidDesc(d));
    d.setup = SETUPS_SPIKES; CHECK(!ValidDesc(d));           // the spike list is shorter
    d = Make(1, 100, 200); d.dir[2] = 0.f; CHECK(!ValidDesc(d));            // no direction
    d = Make(1, 100, 200); d.p[0].m[14] += kPieceFromCentre + 1; CHECK(!ValidDesc(d));
    d = Make(1, 100, 200); d.p[0].m[5] = 0.f; CHECK(!ValidDesc(d));         // a squashed matrix
    d = Make(1, 100, 200); d.p[0].kind = PIECE_NONE; CHECK(!ValidDesc(d));
    d = Make(1, 2e6f, 200); CHECK(!ValidDesc(d));
}

TEST(roadblock_count_kinds) {
    Desc d = Make(3, 0, 0, 3);
    d.p[0].kind = PIECE_SPIKES; d.p[2].kind = PIECE_SPIKES;
    CHECK_EQ(CountKind(d, PIECE_SPIKES), 2);
    CHECK_EQ(CountKind(d, PIECE_SAWHORSE), 1);
    d.nPieces = 1;
    CHECK_EQ(CountKind(d, PIECE_SPIKES), 1);
}

TEST(roadblock_peer_table_alloc_and_evict) {
    PeerTable t;
    bool ev = true;
    CHECK_EQ(t.Find(5), -1);
    for (int i = 0; i < MAX_PEER; ++i) {
        int k = t.Alloc(&ev);
        CHECK(!ev);
        t.e[k].used = true; t.e[k].d = Make(10 + i, 0, 0); t.e[k].lastMs = 1000 + i * 10;
    }
    CHECK_EQ(t.Live(), MAX_PEER);
    CHECK_EQ(t.Find(12), 2);
    t.e[1].lastMs = 900;                                     // heard of longest ago
    int k = t.Alloc(&ev);
    CHECK(ev); CHECK_EQ(k, 1);
    // tick counter wrap: 0xFFFFFF00 is older than 0x10
    PeerTable w; w.e[0].used = w.e[1].used = w.e[2].used = w.e[3].used = true;
    w.e[0].lastMs = 0x10; w.e[1].lastMs = 0xFFFFFF00u; w.e[2].lastMs = 0x20; w.e[3].lastMs = 0x30;
    CHECK_EQ(w.Alloc(&ev), 1);
}

TEST(roadblock_expiry_and_range) {
    PeerEntry x; x.used = true; x.d = Make(1, 0, 0); x.lastMs = 1000;
    CHECK(!Expired(x, 1000 + kExpireMs));
    CHECK(Expired(x, 1001 + kExpireMs));
    float near[3] = { 0, 10, kPlaceRange - 1 }, far[3] = { 0, 10, kPlaceRange + 1 };
    CHECK(InRange(x, near, kPlaceRange));
    CHECK(!InRange(x, far, kPlaceRange));
    CHECK(!InRange(x, nullptr, kPlaceRange));
}

TEST(roadblock_suppress_ours_only_near_a_live_one_of_theirs) {
    PeerTable t;
    float me[3] = { 0, 10, 0 };
    CHECK(!SuppressOurs(t, me, 5000, true));                 // none of theirs
    t.e[0].used = true; t.e[0].d = Make(1, 0, 250); t.e[0].lastMs = 5000;
    CHECK(SuppressOurs(t, me, 5000, true));                  // theirs 250 m ahead: ours waits
    CHECK(!SuppressOurs(t, me, 5000, false));                // not linked (older build, menus): vanilla
    CHECK(!SuppressOurs(t, nullptr, 5000, true));
    CHECK(!SuppressOurs(t, me, 5001 + kExpireMs, true));     // the owner stopped saying it is there
    float away[3] = { 0, 10, 250 + kSuppressRange + 1 };
    CHECK(!SuppressOurs(t, away, 5000, true));               // their pursuit is elsewhere
}

TEST(roadblock_passing_theirs_counts) {
    Desc d = Make(7, 100.f, 200.f);             // road along +z
    float along, across, up;
    float before[3] = { 103.f, 10.5f, 195.f }, after[3] = { 103.f, 10.5f, 206.f };
    RoadblockFrame(d, before, &along, &across, &up);
    CHECK_NEAR(along, -5.f, 1e-4); CHECK_NEAR(std::fabs(across), 3.f, 1e-4);   // which side is "left" does not matter
    float a0 = along;
    RoadblockFrame(d, after, &along, &across, &up);
    CHECK(PassedRoadblock(a0, along, across, up));      // through it
    CHECK(PassedRoadblock(along, a0, across, up));      // the other way too
    CHECK(!PassedRoadblock(-8.f, -2.f, 3.f, 0.f));      // not there yet
    CHECK(!PassedRoadblock(-5.f, 5.f, kPassHalfWidth + 1.f, 0.f));   // on another road beside it
    CHECK(!PassedRoadblock(-5.f, 5.f, 0.f, kPassHeight + 1.f));      // on a bridge above it
    CHECK(!PassedRoadblock(-30.f, 30.f, 0.f, 0.f));     // a jump (reset to the road), not a pass
    float nan = std::numeric_limits<float>::quiet_NaN();
    CHECK(!PassedRoadblock(nan, 5.f, 0.f, 0.f));
    Desc flat = Make(8, 0.f, 0.f); flat.dir[0] = flat.dir[2] = 0.f;   // no direction: still well-defined
    float p[3] = { 2.f, 10.f, 0.f };
    RoadblockFrame(flat, p, &along, &across, &up);
    CHECK_NEAR(along, 2.f, 1e-4);
}
