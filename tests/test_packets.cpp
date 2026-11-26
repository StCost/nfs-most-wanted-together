// Traffic, event, roster, racer-name and restart packets. Each round trip builds the datagrams with
// the real Net::Send* serializers on one "PC" (host role), then feeds them to the packet handler
// of the other (client role) and checks what the game layer's callbacks receive.
#include "net_helpers.h"
#include <limits>

using namespace th;

namespace {
    using Datagrams = std::vector<std::vector<uint8_t>>;

    // what the receiving side's callbacks saw
    struct Seen {
        int trafficCalls = 0, trafficCount = -1; double trafficSendMs = 0; std::vector<TrafficEntry> traffic;
        int eventCalls = 0; uint32_t eventHash = 0; std::string eventId;
        int restartCalls = 0; uint32_t restartHash = 0;
        int nameCalls = 0; uint16_t nameId = 0; std::string name;
        int rosterCalls = 0; std::vector<RosterPacket> roster;
    };
    Seen g_seen;

    void HookCallbacks() {
        g_seen = Seen{};
        Net::OnTraffic = [](const TrafficEntry* e, int n, double sendMs) {
            ++g_seen.trafficCalls; g_seen.trafficCount = n; g_seen.trafficSendMs = sendMs;
            g_seen.traffic.assign(e, e + n);
        };
        Net::OnEvent = [](uint32_t h, const char* id) { ++g_seen.eventCalls; g_seen.eventHash = h; g_seen.eventId = id; };
        Net::OnRestart = [](uint32_t h, uint32_t) { ++g_seen.restartCalls; g_seen.restartHash = h; };
        Net::OnRacerName = [](uint16_t id, const char* n) { ++g_seen.nameCalls; g_seen.nameId = id; g_seen.name = n; };
        Net::OnRoster = [](const RosterPacket& p) { ++g_seen.rosterCalls; g_seen.roster.push_back(p); };
    }

    // run `send` on a connected host and return the datagrams it produced
    template <class F> Datagrams SerializeOnHost(F send) {
        NetFixture host(true);
        Connect("Client");
        g_sent.clear();
        send();
        return g_sent;
    }
    void DeliverToClient(const Datagrams& d) {
        for (auto& p : d) InjectRaw(p.data(), (int)p.size());
    }
}

// ---- traffic ----------------------------------------------------------------------------------
TEST(packets_traffic_round_trip) {
    Datagrams d = SerializeOnHost([] {
        TrafficEntry e[5];
        for (int i = 0; i < 5; ++i) e[i] = MakeTrafficEntry((uint16_t)(i + 1), 100.0f * i);
        e[2].flags = 1; e[2].glare = 0xF0F0;
        Net::SendTraffic(e, 5, 12.5f);
    });
    REQUIRE(d.size() == 1);
    CHECK_EQ(d[0].size(), offsetof(TrafficPacket, e) + 5 * sizeof(TrafficEntry));   // only the used entries travel

    NetFixture client(false);
    Connect("Host");
    HookCallbacks();
    DeliverToClient(d);
    REQUIRE(g_seen.trafficCalls == 1);
    REQUIRE(g_seen.trafficCount == 5);
    CHECK_NEAR(g_seen.trafficSendMs, 12500, 1e-6);
    for (int i = 0; i < 5; ++i) {
        CHECK_EQ(g_seen.traffic[i].id, i + 1);
        CHECK_NEAR(g_seen.traffic[i].pos[0], 100.0 * i, 1e-4);
        CHECK(NetUtil::TrafficIdAccepted(g_seen.traffic[i].id, /*weAreHost*/ false));
    }
    CHECK_EQ(g_seen.traffic[2].flags, 1);
    CHECK_EQ(g_seen.traffic[2].glare, 0xF0F0);
}

