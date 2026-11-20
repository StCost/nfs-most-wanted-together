// The small file transfer (career save backup / restore): real serializer -> handler round trips,
// lost / duplicated / damaged chunks, the request and "no file" packets; and src/saveutil.h, the
// pure helpers of the save sync (names from the network, which copy wins, safe file writes).
#include "net_helpers.h"
#include "../src/saveutil.h"

using namespace th;

namespace {
    typedef std::vector<uint8_t> Dgram;

    struct Got {
        int files = 0, requests = 0, noFile = 0;
        std::string owner, profile, reqOwner;
        std::vector<uint8_t> data;
        uint64_t mtime = 0;
    };
    Got g_got;

    void HookFileCallbacks() {
        g_got = Got{};
        Net::OnFile = [](const char* owner, const char* profile, const uint8_t* d, uint32_t n, uint64_t mtime) {
            ++g_got.files;
            g_got.owner = owner; g_got.profile = profile; g_got.mtime = mtime;
            g_got.data.assign(d, d + n);
        };
        Net::OnFileRequest = [](const char* owner) { ++g_got.requests; g_got.reqOwner = owner; };
        Net::OnNoFile = []() { ++g_got.noFile; };
    }

    std::vector<uint8_t> Pattern(uint32_t n, uint32_t salt = 0) {
        std::vector<uint8_t> v(n);
        uint32_t x = 0x1234567u + salt;
        for (uint32_t i = 0; i < n; ++i) { x = x * 1664525u + 1013904223u; v[i] = (uint8_t)(x >> 24); }
        return v;
    }
    uint32_t Fnv(const std::vector<uint8_t>& v) {      // the hash the wire format uses (FNV-1a)
        uint32_t h = 2166136261u;
        for (uint8_t b : v) { h ^= b; h *= 16777619u; }
        return h;
    }
    uint32_t Chunks(uint32_t size) { return (size + FILE_CHUNK - 1) / FILE_CHUNK; }

    // captured datagrams of one type, removed from the capture
    std::vector<Dgram> Take(uint16_t type) {
        std::vector<Dgram> out, rest;
        for (auto& p : g_sent) {
            bool match = p.size() >= sizeof(PacketHeader) && ((const PacketHeader*)p.data())->type == type;
            (match ? out : rest).push_back(p);
        }
        g_sent.swap(rest);
        return out;
    }
    // let the sender put out everything it will send before the next pause (6 chunks per frame)
    std::vector<Dgram> SendRound() {
        for (int i = 0; i < 64; ++i) Net::Tick();
        return Take(PKT_FILE);
    }
    void InjectAll(const std::vector<Dgram>& v, uint32_t ip = PEER_IP) {
        for (auto& p : v) InjectRaw(p.data(), (int)p.size(), ip);
    }

    // hand-made chunks (not from the serializer), so a test can make them wrong
    std::vector<Dgram> MakeChunks(const std::vector<uint8_t>& data, const char* owner, const char* profile,
                                  uint64_t mtime, uint32_t seq0 = 1000) {
        std::vector<Dgram> out;
        uint32_t total = Chunks((uint32_t)data.size()), hash = Fnv(data);
        for (uint32_t i = 0; i < total; ++i) {
            FileChunkPacket p{};
            p.h = Hdr(PKT_FILE, seq0 + i);
            p.fileHash = hash; p.size = (uint32_t)data.size(); p.mtime = mtime;
            p.index = (uint16_t)i; p.total = (uint16_t)total;
            strncpy(p.owner, owner, sizeof(p.owner)); strncpy(p.profile, profile, sizeof(p.profile));   // may fill the field
            uint32_t off = i * FILE_CHUNK, len = (uint32_t)data.size() - off;
            if (len > FILE_CHUNK) len = FILE_CHUNK;
            memcpy(p.data, data.data() + off, len);
            const uint8_t* b = (const uint8_t*)&p;
            out.emplace_back(b, b + offsetof(FileChunkPacket, data) + len);
        }
        return out;
    }
    FileChunkPacket* Chunk(Dgram& d) { return (FileChunkPacket*)d.data(); }
}

// ---- round trips through the real sender ---------------------------------------------------------

