#include <winsock2.h>
#include <ws2tcpip.h>
#include "net.h"
#include "config.h"
#include "log.h"
#include <cstring>
#include <cstddef>
#include <cstdio>
#include <cmath>
#include <algorithm>

#pragma comment(lib, "ws2_32.lib")

namespace {
    SOCKET      g_sock = INVALID_SOCKET;
    sockaddr_in g_peer{};
    bool        g_havePeer = false;     // host: learned from first packet; client: from config
    uint32_t    g_seq = 0;
    uint32_t    g_lastHello = 0, g_lastPing = 0;
    uint32_t    g_lastStateSeq = 0;
    NetStats    g_stats;
    Net::Snapshot g_latest, g_prev;
    char        g_err[128] = "";
    CarInfo     g_localCar{}, g_peerCar{};
    struct TimedSnap { double sendMs, recvMs; CarState s; };
    TimedSnap   g_ring[32];
    int         g_ringHead = 0, g_ringCount = 0;
    char        g_peerCarFile[MAX_PATH] = "";
    bool        g_haveLocalCar = false, g_havePeerCar = false;
    uint32_t    g_lastCarSend = 0;

    void FillHeader(PacketHeader& h, PacketType t);
    void SendRaw(const void* data, int len);
    void SendCar() {
        CarPacket p{};
        FillHeader(p.h, PKT_CAR);
        p.c = g_localCar;
        SendRaw(&p, sizeof(p));
        g_lastCarSend = GetTickCount();
    }

    // Loading screens stall the render loop (no packets) for several seconds, so be generous.
    constexpr uint32_t TIMEOUT_MS = 15000;

    void FillHeader(PacketHeader& h, PacketType t) {
        h.magic = COOP_MAGIC;
        h.version = COOP_VERSION;
        h.type = (uint16_t)t;
        h.seq = ++g_seq;
        h.timeMs = (uint32_t)Net::NowMs();
    }

    bool  g_leaving = false;   // we said bye: send nothing more (the peer would see us reconnect)
    DWORD g_byeAt = 0;         // the peer said bye: ignore its leftover packets for a moment

    void SendRaw(const void* data, int len) {
        if (g_sock == INVALID_SOCKET || !g_havePeer || g_leaving) return;
        int r = sendto(g_sock, (const char*)data, len, 0, (sockaddr*)&g_peer, sizeof(g_peer));
        if (r == len) g_stats.packetsOut++;
    }

    void SendHello() {
        HelloPacket p{};
        FillHeader(p.h, PKT_HELLO);
        strncpy_s(p.name, g_cfg.playerName.c_str(), _TRUNCATE);
        SendRaw(&p, sizeof(p));
    }

    std::string AddrToStr(const sockaddr_in& a) {
        char ip[64] = "?";
        inet_ntop(AF_INET, (void*)&a.sin_addr, ip, sizeof(ip));
        char buf[96];
        sprintf_s(buf, "%s:%u", ip, ntohs(a.sin_port));
        return buf;
    }