TEST(packets_traffic_empty_and_clamped_counts) {
    Datagrams d = SerializeOnHost([] {
        TrafficEntry e[30];
        for (int i = 0; i < 30; ++i) e[i] = MakeTrafficEntry((uint16_t)(i + 1), 0);
        Net::SendTraffic(e, 0, 1.0f);                // "my cops are gone"
        Net::SendTraffic(e, 30, 1.0f);               // more than fits: the first 20
        Net::SendTraffic(e, -3, 1.0f);               // nonsense: sent as empty, no huge memcpy
        Net::SendTraffic(nullptr, 4, 1.0f);
    });
    REQUIRE(d.size() == 4);
    CHECK_EQ(d[0].size(), offsetof(TrafficPacket, e));
    CHECK_EQ(d[1].size(), sizeof(TrafficPacket));
    CHECK_EQ(((const TrafficPacket*)d[1].data())->count, MAX_TRAFFIC_PER_PACKET);
    CHECK_EQ(d[2].size(), offsetof(TrafficPacket, e));
    CHECK_EQ(((const TrafficPacket*)d[2].data())->count, 0);
    CHECK_EQ(d[3].size(), offsetof(TrafficPacket, e));

    NetFixture client(false);
    Connect("Host");
    HookCallbacks();
    DeliverToClient(d);
    CHECK_EQ(g_seen.trafficCalls, 4);
    CHECK_EQ(g_seen.trafficCount, 0);
}

TEST(packets_traffic_bad_counts_are_dropped) {
    NetFixture client(false);
    Connect("Host");
    HookCallbacks();

    TrafficPacket p{};
    p.h = Hdr(PKT_TRAFFIC, 5);
    for (int i = 0; i < MAX_TRAFFIC_PER_PACKET; ++i) p.e[i] = MakeTrafficEntry((uint16_t)(i + 1), 0);

    p.count = MAX_TRAFFIC_PER_PACKET + 1;            // claims more entries than a packet can hold
    Inject(p);
    p.count = 255;
    Inject(p);
    p.count = 5;                                     // claims 5, carries 4 and a half
    InjectRaw(&p, (int)(offsetof(TrafficPacket, e) + 4 * sizeof(TrafficEntry) + 32));
    p.count = 1;
    InjectRaw(&p, (int)offsetof(TrafficPacket, e));  // claims 1, carries none
    CHECK_EQ(g_seen.trafficCalls, 0);

    p.count = 5;                                     // extra bytes after the entries are harmless
    Inject(p);
    CHECK_EQ(g_seen.trafficCalls, 1);
    CHECK_EQ(g_seen.trafficCount, 5);
    p.count = MAX_TRAFFIC_PER_PACKET;
    Inject(p);
    CHECK_EQ(g_seen.trafficCount, MAX_TRAFFIC_PER_PACKET);
}

TEST(packets_traffic_bad_numbers_are_dropped) {
    NetFixture client(false);
    Connect("Host");
    HookCallbacks();
    TrafficPacket p{};
    p.h = Hdr(PKT_TRAFFIC, 5);
    p.count = 3;
    p.simTime = 4.0f;
    for (int i = 0; i < 3; ++i) p.e[i] = MakeTrafficEntry((uint16_t)(i + 1), 0);

    TrafficPacket bad = p; bad.e[2].pos[1] = std::numeric_limits<float>::quiet_NaN();
    Inject(bad);
    bad = p; bad.e[0].linVel[0] = std::numeric_limits<float>::infinity();
    Inject(bad);
    bad = p; bad.simTime = std::numeric_limits<float>::quiet_NaN();
    Inject(bad);
    CHECK_EQ(g_seen.trafficCalls, 0);

    bad = p; bad.e[3].pos[0] = std::numeric_limits<float>::quiet_NaN();   // beyond `count`: not looked at
    Inject(bad);
    CHECK_EQ(g_seen.trafficCalls, 1);
}

// ---- events -----------------------------------------------------------------------------------
TEST(packets_event_round_trip) {
    Datagrams d = SerializeOnHost([] { Net::SendEvent(0xCAFEF00D, "15.1.1"); });
    CHECK_EQ(d.size(), 3);                           // important and tiny: three copies
    for (auto& p : d) CHECK_EQ(p.size(), sizeof(EventPacket));

    NetFixture client(false);
    Connect("Host");
    HookCallbacks();
    DeliverToClient(d);
    // the network layer hands over every copy; the game layer (event.inl) acts on the first only
    CHECK_EQ(g_seen.eventCalls, 3);
    CHECK_EQ(g_seen.eventHash, 0xCAFEF00D);
    CHECK_STR(g_seen.eventId, "15.1.1");
}