TEST(file_round_trip) {
    NetFixture f(true);
    Connect("Client");
    HookFileCallbacks();
    std::vector<uint8_t> data = Pattern(3 * FILE_CHUNK + 500);
    const uint64_t mtime = 0x01DB123456789ABCull;
    CHECK(!Net::SendingFile());
    CHECK(!Net::LastFileDelivered());
    REQUIRE(Net::SendFile("Tester", "MyCareer", data.data(), (uint32_t)data.size(), mtime));
    CHECK(Net::SendingFile());
    CHECK(!Net::LastFileDelivered());

    std::vector<Dgram> chunks = SendRound();
    REQUIRE(chunks.size() == 4);
    for (size_t i = 0; i < chunks.size(); ++i) {
        REQUIRE(chunks[i].size() >= offsetof(FileChunkPacket, data));
        const FileChunkPacket* p = Chunk(chunks[i]);
        CHECK_EQ(p->h.magic, COOP_MAGIC);
        CHECK_EQ(p->h.version, COOP_VERSION);
        CHECK_EQ(p->index, i);
        CHECK_EQ(p->total, 4);
        CHECK_EQ(p->size, data.size());
        CHECK_EQ(p->fileHash, Fnv(data));
        CHECK(p->mtime == mtime);
        CHECK_STR(std::string(p->owner, strnlen(p->owner, 32)), "Tester");
        CHECK_STR(std::string(p->profile, strnlen(p->profile, 32)), "MyCareer");
        // only the bytes of the chunk travel: the last datagram is short
        CHECK_EQ(chunks[i].size(), offsetof(FileChunkPacket, data) + (i < 3 ? FILE_CHUNK : 500));
    }

    InjectAll(chunks);                                   // the other side receives them
    CHECK_EQ(g_got.files, 1);
    CHECK_STR(g_got.owner, "Tester");
    CHECK_STR(g_got.profile, "MyCareer");
    CHECK(g_got.mtime == mtime);
    CHECK(g_got.data == data);

    std::vector<Dgram> acks = Take(PKT_FILEACK);
    REQUIRE(acks.size() == 1);
    CHECK_EQ(acks[0].size(), sizeof(FileAckPacket));
    CHECK_EQ(((const FileAckPacket*)acks[0].data())->fileHash, Fnv(data));
    InjectAll(acks);                                     // ... and the sender hears about it
    CHECK(!Net::SendingFile());
    CHECK(Net::LastFileDelivered());

    f.Advance(800);
    CHECK_EQ(SendRound().size(), 0);                     // nothing more is sent
}

TEST(file_round_trip_one_byte_and_exact_chunk_and_maximum) {
    const uint32_t sizes[] = { 1, FILE_CHUNK - 1, FILE_CHUNK, FILE_CHUNK + 1, 2 * FILE_CHUNK, 65536, FILE_MAX - 1, FILE_MAX };
    uint32_t salt = 0;
    for (uint32_t size : sizes) {
        NetFixture f(true);
        Connect("Client");
        HookFileCallbacks();
        std::vector<uint8_t> data = Pattern(size, ++salt);
        REQUIRE(Net::SendFile("A", "B", data.data(), size, 7));
        std::vector<Dgram> chunks = SendRound();
        CHECK_EQ(chunks.size(), Chunks(size));
        InjectAll(chunks);
        CHECK_EQ(g_got.files, 1);
        CHECK(g_got.data == data);
        InjectAll(Take(PKT_FILEACK));
        CHECK(Net::LastFileDelivered());
    }
}

TEST(file_lost_chunks_arrive_in_a_later_round) {
    NetFixture f(true);
    Connect("Client");
    HookFileCallbacks();
    std::vector<uint8_t> data = Pattern(5 * FILE_CHUNK + 17);
    REQUIRE(Net::SendFile("Tester", "Save", data.data(), (uint32_t)data.size(), 42));

    std::vector<Dgram> r1 = SendRound();
    REQUIRE(r1.size() == 6);
    for (size_t i = 0; i < r1.size(); ++i) if (i != 1 && i != 5) InjectRaw(r1[i].data(), (int)r1[i].size());   // 1 and 5 are lost
    CHECK_EQ(g_got.files, 0);
    CHECK_EQ(Take(PKT_FILEACK).size(), 0);
    CHECK(Net::SendingFile());

    CHECK_EQ(SendRound().size(), 0);                     // the sender waits before it repeats
    f.Advance(300);
    CHECK_EQ(SendRound().size(), 0);
    f.Advance(500);
    std::vector<Dgram> r2 = SendRound();                 // second round: everything again
    REQUIRE(r2.size() == 6);
    InjectRaw(r2[5].data(), (int)r2[5].size());          // this time only chunk 1 is lost
    CHECK_EQ(g_got.files, 0);

    f.Advance(800);
    std::vector<Dgram> r3 = SendRound();
    REQUIRE(r3.size() == 6);
    InjectAll(r3);
    CHECK_EQ(g_got.files, 1);
    CHECK(g_got.data == data);
    InjectAll(Take(PKT_FILEACK));
    CHECK(!Net::SendingFile());
    CHECK(Net::LastFileDelivered());
}

TEST(file_duplicate_and_reordered_chunks) {
    NetFixture f(true);
    Connect("Client");
    HookFileCallbacks();
    std::vector<uint8_t> data = Pattern(4 * FILE_CHUNK);
    REQUIRE(Net::SendFile("Tester", "Save", data.data(), (uint32_t)data.size(), 42));
    std::vector<Dgram> c = SendRound();
    REQUIRE(c.size() == 4);
    const int order[] = { 3, 3, 0, 2, 0, 3, 2, 2, 1 };  // every chunk several times, the last one first
    for (int i : order) InjectRaw(c[i].data(), (int)c[i].size());
    CHECK_EQ(g_got.files, 1);                            // delivered exactly once
    CHECK(g_got.data == data);
    CHECK_EQ(Take(PKT_FILEACK).size(), 1);
}

