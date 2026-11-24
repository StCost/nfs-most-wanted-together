// Notices in the centre of the screen (src/noticeutil.h).
#include "mwt_test.h"
#include "../src/noticeutil.h"
#include <cstring>

TEST(notice_short_stays_whole) {
    auto p = Notice::Split("Laptop joined");
    CHECK_EQ((int)p.size(), 1); CHECK(p[0] == "Laptop joined");
    CHECK(Notice::Split("").empty());
    std::string exact(63, 'a');
    CHECK_EQ((int)Notice::Split(exact).size(), 1);
}

TEST(notice_split_at_dash) {
    auto p = Notice::Split("No answer from the host - check the host's IP and allow UDP 47555 in this PC's firewall");
    CHECK_EQ((int)p.size(), 2);
    CHECK(p[0] == "No answer from the host");
    CHECK(p[1] == "Check the host's IP and allow UDP 47555 in this PC's firewall");
}

TEST(notice_parts_fit) {
    const char* texts[] = {
        "No answer from the host - check the host's IP, and allow UDP 47555 in the firewall on this PC",
        "Laptop is at Blacklist #12, you are at #13 - beat your rival to catch up",
        "Laptop doesn't receive your game - allow UDP 47555 in the firewall on their PC",
        "averyveryveryveryveryveryveryveryveryveryveryveryveryveryverylongwordthatneverends and more" };
    for (const char* t : texts) {
        auto p = Notice::Split(t);
        CHECK(!p.empty());
        size_t chars = 0;
        for (auto& s : p) { CHECK(s.size() <= Notice::MaxChars); CHECK(!s.empty()); chars += s.size(); }
        CHECK(chars + 3 * p.size() >= strlen(t) - 3 * 4);   // nothing but separators lost
    }
}

TEST(notice_packs_small_pieces) {
    auto p = Notice::Split("aaaa - bbbb - " + std::string(60, 'c'));
    CHECK_EQ((int)p.size(), 2);
    CHECK(p[0] == "aaaa - bbbb");
}

TEST(notice_join_waits_for_the_name) {
    Notice::JoinNotice j;
    CHECK(Notice::JoinTick(j, false, "", 0).empty());
    CHECK(Notice::JoinTick(j, true, "", 1000).empty());           // connected, no hello yet: nothing
    CHECK(Notice::JoinTick(j, true, "", 1500).empty());
    CHECK(Notice::JoinTick(j, true, "Laptop", 1600) == "Laptop joined");   // one notice, with the name
    CHECK(Notice::JoinTick(j, true, "Laptop", 1700).empty());     // and only one
    CHECK(Notice::JoinTick(j, true, "Laptop", 9000).empty());
    // name already known (hello came with the first packets, or a reconnect)
    CHECK(Notice::JoinTick(j, false, "Laptop", 10000).empty());
    CHECK(Notice::JoinTick(j, true, "Laptop", 10100) == "Laptop joined");
    CHECK(Notice::JoinTick(j, true, "Laptop", 10200).empty());
}

TEST(notice_join_falls_back_without_a_name) {
    Notice::JoinNotice j;
    CHECK(Notice::JoinTick(j, true, "", 5000).empty());
    CHECK(Notice::JoinTick(j, true, "", 5000 + Notice::JoinNameWaitMs - 1).empty());
    CHECK(Notice::JoinTick(j, true, "", 5000 + Notice::JoinNameWaitMs) == "The other player joined");
    CHECK(Notice::JoinTick(j, true, "Laptop", 9000).empty());     // the late name does not repeat it
    // lost before the name came: no join notice at all
    Notice::JoinNotice k;
    CHECK(Notice::JoinTick(k, true, "", 0).empty());
    CHECK(Notice::JoinTick(k, false, "", 500).empty());
    CHECK(Notice::JoinTick(k, false, "", 9000).empty());
    // tick counter wrap
    Notice::JoinNotice w;
    CHECK(Notice::JoinTick(w, true, "", 0xFFFFFF00u).empty());
    CHECK(Notice::JoinTick(w, true, "", 0x00000100u).empty());
    CHECK(Notice::JoinTick(w, true, "", 0xFFFFFF00u + Notice::JoinNameWaitMs) == "The other player joined");
}