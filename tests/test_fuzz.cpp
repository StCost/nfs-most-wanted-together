// Crash / fuzz tests for the packet handler: whatever arrives on the UDP port must never crash the
// game, read outside the datagram, or hand the game layer something it can't use.
//
// Every datagram is placed so that it ends exactly at an inaccessible guard page: reading even one
// byte past its length faults at once (also in the non-ASan build). Deterministic: the seed is
// printed and can be set with MWT_FUZZ_SEED; MWT_FUZZ_ITERS scales the number of datagrams.
#include "net_helpers.h"
#include <cstdlib>

using namespace th;

namespace {
    // ---- xorshift RNG (the same sequence everywhere) ------------------------------------------
    struct Rng {
        uint64_t s;
        explicit Rng(uint64_t seed) : s(seed ? seed : 0x9E3779B97F4A7C15ull) {}
        uint32_t Next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return (uint32_t)(s >> 16); }
        uint32_t Below(uint32_t n) { return n ? Next() % n : 0; }
        bool Chance(uint32_t percent) { return Below(100) < percent; }
        void Fill(void* p, size_t n) { uint8_t* b = (uint8_t*)p; for (size_t i = 0; i < n; ++i) b[i] = (uint8_t)Next(); }
        float Float(float lo, float hi) { return lo + (hi - lo) * (float)(Next() & 0xFFFFFF) / (float)0xFFFFFF; }
    };

    uint64_t Seed() {
        if (const char* e = getenv("MWT_FUZZ_SEED")) return _strtoui64(e, nullptr, 0);
        return 0x4D5754ull;   // fixed by default so a failure reproduces; set MWT_FUZZ_SEED to explore
    }
    int Iters(int base) {
        if (const char* e = getenv("MWT_FUZZ_ITERS")) { int n = atoi(e); if (n > 0) return n; }
        return base;
    }

    // ---- guard-page buffer --------------------------------------------------------------------
    struct GuardedBuffer {
        static constexpr size_t CAP = 8192;          // two pages of data, then one no-access page
        uint8_t* base = nullptr;
        GuardedBuffer() {
            base = (uint8_t*)VirtualAlloc(nullptr, CAP + 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            DWORD old;
            if (base) VirtualProtect(base + CAP, 4096, PAGE_NOACCESS, &old);
        }
        ~GuardedBuffer() { if (base) VirtualFree(base, 0, MEM_RELEASE); }
        // copy of `data` whose last byte is the last readable byte
        const uint8_t* Place(const void* data, size_t len) {
            uint8_t* p = base + CAP - len;
            if (len) memcpy(p, data, len);
            return p;
        }
    };

    // ---- callbacks that use everything they are given -----------------------------------------
    struct Counters { long long traffic = 0, entries = 0, event = 0, restart = 0, name = 0, roster = 0, nisSkip = 0, bad = 0; };
    Counters g_c;
    volatile uint32_t g_sink;

    void HookStrictCallbacks() {
        g_c = Counters{};
        Net::OnTraffic = [](const TrafficEntry* e, int n, double sendMs) {
            ++g_c.traffic;
            if (n < 0 || n > MAX_TRAFFIC_PER_PACKET || !(sendMs == sendMs)) { ++g_c.bad; return; }
            for (int i = 0; i < n; ++i) {
                ++g_c.entries;
                if (!NetUtil::ValidTrafficEntry(e[i])) ++g_c.bad;
                g_sink += e[i].id + e[i].model + e[i].glare + e[i].flags;   // touch every entry
            }
        };
        Net::OnEvent = [](uint32_t h, const char* id) {
            ++g_c.event;
            if (strlen(id) > 40) ++g_c.bad;
            g_sink += h;
        };
        Net::OnRestart = [](uint32_t h, uint32_t) { ++g_c.restart; g_sink += h; };
        Net::OnRacerName = [](uint16_t id, const char* n) {
            ++g_c.name;
            if (strlen(n) > 30) ++g_c.bad;
            char slot[NetUtil::HUD_NAME_SIZE];
            NetUtil::CopyHudName(slot, n);
            g_sink += id + (uint8_t)slot[0];
        };
        Net::OnRoster = [](const RosterPacket& p) {
            ++g_c.roster;
            // what event.inl does with it: bounds-check the index, copy the record and the name
            static struct { uint32_t key; bool hasCustom; uint8_t custom[CUSTOM_RECORD_SIZE]; char name[24]; } roster[8];
            if (p.index >= 8 || p.count > 8) return;
            auto& r = roster[p.index];
            r.key = p.carKey; r.hasCustom = p.hasCustom != 0;
            memcpy(r.custom, p.custom, sizeof(r.custom));
            memcpy(r.name, p.name, sizeof(r.name)); r.name[23] = 0;
            g_sink += r.custom[CUSTOM_RECORD_SIZE - 1] + (uint32_t)strlen(r.name);
        };
        Net::OnNisSkip = [](uint32_t h) { ++g_c.nisSkip; g_sink += h; };
    }

    constexpr uint16_t ALL_TYPES[] = { PKT_HELLO, PKT_STATE, PKT_PING, PKT_PONG, PKT_BYE, PKT_CAR, PKT_TRAFFIC,
                                       PKT_EVENT, PKT_ROSTER, PKT_RACERNAME, PKT_RESTART, PKT_NISSKIP,
                                       PKT_FILE, PKT_FILEACK, PKT_FILEREQ, PKT_NOFILE, PKT_RACEPROG, PKT_PSTATS };
    // any packet type; a bye only now and then, or the run would sit in the 2 s "ignore after bye"
    // window most of the time
    uint16_t PickType(Rng& r) {
        uint16_t t = ALL_TYPES[r.Below(_countof(ALL_TYPES))];
        if (t == PKT_BYE && !r.Chance(4)) t = PKT_STATE;
        return t;
    }
    size_t FullSize(uint16_t type) {
        switch (type) {
        case PKT_HELLO: return sizeof(HelloPacket);
        case PKT_STATE: return sizeof(StatePacket);
        case PKT_PING: case PKT_PONG: return sizeof(PingPacket);
        case PKT_BYE: return sizeof(PacketHeader);
        case PKT_CAR: return sizeof(CarPacket);
        case PKT_TRAFFIC: return sizeof(TrafficPacket);
        case PKT_EVENT: case PKT_RESTART: case PKT_NISSKIP: return sizeof(EventPacket);
        case PKT_ROSTER: return sizeof(RosterPacket);
        case PKT_RACERNAME: return sizeof(RacerNamePacket);
        case PKT_FILE: return sizeof(FileChunkPacket);
        case PKT_FILEACK: return sizeof(FileAckPacket);
        case PKT_FILEREQ: return sizeof(FileReqPacket);
        case PKT_NOFILE: return sizeof(PacketHeader);
        case PKT_RACEPROG: return sizeof(RaceProgPacket);
        case PKT_PSTATS: return sizeof(PursuitStatsPacket);
        }
        return sizeof(PacketHeader);
    }

    // a well-formed packet of the given type with plausible contents
    size_t BuildValid(Rng& r, uint16_t type, uint8_t* out /*[1500]*/, uint32_t seq) {
        memset(out, 0, 1500);
        PacketHeader h = Hdr(type, seq, r.Next());
        memcpy(out, &h, sizeof(h));
        switch (type) {
        case PKT_HELLO: { HelloPacket* p = (HelloPacket*)out; sprintf_s(p->name, "Peer%u", r.Below(1000)); break; }
        case PKT_STATE: {
            StatePacket* p = (StatePacket*)out;
            p->s.gameFlowState = 6; p->s.hasCar = 1;
            for (int i = 0; i < 3; ++i) { p->s.pos[i] = r.Float(-5000, 5000); p->s.linVel[i] = r.Float(-90, 90); p->s.angVel[i] = r.Float(-5, 5); }
            for (int i = 0; i < 4; ++i) p->s.rot[i] = r.Float(-1, 1);
            p->s.speedMps = r.Float(0, 100);
            p->s.simTime = r.Float(0, 30);
            break;
        }
        case PKT_PING: case PKT_PONG: ((PingPacket*)out)->echoTimeMs = r.Next(); break;
        case PKT_CAR: { CarPacket* p = (CarPacket*)out; p->c.carKey = r.Next(); p->c.hasCustom = 1; p->c.customHash = r.Next(); r.Fill(p->c.custom, sizeof(p->c.custom)); break; }
        case PKT_TRAFFIC: {
            TrafficPacket* p = (TrafficPacket*)out;
            p->count = (uint8_t)r.Below(MAX_TRAFFIC_PER_PACKET + 1);
            p->simTime = r.Float(0, 30);
            for (int i = 0; i < p->count; ++i) {
                p->e[i] = MakeTrafficEntry((uint16_t)r.Next(), r.Float(-5000, 5000));
                p->e[i].flags = (uint16_t)r.Below(4);
            }
            return offsetof(TrafficPacket, e) + p->count * sizeof(TrafficEntry);
        }
        case PKT_EVENT: case PKT_RESTART: case PKT_NISSKIP: { EventPacket* p = (EventPacket*)out; p->eventHash = r.Next(); sprintf_s(p->eventId, "%u.%u.%u", r.Below(16), r.Below(9), r.Below(9)); break; }
        case PKT_ROSTER: { RosterPacket* p = (RosterPacket*)out; p->index = (uint8_t)r.Below(8); p->count = 8; p->carKey = r.Next(); r.Fill(p->custom, sizeof(p->custom)); sprintf_s(p->name, "Racer%u", r.Below(99)); break; }
        case PKT_RACERNAME: { RacerNamePacket* p = (RacerNamePacket*)out; p->id = (uint16_t)r.Next(); sprintf_s(p->name, "Name%u", r.Below(99)); break; }
        case PKT_FILE: {
            // one chunk of a file of up to 4 chunks; a file of one chunk carries its real hash, so it
            // completes and reaches the callback
            FileChunkPacket* p = (FileChunkPacket*)out;
            p->size = 1 + r.Below(r.Chance(50) ? FILE_CHUNK : 4 * FILE_CHUNK);
            uint32_t total = (p->size + FILE_CHUNK - 1) / FILE_CHUNK;
            p->total = (uint16_t)total; p->index = (uint16_t)r.Below(total);
            p->mtime = ((uint64_t)r.Next() << 32) | r.Next();
            sprintf_s(p->owner, "Player%u", r.Below(99)); sprintf_s(p->profile, "Save%u", r.Below(99));
            uint32_t off = p->index * FILE_CHUNK, len = p->size - off;
            if (len > FILE_CHUNK) len = FILE_CHUNK;
            r.Fill(p->data, len);
            uint32_t hash = 2166136261u;
            for (uint32_t i = 0; i < len; ++i) { hash ^= p->data[i]; hash *= 16777619u; }
            p->fileHash = total == 1 ? hash : r.Below(4);    // few hashes: chunks of "the same file" meet
            return offsetof(FileChunkPacket, data) + len;
        }
        case PKT_FILEACK: ((FileAckPacket*)out)->fileHash = r.Below(4); break;
        case PKT_FILEREQ: sprintf_s(((FileReqPacket*)out)->owner, "Player%u", r.Below(99)); break;
        }
        return FullSize(type);
    }

    uint32_t PickAddr(Rng& r) { return r.Chance(85) ? PEER_IP : (r.Chance(50) ? OTHER_IP : r.Next()); }

    // after any input: the public API still works and never returns non-finite motion
    void CheckInvariants(Rng& r, NetFixture& f) {
        CarState out{}; double ex = 0;
        if (Net::SampleRemote((double)r.Below(300), out, &ex)) {
            if (!AllFinite(out)) CHECK(!"SampleRemote returned a non-finite state");
            if (!(ex >= 0 && ex <= 250)) CHECK(!"extrapolation outside 0..250 ms");
        }
        double off = Net::PeerClockOffsetMs();
        if (!(off == off)) CHECK(!"clock offset is NaN");
        const Net::Snapshot& l = Net::Latest();
        if (l.valid && !NetUtil::ValidCarState(l.s)) CHECK(!"Latest() holds an invalid state");
        Net::TestState st = Net::Test_GetState();
        if (st.ringCount < 0 || st.ringCount > 32) CHECK(!"ring count out of range");
        if (Net::Stats().peerName.size() > 24) CHECK(!"peer name longer than its field");
        if (r.Chance(30)) { f.Advance(r.Below(400)); Net::Tick(); }
        if (r.Chance(2)) f.Advance(r.Below(20000));           // long stall / timeout
        g_clockMs += r.Below(40);
        if (r.Chance(1)) g_clockMs = r.Below(100000);         // the simulation clock restarts on a world load
    }

    void RunFuzz(bool host, uint64_t seed, int iters) {
        NetFixture f(host);
        // sends are captured: count them only, or the capture list grows without bound
        Net::Test_SetCapture([](const void* d, int len) { g_sink += (uint32_t)len + *(const uint8_t*)d; });
        HookStrictCallbacks();
        GuardedBuffer gb;
        REQUIRE(gb.base != nullptr);
        Rng r(seed);
        uint8_t scratch[1500];
        uint32_t seq = 1;

        for (int i = 0; i < iters; ++i) {
            size_t len = 0;
            switch (r.Below(6)) {
            case 0:   // pure noise, any length 0..1500
                len = r.Below(1501);
                r.Fill(scratch, len);
                break;
            case 1: { // a valid header (sometimes an unknown type) followed by noise of any length
                len = r.Below(1501);
                r.Fill(scratch, len);
                PacketHeader h = Hdr(r.Chance(90) ? PickType(r) : (uint16_t)r.Next(), r.Chance(70) ? seq++ : r.Next(), r.Next());
                memcpy(scratch, &h, len < sizeof(h) ? len : sizeof(h));
                break;
            }
            case 2: { // right size for its type, noise inside (NaNs, unterminated strings, huge counts)
                uint16_t t = PickType(r);
                len = FullSize(t);
                r.Fill(scratch, len);
                PacketHeader h = Hdr(t, seq++, r.Next());
                memcpy(scratch, &h, sizeof(h));
                break;
            }
            case 3: { // a valid packet, cut short somewhere
                len = BuildValid(r, PickType(r), scratch, seq++);
                len = r.Below((uint32_t)len + 1);
                break;
            }
            case 4: { // a valid packet with a few bytes or bits changed
                len = BuildValid(r, PickType(r), scratch, seq++);
                for (int k = r.Below(8) + 1; k > 0 && len; --k) {
                    size_t at = r.Below((uint32_t)len);
                    if (r.Chance(50)) scratch[at] = (uint8_t)r.Next(); else scratch[at] ^= (uint8_t)(1u << r.Below(8));
                }
                break;
            }
            default:  // valid traffic: keeps the connection alive and the state deep (ring full, etc.)
                len = BuildValid(r, r.Chance(60) ? (uint16_t)PKT_STATE : PickType(r), scratch, seq++);
                if (r.Chance(1)) seq = 1;             // the peer restarted
                break;
            }
            const uint8_t* p = gb.Place(scratch, len);
            Net::Test_HandlePacket(p, (int)len, PickAddr(r), r.Chance(95) ? PEER_PORT : (uint16_t)r.Next());
            if ((i & 7) == 0) CheckInvariants(r, f);
            if (mwt::Failures()) { printf("    at iteration %d (seed 0x%llX, %s)\n", i, seed, host ? "host" : "client"); return; }
        }
        CHECK_EQ(g_c.bad, 0);
        // the run must have reached the interesting code, not just bounced off the header check
        CHECK(g_c.traffic > 0 && g_c.event > 0 && g_c.restart > 0 && g_c.name > 0 && g_c.roster > 0);
        CHECK(g_c.nisSkip > 0);
        CHECK(Net::Stats().packetsIn > (uint32_t)(iters / 20));
        printf("    %s: %d datagrams, %u accepted; callbacks: traffic %lld (%lld cars), event %lld, restart %lld, name %lld, roster %lld\n",
               host ? "host" : "client", iters, Net::Stats().packetsIn, g_c.traffic, g_c.entries, g_c.event, g_c.restart, g_c.name, g_c.roster);
    }
}