TEST(file_ack_lost_is_sent_again) {
    NetFixture f(true);
    Connect("Client");
    HookFileCallbacks();
    std::vector<uint8_t> data = Pattern(2 * FILE_CHUNK + 1);
    REQUIRE(Net::SendFile("Tester", "Save", data.data(), (uint32_t)data.size(), 42));
    InjectAll(SendRound());
    CHECK_EQ(g_got.files, 1);
    CHECK_EQ(Take(PKT_FILEACK).size(), 1);               // ... which gets lost
    CHECK(Net::SendingFile());

    f.Advance(800);
    std::vector<Dgram> again = SendRound();              // the sender repeats the file
    REQUIRE(again.size() == 3);
    InjectAll(again);
    CHECK_EQ(g_got.files, 1);                            // not delivered a second time
    std::vector<Dgram> acks = Take(PKT_FILEACK);
    CHECK_EQ(acks.size(), 3);                            // every repeated chunk is answered
    InjectAll(acks);
    CHECK(!Net::SendingFile());
    CHECK(Net::LastFileDelivered());
}

TEST(file_sender_gives_up_without_an_ack) {
    NetFixture f(true);
    Connect("Client");
    std::vector<uint8_t> data = Pattern(2 * FILE_CHUNK);
    REQUIRE(Net::SendFile("Tester", "Save", data.data(), (uint32_t)data.size(), 42));
    size_t sent = 0;
    int rounds = 0;
    for (; rounds < 40 && Net::SendingFile(); ++rounds) {
        sent += SendRound().size();
        // the peer is alive (or this would be a timeout), it just never acknowledges
        PingPacket ping{}; ping.h = Hdr(PKT_PING, 5000 + rounds); Inject(ping);
        f.Advance(750);
    }
    CHECK(!Net::SendingFile());
    CHECK(!Net::LastFileDelivered());                    // the caller must be able to tell and try again
    CHECK_EQ(sent, 14 * 2);                              // 14 rounds, about 10 s
    CHECK(Net::Stats().connected);

    // and a new transfer can start afterwards
    CHECK(Net::SendFile("Tester", "Save", data.data(), (uint32_t)data.size(), 43));
    CHECK(!Net::LastFileDelivered());
}

TEST(file_sender_drops_the_transfer_when_the_peer_leaves) {
    {   // a clean bye
        NetFixture f(true);
        Connect("Client");
        std::vector<uint8_t> data = Pattern(20 * FILE_CHUNK);
        REQUIRE(Net::SendFile("Tester", "Save", data.data(), (uint32_t)data.size(), 42));
        Net::Tick();
        CHECK_EQ(Take(PKT_FILE).size(), 6);
        PacketHeader bye = Hdr(PKT_BYE, 50);
        Inject(bye);
        CHECK(!Net::Stats().connected);
        Net::Tick();
        CHECK(!Net::SendingFile());
        CHECK(!Net::LastFileDelivered());
        CHECK_EQ(Take(PKT_FILE).size(), 0);
        // a late ack of the dropped transfer doesn't count as delivered
        Connect("Client");
        FileAckPacket a{}; a.h = Hdr(PKT_FILEACK, 60); a.fileHash = Fnv(data);
        f.Advance(3000);
        Inject(a);
        CHECK(!Net::LastFileDelivered());
    }
    {   // a timeout
        NetFixture f(false);
        Connect("Host");
        std::vector<uint8_t> data = Pattern(20 * FILE_CHUNK);
        REQUIRE(Net::SendFile("Tester", "Save", data.data(), (uint32_t)data.size(), 42));
        Net::Tick();
        for (int i = 0; i < 20 && Net::Stats().connected; ++i) { f.Advance(1900); Net::Tick(); }
        CHECK(!Net::Stats().connected);
        CHECK(Net::SendingFile());                       // (it had not given up by itself yet)
        Net::Tick();
        CHECK(!Net::SendingFile());
        CHECK(!Net::LastFileDelivered());
    }
}