TEST(packets_event_id_limits) {
    Datagrams d = SerializeOnHost([] {
        Net::SendEvent(1, "0123456789012345678901234567890123456789012345678901234567890123456789");
        Net::SendEvent(2, nullptr);
    });
    REQUIRE(d.size() == 6);
    const EventPacket* p = (const EventPacket*)d[0].data();
    CHECK_EQ(p->eventId[39], 0);                     // no race context given
    CHECK_EQ(strnlen(p->eventId, 40), 38);           // truncated; the last byte carries the race context
    CHECK_EQ(((const EventPacket*)d[3].data())->eventId[0], 0);

    // an id that fills the field without a terminator must still reach the callback terminated
    NetFixture client(false);
    Connect("Host");
    HookCallbacks();
    EventPacket raw{};
    raw.h = Hdr(PKT_EVENT, 9);
    raw.eventHash = 7;
    memset(raw.eventId, 'Z', sizeof(raw.eventId));
    Inject(raw);
    CHECK_EQ(g_seen.eventCalls, 1);
    CHECK_EQ(g_seen.eventId.size(), 40);
    CHECK_EQ(Net::LastEventContext(), -1);           // 'Z' is no context byte
}

TEST(packets_event_carries_the_race_context) {
    Datagrams d = SerializeOnHost([] { Net::SendEvent(0x1234, "Q.1", 0); Net::SendEvent(0x5678, "1.2.3", 2); Net::SendEvent(9, "1.1", -1); });
    REQUIRE(d.size() == 9);
    NetFixture client(false);
    Connect("Host");
    HookCallbacks();
    DeliverToClient(Datagrams(d.begin(), d.begin() + 1));
    CHECK_STR(g_seen.eventId, "Q.1");
    CHECK_EQ(Net::LastEventContext(), 0);            // quick race / Challenge Series
    DeliverToClient(Datagrams(d.begin() + 3, d.begin() + 4));
    CHECK_EQ(Net::LastEventContext(), 2);            // career
    DeliverToClient(Datagrams(d.begin() + 6, d.begin() + 7));
    CHECK_EQ(Net::LastEventContext(), -1);           // as an older build sends it
}

TEST(packets_restart_round_trip) {
    Datagrams d = SerializeOnHost([] { Net::SendRestart(0x0BADCAFE, 1); });
    CHECK_EQ(d.size(), 3);
    REQUIRE(!d.empty());
    CHECK_EQ(d[0].size(), sizeof(EventPacket));      // same layout as an event
    CHECK_EQ(((const PacketHeader*)d[0].data())->type, PKT_RESTART);

    NetFixture client(false);
    Connect("Host");
    HookCallbacks();
    DeliverToClient(d);
    CHECK_EQ(g_seen.restartCalls, 3);
    CHECK_EQ(g_seen.restartHash, 0x0BADCAFE);
    CHECK_EQ(g_seen.eventCalls, 0);                  // a restart is not an event start
}

// ---- roster -----------------------------------------------------------------------------------
TEST(packets_roster_round_trip) {
    Datagrams d = SerializeOnHost([] {
        for (int i = 0; i < 3; ++i) {
            RosterPacket rp{};
            rp.h.magic = 0xDEAD; rp.h.type = 77;     // whatever the caller left there is overwritten
            rp.index = (uint8_t)i; rp.count = 3;
            rp.carKey = 0x100u + i; rp.hasCustom = 1;
            rp.role = (uint8_t)(i == 0 ? 0 : 0x80 | i);   // 0: from 1.7.9 (no role), then rival, helper
            sprintf_s(rp.name, "Racer %d", i);
            memset(rp.custom, 0x40 + i, sizeof(rp.custom));
            Net::SendRoster(rp);
        }
    });
    REQUIRE(d.size() == 9);                          // three opponents, three copies each
    for (auto& p : d) {
        CHECK_EQ(p.size(), sizeof(RosterPacket));
        CHECK_EQ(((const PacketHeader*)p.data())->magic, COOP_MAGIC);
        CHECK_EQ(((const PacketHeader*)p.data())->type, PKT_ROSTER);
    }

    NetFixture client(false);
    Connect("Host");
    HookCallbacks();
    DeliverToClient(d);
    REQUIRE(g_seen.rosterCalls == 9);
    for (int i = 0; i < 9; ++i) {
        const RosterPacket& r = g_seen.roster[i];
        CHECK_EQ(r.index, i / 3);                    // in order: 0,0,0,1,1,1,2,2,2
        CHECK_EQ(r.count, 3);
        CHECK_EQ(r.carKey, 0x100u + i / 3);
        CHECK_EQ(r.custom[CUSTOM_RECORD_SIZE - 1], 0x40 + i / 3);
        CHECK_EQ(r.role, i / 3 == 0 ? 0 : 0x80 | (i / 3));
        char want[24]; sprintf_s(want, "Racer %d", i / 3);
        CHECK_STR(std::string(r.name, strnlen(r.name, sizeof(r.name))), want);
    }
}

