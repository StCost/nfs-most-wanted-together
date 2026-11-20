// Pure helpers from src/netutil.h: names, traffic ids, interpolation math, validation.
#include "net_helpers.h"
#include <limits>

using namespace NetUtil;

TEST(util_hud_name_truncates_to_15) {
    char buf[32];
    memset(buf, 'x', sizeof(buf));
    CopyHudName(buf, "ABCDEFGHIJKLMNOPQRSTUVWXYZ");
    CHECK_STR(buf, "ABCDEFGHIJKLMNO");               // 15 characters
    CHECK_EQ(strlen(buf), 15);
    CHECK_EQ(buf[16], 'x');                          // nothing written past the 16-byte slot

    CopyHudName(buf, "Short");
    CHECK_STR(buf, "Short");
    CopyHudName(buf, "123456789012345");             // exactly 15 fits
    CHECK_STR(buf, "123456789012345");
    CopyHudName(buf, "");
    CHECK_STR(buf, "");
    CopyHudName(buf, nullptr);
    CHECK_STR(buf, "");
}

TEST(util_traffic_id_scheme) {
    CHECK_EQ(CLIENT_ID_BIT, 0x8000);
    // host ids go out as they are, client ids carry bit 15
    CHECK_EQ(TrafficWireId(5, true), 5);
    CHECK_EQ(TrafficWireId(5, false), 0x8005);
    CHECK(!TrafficIdIsClients(5));
    CHECK(TrafficIdIsClients(0x8005));
    // each side mirrors only the other side's cars
    CHECK(TrafficIdAccepted(0x8005, /*weAreHost*/ true));
    CHECK(!TrafficIdAccepted(5, true));
    CHECK(TrafficIdAccepted(5, false));
    CHECK(!TrafficIdAccepted(0x8005, false));
    // a car never comes back to its owner as a mirror
    for (uint16_t id : { (uint16_t)1, (uint16_t)77, (uint16_t)0x7FFF }) {
        CHECK(!TrafficIdAccepted(TrafficWireId(id, true), true));
        CHECK(!TrafficIdAccepted(TrafficWireId(id, false), false));
        CHECK(TrafficIdAccepted(TrafficWireId(id, true), false));
        CHECK(TrafficIdAccepted(TrafficWireId(id, false), true));
    }
}

TEST(util_traffic_id_allocation_wraps) {
    CHECK_EQ(NextTrafficLocalId(1), 2);
    CHECK_EQ(NextTrafficLocalId(0x7FFE), 0x7FFF);
    CHECK_EQ(NextTrafficLocalId(0x7FFF), 1);         // wraps before the client bit, skipping 0
    CHECK_EQ(NextTrafficLocalId(0xFFFF), 1);
    uint16_t id = 1;
    for (int i = 0; i < 70000; ++i) {
        id = NextTrafficLocalId(id);
        if (id == 0 || (id & CLIENT_ID_BIT)) { CHECK(!"local id left 1..0x7FFF"); break; }
    }
}

TEST(util_hermite) {
    // endpoints
    CHECK_NEAR(Hermite(3, 7, 9, -2, 0.5f, 0.0f), 3, 1e-5);
    CHECK_NEAR(Hermite(3, 7, 9, -2, 0.5f, 1.0f), 9, 1e-5);
    // constant velocity is reproduced exactly: p(t) = 10 + 20 t over 0.1 s
    for (float u = 0; u <= 1.0f; u += 0.125f)
        CHECK_NEAR(Hermite(10, 20, 12, 20, 0.1f, u), 10 + 2 * u, 1e-4);
    // constant acceleration (v 0 -> 10 over 1 s, 5 m travelled): midpoint is 1.25 m
    CHECK_NEAR(Hermite(0, 0, 5, 10, 1.0f, 0.5f), 1.25, 1e-4);
    // standing still stays still
    CHECK_NEAR(Hermite(4, 0, 4, 0, 0.1f, 0.3f), 4, 1e-5);
    // zero time span: tangents drop out, no NaN
    float z = Hermite(1, 100, 2, 100, 0.0f, 0.5f);
    CHECK_NEAR(z, 1.5, 1e-5);
}

TEST(util_lerp3) {
    float a[3] = { 0, 10, -4 }, b[3] = { 2, 20, 4 }, o[3];
    Lerp3(a, b, 0.25f, o);
    CHECK_NEAR(o[0], 0.5, 1e-6); CHECK_NEAR(o[1], 12.5, 1e-6); CHECK_NEAR(o[2], -2, 1e-6);
    Lerp3(a, b, 0, o); CHECK_NEAR(o[1], 10, 1e-6);
    Lerp3(a, b, 1, o); CHECK_NEAR(o[1], 20, 1e-6);
}

static float QLen(const float* q) { return sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]); }