TEST(file_send_refused) {
    NetFixture f(true);
    std::vector<uint8_t> data = Pattern(100);
    CHECK(!Net::SendFile("A", "B", data.data(), 100, 1));            // nobody connected
    Connect("Client");
    CHECK(!Net::SendFile("A", "B", data.data(), 0, 1));              // empty
    std::vector<uint8_t> big(FILE_MAX + 1);
    CHECK(!Net::SendFile("A", "B", big.data(), FILE_MAX + 1, 1));    // too large
    CHECK(!Net::SendFile("A", "B", nullptr, 100, 1));
    CHECK(!Net::SendFile(nullptr, "B", data.data(), 100, 1));
    CHECK(!Net::SendFile("A", nullptr, data.data(), 100, 1));
    CHECK(!Net::SendingFile());
    CHECK(Net::SendFile("A", "B", data.data(), 100, 1));
    CHECK(!Net::SendFile("A", "C", data.data(), 100, 1));            // one at a time
    FileAckPacket a{}; a.h = Hdr(PKT_FILEACK, 70); a.fileHash = Fnv(data);
    Inject(a);
    REQUIRE(!Net::SendingFile());
    // names longer than the field are cut, never overrun
    HookFileCallbacks();
    std::vector<uint8_t> other = Pattern(100, 9);
    REQUIRE(Net::SendFile("OwnerNameThatIsMuchLongerThanThirtyTwoCharacters", "ProfileNameThatIsMuchLongerThanThirtyTwoCharacters", other.data(), 100, 1));
    InjectAll(SendRound());
    CHECK_EQ(g_got.files, 1);
    CHECK_STR(g_got.owner, "OwnerNameThatIsMuchLongerThanTh");
    CHECK_STR(g_got.profile, "ProfileNameThatIsMuchLongerThan");
}

// ---- the receiver against hand-made chunks ---------------------------------------------------------

TEST(file_wrong_hash_is_dropped) {
    NetFixture f(true);
    Connect("Client");
    HookFileCallbacks();
    std::vector<uint8_t> data = Pattern(3 * FILE_CHUNK);
    std::vector<Dgram> bad = MakeChunks(data, "Tester", "Save", 42);
    Chunk(bad[1])->data[10] ^= 0x40;                     // one bit of the contents differs
    g_sent.clear();
    InjectAll(bad);
    CHECK_EQ(g_got.files, 0);
    CHECK_EQ(Take(PKT_FILEACK).size(), 0);               // not acknowledged: the sender will repeat it
    InjectAll(bad);                                      // the same damage again: still nothing
    CHECK_EQ(g_got.files, 0);
    CHECK_EQ(Take(PKT_FILEACK).size(), 0);

    InjectAll(MakeChunks(data, "Tester", "Save", 42));   // the real thing gets through afterwards
    CHECK_EQ(g_got.files, 1);
    CHECK(g_got.data == data);
    CHECK_EQ(Take(PKT_FILEACK).size(), 1);
}

TEST(file_hash_zero_is_not_acknowledged_out_of_nowhere) {
    // regression: "the last completed hash" started as 0, so a chunk claiming hash 0 was acknowledged
    // without anything having been received
    NetFixture f(true);
    Connect("Client");
    HookFileCallbacks();
    std::vector<Dgram> c = MakeChunks(Pattern(2 * FILE_CHUNK), "Tester", "Save", 42);
    for (auto& d : c) Chunk(d)->fileHash = 0;
    g_sent.clear();
    InjectRaw(c[0].data(), (int)c[0].size());
    CHECK_EQ(Take(PKT_FILEACK).size(), 0);
    InjectRaw(c[1].data(), (int)c[1].size());
    CHECK_EQ(g_got.files, 0);                            // (its real hash isn't 0)
    CHECK_EQ(Take(PKT_FILEACK).size(), 0);
}

TEST(file_same_hash_other_size_is_a_new_file) {
    NetFixture f(true);
    Connect("Client");
    HookFileCallbacks();
    std::vector<uint8_t> data = Pattern(FILE_CHUNK + 5);
    g_sent.clear();
    InjectAll(MakeChunks(data, "Tester", "Save", 42));
    CHECK_EQ(g_got.files, 1);
    CHECK_EQ(Take(PKT_FILEACK).size(), 1);
    // claims the hash we just completed, but another size: must not be acknowledged as "have it"
    std::vector<Dgram> other = MakeChunks(Pattern(3 * FILE_CHUNK, 7), "Tester", "Save", 43);
    for (auto& d : other) Chunk(d)->fileHash = Fnv(data);
    InjectRaw(other[0].data(), (int)other[0].size());
    CHECK_EQ(Take(PKT_FILEACK).size(), 0);
    CHECK_EQ(g_got.files, 1);
}

