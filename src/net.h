#pragma once
#include <cstdint>
#include <string>

// Wire protocol. Little-endian, both peers are x86 so we send structs raw.
#pragma pack(push, 1)
enum PacketType : uint16_t {
    PKT_HELLO = 1,   // client -> host, host -> client (reply)
    PKT_STATE = 2,   // car state snapshot
    PKT_PING  = 3,
    PKT_PONG  = 4,
    PKT_BYE   = 5,
    PKT_CAR   = 6,   // which car the sender drives: model key + full customization (tuning, parts, paint)
    PKT_TRAFFIC = 7, // host -> client: the host's traffic cars near the client
    PKT_EVENT   = 8, // "I just started this career event" (both directions)
    PKT_ROSTER  = 9, // one AI opponent of the event the sender starts (sent before PKT_EVENT)
    PKT_RACERNAME = 10, // host -> client: display name of a race opponent (by traffic id)
    PKT_RESTART = 11,
    PKT_NISSKIP = 12,   // "I want to skip this cutscene" (EventPacket layout, eventHash = scene hash)   // "I restarted the current race" (EventPacket layout)
};

struct PacketHeader {
    uint32_t magic;    // 'MWCO'
    uint16_t version;
    uint16_t type;
    uint32_t seq;
    uint32_t timeMs;   // sender's GetTickCount()
};

struct HelloPacket {
    PacketHeader h;
    char name[24];
};

struct PingPacket {
    PacketHeader h;
    uint32_t echoTimeMs; // for PONG: the PING's timeMs
};

struct CarState {
    int32_t  gameFlowState; // sender's game flow state (6 = in game)
    uint8_t  hasCar;        // 0 if sender is in menus / loading
    uint8_t  pad[3];
    float    pos[3];
    float    rot[4];        // orientation quaternion (x,y,z,w)
    float    linVel[3];
    float    angVel[3];
    float    speedMps;
    float    simTime;       // sender's game simulation clock (s) when this state was read
    // race progress of the sender's own racer (GRacerInfo), so standings rank them correctly
    uint8_t  inRace;
    uint8_t  finished;
    int16_t  laps;
    int16_t  checkpoints;
    int16_t  pursuitHeat;   // heat x10 of the sender's pursuit, 0 = not in a pursuit
    float    racePct;
    float    lapPct;
    uint32_t nisHash;       // cutscene the sender is watching (scene name hash), 0 = none
};

struct StatePacket {
    PacketHeader h;
    CarState s;
};

constexpr int CUSTOM_RECORD_SIZE = 0x198; // FECustomizationRecord (plain data: part indices, upgrades, tunings)

struct CarInfo {
    uint32_t carKey;      // vehicle model key (Attrib collection key), 0 = unknown
    uint8_t  hasCustom;
    uint8_t  pad[3];
    uint32_t customHash;  // FNV-1a of custom[], to detect changes cheaply
    uint8_t  custom[CUSTOM_RECORD_SIZE];
};

struct CarPacket {
    PacketHeader h;
    CarInfo c;
};

struct TrafficEntry {
    uint16_t id;          // host-side id (stable while the car exists)
    uint16_t flags;       // bit0 = cop car
    uint32_t model;       // model key (hash of the model name)
    uint32_t glare;       // light bits (IVehicle+0x9C): head/brake/reverse/signals/cop lights
    float    pos[3];
    float    rot[4];
    float    linVel[3];
    float    angVel[3];
};
constexpr int MAX_TRAFFIC_PER_PACKET = 20;
struct EventPacket {
    PacketHeader h;
    uint32_t eventHash;   // Attrib::StringHash32 of the event ID
    char     eventId[40]; // e.g. "1.2.3"
};
struct RosterPacket {
    PacketHeader h;
    uint8_t  index, count;  // opponent index (construction order) / total AI opponents
    uint8_t  hasCustom, pad;
    uint32_t carKey;        // model key the opponent was built with
    char     name[24];      // display name (may be empty)
    uint8_t  custom[CUSTOM_RECORD_SIZE];
};
struct RacerNamePacket {
    PacketHeader h;
    uint16_t id;           // TrafficEntry id of that racer
    char     name[30];
};
struct TrafficPacket {
    PacketHeader h;
    uint8_t  count;
    uint8_t  pad[3];
    float    simTime;     // host simulation clock (s) for all entries
    TrafficEntry e[MAX_TRAFFIC_PER_PACKET];
};
#pragma pack(pop)

