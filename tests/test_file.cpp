// Builds up to 1.7.5 kept the client's career save on the host (PKT_FILE / FILEACK / FILEREQ / NOFILE).
// This build keeps none but answers them, so an older peer neither waits for its save nor sends it
// again and again: a request gets "no save", every chunk an acknowledgement of its file's hash.
#include "net_helpers.h"

using namespace th;

namespace {
    typedef std::vector<uint8_t> Dgram;
    std::vector<Dgram> Take(uint16_t type) {          // captured datagrams of one type, removed from the capture
        std::vector<Dgram> out, rest;
        for (auto& p : g_sent) {
            bool match = p.size() >= sizeof(PacketHeader) && ((const PacketHeader*)p.data())->type == type;
            (match ? out : rest).push_back(p);
        }
        g_sent.swap(rest);
        return out;
    }
}

TEST(old_save_request_gets_no_save) {
    for (int host = 0; host < 2; ++host) {
        NetFixture f(host != 0);
        Connect("Old");
        g_sent.clear();
        FileReqPacket q{}; q.h = Hdr(PKT_FILEREQ, 50); strcpy_s(q.owner, "Old");
        InjectRaw(&q, sizeof(q));
        std::vector<Dgram> nf = Take(PKT_NOFILE);
        REQUIRE(nf.size() == 2);                         // twice (UDP), as the old host did
        CHECK_EQ(nf[0].size(), sizeof(PacketHeader));
        CHECK(Take(PKT_FILE).empty());
    }
}

TEST(old_save_chunks_are_acknowledged_and_dropped) {
    NetFixture f(true);
    Connect("Old");
    g_sent.clear();
    FileChunkPacket p{}; p.h = Hdr(PKT_FILE, 60);
    p.fileHash = 0xC0FFEE11; p.size = 3000; p.total = 3; p.index = 1;
    strcpy_s(p.owner, "Old"); strcpy_s(p.profile, "Career");
    InjectRaw(&p, sizeof(p));
    std::vector<Dgram> a = Take(PKT_FILEACK);
    REQUIRE(a.size() == 1);
    REQUIRE(a[0].size() == sizeof(FileAckPacket));
    CHECK_EQ(((const FileAckPacket*)a[0].data())->fileHash, 0xC0FFEE11u);
    // a chunk cut before its data is not answered; nothing else is sent back, nothing stored
    p.h = Hdr(PKT_FILE, 61);
    InjectRaw(&p, (int)offsetof(FileChunkPacket, data) - 1);
    CHECK(Take(PKT_FILEACK).empty());
    CHECK(Take(PKT_FILE).empty());
    // acks and "no save" from an old peer are simply ignored
    FileAckPacket k{}; k.h = Hdr(PKT_FILEACK, 62); InjectRaw(&k, sizeof(k));
    PacketHeader n = Hdr(PKT_NOFILE, 63); InjectRaw(&n, sizeof(n));
    CHECK(Take(PKT_FILEACK).empty());
    CHECK(Take(PKT_NOFILE).empty());
}