TEST(file_oversize_and_inconsistent_chunks_are_ignored) {
    NetFixture f(true);
    Connect("Client");
    HookFileCallbacks();
    std::vector<uint8_t> data = Pattern(3 * FILE_CHUNK);
    std::vector<Dgram> good = MakeChunks(data, "Tester", "Save", 42);
    g_sent.clear();
    uint32_t in0 = Net::Stats().packetsIn;

    Dgram d = good[0];
    Chunk(d)->size = FILE_MAX + 1; Chunk(d)->total = (uint16_t)Chunks(FILE_MAX + 1);
    InjectRaw(d.data(), (int)d.size());                  // larger than any save
    d = good[0]; Chunk(d)->size = 0xFFFFFFFFu; Chunk(d)->total = 0;                   // the chunk count would overflow
    InjectRaw(d.data(), (int)d.size());
    d = good[0]; Chunk(d)->size = 0xFFFFFC01u; Chunk(d)->total = 0;
    InjectRaw(d.data(), (int)d.size());
    d = good[0]; Chunk(d)->size = 0; Chunk(d)->total = 0;                             // empty
    InjectRaw(d.data(), (int)d.size());
    d = good[0]; Chunk(d)->total = 4;                                                  // count doesn't match the size
    InjectRaw(d.data(), (int)d.size());
    d = good[0]; Chunk(d)->total = 2;
    InjectRaw(d.data(), (int)d.size());
    d = good[0]; Chunk(d)->index = 3;                                                  // past the end
    InjectRaw(d.data(), (int)d.size());
    d = good[0]; Chunk(d)->index = 0xFFFF;
    InjectRaw(d.data(), (int)d.size());
    CHECK_EQ(Net::Stats().packetsIn, in0 + 8);           // they were looked at ...
    CHECK_EQ(g_got.files, 0);                            // ... and dropped
    CHECK_EQ(Take(PKT_FILEACK).size(), 0);

    // none of them disturbed the receiver: the real file still arrives whole
    InjectAll(good);
    CHECK_EQ(g_got.files, 1);
    CHECK(g_got.data == data);
}

TEST(file_truncated_chunk_is_ignored) {
    NetFixture f(true);
    Connect("Client");
    HookFileCallbacks();
    std::vector<uint8_t> data = Pattern(2 * FILE_CHUNK + 300);
    std::vector<Dgram> c = MakeChunks(data, "Tester", "Save", 42);
    REQUIRE(c.size() == 3);
    g_sent.clear();
    InjectRaw(c[0].data(), (int)c[0].size());
    InjectRaw(c[1].data(), (int)c[1].size() - 1);        // one byte short
    InjectRaw(c[2].data(), (int)c[2].size() - 1);        // the short last chunk, one byte short
    InjectRaw(c[2].data(), (int)offsetof(FileChunkPacket, data));       // header only
    InjectRaw(c[2].data(), (int)offsetof(FileChunkPacket, data) - 1);   // not even that
    InjectRaw(c[2].data(), (int)sizeof(PacketHeader));
    CHECK_EQ(g_got.files, 0);
    CHECK_EQ(Take(PKT_FILEACK).size(), 0);
    InjectRaw(c[2].data(), (int)c[2].size());            // exactly as long as its contents: fine
    CHECK_EQ(g_got.files, 0);
    InjectRaw(c[1].data(), (int)c[1].size());
    CHECK_EQ(g_got.files, 1);
    CHECK(g_got.data == data);                           // nothing from the cut datagrams got in

    // a datagram longer than needed is fine too (the padded struct)
    HookFileCallbacks();
    std::vector<uint8_t> small = Pattern(10, 3);
    Dgram one = MakeChunks(small, "Tester", "Save", 42)[0];
    one.resize(sizeof(FileChunkPacket), 0xEE);
    InjectRaw(one.data(), (int)one.size());
    CHECK_EQ(g_got.files, 1);
    CHECK(g_got.data == small);
}

TEST(file_names_without_terminator_are_terminated) {
    NetFixture f(true);
    Connect("Client");
    HookFileCallbacks();
    std::string full(32, 'N');                           // fills the field: no NUL on the wire
    InjectAll(MakeChunks(Pattern(50), full.c_str(), full.c_str(), 42));
    CHECK_EQ(g_got.files, 1);
    CHECK_EQ(g_got.owner.size(), 32);
    CHECK_EQ(g_got.profile.size(), 32);
}

TEST(file_a_second_file_replaces_a_half_received_one) {
    // one incoming slot: the sender sends one file at a time, so a different file means the first
    // one was given up
    NetFixture f(true);
    Connect("Client");
    HookFileCallbacks();
    std::vector<uint8_t> a = Pattern(3 * FILE_CHUNK, 1), b = Pattern(2 * FILE_CHUNK, 2);
    std::vector<Dgram> ca = MakeChunks(a, "Tester", "A", 1), cb = MakeChunks(b, "Tester", "B", 2, 2000);
    InjectRaw(ca[0].data(), (int)ca[0].size());
    InjectRaw(ca[1].data(), (int)ca[1].size());
    InjectAll(cb);
    CHECK_EQ(g_got.files, 1);
    CHECK_STR(g_got.profile, "B");
    CHECK(g_got.data == b);
    InjectRaw(ca[2].data(), (int)ca[2].size());          // A starts over: one chunk isn't the file
    CHECK_EQ(g_got.files, 1);
    InjectRaw(ca[0].data(), (int)ca[0].size());
    InjectRaw(ca[1].data(), (int)ca[1].size());
    CHECK_EQ(g_got.files, 2);
    CHECK(g_got.data == a);                              // no bytes of B mixed in
}

