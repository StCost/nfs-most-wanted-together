#include "mwt_test.h"
#include "../src/paintutil.h"
#include <cstring>

TEST(paint_base_index_slot_76) {
    uint8_t rec[0x198]{};
    CHECK_EQ(Paint::BasePaintIndex(rec, sizeof(rec)), 0);
    int16_t idx = 1234;
    memcpy(rec + 76 * 2, &idx, 2);
    CHECK_EQ(Paint::BasePaintIndex(rec, sizeof(rec)), 1234);
    idx = -1;
    memcpy(rec + 76 * 2, &idx, 2);
    CHECK_EQ(Paint::BasePaintIndex(rec, sizeof(rec)), -1);
    CHECK_EQ(Paint::BasePaintIndex(rec, 10), -1);
    CHECK_EQ(Paint::BasePaintIndex(nullptr, 0x198), -1);
    uint8_t out[0x198]{};
    Paint::SetBasePaintIndex(out, sizeof(out), 77);
    CHECK_EQ(Paint::BasePaintIndex(out, sizeof(out)), 77);
}

TEST(paint_pack_argb) {
    CHECK_EQ(Paint::PackArgb(0xE2, 0x6C, 0x12), 0xFFE26C12u);
    CHECK_EQ(Paint::PackArgb(0x90, 0x20, 0xC0), 0xFF9020C0u);
    CHECK_EQ(Paint::PackArgb(-4, 300, 7), 0xFF00FF07u);
}

TEST(paint_purple_score) {
    CHECK(Paint::PurpleScore(0x90, 0x20, 0xC0) > 0);
    CHECK_EQ(Paint::PurpleScore(0xE2, 0x6C, 0x12), -1);
    CHECK(Paint::PurpleScore(0x90, 0x20, 0xC0) > Paint::PurpleScore(0x60, 0x20, 0x80));
}
