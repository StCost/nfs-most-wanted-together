// Loopback test over real UDP sockets on 127.0.0.1.
//
// Net keeps its state in globals, so two instances can't live in one process. Instead the real Net
// (host role: Init, Poll, Tick, the Send* functions, the keep-alive thread, SendBye, Shutdown) talks
// to a plain UDP socket that plays the client. What Net sends is read back from that socket and
// checked byte for byte; the serializer -> handler direction is covered in test_packets.cpp.
#include "net_helpers.h"
#include <ws2tcpip.h>

using namespace th;

namespace {
    struct PeerSocket {
        SOCKET s = INVALID_SOCKET;
        uint16_t port = 0;
        sockaddr_in to{};
        bool Open(uint16_t netPort) {
            s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (s == INVALID_SOCKET) return false;
            sockaddr_in a{};
            a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = 0;
            if (bind(s, (sockaddr*)&a, sizeof(a)) != 0) return false;
            int len = sizeof(a);
            getsockname(s, (sockaddr*)&a, &len);
            port = ntohs(a.sin_port);
            to.sin_family = AF_INET; to.sin_addr.s_addr = htonl(INADDR_LOOPBACK); to.sin_port = htons(netPort);
            return true;
        }
        ~PeerSocket() { if (s != INVALID_SOCKET) closesocket(s); }
        template <class T> void Send(const T& p) { sendto(s, (const char*)&p, sizeof(T), 0, (sockaddr*)&to, sizeof(to)); }
        // next datagram within timeoutMs, empty if none
        std::vector<uint8_t> Recv(int timeoutMs) {
            fd_set fds; FD_ZERO(&fds); FD_SET(s, &fds);
            timeval tv{ timeoutMs / 1000, (timeoutMs % 1000) * 1000 };
            if (select(0, &fds, nullptr, nullptr, &tv) <= 0) return {};
            char buf[2000];
            int r = recv(s, buf, sizeof(buf), 0);
            if (r <= 0) return {};
            return std::vector<uint8_t>(buf, buf + r);
        }
        // datagrams until one of `type` arrives (returned) or the time is up (empty)
        std::vector<uint8_t> RecvType(uint16_t type, int timeoutMs) {
            DWORD end = GetTickCount() + timeoutMs;
            for (;;) {
                int left = (int)(end - GetTickCount());
                if (left <= 0) return {};
                std::vector<uint8_t> d = Recv(left);
                if (d.size() >= sizeof(PacketHeader) && ((const PacketHeader*)d.data())->type == type) return d;
            }
        }
    };

    uint16_t FreeUdpPort() {
        SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in a{};
        a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = 0;
        bind(s, (sockaddr*)&a, sizeof(a));
        int len = sizeof(a);
        getsockname(s, (sockaddr*)&a, &len);
        closesocket(s);
        return ntohs(a.sin_port);
    }

    // pump Net until cond() or timeout
    template <class F> bool PumpUntil(F cond, int timeoutMs) {
        DWORD end = GetTickCount() + timeoutMs;
        while ((int)(end - GetTickCount()) > 0) {
            Net::Poll();
            if (cond()) return true;
            Sleep(5);
        }
        return cond();
    }
}

