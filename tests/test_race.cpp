// Races in co-op (src/raceutil.h): finishing as the winner when the other player won, the place the
// race script gets for our racer, and which of our racers follows a host opponent.
#include "mwt_test.h"
#include "../src/raceutil.h"
#include <cstring>

using namespace CoopRace;

static FinishInputs Ready() {
    FinishInputs in{};
    in.sharedWins = true; in.peerWon = true; in.racing = true; in.haveLocalRacer = true;
    in.localFinished = false; in.timerRunning = true; in.restartPending = false; in.cutscene = false;
    in.raceType = 0; in.alreadyTried = false;
    return in;
}

TEST(race_finish_as_winner_when_partner_won) {
    CHECK(ShouldFinishAsWinner(Ready()));
    for (int t = 0; t <= 5; ++t) { FinishInputs in = Ready(); in.raceType = t; CHECK(ShouldFinishAsWinner(in)); }
}

TEST(race_finish_as_winner_needs_every_condition) {
    FinishInputs in;
    in = Ready(); in.sharedWins = false;     CHECK(!ShouldFinishAsWinner(in));
    in = Ready(); in.peerWon = false;        CHECK(!ShouldFinishAsWinner(in));   // they are 2nd, or still racing
    in = Ready(); in.racing = false;         CHECK(!ShouldFinishAsWinner(in));   // free roam / results
    in = Ready(); in.haveLocalRacer = false; CHECK(!ShouldFinishAsWinner(in));
    in = Ready(); in.localFinished = true;   CHECK(!ShouldFinishAsWinner(in));   // crossed the line ourselves
    in = Ready(); in.timerRunning = false;   CHECK(!ShouldFinishAsWinner(in));   // countdown: script not in "racing"
    in = Ready(); in.restartPending = true;  CHECK(!ShouldFinishAsWinner(in));
    in = Ready(); in.cutscene = true;        CHECK(!ShouldFinishAsWinner(in));
    in = Ready(); in.alreadyTried = true;    CHECK(!ShouldFinishAsWinner(in));   // once per win, never twice
}

TEST(race_finish_only_finish_line_events) {
    FinishInputs in = Ready();
    in.raceType = -1; CHECK(!ShouldFinishAsWinner(in));   // unknown
    in.raceType = 6;  CHECK(!ShouldFinishAsWinner(in));
    in.raceType = 8;  CHECK(!ShouldFinishAsWinner(in));   // pursuit challenge
    CHECK(FinishLineRaceType(4)); CHECK(!FinishLineRaceType(7));
}

TEST(race_placing_for_script) {
    CHECK_EQ(PlacingForScript(true, true, 2), 1);    // their racer crossed our line first: we still won
    CHECK_EQ(PlacingForScript(true, true, 1), 1);
    CHECK_EQ(PlacingForScript(true, false, 3), 3);   // they didn't win: the game's place
    CHECK_EQ(PlacingForScript(false, true, 2), 2);   // an AI racer keeps its place
    CHECK_EQ(PlacingForScript(false, true, 1), 1);
}

TEST(race_pick_racer_same_model_first) {
    RacerCand c[3] = { { 0x11, true }, { 0x22, true }, { 0x33, true } };
    CHECK_EQ(PickRacer(c, 3, 0x22, false), 1);
    CHECK_EQ(PickRacer(c, 3, 0x22, true), 1);         // same model wins even when any would do
    c[1].free = false;                                 // taken by another mirror / the partner's car
    CHECK_EQ(PickRacer(c, 3, 0x22, false), -1);       // waits for a moment...
    CHECK_EQ(PickRacer(c, 3, 0x22, true), 0);         // ... then takes any free racer
}

TEST(race_pick_racer_rival_of_another_model) {
    // rival race: our game built another car as the rival than the host did ("no racer of model")
    RacerCand c[1] = { { 0xAAAA0001u, true } };
    CHECK_EQ(PickRacer(c, 1, 0x79054B53u, false), -1);
    CHECK_EQ(PickRacer(c, 1, 0x79054B53u, true), 0);
    RacerCand none[1] = { { 0x79054B53u, false } };
    CHECK_EQ(PickRacer(none, 1, 0x79054B53u, true), -1);   // nothing free: nothing bound
    CHECK_EQ(PickRacer(none, 0, 0x79054B53u, true), -1);
    CHECK(ANY_MODEL_AFTER_MS > 0);
}

TEST(race_peer_ghost_is_never_a_racer_or_traffic_slot) {
    // the other player's car (ghost, or the racer built as their model) is not a bind candidate
    CHECK(MayBindAsRaceMirror(false, false, false));
    CHECK(!MayBindAsRaceMirror(true, false, false));   // local player
    CHECK(!MayBindAsRaceMirror(false, true, false));   // current ghost
    CHECK(!MayBindAsRaceMirror(false, false, true));   // factory-built peer racer
    CHECK(!MayBindAsRaceMirror(true, true, true));
    // PickRacer must not take a reserved slot even when "any model" is allowed
    RacerCand c[3] = { { 0x11, false }, { 0x22, false }, { 0x33, true } };
    CHECK_EQ(PickRacer(c, 3, 0x11, true), 2);         // reserved first two: only the free AI racer
    c[2].free = false;
    CHECK_EQ(PickRacer(c, 3, 0x11, true), -1);         // nothing left: do not steal the peer car
}