// ---- racer names ------------------------------------------------------------------------------
TEST(packets_racer_name_round_trip_and_truncation) {
    Datagrams d = SerializeOnHost([] {
        Net::SendRacerName(42, "Razor");
        Net::SendRacerName(43, "A very long opponent name indeed");   // HUD slots hold 15 characters
        Net::SendRacerName(44, "");
        Net::SendRacerName(45, nullptr);
    });
    REQUIRE(d.size() == 4);
    for (auto& p : d) CHECK_EQ(p.size(), sizeof(RacerNamePacket));
    CHECK_STR(((const RacerNamePacket*)d[1].data())->name, "A very long opp");
    CHECK_EQ(strlen(((const RacerNamePacket*)d[1].data())->name), 15);

    NetFixture client(false);
    Connect("Host");
    HookCallbacks();
    InjectRaw(d[0].data(), (int)d[0].size());
    CHECK_EQ(g_seen.nameId, 42);
    CHECK_STR(g_seen.name, "Razor");
    InjectRaw(d[1].data(), (int)d[1].size());
    CHECK_EQ(g_seen.nameId, 43);
    CHECK_STR(g_seen.name, "A very long opp");
    InjectRaw(d[2].data(), (int)d[2].size());
    CHECK_STR(g_seen.name, "");
    InjectRaw(d[3].data(), (int)d[3].size());
    CHECK_EQ(g_seen.nameCalls, 4);

    // a name from the wire that fills all 30 bytes: terminated before the callback sees it, and the
    // game side cuts it to the 15 characters a HUD slot holds
    RacerNamePacket raw{};
    raw.h = Hdr(PKT_RACERNAME, 20);
    raw.id = 9;
    memset(raw.name, 'N', sizeof(raw.name));
    Inject(raw);
    CHECK_EQ(g_seen.name.size(), 30);
    char slot[NetUtil::HUD_NAME_SIZE];
    NetUtil::CopyHudName(slot, g_seen.name.c_str());
    CHECK_EQ(strlen(slot), 15);
}

TEST(packets_no_callbacks_installed_is_safe) {
    // before the game hooks are installed (or when they failed) the callbacks are null
    Datagrams d = SerializeOnHost([] {
        TrafficEntry e = MakeTrafficEntry(1, 0);
        Net::SendTraffic(&e, 1, 1.0f);
        Net::SendEvent(1, "1.1.1");
        Net::SendRestart(1, 1);
        Net::SendRacerName(1, "x");
        RosterPacket r{};
        Net::SendRoster(r);
    });
    NetFixture client(false);
    Connect("Host");
    uint32_t in = Net::Stats().packetsIn;
    DeliverToClient(d);
    CHECK_EQ(Net::Stats().packetsIn, in + d.size());
}

// ---- truck trailers (PKT_TRAILER) -------------------------------------------------------------
namespace {
    TrailerEntry MakeTrailerEntry(uint16_t truckId, float x) {
        TrailerEntry e{};
        e.id = truckId; e.flags = TRAILER_HITCHED; e.model = 0x2000u + truckId;
        e.pos[0] = x; e.rot[3] = 1.0f; e.linVel[0] = 5.0f;
        return e;
    }
    struct SeenTrailers { int calls = 0, count = -1; double sendMs = 0; std::vector<TrailerEntry> e; };
    SeenTrailers g_trl;
    void HookTrailers() {
        g_trl = SeenTrailers{};
        Net::OnTrailers = [](const TrailerEntry* e, int n, double sendMs) {
            ++g_trl.calls; g_trl.count = n; g_trl.sendMs = sendMs; g_trl.e.assign(e, e + n);
        };
    }
}