TEST(fuzz_guard_page_really_faults) {
    // self-test of the harness: an over-read by one byte must be caught
    GuardedBuffer gb;
    REQUIRE(gb.base != nullptr);
    uint8_t data[10] = {};
    const uint8_t* p = gb.Place(data, sizeof(data));
    g_sink += p[9];                                  // last byte: fine
    CHECK(IsBadReadPtr(p + 10, 1));                  // one past: not readable
    CHECK(!IsBadReadPtr(p, 10));
}

TEST(fuzz_random_datagrams_client) {
    uint64_t seed = Seed();
    printf("    seed 0x%llX\n", seed);
    RunFuzz(false, seed, Iters(150000));
}

TEST(fuzz_random_datagrams_host) {
    uint64_t seed = Seed() ^ 0xA5A5A5A5ull;
    RunFuzz(true, seed, Iters(150000));
}

TEST(fuzz_every_truncation_of_every_packet_type) {
    // exhaustive: each packet type, valid contents, at every length from 0 to its full size (+ a few)
    for (int role = 0; role < 2; ++role) {
        NetFixture f(role == 0);
        Net::Test_SetCapture([](const void*, int) {});
        HookStrictCallbacks();
        GuardedBuffer gb;
        REQUIRE(gb.base != nullptr);
        Rng r(Seed() + 17);
        uint8_t scratch[1500];
        uint32_t seq = 1;
        for (uint16_t t : ALL_TYPES) {
            size_t full = FullSize(t);
            for (size_t len = 0; len <= full + 4 && len <= sizeof(scratch); ++len) {
                BuildValid(r, t, scratch, seq++);
                if (t == PKT_TRAFFIC) ((TrafficPacket*)scratch)->count = MAX_TRAFFIC_PER_PACKET;   // claims a full packet
                Net::Test_HandlePacket(gb.Place(scratch, len), (int)len, PEER_IP, PEER_PORT);
                f.Advance(3000);                     // step past the "ignore after bye" window
            }
        }
        CHECK_EQ(g_c.bad, 0);
        CHECK(g_c.event > 0 && g_c.roster > 0);
    }
}