TEST(util_nlerp) {
    const float id[4] = { 0, 0, 0, 1 };
    const float s = 0.70710678f;
    const float z90[4] = { 0, 0, s, s };             // 90 degrees about z
    float o[4];

    NlerpQ(id, z90, 0, o);
    CHECK_NEAR(o[3], 1, 1e-5); CHECK_NEAR(o[2], 0, 1e-5);
    NlerpQ(id, z90, 1, o);
    CHECK_NEAR(o[2], s, 1e-5); CHECK_NEAR(o[3], s, 1e-5);

    // halfway is the 45 degree rotation, and the result is unit length
    NlerpQ(id, z90, 0.5f, o);
    CHECK_NEAR(QLen(o), 1, 1e-5);
    CHECK_NEAR(o[2], sinf(3.14159265f / 8), 1e-4);
    CHECK_NEAR(o[3], cosf(3.14159265f / 8), 1e-4);

    // q and -q are the same rotation: must take the short way, not swing through zero
    const float negZ90[4] = { 0, 0, -s, -s };
    float o2[4];
    NlerpQ(id, negZ90, 0.5f, o2);
    CHECK_NEAR(QLen(o2), 1, 1e-5);
    for (int i = 0; i < 4; ++i) CHECK_NEAR(o2[i], o[i], 1e-5);

    // always unit length for unit inputs
    for (float t = 0; t <= 1.0f; t += 0.1f) { NlerpQ(id, z90, t, o); CHECK_NEAR(QLen(o), 1, 1e-5); }

    // degenerate input (all zeros, e.g. a peer that isn't driving): no NaN, no division by zero
    const float zero[4] = { 0, 0, 0, 0 };
    NlerpQ(zero, zero, 0.5f, o);
    for (int i = 0; i < 4; ++i) CHECK(o[i] == 0.0f);
}

TEST(util_validation_rejects_bad_floats) {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();

    CarState s{};                                    // all zeros: "not driving" - valid
    CHECK(ValidCarState(s));
    s = th::MakeState(1, 12.5f, 3000.0f, 90.0f).s;
    CHECK(ValidCarState(s));

    CarState b = s; b.pos[1] = nan;        CHECK(!ValidCarState(b));
    b = s; b.pos[2] = inf;                 CHECK(!ValidCarState(b));
    b = s; b.pos[0] = -inf;                CHECK(!ValidCarState(b));
    b = s; b.pos[0] = 5.0e6f;              CHECK(!ValidCarState(b));   // outside any game world
    b = s; b.rot[0] = nan;                 CHECK(!ValidCarState(b));
    b = s; b.rot[3] = 50.0f;               CHECK(!ValidCarState(b));   // not a rotation
    b = s; b.linVel[2] = 1.0e30f;          CHECK(!ValidCarState(b));
    b = s; b.angVel[0] = nan;              CHECK(!ValidCarState(b));
    b = s; b.speedMps = inf;               CHECK(!ValidCarState(b));
    b = s; b.simTime = nan;                CHECK(!ValidCarState(b));
    b = s; b.simTime = inf;                CHECK(!ValidCarState(b));
    b = s; b.steer = nan;                  CHECK(!ValidCarState(b));
    b = s; b.steer = 40.0f;                CHECK(!ValidCarState(b));   // not a steering value
    b = s; b.rot[0] = b.rot[1] = b.rot[2] = b.rot[3] = 0; CHECK(!ValidCarState(b));   // a car with no rotation
    b = s; b.racePct = nan;                CHECK(!ValidCarState(b));
    b = s; b.racePct = 1.0e30f;            CHECK(!ValidCarState(b));
    b = s; b.lapPct = -50.0f;              CHECK(!ValidCarState(b));
    b = s; b.laps = -3;                    CHECK(!ValidCarState(b));
    b = s; b.checkpoints = 30000;          CHECK(!ValidCarState(b));
    b = s; b.racePct = 100.0f; b.laps = 3; CHECK(ValidCarState(b));    // a finished race

    TrafficEntry e = th::MakeTrafficEntry(1, 100.0f);
    CHECK(ValidTrafficEntry(e));
    TrafficEntry eb = e; eb.pos[0] = nan;  CHECK(!ValidTrafficEntry(eb));
    eb = e; eb.linVel[1] = inf;            CHECK(!ValidTrafficEntry(eb));
    eb = e; eb.rot[2] = nan;               CHECK(!ValidTrafficEntry(eb));
    eb = e; eb.angVel[2] = -1.0e20f;       CHECK(!ValidTrafficEntry(eb));

    // limits are inclusive, signs don't matter
    float v[3] = { -MAX_POS, MAX_POS, 0 };
    CHECK(FiniteWithin(v, 3, MAX_POS));
    v[0] = -MAX_POS * 1.01f;
    CHECK(!FiniteWithin(v, 3, MAX_POS));
}