TEST(packets_trailer_round_trip) {
    Datagrams d = SerializeOnHost([] {
        TrailerEntry e[3];
        for (int i = 0; i < 3; ++i) e[i] = MakeTrailerEntry((uint16_t)(i + 7), 10.0f * i);
        e[1].flags = TRAILER_EMULATED;                     // not hitched there, floating
        Net::SendTrailers(e, 3, 8.25f);
        Net::SendTrailers(e, 0, 8.30f);                    // "no trucks near you": still sent (tells we send trailers)
    });
    REQUIRE(d.size() == 2);
    CHECK_EQ(d[0].size(), offsetof(TrailerPacket, e) + 3 * sizeof(TrailerEntry));
    CHECK_EQ(d[1].size(), offsetof(TrailerPacket, e));

    NetFixture client(false);
    Connect("Host");
    HookTrailers();
    DeliverToClient({ d[0] });
    REQUIRE(g_trl.calls == 1);
    REQUIRE(g_trl.count == 3);
    CHECK_NEAR(g_trl.sendMs, 8250, 1e-6);
    CHECK_EQ(g_trl.e[0].id, 7);
    CHECK_EQ(g_trl.e[0].flags, TRAILER_HITCHED);
    CHECK_EQ(g_trl.e[1].flags, TRAILER_EMULATED);
    CHECK_NEAR(g_trl.e[2].pos[0], 20.0, 1e-4);
    DeliverToClient({ d[1] });
    CHECK_EQ(g_trl.calls, 2);
    CHECK_EQ(g_trl.count, 0);
}

TEST(packets_trailer_counts_clamped_on_send) {
    Datagrams d = SerializeOnHost([] {
        TrailerEntry e[12];
        for (int i = 0; i < 12; ++i) e[i] = MakeTrailerEntry((uint16_t)(i + 1), 0);
        Net::SendTrailers(e, 12, 1.0f);                    // more than fits: the first 8
        Net::SendTrailers(e, -1, 1.0f);
        Net::SendTrailers(nullptr, 3, 1.0f);
    });
    REQUIRE(d.size() == 3);
    CHECK_EQ(d[0].size(), sizeof(TrailerPacket));
    CHECK_EQ(((const TrailerPacket*)d[0].data())->count, MAX_TRAILERS_PER_PACKET);
    CHECK_EQ(d[1].size(), offsetof(TrailerPacket, e));
    CHECK_EQ(d[2].size(), offsetof(TrailerPacket, e));
}

TEST(packets_trailer_bad_packets_are_dropped) {
    NetFixture client(false);
    Connect("Host");
    HookTrailers();
    TrailerPacket p{};
    p.h = Hdr(PKT_TRAILER, 5);
    p.simTime = 2.0f;
    for (int i = 0; i < MAX_TRAILERS_PER_PACKET; ++i) p.e[i] = MakeTrailerEntry((uint16_t)(i + 1), 0);

    p.count = MAX_TRAILERS_PER_PACKET + 1; Inject(p);   // more than a packet holds
    p.count = 255; Inject(p);
    p.count = 3; InjectRaw(&p, (int)(offsetof(TrailerPacket, e) + 2 * sizeof(TrailerEntry) + 10));   // claims 3, carries 2.2
    InjectRaw(&p, (int)offsetof(TrailerPacket, e) - 1);  // shorter than the header part
    TrailerPacket bad = p;
    bad.e[1].pos[2] = std::numeric_limits<float>::quiet_NaN(); Inject(bad);
    bad = p; bad.e[0].rot[0] = bad.e[0].rot[3] = 0.f; Inject(bad);   // a zero quaternion is no rotation
    bad = p; bad.e[2].angVel[1] = std::numeric_limits<float>::infinity(); Inject(bad);
    bad = p; bad.e[0].flags = 0x80; Inject(bad);         // unknown flag bits
    bad = p; bad.simTime = std::numeric_limits<float>::quiet_NaN(); Inject(bad);
    CHECK_EQ(g_trl.calls, 0);

    bad = p; bad.e[5].pos[0] = std::numeric_limits<float>::quiet_NaN();   // beyond `count`: not looked at
    Inject(bad);
    CHECK_EQ(g_trl.calls, 1);
    CHECK_EQ(g_trl.count, 3);
}