TEST(file_client_takes_files_only_from_the_host) {
    NetFixture f(false);
    Connect("Host");
    HookFileCallbacks();
    std::vector<uint8_t> data = Pattern(2 * FILE_CHUNK);
    std::vector<Dgram> c = MakeChunks(data, "Tester", "Save", 42);
    g_sent.clear();
    InjectAll(c, OTHER_IP);                              // someone else on the network
    CHECK_EQ(g_got.files, 0);
    CHECK_EQ(Take(PKT_FILEACK).size(), 0);
    PacketHeader nf = Hdr(PKT_NOFILE, 3000);
    Inject(nf, OTHER_IP);
    CHECK_EQ(g_got.noFile, 0);
    FileReqPacket rq{}; rq.h = Hdr(PKT_FILEREQ, 3001); strcpy_s(rq.owner, "Tester");
    Inject(rq, OTHER_IP);
    CHECK_EQ(g_got.requests, 0);

    InjectAll(c, PEER_IP);                               // the host
    CHECK_EQ(g_got.files, 1);
    CHECK(g_got.data == data);
    Inject(nf, PEER_IP);
    CHECK_EQ(g_got.noFile, 1);
}

TEST(file_ack_of_another_file_does_not_end_the_transfer) {
    NetFixture f(true);
    Connect("Client");
    std::vector<uint8_t> data = Pattern(500);
    REQUIRE(Net::SendFile("A", "B", data.data(), 500, 1));
    FileAckPacket a{}; a.h = Hdr(PKT_FILEACK, 70); a.fileHash = Fnv(data) ^ 1;
    Inject(a);
    CHECK(Net::SendingFile());
    a.h.seq = 71; a.fileHash = Fnv(data);
    InjectRaw(&a, (int)sizeof(a) - 1);                   // cut short
    CHECK(Net::SendingFile());
    Inject(a);
    CHECK(!Net::SendingFile());
    CHECK(Net::LastFileDelivered());
}

// ---- request / "no file" ---------------------------------------------------------------------------

TEST(file_request_and_nofile_round_trip) {
    NetFixture f(false);
    Connect("Host");
    HookFileCallbacks();
    g_sent.clear();
    Net::RequestFile("Tester");
    std::vector<Dgram> req = Take(PKT_FILEREQ);
    REQUIRE(req.size() == 2);                            // sent twice (UDP)
    CHECK_EQ(req[0].size(), sizeof(FileReqPacket));
    InjectAll(req);
    CHECK_EQ(g_got.requests, 2);
    CHECK_STR(g_got.reqOwner, "Tester");

    Net::SendNoFile();
    std::vector<Dgram> nf = Take(PKT_NOFILE);
    REQUIRE(nf.size() == 2);
    CHECK_EQ(nf[0].size(), sizeof(PacketHeader));
    InjectAll(nf);
    CHECK_EQ(g_got.noFile, 2);
    CHECK_EQ(g_got.files, 0);

    // a long or missing name can't overrun the field
    Net::RequestFile("NameThatIsMuchLongerThanThirtyTwoCharactersLong");
    Net::RequestFile(nullptr);
    req = Take(PKT_FILEREQ);
    REQUIRE(req.size() == 4);
    InjectRaw(req[0].data(), (int)req[0].size());
    CHECK_STR(g_got.reqOwner, "NameThatIsMuchLongerThanThirtyT");
    InjectRaw(req[2].data(), (int)req[2].size());
    CHECK_STR(g_got.reqOwner, "");
}

TEST(file_request_truncated_or_unterminated) {
    NetFixture f(true);
    Connect("Client");
    HookFileCallbacks();
    FileReqPacket rq{}; rq.h = Hdr(PKT_FILEREQ, 10);
    memset(rq.owner, 'x', sizeof(rq.owner));             // no NUL on the wire
    InjectRaw(&rq, (int)sizeof(rq) - 1);
    CHECK_EQ(g_got.requests, 0);
    Inject(rq);
    CHECK_EQ(g_got.requests, 1);
    CHECK_EQ(g_got.reqOwner.size(), 32);
    // without callbacks nothing happens (and nothing crashes)
    Net::OnFileRequest = nullptr; Net::OnNoFile = nullptr; Net::OnFile = nullptr;
    rq.h.seq = 11; Inject(rq);
    PacketHeader nf = Hdr(PKT_NOFILE, 12); Inject(nf);
    InjectAll(MakeChunks(Pattern(10), "A", "B", 1));
    CHECK_EQ(g_got.requests, 1);
}

// ---- saveutil.h: names -------------------------------------------------------------------------------

static std::string Safe(const char* s, size_t n = 40) {
    char out[64] = "?";
    return SaveUtil::SafeName(s, out, n) ? std::string(out) : std::string("<rejected>") + out;
}

TEST(save_safe_name_keeps_plain_names) {
    CHECK_STR(Safe("Player"), "Player");
    CHECK_STR(Safe("My Save_1-a"), "My Save_1-a");
    CHECK_STR(Safe("0123456789"), "0123456789");
    CHECK_STR(Safe(" lead"), " lead");
}