    void Handle(const char* buf, int len, const sockaddr_in& from) {
        if (len < (int)sizeof(PacketHeader)) return;
        const PacketHeader* h = (const PacketHeader*)buf;
        if (h->magic != COOP_MAGIC) return;
        if (h->version != COOP_VERSION) {
            sprintf_s(g_err, "peer protocol v%u != ours v%u", h->version, COOP_VERSION);
            return;
        }
        // after a bye, the leaving game may still send a few packets (and the bye's copies): don't
        // reconnect to it - only a fresh HELLO (the game started again) counts
        if (g_byeAt && GetTickCount() - g_byeAt < 2000 && h->type != PKT_HELLO) return;

        if (g_cfg.isHost) {
            // Host accepts the first peer, or the same peer again (port may change after NAT rebind).
            bool same = g_havePeer && g_peer.sin_addr.s_addr == from.sin_addr.s_addr && g_peer.sin_port == from.sin_port;
            bool stale = g_havePeer && (GetTickCount() - g_stats.lastRecvTick > TIMEOUT_MS);
            if (!g_havePeer || stale || (!same && h->type == PKT_HELLO && !g_stats.connected)) {
                g_peer = from;
                g_havePeer = true;
                LOG("Host: peer is now %s", AddrToStr(from).c_str());
            } else if (!same) {
                return; // ignore third parties while a peer is connected
            }
        }

        g_stats.packetsIn++;
        g_stats.lastRecvTick = GetTickCount();
        if (!g_stats.connected) {
            g_stats.connected = true;
            g_lastStateSeq = 0; // the peer may have restarted and begun counting from 1 again
            g_ringCount = 0;
            g_latest.valid = g_prev.valid = false;
            g_stats.peerAddr = AddrToStr(from);
            LOG("Connected to peer %s", g_stats.peerAddr.c_str());
        }

        switch (h->type) {
        case PKT_HELLO: {
            if (len < (int)sizeof(HelloPacket)) return;
            const HelloPacket* p = (const HelloPacket*)buf;
            char name[25]; memcpy(name, p->name, 24); name[24] = 0;
            if (g_stats.peerName != name) {
                g_stats.peerName = name;
                LOG("Peer name: %s", name);
            }
            if (g_cfg.isHost) SendHello(); // reply so client learns our name
            break;
        }
        case PKT_PING: {
            if (len < (int)sizeof(PingPacket)) return;
            PingPacket pong{};
            FillHeader(pong.h, PKT_PONG);
            pong.echoTimeMs = h->timeMs;
            SendRaw(&pong, sizeof(pong));
            break;
        }
        case PKT_PONG: {
            if (len < (int)sizeof(PingPacket)) return;
            const PingPacket* p = (const PingPacket*)buf;
            int rtt = (int)((uint32_t)Net::NowMs() - p->echoTimeMs);
            g_stats.pingMs = (g_stats.pingMs < 0) ? rtt : (g_stats.pingMs * 3 + rtt) / 4;
            break;
        }
        case PKT_STATE: {
            if (len < (int)sizeof(StatePacket)) return;
            const StatePacket* p = (const StatePacket*)buf;
            // Drop late / duplicate packets, but a big jump backwards means the peer restarted.
            if (h->seq <= g_lastStateSeq && g_lastStateSeq - h->seq < 300) return;
            g_lastStateSeq = h->seq;
            {   // interpolation history
                double recv = Net::LocalClockMs();
                if (g_ringCount > 0) {
                    const TimedSnap& last = g_ring[(g_ringHead + 31) % 32];
                    double sms = (double)p->s.simTime * 1000.0;
                    if (sms < last.sendMs - 5000 || sms > last.sendMs + 5000) g_ringCount = 0; // peer restarted / reloaded
                }
                TimedSnap& t = g_ring[g_ringHead];
                t.sendMs = (double)p->s.simTime * 1000.0;   // sender's simulation clock
                t.recvMs = recv;
                t.s = p->s;
                g_ringHead = (g_ringHead + 1) % 32;
                if (g_ringCount < 32) ++g_ringCount;
            }
            g_prev = g_latest;
            g_latest.valid = true;
            g_latest.recvTick = GetTickCount();
            g_latest.seq = h->seq;
            g_latest.s = p->s;
            break;
        }
        case PKT_CAR: {
            if (len < (int)sizeof(CarPacket)) return;
            const CarPacket* p = (const CarPacket*)buf;
            if (!g_havePeerCar || p->c.carKey != g_peerCar.carKey || p->c.customHash != g_peerCar.customHash)
                LOG("Peer car: key %08X custom %s (hash %08X)", p->c.carKey, p->c.hasCustom ? "yes" : "no", p->c.customHash);
            if (!g_havePeerCar || p->c.carKey != g_peerCar.carKey || p->c.customHash != g_peerCar.customHash) {
                // remember it for the next launch: races load before the peer is driving
                FILE* f = nullptr;
                if (fopen_s(&f, g_peerCarFile, "wb") == 0 && f) { fwrite(&p->c, sizeof(CarInfo), 1, f); fclose(f); }
            }
            g_peerCar = p->c;
            g_havePeerCar = p->c.carKey != 0;
            break;
        }
        case PKT_TRAFFIC: {
            if (len < (int)offsetof(TrafficPacket, e)) return;
            const TrafficPacket* p = (const TrafficPacket*)buf;
            int n = p->count;
            if (n > MAX_TRAFFIC_PER_PACKET || len < (int)(offsetof(TrafficPacket, e) + n * sizeof(TrafficEntry))) return;
            if (Net::OnTraffic) Net::OnTraffic(p->e, n, (double)p->simTime * 1000.0);
            break;
        }
        case PKT_EVENT: {
            if (len < (int)sizeof(EventPacket)) return;
            const EventPacket* p = (const EventPacket*)buf;
            char id[41]; memcpy(id, p->eventId, 40); id[40] = 0;
            LOG("Peer started event %s (%08X)", id, p->eventHash);
            if (Net::OnEvent) Net::OnEvent(p->eventHash, id);
            break;
        }
        case PKT_RESTART: {
            if (len < (int)sizeof(EventPacket)) return;
            const EventPacket* p = (const EventPacket*)buf;
            if (Net::OnRestart) Net::OnRestart(p->eventHash);
            break;
        }
        case PKT_RACERNAME: {
            if (len < (int)sizeof(RacerNamePacket)) return;
            const RacerNamePacket* p = (const RacerNamePacket*)buf;
            char nm[31]; memcpy(nm, p->name, 30); nm[30] = 0;
            if (Net::OnRacerName) Net::OnRacerName(p->id, nm);
            break;
        }
        case PKT_ROSTER: {
            if (len < (int)sizeof(RosterPacket)) return;
            if (Net::OnRoster) Net::OnRoster(*(const RosterPacket*)buf);
            break;
        }
        case PKT_BYE:
            g_byeAt = GetTickCount();
            LOG("Peer said bye");
            g_stats.connected = false;
            g_latest.valid = g_prev.valid = false;
            if (g_cfg.isHost) g_havePeer = false;
            break;
        }
    }
}