TEST(packets_trailer_without_callback_is_safe) {
    NetFixture client(false);
    Connect("Host");
    Net::OnTrailers = nullptr;
    TrailerPacket p{};
    p.h = Hdr(PKT_TRAILER, 5);
    p.count = 1; p.simTime = 1.0f; p.e[0] = MakeTrailerEntry(1, 0);
    uint32_t in = Net::Stats().packetsIn;
    Inject(p);
    CHECK_EQ(Net::Stats().packetsIn, in + 1);
}

// ---- opponents' race progress (PKT_RACEPROG) ---------------------------------------------------
namespace {
    struct RaceProgCapture { int calls = 0; int count = 0; RaceProgEntry e[MAX_RACEPROG]; } g_rp;
    void HookRaceProg() {
        g_rp = RaceProgCapture{};
        Net::OnRaceProg = [](const RaceProgEntry* e, int n) { ++g_rp.calls; g_rp.count = n; memcpy(g_rp.e, e, n * sizeof(RaceProgEntry)); };
    }
    RaceProgEntry MakeProg(uint16_t id, float pct) {
        RaceProgEntry e{}; e.id = id; e.pct = pct; e.lapPct = pct; e.toNext = 120.f; e.laps = 1; e.cps = 3; return e;
    }
}
TEST(packets_raceprog_round_trip) {
    NetFixture client(false);
    Connect("Host");
    HookRaceProg();
    RaceProgPacket p{};
    p.h = Hdr(PKT_RACEPROG, 5);
    p.count = 2; p.e[0] = MakeProg(7, 12.5f); p.e[1] = MakeProg(9, 40.f);
    InjectRaw(&p, (int)(offsetof(RaceProgPacket, e) + 2 * sizeof(RaceProgEntry)));
    REQUIRE(g_rp.calls == 1);
    CHECK_EQ(g_rp.count, 2);
    CHECK_EQ(g_rp.e[1].id, 9);
    CHECK_NEAR(g_rp.e[0].pct, 12.5f, 1e-4f);
    CHECK_NEAR(g_rp.e[1].toNext, 120.f, 1e-4f);
    Net::OnRaceProg = nullptr;
}
TEST(packets_raceprog_bad_packets) {
    NetFixture client(false);
    Connect("Host");
    HookRaceProg();
    RaceProgPacket p{};
    p.h = Hdr(PKT_RACEPROG, 5);
    p.count = 3; for (int i = 0; i < 3; ++i) p.e[i] = MakeProg((uint16_t)(i + 1), 10.f * i);
    InjectRaw(&p, (int)(offsetof(RaceProgPacket, e) + 2 * sizeof(RaceProgEntry) + 4));   // claims 3, carries 2.2
    InjectRaw(&p, (int)offsetof(RaceProgPacket, e) - 1);                                 // shorter than the header part
    p.count = MAX_RACEPROG + 1; Inject(p);                                               // more than a packet holds
    CHECK_EQ(g_rp.calls, 0);
    p.count = 3;
    p.e[1].pct = std::numeric_limits<float>::quiet_NaN();
    p.e[2].toNext = -5.f;
    Inject(p);                                                                           // bad entries are left out
    REQUIRE(g_rp.calls == 1);
    CHECK_EQ(g_rp.count, 1);
    CHECK_EQ(g_rp.e[0].id, 1);
    Net::OnRaceProg = nullptr;
}
// ---- pursuit stats (PKT_PSTATS): cost to state / bounty the sender earned itself -----------------
TEST(packets_pursuit_stats_round_trip_and_bad_values) {
    NetFixture client(false);
    Connect("Host");
    uint32_t id = 0, n0 = 0, n = 0; int32_t v[PSTATS_COUNT] = {};
    Net::PeerPursuitStats(id, v, n0);
    PursuitStatsPacket p{};
    p.h = Hdr(PKT_PSTATS, 5);
    p.pursuitId = 3;
    for (int i = 0; i < PSTATS_COUNT; ++i) p.v[i] = i * 10;
    Inject(p);
    REQUIRE(Net::PeerPursuitStats(id, v, n));
    CHECK_EQ(n, n0 + 1);
    CHECK_EQ(id, 3u);
    CHECK_EQ(v[4], 40);
    PursuitStatsPacket bad = p;
    bad.h = Hdr(PKT_PSTATS, 6); bad.pursuitId = 4; bad.v[2] = -1;           // negative: refused
    Inject(bad);
    bad.h = Hdr(PKT_PSTATS, 7); bad.v[2] = 200000000;                       // absurd: refused
    Inject(bad);
    InjectRaw(&p, (int)sizeof(p) - 1);                                       // short: refused
    Net::PeerPursuitStats(id, v, n);
    CHECK_EQ(n, n0 + 1);
    CHECK_EQ(id, 3u);
}

