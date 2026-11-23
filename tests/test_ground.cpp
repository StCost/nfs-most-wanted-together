// Copies of the other game's cars on the ground (src/groundutil.h).
#include "mwt_test.h"
#include "../src/groundutil.h"

TEST(ground_learn_ride_height) {
    float ride = 0; bool known = false;
    CHECK(Ground::Learn(10.6f, 10.0f, 0.f, ride, known));
    CHECK(known); CHECK(std::fabs(ride - 0.6f) < 1e-4f);
    CHECK(Ground::Learn(10.7f, 10.0f, 0.2f, ride, known));             // averaged, not replaced
    CHECK(std::fabs(ride - 0.62f) < 1e-4f);
    CHECK(!Ground::Learn(14.0f, 10.0f, 0.f, ride, known));             // far above: a jump / another deck
    CHECK(!Ground::Learn(10.0f, 10.0f, 0.f, ride, known));             // at the ground: not a ride height
    CHECK(!Ground::Learn(10.6f, 10.0f, -3.f, ride, known));            // falling
    CHECK(std::fabs(ride - 0.62f) < 1e-4f);
}

TEST(ground_target) {
    // floating 0.3 m above where it should be
    CHECK(std::fabs(Ground::Target(true, true, 10.9f, 10.0f, true, 0.6f) + 0.3f) < 1e-4f);
    // sunk 0.2 m
    CHECK(std::fabs(Ground::Target(true, true, 10.4f, 10.0f, true, 0.6f) - 0.2f) < 1e-4f);
    CHECK_EQ(Ground::Target(false, true, 10.9f, 10.0f, true, 0.6f), 0.f);   // full physics on the owner: as sent
    CHECK_EQ(Ground::Target(true, false, 10.9f, 10.0f, true, 0.6f), 0.f);   // no ground here
    CHECK_EQ(Ground::Target(true, true, 10.9f, 10.0f, false, 0.6f), 0.f);   // ride height unknown
    CHECK_EQ(Ground::Target(true, true, 15.0f, 10.0f, true, 0.6f), 0.f);    // the ground found is another deck
}

TEST(ground_step_is_smooth) {
    float c = Ground::Step(0.f, -0.3f, 0.016f);
    CHECK(c < 0.f && c > -0.03f);                                       // 1.5 m/s
    CHECK_EQ(Ground::Step(-0.29f, -0.3f, 0.016f), -0.3f);              // reaches it exactly
    CHECK(Ground::Step(0.f, 1.f, 5.f) <= 0.15f + 1e-4f);               // a long frame is capped
    CHECK_EQ(Ground::Step(0.1f, 0.1f, 0.016f), 0.1f);
}

TEST(ground_far_from_owner_counts_as_floating) {
    CHECK(Ground::LikelyFloating(true, 10.f, 0.f));
    CHECK(Ground::LikelyFloating(false, 300.f, 0.f));       // host spawned it near the client: no collision there
    CHECK(!Ground::LikelyFloating(false, 50.f, 0.f));       // full physics near the host: as sent
    CHECK(!Ground::LikelyFloating(true, 300.f, -5.f));      // falling / jumping: leave it
}

TEST(ground_ride_height_sources) {
    using Ground::RideFrom;
    CHECK(Ground::PickRide(true, true, 0.6f) == RideFrom::Car);
    CHECK(Ground::PickRide(false, true, 0.6f) == RideFrom::Model);
    CHECK(Ground::PickRide(false, false, 0.6f) == RideFrom::Shape);
    CHECK(Ground::PickRide(false, false, 0.f) == RideFrom::None);
    CHECK_EQ(Ground::ShapeTarget(-0.4f), -0.4f);            // floating: pulled down
    CHECK_EQ(Ground::ShapeTarget(0.2f), 0.f);               // never lifted on a guess
}