static bool Resolve(const std::string& host, sockaddr_in& out) {
    out = sockaddr_in{};
    out.sin_family = AF_INET;
    out.sin_port = htons((u_short)g_cfg.port);
    if (inet_pton(AF_INET, host.c_str(), &out.sin_addr) == 1) return true;
    addrinfo hints{}, *res = nullptr; // allow hostnames too
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) return false;
    out.sin_addr = ((sockaddr_in*)res->ai_addr)->sin_addr;
    freeaddrinfo(res);
    return true;
}

void Net::SetDataDir(const char* dir) {
    sprintf_s(g_peerCarFile, "%sMWTogether.peercar.bin", dir);
    FILE* f = nullptr;
    if (fopen_s(&f, g_peerCarFile, "rb") == 0 && f) {
        CarInfo c{};
        if (fread(&c, sizeof(c), 1, f) == 1 && c.carKey) {
            g_peerCar = c;
            g_havePeerCar = true;
            LOG("Peer car from last session: key %08X custom %s", c.carKey, c.hasCustom ? "yes" : "no");
        }
        fclose(f);
    }
}

static DWORD WINAPI KeepAliveThread(void*);

bool Net::Init() {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { strcpy_s(g_err, "WSAStartup failed"); return false; }

    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_sock == INVALID_SOCKET) { sprintf_s(g_err, "socket() failed: %d", WSAGetLastError()); return false; }

    u_long nb = 1;
    ioctlsocket(g_sock, FIONBIO, &nb);

    // Windows: ignore ICMP "port unreachable" resets on UDP so recvfrom doesn't keep failing
    // while the other side isn't running yet.
    #ifndef SIO_UDP_CONNRESET
    #define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
    #endif
    BOOL bNewBehavior = FALSE; DWORD ret = 0;
    WSAIoctl(g_sock, SIO_UDP_CONNRESET, &bNewBehavior, sizeof(bNewBehavior), nullptr, 0, &ret, nullptr, nullptr);

    // Both sides bind the same fixed port. Sending first from that port makes each side's
    // firewall treat the other side's packets as replies, so no inbound rule is needed on a LAN.
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons((u_short)g_cfg.port);
    if (bind(g_sock, (sockaddr*)&local, sizeof(local)) != 0) {
        int e = WSAGetLastError();
        if (g_cfg.isHost) {
            sprintf_s(g_err, "bind(port %d) failed: %d", g_cfg.port, e);
            LOG("%s", g_err);
            return false;
        }
        LOG("Client: port %d busy (%d), using a random port", g_cfg.port, e); // e.g. host on same PC
        local.sin_port = 0;
        if (bind(g_sock, (sockaddr*)&local, sizeof(local)) != 0) {
            sprintf_s(g_err, "bind failed: %d", WSAGetLastError());
            LOG("%s", g_err);
            return false;
        }
    }

    if (!g_cfg.isHost) {
        if (!Resolve(g_cfg.peerIp, g_peer)) {
            sprintf_s(g_err, "can't resolve HostIP '%s'", g_cfg.peerIp.c_str());
            LOG("%s", g_err);
            return false;
        }
        g_havePeer = true;
        LOG("Client: will connect to %s", AddrToStr(g_peer).c_str());
    } else {
        LOG("Host: listening on UDP port %d", g_cfg.port);
        if (!g_cfg.clientIp.empty()) {
            if (Resolve(g_cfg.clientIp, g_peer)) {
                g_havePeer = true;
                LOG("Host: also calling client at %s", AddrToStr(g_peer).c_str());
            } else {
                LOG("Host: can't resolve ClientIP '%s' (ignored)", g_cfg.clientIp.c_str());
            }
        }
    }
    if (HANDLE th = CreateThread(nullptr, 0, KeepAliveThread, nullptr, 0, nullptr)) CloseHandle(th);
    return true;
}