// ---- the client's grid slot at an event start (PKT_GRIDSLOT, host -> client) --------------------
TEST(packets_grid_slot_round_trip_and_bad_values) {
    Datagrams d = SerializeOnHost([] {
        float pos[3] = { 330.2f, 22.3f, 3697.8f };
        Net::SendGridSlot(0xC306E179u, pos, true);
    });
    REQUIRE(d.size() == 1);
    CHECK_EQ(d[0].size(), sizeof(GridSlotPacket));
    NetFixture client(false);
    Connect("Host");
    float got[3] = {}; bool racer = false;
    CHECK(!Net::PeerGridSlot(0xC306E179u, got, &racer));                      // nothing yet
    DeliverToClient(d);
    REQUIRE(Net::PeerGridSlot(0xC306E179u, got, &racer));
    CHECK_NEAR(got[0], 330.2f, 0.001); CHECK_NEAR(got[2], 3697.8f, 0.001);
    CHECK(racer);
    CHECK(!Net::PeerGridSlot(0x12345678u, got));                              // for another event: not ours
    CHECK(!Net::PeerGridSlot(0, got));
    GridSlotPacket bad{};
    bad.h = Hdr(PKT_GRIDSLOT, 50); bad.eventHash = 0x11111111u;
    bad.pos[0] = std::numeric_limits<float>::quiet_NaN(); bad.pos[1] = 1; bad.pos[2] = 1;
    Inject(bad);                                                               // NaN: refused
    bad.h = Hdr(PKT_GRIDSLOT, 51); bad.pos[0] = 0; bad.pos[1] = 0; bad.pos[2] = 0;
    Inject(bad);                                                               // never placed: refused
    bad.h = Hdr(PKT_GRIDSLOT, 52); bad.pos[0] = 5e7f;
    Inject(bad);                                                               // absurd: refused
    CHECK(!Net::PeerGridSlot(0x11111111u, got));
    GridSlotPacket ok{};
    ok.h = Hdr(PKT_GRIDSLOT, 53); ok.eventHash = 0x22222222u; ok.pos[0] = 10; ok.pos[1] = 2; ok.pos[2] = -30;
    InjectRaw(&ok, (int)sizeof(ok) - 1);                                       // short: refused
    CHECK(!Net::PeerGridSlot(0x22222222u, got));
    Inject(ok);
    REQUIRE(Net::PeerGridSlot(0x22222222u, got, &racer));                     // the newest event's slot
    CHECK(!racer);
    CHECK(!Net::PeerGridSlot(0xC306E179u, got));
}
TEST(packets_grid_slot_ignored_by_the_host) {
    NetFixture host(true);
    Connect("Client");
    GridSlotPacket p{};
    p.h = Hdr(PKT_GRIDSLOT, 60); p.eventHash = 0x33333333u; p.pos[0] = 10; p.pos[1] = 2; p.pos[2] = -30;
    Inject(p);
    float got[3];
    CHECK(!Net::PeerGridSlot(0x33333333u, got));                              // only the host decides
}

