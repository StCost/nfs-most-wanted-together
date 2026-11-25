// Fixed AI opponents: name seeds and the opponents book (src/opponentsutil.h).
#include "mwt_test.h"
#include "../src/opponentsutil.h"

using namespace Opponents;

TEST(opponents_name_seed_is_fixed_per_event_and_slot) {
    CHECK(NameSeed(0x8B0E928A, 1) == NameSeed(0x8B0E928A, 1));     // every restart, both games
    CHECK(NameSeed(0x8B0E928A, 1) != NameSeed(0x8B0E928A, 2));     // another racer
    CHECK(NameSeed(0x8B0E928A, 1) != NameSeed(0x1234, 1));         // another event
    for (int s = 0; s < 16; ++s) CHECK(NameSeed(0, s) != 0);       // never the generator's dead seed
}

TEST(opponents_book_replays_the_first_build) {
    Book b;
    uint8_t rec[kRecordBytes]; for (int i = 0; i < kRecordBytes; ++i) rec[i] = (uint8_t)i;
    CHECK(b.Get(7, 0) == nullptr);
    CHECK(b.Put(7, 0, 0xAAAA, rec));
    CHECK(b.Put(7, 1, 0xBBBB, nullptr));
    CHECK(!b.Put(7, 0, 0xCCCC, rec));            // already there: the first build stays
    CHECK(!b.Put(7, 3, 0xDDDD, rec));            // a gap: not in order
    CHECK(!b.Put(8, 1, 0xDDDD, rec));            // a new event starts at its first racer
    CHECK(!b.Put(0, 0, 0xDDDD, rec));
    CHECK(!b.Put(7, kMaxPerEvent, 1, rec));
    CHECK(b.Count(7) == 2);
    const Entry* e = b.Get(7, 0);
    CHECK(e && e->key == 0xAAAA && e->hasCustom && e->custom[100] == 100);
    e = b.Get(7, 1);
    CHECK(e && e->key == 0xBBBB && !e->hasCustom);
    CHECK(b.Get(7, 2) == nullptr);
}

TEST(opponents_book_save_and_load) {
    Book b;
    uint8_t rec[kRecordBytes] = {}; rec[5] = 42;
    b.Put(1, 0, 11, rec); b.Put(1, 1, 12, rec); b.Put(2, 0, 21, nullptr);
    std::vector<uint8_t> f = b.Save();
    Book c;
    CHECK(c.Load(f.data(), f.size()));
    CHECK(c.Events() == 2 && c.Count(1) == 2 && c.Count(2) == 1);
    CHECK(c.Get(1, 1)->key == 12 && c.Get(1, 1)->custom[5] == 42);
    CHECK(!c.Get(2, 0)->hasCustom);
    // damaged files: refused, nothing half loaded
    for (size_t cut = 0; cut + 1 < f.size(); cut += 97) CHECK(!c.Load(f.data(), cut) && c.Events() == 0);
    std::vector<uint8_t> bad = f; bad[0] ^= 1;
    CHECK(!c.Load(bad.data(), bad.size()));
    CHECK(!c.Load(nullptr, 0));
}

TEST(opponents_book_keeps_the_newest_events) {
    Book b;
    for (uint32_t h = 1; h <= (uint32_t)kMaxEvents + 5; ++h) b.Put(h, 0, h, nullptr);
    CHECK((int)b.Events() == kMaxEvents);
    CHECK(b.Count(1) == 0 && b.Count(kMaxEvents + 5) == 1);
    b.Forget(kMaxEvents + 5);
    CHECK(b.Count(kMaxEvents + 5) == 0);
}

TEST(opponents_new_build) {
    CHECK(NewBuild(0, 5, 0));                    // another event
    CHECK(!NewBuild(5, 5, 200));                 // the same build (racers come one after another)
    CHECK(NewBuild(5, 5, kNewBuildGapMs));       // a restart of the same event
}