void Net::SendBye() {
    static bool sent = false;
    if (sent || g_sock == INVALID_SOCKET) return;
    sent = true;
    PacketHeader bye{};
    FillHeader(bye, PKT_BYE);
    for (int i = 0; i < 3; ++i) SendRaw(&bye, sizeof(bye));
    g_leaving = true;
    LOG("Sent bye to peer");
}

void Net::Shutdown() {
    if (g_sock != INVALID_SOCKET) {
        PacketHeader bye{};
        FillHeader(bye, PKT_BYE);
        SendRaw(&bye, sizeof(bye));
        closesocket(g_sock);
        g_sock = INVALID_SOCKET;
    }
    WSACleanup();
}

void Net::Poll() {
    if (g_sock == INVALID_SOCKET) return;
    char buf[1500];
    for (int i = 0; i < 256; ++i) {
        sockaddr_in from{}; int fromLen = sizeof(from);
        int r = recvfrom(g_sock, buf, sizeof(buf), 0, (sockaddr*)&from, &fromLen);
        if (r <= 0) break;
        Handle(buf, r, from);
    }
}

// While the game is paused (alt-tab, world map, loading) its frame loop stops and so does Net::Tick.
// This thread keeps pinging the peer meanwhile so neither side times out.
namespace { volatile DWORD g_lastTickCall = 0; }
static DWORD WINAPI KeepAliveThread(void*) {
    for (;;) {
        Sleep(500);
        SOCKET s = g_sock;
        if (s == INVALID_SOCKET) return 0;
        DWORD last = g_lastTickCall;
        if (!last || !g_stats.connected || g_leaving || GetTickCount() - last < 1000) continue;
        PingPacket p{};
        p.h.magic = COOP_MAGIC; p.h.version = COOP_VERSION; p.h.type = (uint16_t)PKT_PING;
        p.h.seq = 0; p.h.timeMs = (uint32_t)Net::NowMs();
        sendto(s, (const char*)&p, sizeof(p), 0, (sockaddr*)&g_peer, sizeof(g_peer));
    }
}

void Net::Tick() {
    if (g_sock == INVALID_SOCKET) return;
    uint32_t now = GetTickCount();
    // we were stalled ourselves: the peer's packets are still queued, don't count that as silence
    if (g_lastTickCall && now - g_lastTickCall > 2000) g_stats.lastRecvTick = now;
    g_lastTickCall = now;

    if (g_stats.connected && now - g_stats.lastRecvTick > TIMEOUT_MS) {
        LOG("Peer timed out");
        g_stats.connected = false;
        g_stats.pingMs = -1;
        g_latest.valid = g_prev.valid = false;
    }
    // Car info is small and important: resend every 2 s (UDP may drop it).
    if (g_stats.connected && g_haveLocalCar && now - g_lastCarSend > 2000) SendCar();
    // Client always says hello; host does too while it has a ClientIP but no connection yet.
    bool sayHello = !g_cfg.isHost || (!g_stats.connected && g_havePeer);
    if (sayHello && now - g_lastHello > (g_stats.connected ? 3000u : 1000u)) {
        g_lastHello = now;
        SendHello();
    }
    if (g_stats.connected && now - g_lastPing > 1000) {
        g_lastPing = now;
        PingPacket p{};
        FillHeader(p.h, PKT_PING);
        SendRaw(&p, sizeof(p));
    }
}