TEST(race_ghost_model_is_always_the_peers_car) {
    const uint32_t peer = 0xAABB0001u, ai = 0x79054B53u;
    CHECK(GhostModelIsPeers(peer, peer, true));
    CHECK(!GhostModelIsPeers(ai, peer, true));        // an AI racer body must not stand in
    CHECK(GhostModelIsPeers(ai, peer, false));        // peer model not known yet: don't drop
    CHECK(GhostModelIsPeers(0, peer, true));          // model unread: don't drop
    CHECK(MayAcquireGhostRacer(true, peer, peer, true));
    CHECK(!MayAcquireGhostRacer(true, ai, peer, true));
    CHECK(!MayAcquireGhostRacer(false, peer, peer, true));  // ordinary AI racer, even if the model happens to match
    CHECK(AcceptReplacedRacer(true));
    CHECK(!AcceptReplacedRacer(false));                // construct of their model failed: keep the AI opponent
}

TEST(race_boss_helper_car_list) {
    CHECK(CoopRace::BossHelperCar(6) && strcmp(CoopRace::BossHelperCar(6), "imprezawrx") == 0);
    CHECK(CoopRace::BossHelperCar(11) && strcmp(CoopRace::BossHelperCar(11), "eclipsegt") == 0);
    CHECK(CoopRace::BossHelperCar(15) == nullptr);   // no partner known: the game's pick
    CHECK(CoopRace::BossHelperCar(0) == nullptr);
    CHECK(CoopRace::BossHelperCar(16) == nullptr);
}

TEST(race_win_counts_only_when_raced_together) {
    unsigned long since = 0;
    CHECK(!CoopRace::RacedTogether(true, false, since, 1000));    // they race, we're still in the intro
    CHECK_EQ(since, 0UL);
    CHECK(!CoopRace::RacedTogether(true, true, since, 2000));     // together from now
    CHECK(!CoopRace::RacedTogether(true, true, since, 4000));
    CHECK(CoopRace::RacedTogether(true, true, since, 5001));      // 3 s together
    unsigned long s2 = 0;
    CHECK(!CoopRace::RacedTogether(true, true, s2, 100));
    CHECK(!CoopRace::RacedTogether(false, true, s2, 1500));       // they finished: no overlap any more
    CHECK_EQ(s2, 0UL);
}

TEST(race_loss_restart_quit_never_mark_a_win) {
    const int types[] = { kTypeSprint, kTypeCircuit, kTypeDrag, kTypeTollbooth, kTypeSpeedtrap };
    for (int t : types) {
        // leading when the timer expires / a DNF: 1st place, course not finished
        CHECK(!LocalObjectiveWin(1, 80.f, t));
        CHECK_EQ((int)FinishRankToReport(1, 1, 80.f, t), 2);
        // 2nd, even with the course done
        CHECK(!LocalObjectiveWin(2, 100.f, t));
        CHECK_EQ((int)FinishRankToReport(1, 2, 100.f, t), 2);
        // still racing, in the lead
        CHECK(!LocalObjectiveWin(1, 50.f, t));
        CHECK_EQ((int)FinishRankToReport(0, 1, 50.f, t), 0);
        CHECK(!CourseCompleted(80.f));
    }
    const uint32_t ev = 0x11111111u, other = 0x22222222u;
    // peer lost (finished 2nd)
    CHECK(!PeerReportedWin(ev, ev, true, 2, true, false, false));
    // peer quit (left the race, or their event hash dropped to 0)
    CHECK(!PeerReportedWin(ev, ev, false, 1, true, false, false));
    CHECK(!PeerReportedWin(ev, 0, true, 1, true, false, false));
    // restart
    CHECK(!PeerReportedWin(ev, ev, true, 1, true, false, true));
    // not a race we drove together
    CHECK(!PeerReportedWin(ev, ev, true, 1, false, false, false));
    // a win of event A must not complete event B (wrong / leftover event)
    CHECK(!PeerReportedWin(ev, other, true, 1, true, false, false));
    CHECK(!LatchedPeerWin(ev, other, false));
    CHECK(!LatchedPeerWin(other, ev, false));
    CHECK(!LatchedPeerWin(0, ev, false));
    CHECK(!LatchedPeerWin(ev, 0, false));
    // the prologue's scripted loss
    CHECK(!PeerReportedWin(ev, ev, true, 1, true, true, false));
    CHECK(!LatchedPeerWin(ev, ev, true));
}

TEST(race_win_same_event_host_and_client) {
    const uint32_t ev = 0xABCD0001u;
    // the decision is the same in both roles: host seeing the client, or client seeing the host
    for (int role = 0; role < 2; ++role) {
        (void)role;
        CHECK(PeerReportedWin(ev, ev, true, 1, true, false, false));
        CHECK(LatchedPeerWin(ev, ev, false));
        CHECK(!PeerReportedWin(ev, ev, true, 2, true, false, false));  // they finished, they lost
        FinishInputs noWin = Ready(); noWin.peerWon = false; noWin.restartPending = true;
        CHECK(!ShouldFinishAsWinner(noWin));
    }
    for (int t : { kTypeSprint, kTypeCircuit, kTypeDrag, kTypeTollbooth, kTypeSpeedtrap }) {
        CHECK(LocalObjectiveWin(1, 100.f, t));
        CHECK(LocalObjectiveWin(1, 99.f, t));
        CHECK_EQ((int)FinishRankToReport(1, 1, 100.f, t), 1);
        CHECK(!LocalObjectiveWin(1, 98.5f, t));   // just short of the line / the booth
    }
    // lap knockout: last opponent out mid-lap is still a win
    CHECK(LocalObjectiveWin(1, 40.f, kTypeKnockout));
    CHECK_EQ((int)FinishRankToReport(1, 1, 40.f, kTypeKnockout), 1);
    CHECK(!LocalObjectiveWin(2, 40.f, kTypeKnockout));
}