TEST(save_safe_name_blocks_path_tricks) {
    CHECK_STR(Safe("..\\..\\Windows\\system32"), "Windowssystem32");
    CHECK_STR(Safe("../../etc/passwd"), "etcpasswd");
    CHECK_STR(Safe("C:\\evil"), "Cevil");
    CHECK_STR(Safe("a.b"), "ab");
    CHECK_STR(Safe("save.ini"), "saveini");              // can't name the host's own index file
    CHECK_STR(Safe("x:stream"), "xstream");              // no alternate data streams
    CHECK_STR(Safe("a*b?c<d>e|f\"g"), "abcdefg");
    CHECK_STR(Safe("tab\there\r\n"), "tabhere");
    CHECK_STR(Safe("%TEMP%"), "TEMP");
    CHECK_STR(Safe("\xC8\xE3\xF0\xEE\xEA" "1"), "1");    // only ASCII, whatever the locale
    // nothing usable left
    CHECK_STR(Safe(""), "<rejected>");
    CHECK_STR(Safe(".."), "<rejected>");
    CHECK_STR(Safe("."), "<rejected>");
    CHECK_STR(Safe("\\/"), "<rejected>");
    CHECK_STR(Safe("   "), "<rejected>");
    CHECK_STR(Safe("\xC8\xE3\xF0"), "<rejected>");
    CHECK_STR(Safe(nullptr), "<rejected>");
    // spaces at the end would be dropped by Windows and make two names one folder
    CHECK_STR(Safe("Bob  "), "Bob");
    CHECK_STR(Safe("Bob ."), "Bob");
}

TEST(save_safe_name_rejects_device_names) {
    for (const char* s : { "CON", "con", "Con", "PRN", "AUX", "NUL", "nul", "COM1", "com9", "COM0", "LPT1", "lpt9", "NUL ", "C.O.N", "N/U/L" })
        CHECK_STR(Safe(s), "<rejected>");
    for (const char* s : { "CONSOLE", "NULL", "COM", "COM10", "LPT", "COMA", "ICON", "AUX1" })
        CHECK_STR(Safe(s), s);
}

TEST(save_safe_name_respects_the_buffer) {
    CHECK_STR(Safe("abcdef", 4), "abc");
    CHECK_STR(Safe("abcdef", 1), "<rejected>");
    CHECK_STR(Safe("a.b.c.d.e.f", 4), "abc");            // dropped characters don't count
    char out[8];
    memset(out, 'Z', sizeof(out));
    CHECK(SaveUtil::SafeName("abcdefghijklmnop", out, 5));
    CHECK(!memcmp(out, "abcd\0ZZZ", 8));                 // nothing written past n
    CHECK(!SaveUtil::SafeName("abc", out, 0));
    CHECK(!SaveUtil::SafeName("abc", nullptr, 8));
    // cutting can leave a space at the end or a device name: both handled after the cut
    CHECK_STR(Safe("ab cd", 4), "ab");
    CHECK_STR(Safe("CONSOLE", 4), "<rejected>");
    // a name straight from a 32-byte packet field
    std::string full(32, 'q');
    CHECK_STR(Safe(full.c_str()), full);
}

TEST(save_plain_name) {
    CHECK(SaveUtil::PlainName("Player"));
    CHECK(SaveUtil::PlainName("My Save_1-a"));
    CHECK(!SaveUtil::PlainName("a.b"));
    CHECK(!SaveUtil::PlainName("..\\x"));
    CHECK(!SaveUtil::PlainName("x "));
    CHECK(!SaveUtil::PlainName(""));
    CHECK(!SaveUtil::PlainName("CON"));
    CHECK(!SaveUtil::PlainName(nullptr));
    CHECK(SaveUtil::PlainName(std::string(32, 'a').c_str()));
    CHECK(!SaveUtil::PlainName(std::string(200, 'a').c_str()));
}

TEST(save_same_owner) {
    CHECK(SaveUtil::SameOwner("Kosty", "Kosty"));
    CHECK(SaveUtil::SameOwner("Kosty", "KOSTY"));
    // regression: the host answers with the file-name form of the name; the client compared it with
    // its raw name, so a player called "J.D." never got a save back
    CHECK(SaveUtil::SameOwner("JD", "J.D."));
    CHECK(SaveUtil::SameOwner("Mr Smith", "Mr. Smith "));
    CHECK(!SaveUtil::SameOwner("Kosty", "Kostya"));
    CHECK(!SaveUtil::SameOwner("", ""));
    CHECK(!SaveUtil::SameOwner("...", "..."));           // no usable name: never "the same"
    CHECK(!SaveUtil::SameOwner(nullptr, "a"));
    CHECK(!SaveUtil::SameOwner("NUL", "NUL"));
}