void Net::SendState(const CarState& s) {
    if (!g_stats.connected && g_cfg.isHost) return; // host has nobody to send to yet
    StatePacket p{};
    FillHeader(p.h, PKT_STATE);
    p.s = s;
    SendRaw(&p, sizeof(p));
}

void Net::SetLocalCar(const CarInfo& c) {
    bool changed = !g_haveLocalCar || c.carKey != g_localCar.carKey || c.customHash != g_localCar.customHash;
    g_localCar = c;
    g_haveLocalCar = c.carKey != 0;
    if (changed && g_haveLocalCar) {
        LOG("Local car: key %08X custom %s (hash %08X)", c.carKey, c.hasCustom ? "yes" : "no", c.customHash);
        if (g_stats.connected) SendCar();
    }
}

void (*Net::OnTraffic)(const TrafficEntry*, int, double) = nullptr;
double (*Net::LocalClockMs)() = &Net::NowMs;
void (*Net::OnEvent)(uint32_t, const char*) = nullptr;
void (*Net::OnRoster)(const RosterPacket&) = nullptr;
void (*Net::OnRacerName)(uint16_t, const char*) = nullptr;
void (*Net::OnRestart)(uint32_t) = nullptr;

void Net::SendRestart(uint32_t eventHash) {
    if (!g_stats.connected) return;
    EventPacket p{};
    FillHeader(p.h, PKT_RESTART);
    p.eventHash = eventHash;
    for (int i = 0; i < 3; ++i) SendRaw(&p, sizeof(p));
}

void Net::SendRacerName(uint16_t id, const char* name) {
    if (!g_stats.connected) return;
    RacerNamePacket p{};
    FillHeader(p.h, PKT_RACERNAME);
    p.id = id;
    strncpy_s(p.name, 16, name, _TRUNCATE);   // the HUD standings copy names into 16-byte slots
    SendRaw(&p, sizeof(p));
}

void Net::SendRoster(const RosterPacket& in) {
    if (!g_stats.connected) return;
    RosterPacket p = in;
    FillHeader(p.h, PKT_ROSTER);
    for (int i = 0; i < 3; ++i) SendRaw(&p, sizeof(p));
}

void Net::SendEvent(uint32_t eventHash, const char* eventId) {
    if (!g_stats.connected) return;
    EventPacket p{};
    FillHeader(p.h, PKT_EVENT);
    p.eventHash = eventHash;
    strncpy_s(p.eventId, eventId, _TRUNCATE);
    for (int i = 0; i < 3; ++i) SendRaw(&p, sizeof(p));   // important + tiny: send 3 copies (UDP)
}

// Clock mapping (our ms - peer ms). The raw estimate (fastest packet in the last ~0.5 s) jumps by a
// few ms whenever that packet ages out, which would make remote cars jump. So we glide the estimate
// towards the raw value by at most 2 ms per second, and only snap on big changes (peer restarted).
static double g_offsetEst = 0, g_offsetAt = 0;
static bool   g_haveOffset = false;
double Net::PeerClockOffsetMs() {
    if (g_ringCount == 0) return g_haveOffset ? g_offsetEst : 0.0;
    double raw = 1e300;
    for (int i = 0; i < g_ringCount; ++i) {
        const TimedSnap& t = g_ring[(g_ringHead + 32 - 1 - i) % 32];
        raw = (std::min)(raw, t.recvMs - t.sendMs);
    }
    double now = Net::LocalClockMs();
    if (!g_haveOffset || fabs(raw - g_offsetEst) > 50.0) {
        g_offsetEst = raw; g_haveOffset = true;
    } else {
        double el = now - g_offsetAt;
        if (el < 0) el = 0;
        double maxStep = el * 0.002;                     // 2 ms per second
        double d = raw - g_offsetEst;
        if (d > maxStep) d = maxStep; else if (d < -maxStep) d = -maxStep;
        g_offsetEst += d;
    }
    g_offsetAt = now;
    return g_offsetEst;
}

