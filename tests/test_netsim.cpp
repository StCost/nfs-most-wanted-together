#include "mwt_test.h"
#include "../src/netsimutil.h"

using NetSimUtil::Settings;
using NetSimUtil::ClampSettings;
using NetSimUtil::Enabled;
using NetSimUtil::ComputeDelay;
using NetSimUtil::Decide;

TEST(netsim_disabled_when_all_zero) {
    Settings s{};
    CHECK(!Enabled(s));
    CHECK(Enabled(Settings{1, 0, 0, 0, 0}));
    CHECK(Enabled(Settings{0, 1, 0, 0, 0}));
    CHECK(Enabled(Settings{0, 0, 1, 0, 0}));
    CHECK(Enabled(Settings{0, 0, 0, 1, 0}));
    CHECK(Enabled(Settings{0, 0, 0, 0, 1}));
}

TEST(netsim_clamp_ranges) {
    Settings s = ClampSettings({-5, -3, -1, 150, 200});
    CHECK_EQ(s.delayMs, 0);
    CHECK_EQ(s.jitterMs, 0);
    CHECK_EQ(s.lossPercent, 0);
    CHECK_EQ(s.duplicatePercent, 100);
    CHECK_EQ(s.reorderPercent, 100);
    s = ClampSettings({5000, 4000, 40, -2, 2});
    CHECK_EQ(s.delayMs, 2000);
    CHECK_EQ(s.jitterMs, 1000);
    CHECK_EQ(s.lossPercent, 40);
    CHECK_EQ(s.duplicatePercent, 0);
    CHECK_EQ(s.reorderPercent, 2);
}

TEST(netsim_delay_without_jitter_is_fixed) {
    Settings s{120, 0, 0, 0, 0};
    for (uint32_t r = 0; r < 50; ++r)
        CHECK_EQ(ComputeDelay(s, r * 17u + 3u), 120);
}

TEST(netsim_jitter_stays_in_range_and_hits_both_ends) {
    Settings s{100, 40, 0, 0, 0};
    int lo = 1000, hi = -1;
    bool seenMin = false, seenMax = false;
    for (uint32_t r = 0; r < 4000; ++r) {
        int d = ComputeDelay(s, r);
        CHECK(d >= 60);
        CHECK(d <= 140);
        if (d < lo) lo = d;
        if (d > hi) hi = d;
        if (d == 60) seenMin = true;
        if (d == 140) seenMax = true;
    }
    CHECK(seenMin);
    CHECK(seenMax);
    CHECK_EQ(lo, 60);
    CHECK_EQ(hi, 140);
}

TEST(netsim_jitter_does_not_go_negative) {
    Settings s{5, 40, 0, 0, 0};
    for (uint32_t r = 0; r < 500; ++r)
        CHECK(ComputeDelay(s, r) >= 0);
}

TEST(netsim_loss_drops_expected_fraction) {
    Settings s{0, 0, 2, 0, 0};   // 2%
    int drops = 0, n = 10000;
    for (int i = 0; i < n; ++i) {
        auto d = Decide(s, (uint32_t)i, 0, 0, 0, 0);
        if (d.drop) {
            CHECK_EQ(d.copies, 0);
            drops++;
        } else {
            CHECK_EQ(d.copies, 1);
            CHECK_EQ(d.delayMs[0], 0);
        }
    }
    // i % 100 < 2 -> exactly 2 in every 100
    CHECK_EQ(drops, 200);
}

TEST(netsim_hundred_percent_loss_drops_everything) {
    Settings s{50, 10, 100, 50, 50};
    for (uint32_t i = 0; i < 30; ++i) {
        auto d = Decide(s, i, i, i, i, i);
        CHECK(d.drop);
        CHECK_EQ(d.copies, 0);
    }
}

TEST(netsim_duplicate_adds_a_later_copy) {
    Settings s{80, 0, 0, 100, 0};  // always duplicate, no jitter
    auto d = Decide(s, 0, 0, 0, 0, 0);
    CHECK(!d.drop);
    CHECK_EQ(d.copies, 2);
    CHECK_EQ(d.delayMs[0], 80);
    CHECK(d.delayMs[1] > d.delayMs[0]);
}

TEST(netsim_reorder_adds_extra_delay) {
    Settings s{120, 0, 0, 0, 100};  // always reorder
    auto plain = Decide(Settings{120, 0, 0, 0, 0}, 0, 0, 0, 0, 0);
    auto reord = Decide(s, 0, 0, 0, 0, 0);
    CHECK(!reord.drop);
    CHECK_EQ(reord.copies, 1);
    CHECK(reord.delayMs[0] > plain.delayMs[0]);
}

TEST(netsim_zero_loss_never_drops) {
    Settings s{120, 40, 0, 0, 0};
    for (uint32_t i = 0; i < 200; ++i) {
        auto d = Decide(s, i * 13u, i, i, i * 7u, i * 3u);
        CHECK(!d.drop);
        CHECK_EQ(d.copies, 1);
    }
}