TEST(save_host_copy_is_newer) {
    CHECK(SaveUtil::HostCopyIsNewer(0, 5));              // we have no file
    CHECK(SaveUtil::HostCopyIsNewer(4, 5));
    CHECK(!SaveUtil::HostCopyIsNewer(5, 5));             // the same save: leave it alone
    CHECK(!SaveUtil::HostCopyIsNewer(6, 5));             // ours is newer: never overwritten
    CHECK(!SaveUtil::HostCopyIsNewer(5, 0));             // a copy without a time never wins
    CHECK(!SaveUtil::HostCopyIsNewer(0, 0));
    CHECK(SaveUtil::HostCopyIsNewer(0x01DB000000000000ull, 0x01DB000000000001ull));   // all 64 bits count
    CHECK(!SaveUtil::HostCopyIsNewer(0x01DB000000000001ull, 0x01DA0000FFFFFFFFull));
}

// ---- saveutil.h: files -------------------------------------------------------------------------------

TEST(save_write_and_read_back) {
    std::string dir = TempDir();
    std::string path = dir + "career_rt";
    DeleteFileA(path.c_str());
    std::vector<uint8_t> data = Pattern(70000), back;
    const uint64_t mtime = 0x01DB123456780000ull;
    CHECK_EQ(SaveUtil::FileTime64(path), 0);
    CHECK(!SaveUtil::ReadAll(path, back, FILE_MAX));     // no such file
    REQUIRE(SaveUtil::WriteAllAtomic(path, data.data(), (uint32_t)data.size(), mtime));
    CHECK(SaveUtil::FileTime64(path) == mtime);          // the owner's time, not "now"
    REQUIRE(SaveUtil::ReadAll(path, back, FILE_MAX));
    CHECK(back == data);
    CHECK_EQ(GetFileAttributesA((path + ".mwt-tmp").c_str()), INVALID_FILE_ATTRIBUTES);   // nothing left behind

    // replacing it
    std::vector<uint8_t> newer = Pattern(1000, 5);
    REQUIRE(SaveUtil::WriteAllAtomic(path, newer.data(), (uint32_t)newer.size(), mtime + 10000000));
    REQUIRE(SaveUtil::ReadAll(path, back, FILE_MAX));
    CHECK(back == newer);
    CHECK(SaveUtil::FileTime64(path) == mtime + 10000000);

    // without a time the file is simply new
    REQUIRE(SaveUtil::WriteAllAtomic(path, newer.data(), (uint32_t)newer.size(), 0));
    CHECK(SaveUtil::FileTime64(path) > mtime + 10000000);
    DeleteFileA(path.c_str());
}

TEST(save_read_limits) {
    std::string dir = TempDir();
    std::string path = dir + "career_lim";
    std::vector<uint8_t> data = Pattern(2000), back = { 1, 2, 3 };
    REQUIRE(SaveUtil::WriteAllAtomic(path, data.data(), 2000, 0));
    CHECK(SaveUtil::ReadAll(path, back, 2000));
    CHECK(!SaveUtil::ReadAll(path, back, 1999));         // larger than we would ever send
    CHECK(back.empty());
    REQUIRE(SaveUtil::WriteAllAtomic(path, data.data(), 0, 0));
    CHECK(!SaveUtil::ReadAll(path, back, 2000));         // an empty file is no save
    CHECK(!SaveUtil::ReadAll(dir, back, 2000));          // a folder
    CHECK_EQ(SaveUtil::FileTime64(dir), 0);
    DeleteFileA(path.c_str());
}

TEST(save_failed_write_keeps_the_old_file) {
    std::string dir = TempDir();
    std::string path = dir + "career_keep";
    SetFileAttributesA(path.c_str(), FILE_ATTRIBUTE_NORMAL);
    std::vector<uint8_t> good = Pattern(5000, 1), bad = Pattern(300, 2), back;
    REQUIRE(SaveUtil::WriteAllAtomic(path, good.data(), (uint32_t)good.size(), 0x01DB000000000000ull));
    REQUIRE(SetFileAttributesA(path.c_str(), FILE_ATTRIBUTE_READONLY));     // the replace will be refused
    CHECK(!SaveUtil::WriteAllAtomic(path, bad.data(), (uint32_t)bad.size(), 0x01DC000000000000ull));
    REQUIRE(SaveUtil::ReadAll(path, back, FILE_MAX));
    CHECK(back == good);                                 // untouched, not half-written
    CHECK(SaveUtil::FileTime64(path) == 0x01DB000000000000ull);
    CHECK_EQ(GetFileAttributesA((path + ".mwt-tmp").c_str()), INVALID_FILE_ATTRIBUTES);
    SetFileAttributesA(path.c_str(), FILE_ATTRIBUTE_NORMAL);
    DeleteFileA(path.c_str());

    // a folder that doesn't exist, and a folder in the way
    CHECK(!SaveUtil::WriteAllAtomic(dir + "no_such_folder\\x", bad.data(), 300, 0));
    std::string sub = dir + "career_dir";
    CreateDirectoryA(sub.c_str(), nullptr);
    CHECK(!SaveUtil::WriteAllAtomic(sub, bad.data(), 300, 0));
    CHECK_EQ(GetFileAttributesA((sub + ".mwt-tmp").c_str()), INVALID_FILE_ATTRIBUTES);
    RemoveDirectoryA(sub.c_str());
}