// ---- police roadblocks (PKT_ROADBLOCK, both directions) ------------------------------------------
namespace {
    struct RbSeen { int calls = 0; bool alive = false; int msg = -1; bool ownerDodged = false; Roadblock::Desc d{}; };
    RbSeen g_rb;
    Roadblock::Desc SampleRoadblock() {
        Roadblock::Desc d{};
        d.id = 7; d.setup = 6; d.flags = Roadblock::RB_SPIKES; d.nCars = 2; d.nPieces = 3;
        d.centre[0] = 1200.5f; d.centre[1] = 31.f; d.centre[2] = -2400.25f;
        d.dir[2] = 1.f;
        const float xs[3] = { -9.f, 9.f, 0.f };
        const uint8_t kinds[3] = { Roadblock::PIECE_SPIKES, Roadblock::PIECE_SPIKES, Roadblock::PIECE_SAWHORSE };
        for (int i = 0; i < 3; ++i) {
            Roadblock::Piece& p = d.p[i];
            p.kind = kinds[i];
            p.m[0] = 1; p.m[5] = 1; p.m[10] = 1; p.m[15] = 1;
            p.m[12] = d.centre[0] + xs[i]; p.m[13] = d.centre[1]; p.m[14] = d.centre[2] - (i == 2 ? 3.f : 0.f);
        }
        return d;
    }
}
TEST(packets_roadblock_round_trip_and_bad_values) {
    Roadblock::Desc d = SampleRoadblock();
    Datagrams dg = SerializeOnHost([&] { Net::SendRoadblock(d, true); Net::SendRoadblock(d, false);
                                         Net::SendRoadblock(d, true, true); Net::SendRoadblockDodged(d); });
    REQUIRE(dg.size() == 6);                                                   // the dodge notice goes 3 times
    CHECK_EQ(dg[0].size(), sizeof(RoadblockPacket));
    NetFixture client(false);
    Connect("Host");
    g_rb = RbSeen{};
    Net::OnRoadblock = [](const Roadblock::Desc& x, int msg, bool ownerDodged) {
        ++g_rb.calls; g_rb.alive = msg == Net::RB_MSG_ALIVE; g_rb.msg = msg; g_rb.ownerDodged = ownerDodged; g_rb.d = x; };
    DeliverToClient({ dg[0] });
    REQUIRE(g_rb.calls == 1);
    CHECK(g_rb.alive);
    CHECK(memcmp(&g_rb.d, &d, sizeof(d)) == 0);                                // every byte as sent
    CHECK_EQ(Roadblock::CountKind(g_rb.d, Roadblock::PIECE_SPIKES), 2);
    DeliverToClient({ dg[1] });
    CHECK(g_rb.calls == 2 && !g_rb.alive && g_rb.d.id == 7);                   // removed
    DeliverToClient({ dg[2] });
    CHECK(g_rb.calls == 3 && g_rb.alive && g_rb.ownerDodged);                  // there, and its owner dodged it
    DeliverToClient({ dg[3] });
    CHECK(g_rb.calls == 4 && g_rb.msg == Net::RB_MSG_DODGED && !g_rb.ownerDodged && g_rb.d.id == 7);   // "we drove through #7"
    g_rb.calls = 2;
    // bad ones never reach the game
    RoadblockPacket bad{};
    bad.h = Hdr(PKT_ROADBLOCK, 70); bad.alive = 1; bad.d = d; bad.d.p[0].m[12] = std::numeric_limits<float>::quiet_NaN();
    Inject(bad);                                                               // NaN position
    bad.h = Hdr(PKT_ROADBLOCK, 71); bad.d = d; bad.d.p[1].m[12] += 500.f;
    Inject(bad);                                                               // a piece far from the roadblock
    bad.h = Hdr(PKT_ROADBLOCK, 72); bad.d = d; bad.d.p[2].kind = 9;
    Inject(bad);                                                               // unknown piece
    bad.h = Hdr(PKT_ROADBLOCK, 73); bad.d = d; bad.d.nPieces = Roadblock::MAX_PIECES + 1;
    Inject(bad);                                                               // too many pieces
    bad.h = Hdr(PKT_ROADBLOCK, 74); bad.d = d; bad.d.id = 0;
    Inject(bad);                                                               // no id
    bad.h = Hdr(PKT_ROADBLOCK, 75); bad.d = d; bad.d.setup = 40;
    Inject(bad);                                                               // no such setup
    bad.h = Hdr(PKT_ROADBLOCK, 76); bad.d = d; bad.alive = 3;
    Inject(bad);                                                               // alive is 0, 1 or 2
    bad.h = Hdr(PKT_ROADBLOCK, 77); bad.d = d; bad.alive = 1; bad.d.p[0].m[0] = 3.f;
    Inject(bad);                                                               // not an orientation
    RoadblockPacket ok{};
    ok.h = Hdr(PKT_ROADBLOCK, 78); ok.alive = 1; ok.d = d;
    InjectRaw(&ok, (int)sizeof(ok) - 1);                                       // short
    CHECK_EQ(g_rb.calls, 2);
    ok.h = Hdr(PKT_ROADBLOCK, 79);
    Inject(ok);
    CHECK_EQ(g_rb.calls, 3);
    Net::OnRoadblock = nullptr;
}