TEST(fuzz_traffic_with_huge_counts) {
    NetFixture f(false);
    Connect();
    HookStrictCallbacks();
    GuardedBuffer gb;
    REQUIRE(gb.base != nullptr);
    Rng r(Seed() + 99);
    uint8_t scratch[1500];
    long long expected = 0;
    for (int count = 0; count < 256; ++count) {
        for (int trial = 0; trial < 40; ++trial) {
            BuildValid(r, PKT_TRAFFIC, scratch, 10 + count * 40 + trial);
            TrafficPacket* p = (TrafficPacket*)scratch;
            for (int i = 0; i < MAX_TRAFFIC_PER_PACKET; ++i) p->e[i] = MakeTrafficEntry((uint16_t)(i + 1), 0);
            p->count = (uint8_t)count;
            size_t len = r.Chance(30) ? sizeof(TrafficPacket) : offsetof(TrafficPacket, e) + r.Below((uint32_t)(sizeof(TrafficPacket) - offsetof(TrafficPacket, e) + 1));
            if (count <= MAX_TRAFFIC_PER_PACKET && len >= offsetof(TrafficPacket, e) + count * sizeof(TrafficEntry)) ++expected;
            Net::Test_HandlePacket(gb.Place(scratch, len), (int)len, PEER_IP, PEER_PORT);
        }
    }
    CHECK_EQ(g_c.bad, 0);
    CHECK_EQ(g_c.traffic, expected);                 // exactly the well-formed ones got through
    CHECK(expected > 0);
}

