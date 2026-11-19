#pragma once
// Shared helpers for the network tests: a clean Net state per test, fake clocks, packet builders and
// a capture of everything the code under test sends.
#include <winsock2.h>
#include <windows.h>
#include <cstdint>
#include <cstddef>
#include <cmath>
#include <vector>
#include <string>
#include "../src/net.h"
#include "../src/netutil.h"
#include "../src/config.h"
#include "mwt_test.h"

namespace th {
    constexpr uint32_t PEER_IP  = 0x0A000002;   // 10.0.0.2
    constexpr uint32_t OTHER_IP = 0x0A000063;   // 10.0.0.99
    constexpr uint16_t PEER_PORT = 47555;
    constexpr uint32_t T0 = 100000;             // fake GetTickCount at the start of a test

    inline double g_clockMs = 0;                // fake Net::LocalClockMs (the game's simulation clock)
    inline double FakeClock() { return g_clockMs; }

    inline std::vector<std::vector<uint8_t>> g_sent;   // datagrams Net tried to send
    inline void Capture(const void* d, int len) {
        const uint8_t* b = (const uint8_t*)d;
        g_sent.emplace_back(b, b + len);
    }
    inline int SentOfType(uint16_t type) {
        int n = 0;
        for (auto& p : g_sent)
            if (p.size() >= sizeof(PacketHeader) && ((const PacketHeader*)p.data())->type == type) ++n;
        return n;
    }
    // first captured packet of that type, or null
    template <class T> const T* FirstSent(uint16_t type) {
        for (auto& p : g_sent)
            if (p.size() >= sizeof(PacketHeader) && ((const PacketHeader*)p.data())->type == type)
                return p.size() >= sizeof(T) ? (const T*)p.data() : nullptr;
        return nullptr;
    }

    // Fresh Net state in host or client role, fake time, sends captured. Restores on scope exit.
    struct NetFixture {
        uint32_t tick = T0;
        explicit NetFixture(bool host, bool capture = true) {
            Net::Test_Reset();
            g_cfg = Config{};
            g_cfg.isHost = host;
            g_cfg.playerName = "Tester";
            g_sent.clear();
            g_clockMs = 0;
            Net::LocalClockMs = &FakeClock;
            Net::Test_SetTick(true, tick);
            if (capture) Net::Test_SetCapture(&Capture);
            if (!host) Net::Test_SetPeer(PEER_IP, PEER_PORT);   // a client knows the host from its config
        }
        ~NetFixture() { Net::Test_Reset(); g_cfg = Config{}; g_sent.clear(); }
        void SetTick(uint32_t t) { tick = t; Net::Test_SetTick(true, tick); }
        void Advance(uint32_t ms) { SetTick(tick + ms); }
    };

    inline PacketHeader Hdr(uint16_t type, uint32_t seq, uint32_t timeMs = 0) {
        PacketHeader h{};
        h.magic = COOP_MAGIC; h.version = COOP_VERSION; h.type = type; h.seq = seq; h.timeMs = timeMs;
        return h;
    }
    inline void InjectRaw(const void* p, int len, uint32_t ip = PEER_IP, uint16_t port = PEER_PORT) {
        Net::Test_HandlePacket(p, len, ip, port);
    }
    template <class T> void Inject(const T& p, uint32_t ip = PEER_IP, uint16_t port = PEER_PORT) {
        Net::Test_HandlePacket(&p, (int)sizeof(T), ip, port);
    }
    inline HelloPacket MakeHello(const char* name, uint32_t seq = 1) {
        HelloPacket p{};
        p.h = Hdr(PKT_HELLO, seq);
        strncpy_s(p.name, name, _TRUNCATE);
        return p;
    }
    // a car at x metres moving along +x at vx m/s, sender simulation clock simTime (s)
    inline StatePacket MakeState(uint32_t seq, float simTime, float x, float vx) {
        StatePacket p{};
        p.h = Hdr(PKT_STATE, seq);
        p.s.gameFlowState = 6; p.s.hasCar = 1;
        p.s.pos[0] = x; p.s.linVel[0] = vx;
        p.s.rot[3] = 1.0f;
        p.s.speedMps = fabsf(vx);
        p.s.simTime = simTime;
        return p;
    }
    inline TrafficEntry MakeTrafficEntry(uint16_t id, float x) {
        TrafficEntry e{};
        e.id = id; e.model = 0x1000u + id; e.pos[0] = x; e.rot[3] = 1.0f; e.linVel[0] = 5.0f;
        return e;
    }
    // connect: the peer says hello
    inline void Connect(const char* name = "Peer") { Inject(MakeHello(name)); }

    inline bool AllFinite(const CarState& s) {
        return NetUtil::FiniteWithin(s.pos, 3, 3.0e38f) && NetUtil::FiniteWithin(s.rot, 4, 3.0e38f) &&
               NetUtil::FiniteWithin(s.linVel, 3, 3.0e38f) && NetUtil::FiniteWithin(s.angVel, 3, 3.0e38f) &&
               NetUtil::FiniteWithin(&s.speedMps, 1, 3.0e38f);
    }

    // a unique scratch directory under %TEMP% (with trailing backslash)
    inline std::string TempDir() {
        char base[MAX_PATH];
        GetTempPathA(MAX_PATH, base);
        char dir[MAX_PATH];
        sprintf_s(dir, "%smwt_tests_%lu\\", base, GetCurrentProcessId());
        CreateDirectoryA(dir, nullptr);
        return dir;
    }
}
