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

#include "../src/resutil.h"
#include "../src/ddsutil.h"

TEST(resources_logo_is_valid_and_license_texts_are_present) {
    const uint8_t* p = nullptr; size_t n = 0;
    CHECK(ResUtil::Get(ResUtil::kLogo, p, n));
    DdsUtil::Info info; std::string err;
    CHECK(DdsUtil::Parse(p, n, info, err));
    CHECK_EQ(info.width, 512u); CHECK(info.height >= 128);   // the 4:1 menu logo, taller for the Together line
    CHECK(ResUtil::Get(ResUtil::kLicense, p, n));
    std::string license((const char*)p, n);
    CHECK(license.find("Copyright (c) 2026 StCost") != std::string::npos);
    CHECK(license.find("Permission is hereby granted") != std::string::npos);
    CHECK(license.find("THE SOFTWARE IS PROVIDED") != std::string::npos);
    CHECK(ResUtil::Get(ResUtil::kNotices, p, n));
    std::string notices((const char*)p, n);
    CHECK(notices.find("Ultimate ASI Loader") != std::string::npos);
    CHECK(notices.find("Microsoft Visual C++ runtime") != std::string::npos);
    CHECK(!ResUtil::Get("NO_SUCH_RESOURCE", p, n)); CHECK(p == nullptr); CHECK_EQ(n, 0);
}

TEST(resources_default_ini_is_written_on_clean_install_only) {
    const uint8_t* p = nullptr; size_t n = 0;
    CHECK(ResUtil::Get(ResUtil::kDefaultIni, p, n));
    std::string dir = th::TempDir(), ini = dir + "res_MWTogether.ini";
    CHECK(ResUtil::WriteIfMissing(ini.c_str(), p, n));
    FILE* f = fopen(ini.c_str(), "rb"); CHECK(f != nullptr);
    if (f) {
        std::vector<uint8_t> actual(n + 1); size_t got = fread(actual.data(), 1, actual.size(), f); fclose(f);
        CHECK_EQ(got, n); CHECK(memcmp(actual.data(), p, n) == 0);
    }
    CHECK(!ResUtil::WriteIfMissing(ini.c_str(), "not your settings", 17));
    // Even an empty settings file is the player's file: never replace it.
    f = fopen(ini.c_str(), "wb"); if (f) fclose(f);
    CHECK(!ResUtil::WriteIfMissing(ini.c_str(), p, n));
    WIN32_FILE_ATTRIBUTE_DATA a = {}; CHECK(GetFileAttributesExA(ini.c_str(), GetFileExInfoStandard, &a));
    CHECK_EQ(a.nFileSizeLow, 0);
    CHECK(!ResUtil::WriteIfMissing(nullptr, p, n));
    CHECK(!ResUtil::WriteIfMissing((dir + "missing\\MWTogether.ini").c_str(), p, n));
    DeleteFileA(ini.c_str()); RemoveDirectoryA(dir.c_str());
}

TEST(resources_license_log_keeps_complete_lines) {
    const char text[] = "MIT License\r\n\r\nCopyright (c) StCost\nlast";
    auto lines = ResUtil::Lines((const uint8_t*)text, sizeof(text) - 1);
    CHECK_EQ(lines.size(), 4); CHECK_STR(lines[0], "MIT License"); CHECK_STR(lines[1], "");
    CHECK_STR(lines[2], "Copyright (c) StCost"); CHECK_STR(lines[3], "last");
    CHECK(ResUtil::Lines(nullptr, 0).empty());
}