TEST(loopback_real_sockets_host_and_raw_client) {
    Net::Test_Reset();
    g_cfg = Config{};
    g_cfg.isHost = true;
    g_cfg.playerName = "HostPlayer";
    g_cfg.port = FreeUdpPort();
    REQUIRE(Net::Init());
    CHECK(Net::Test_GetState().socketOpen);

    PeerSocket peer;
    REQUIRE(peer.Open((uint16_t)g_cfg.port));

    // --- connect: client hello -> host learns the address and answers ---
    peer.Send(MakeHello("RawClient"));
    REQUIRE(PumpUntil([] { return Net::Stats().connected; }, 2000));
    CHECK_STR(Net::Stats().peerName, "RawClient");
    char want[32]; sprintf_s(want, "127.0.0.1:%u", peer.port);
    CHECK_STR(Net::Stats().peerAddr, want);
    std::vector<uint8_t> d = peer.RecvType(PKT_HELLO, 2000);
    REQUIRE(d.size() == sizeof(HelloPacket));
    CHECK_STR(((const HelloPacket*)d.data())->name, "HostPlayer");

    // --- every sender, as it arrives on the wire ---
    CarState s = MakeState(0, 7.5f, 123.0f, 40.0f).s;
    Net::SendState(s);
    d = peer.RecvType(PKT_STATE, 2000);
    REQUIRE(d.size() == sizeof(StatePacket));
    CHECK(memcmp(&((const StatePacket*)d.data())->s, &s, sizeof(CarState)) == 0);
    CHECK_EQ(((const PacketHeader*)d.data())->magic, COOP_MAGIC);
    CHECK_EQ(((const PacketHeader*)d.data())->version, COOP_VERSION);

    TrafficEntry e[3];
    for (int i = 0; i < 3; ++i) e[i] = MakeTrafficEntry((uint16_t)(i + 1), 50.0f * i);
    Net::SendTraffic(e, 3, 7.5f);
    d = peer.RecvType(PKT_TRAFFIC, 2000);
    REQUIRE(d.size() == offsetof(TrafficPacket, e) + 3 * sizeof(TrafficEntry));
    CHECK_EQ(((const TrafficPacket*)d.data())->count, 3);
    CHECK(memcmp(((const TrafficPacket*)d.data())->e, e, sizeof(e)) == 0);

    Net::SendEvent(0xABCD1234, "2.3.4");
    for (int i = 0; i < 3; ++i) {
        d = peer.RecvType(PKT_EVENT, 2000);
        REQUIRE(d.size() == sizeof(EventPacket));
        CHECK_EQ(((const EventPacket*)d.data())->eventHash, 0xABCD1234);
        CHECK_STR(((const EventPacket*)d.data())->eventId, "2.3.4");
    }
    Net::SendRacerName(2, "Bull");
    d = peer.RecvType(PKT_RACERNAME, 2000);
    REQUIRE(d.size() == sizeof(RacerNamePacket));
    CHECK_STR(((const RacerNamePacket*)d.data())->name, "Bull");

    CarInfo ci{}; ci.carKey = 0x4242; ci.hasCustom = 1; ci.customHash = 1; memset(ci.custom, 0x5A, sizeof(ci.custom));
    Net::SetLocalCar(ci);
    d = peer.RecvType(PKT_CAR, 2000);
    REQUIRE(d.size() == sizeof(CarPacket));
    CHECK(memcmp(&((const CarPacket*)d.data())->c, &ci, sizeof(CarInfo)) == 0);

    // --- ping / pong both ways ---
    PingPacket ping{}; ping.h = Hdr(PKT_PING, 77, 0x11223344);
    peer.Send(ping);
    d.clear();
    PumpUntil([&] { d = peer.RecvType(PKT_PONG, 20); return !d.empty(); }, 2000);
    REQUIRE(d.size() == sizeof(PingPacket));
    CHECK_EQ(((const PingPacket*)d.data())->echoTimeMs, 0x11223344);

    Net::Tick();                                     // sends a ping of its own
    d = peer.RecvType(PKT_PING, 2000);
    REQUIRE(d.size() == sizeof(PingPacket));
    PingPacket pong{}; pong.h = Hdr(PKT_PONG, 78); pong.echoTimeMs = ((const PacketHeader*)d.data())->timeMs;
    peer.Send(pong);
    CHECK(PumpUntil([] { return Net::Stats().pingMs >= 0; }, 2000));
    CHECK(Net::Stats().pingMs < 500);                // loopback round trip

    // --- a state packet from the client reaches the snapshot API ---
    peer.Send(MakeState(100, 3.0f, 55.0f, 0.0f));
    CHECK(PumpUntil([] { return Net::Latest().valid; }, 2000));
    CHECK_NEAR(Net::Latest().s.pos[0], 55, 1e-3);

    // --- keep-alive thread: the frame loop stalls (no Tick), pings keep flowing with seq 0 ---
    while (!peer.Recv(50).empty()) {}                // drain
    bool gotKeepAlive = false;
    DWORD end = GetTickCount() + 4000;
    while ((int)(end - GetTickCount()) > 0 && !gotKeepAlive) {
        d = peer.Recv(200);
        if (d.size() == sizeof(PingPacket)) {
            const PacketHeader* h = (const PacketHeader*)d.data();
            if (h->type == PKT_PING && h->seq == 0 && h->magic == COOP_MAGIC && h->version == COOP_VERSION) gotKeepAlive = true;
        }
    }
    CHECK(gotKeepAlive);

    // --- leaving: three byes, then silence (also from the keep-alive thread) ---
    while (!peer.Recv(50).empty()) {}
    Net::SendBye();
    int byes = 0;
    for (int i = 0; i < 3; ++i) if (!peer.RecvType(PKT_BYE, 1000).empty()) ++byes;
    CHECK_EQ(byes, 3);
    Net::SendState(s);
    Net::Tick();
    CHECK(peer.Recv(1300).empty());                  // longer than two keep-alive periods

    Net::Shutdown();
    CHECK(!Net::Test_GetState().socketOpen);
    Net::Test_Reset();
    g_cfg = Config{};
}