constexpr uint32_t COOP_MAGIC   = 0x4F43574D; // "MWCO"
constexpr uint16_t COOP_VERSION = 10;

struct NetStats {
    bool        connected = false;
    std::string peerName;
    std::string peerAddr;
    int         pingMs = -1;
    uint32_t    lastRecvTick = 0;
    uint32_t    packetsIn = 0, packetsOut = 0;
};

// Non-blocking UDP peer. All calls happen on the game thread (from the frame hook).
namespace Net {
    bool Init();        // binds socket according to g_cfg
    void SetDataDir(const char* dir); // where to keep the remembered peer car (scripts\)
    void Shutdown();
    void SendBye();     // tell the peer we're leaving (window closing); safe to call more than once
    void Poll();        // receive everything pending, handle hello/ping
    void SendState(const CarState& s);
    void SetLocalCar(const CarInfo& c);   // remembered and re-sent periodically / on change
    const CarInfo* PeerCar();              // nullptr until the peer told us its car
    void Tick();        // periodic hello / ping / timeout handling

    // Latest snapshots from peer (newest first in [0]); valid == false if none
    struct Snapshot { bool valid = false; uint32_t recvTick = 0; uint32_t seq = 0; CarState s{}; };
    const Snapshot& Latest();
    const Snapshot& Previous();

    const NetStats& Stats();
    bool PeerSaidBye();   // the last disconnect was a clean bye (not a timeout)

    double NowMs();   // high-resolution clock (QueryPerformanceCounter), milliseconds
    double PeerClockOffsetMs();          // our clock minus the peer's (min one-way), from state packets
    void   SendTraffic(const TrafficEntry* e, int count, float simTime);
    // Local clock used to align remote motion with what this frame shows: the game's simulation
    // clock (set by the game layer); defaults to NowMs.
    extern double (*LocalClockMs)();
    // set by the game layer: called for every received traffic packet (sender time in ms)
    extern void (*OnTraffic)(const TrafficEntry* e, int count, double sendMs);
    void SendEvent(uint32_t eventHash, const char* eventId);
    extern void (*OnEvent)(uint32_t eventHash, const char* eventId);
    void SendRoster(const RosterPacket& p);                 // header is filled in here; sent 3x
    extern void (*OnRoster)(const RosterPacket& p);
    void SendRacerName(uint16_t id, const char* name);
    void SendRestart(uint32_t eventHash);
    void SendNisSkip(uint32_t sceneHash);
    extern void (*OnNisSkip)(uint32_t sceneHash);
    extern void (*OnRestart)(uint32_t eventHash);
    extern void (*OnRacerName)(uint16_t id, const char* name);

    // Snapshot interpolation: state of the remote car as it was `delayMs` ago (in sender time),
    // blended between the two surrounding packets (Hermite on position using velocities,
    // normalized-lerp on rotation). Extrapolates up to 250 ms when packets are late.
    // Returns false if there is no data. *extrapolatedMs = how far past the newest packet we are.
    bool SampleRemote(double delayMs, CarState& out, double* extrapolatedMs = nullptr);
    const char* LastError();

#ifdef MWT_TESTS
    // Test hooks (tests\, built with /DMWT_TESTS; not part of the plugin).
    void Test_Reset();                      // every global back to its start-up value, socket closed
    void Test_HandlePacket(const void* buf, int len, uint32_t fromIp, uint16_t fromPort); // as if received (host byte order)
    void Test_SetTick(bool on, uint32_t ms = 0);          // replace GetTickCount() inside net.cpp
    void Test_SetCapture(void (*fn)(const void* data, int len)); // receive what would be sent (no socket needed)
    void Test_SetPeer(uint32_t ip, uint16_t port);        // what Init does for a client (host byte order)
    struct TestState { bool havePeer; uint32_t peerIp; uint16_t peerPort; uint32_t lastStateSeq; int ringCount; bool leaving; bool socketOpen; };
    TestState Test_GetState();
#endif
}