void Net::SendTraffic(const TrafficEntry* e, int count, float simTime) {
    if (!g_stats.connected) return;
    TrafficPacket p{};
    FillHeader(p.h, PKT_TRAFFIC);
    if (count > MAX_TRAFFIC_PER_PACKET) count = MAX_TRAFFIC_PER_PACKET;
    p.count = (uint8_t)count;
    p.simTime = simTime;
    memcpy(p.e, e, count * sizeof(TrafficEntry));
    SendRaw(&p, (int)(offsetof(TrafficPacket, e) + count * sizeof(TrafficEntry)));
}

double Net::NowMs() {
    static LARGE_INTEGER f{};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

static void Lerp3(const float* a, const float* b, float t, float* o) { for (int i = 0; i < 3; ++i) o[i] = a[i] + (b[i] - a[i]) * t; }

static void NlerpQ(const float* a, const float* b0, float t, float* o) {
    float b[4] = { b0[0], b0[1], b0[2], b0[3] };
    if (a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3] < 0) for (float& x : b) x = -x;
    float len = 0;
    for (int i = 0; i < 4; ++i) { o[i] = a[i] + (b[i] - a[i]) * t; len += o[i] * o[i]; }
    len = sqrtf(len);
    if (len > 1e-6f) for (int i = 0; i < 4; ++i) o[i] /= len;
}

bool Net::SampleRemote(double delayMs, CarState& out, double* extrapolatedMs) {
    if (g_ringCount == 0) return false;
    double target = Net::LocalClockMs() - Net::PeerClockOffsetMs() - delayMs; // in sender sim time
    // newest first
    const TimedSnap* newer = nullptr; const TimedSnap* older = nullptr;
    for (int i = 0; i < g_ringCount; ++i) {
        const TimedSnap& t = g_ring[(g_ringHead + 32 - 1 - i) % 32];
        if (t.sendMs >= target) newer = &t;
        else { older = &t; break; }
    }
    if (extrapolatedMs) *extrapolatedMs = 0;
    if (older && newer && newer->sendMs > older->sendMs) {
        double span = newer->sendMs - older->sendMs;
        float u = (float)((target - older->sendMs) / span);
        float dt = (float)(span / 1000.0);
        // cubic Hermite on position with velocities as tangents
        float u2 = u * u, u3 = u2 * u;
        float h00 = 2 * u3 - 3 * u2 + 1, h10 = u3 - 2 * u2 + u, h01 = -2 * u3 + 3 * u2, h11 = u3 - u2;
        out = newer->s;
        for (int i = 0; i < 3; ++i)
            out.pos[i] = h00 * older->s.pos[i] + h10 * dt * older->s.linVel[i] + h01 * newer->s.pos[i] + h11 * dt * newer->s.linVel[i];
        Lerp3(older->s.linVel, newer->s.linVel, u, out.linVel);
        Lerp3(older->s.angVel, newer->s.angVel, u, out.angVel);
        NlerpQ(older->s.rot, newer->s.rot, u, out.rot);
        out.speedMps = older->s.speedMps + (newer->s.speedMps - older->s.speedMps) * u;
        return true;
    }
    // Only newer samples (target before history) or only older ones (late packets): use the newest,
    // extrapolated by velocity for at most 250 ms.
    const TimedSnap& n = g_ring[(g_ringHead + 31) % 32];
    out = n.s;
    double ex = target - n.sendMs;
    if (ex > 0) {
        if (ex > 250) ex = 250;
        float e = (float)(ex / 1000.0);
        for (int i = 0; i < 3; ++i) out.pos[i] += out.linVel[i] * e;
        if (extrapolatedMs) *extrapolatedMs = ex;
    }
    return true;
}

const CarInfo* Net::PeerCar() { return g_havePeerCar ? &g_peerCar : nullptr; }

const Net::Snapshot& Net::Latest() { return g_latest; }
const Net::Snapshot& Net::Previous() { return g_prev; }
const NetStats& Net::Stats() { return g_stats; }
const char* Net::LastError() { return g_err; }