TEST(fuzz_bit_flips_in_real_serializer_output) {
    // datagrams from the real Send* functions, every single-bit corruption of the first 64 bytes and
    // random ones beyond, fed to the other side
    std::vector<std::vector<uint8_t>> d;
    {
        NetFixture host(true);
        Connect("Client");
        g_sent.clear();
        CarState s = MakeState(1, 5.0f, 100, 30).s;
        Net::SendState(s);
        CarInfo ci{}; ci.carKey = 0x77; ci.hasCustom = 1; ci.customHash = 5;
        Net::SetLocalCar(ci);
        TrafficEntry e[6];
        for (int i = 0; i < 6; ++i) e[i] = MakeTrafficEntry((uint16_t)(i + 1), 10.0f * i);
        Net::SendTraffic(e, 6, 5.0f);
        Net::SendEvent(0x1234, "3.4.5");
        Net::SendRestart(0x1234, 1);
        Net::SendRacerName(3, "Sonny");
        RosterPacket rp{}; rp.count = 1; rp.carKey = 9;
        Net::SendRoster(rp);
        Net::SendNisSkip(0x4321);
        host.Advance(1500); Net::Tick();             // ping (+ hello if due)
        Net::SendBye();
        d = g_sent;
    }
    REQUIRE(d.size() >= 10);

    NetFixture f(false);
    Net::Test_SetCapture([](const void*, int) {});
    HookStrictCallbacks();
    GuardedBuffer gb;
    REQUIRE(gb.base != nullptr);
    Rng r(Seed() + 5);
    long long n = 0;
    for (auto& pkt : d) {
        std::vector<uint8_t> m;
        size_t bits = pkt.size() * 8;
        for (size_t bit = 0; bit < bits; ++bit) {
            if (bit >= 64 * 8 && !r.Chance(3)) continue;
            m = pkt;
            m[bit / 8] ^= (uint8_t)(1u << (bit % 8));
            Net::Test_HandlePacket(gb.Place(m.data(), m.size()), (int)m.size(), PEER_IP, PEER_PORT);
            ++n;
            if ((n & 63) == 0) { CheckInvariants(r, f); f.Advance(2100); }
        }
    }
    CHECK_EQ(g_c.bad, 0);
    CHECK(n > 5000);
}