TEST(loopback_client_falls_back_to_random_port_when_busy) {
    // host and client on one PC: the client can't bind the shared port and must pick another
    Net::Test_Reset();
    g_cfg = Config{};
    SOCKET busy = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in a{};
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = 0;
    REQUIRE(bind(busy, (sockaddr*)&a, sizeof(a)) == 0);
    int len = sizeof(a);
    getsockname(busy, (sockaddr*)&a, &len);
    u_long nb = 1; ioctlsocket(busy, FIONBIO, &nb);

    g_cfg.isHost = false;
    g_cfg.peerIp = "127.0.0.1";
    g_cfg.port = ntohs(a.sin_port);
    g_cfg.playerName = "ClientPlayer";
    REQUIRE(Net::Init());

    // its hello arrives at the "host" socket that owns the port
    bool got = false;
    DWORD end = GetTickCount() + 3000;
    while ((int)(end - GetTickCount()) > 0 && !got) {
        Net::Tick();
        char buf[2000]; sockaddr_in from{}; int fl = sizeof(from);
        int r = recvfrom(busy, buf, sizeof(buf), 0, (sockaddr*)&from, &fl);
        if (r == (int)sizeof(HelloPacket) && ((const PacketHeader*)buf)->type == PKT_HELLO) {
            got = true;
            CHECK_STR(((const HelloPacket*)buf)->name, "ClientPlayer");
            CHECK(from.sin_port != a.sin_port);
        }
        Sleep(20);
    }
    CHECK(got);

    // a host that can't bind its port must fail loudly instead
    Net::Shutdown();
    Net::Test_Reset();
    g_cfg.isHost = true;
    CHECK(!Net::Init());
    CHECK(strstr(Net::LastError(), "bind") != nullptr);

    closesocket(busy);
    Net::Test_Reset();
    g_cfg = Config{};
}

TEST(loopback_unresolvable_host_fails_cleanly) {
    Net::Test_Reset();
    g_cfg = Config{};
    g_cfg.isHost = false;
    g_cfg.port = FreeUdpPort();
    g_cfg.peerIp = "not a host name !";
    CHECK(!Net::Init());
    CHECK(strstr(Net::LastError(), "resolve") != nullptr);
    Net::Test_Reset();
    g_cfg = Config{};
}
