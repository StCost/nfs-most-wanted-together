// Races in co-op (src/raceutil.h): finishing as the winner when the other player won, the place the
// race script gets for our racer, and which of our racers follows a host opponent.
#include "mwt_test.h"
#include "../src/raceutil.h"
#include "../src/joinutil.h"
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

// ---- Challenge Series pursuits (race type 8, 1.7.4) ----------------------------------------------
TEST(race_pursuit_challenge_win_is_reported_as_a_win) {
    // no course: the game finishes the racer when the goal is met. Before 1.7.4 this went out as 2
    // (course "unfinished") and the partner never got the win.
    CHECK(LocalObjectiveWin(1, 0.f, kTypePursuitChallenge));
    CHECK_EQ((int)FinishRankToReport(1, 1, 0.f, kTypePursuitChallenge), 1);
    // busted in this run: a loss, never a win
    CHECK(!LocalObjectiveWin(1, 0.f, kTypePursuitChallenge, true));
    CHECK_EQ((int)FinishRankToReport(1, 1, 0.f, kTypePursuitChallenge, true), 2);
    CHECK_EQ((int)FinishRankToReport(1, 2, 0.f, kTypePursuitChallenge), 2);
    CHECK_EQ((int)FinishRankToReport(0, 1, 0.f, kTypePursuitChallenge), 0);       // still going
    // "busted" only matters there: a tollbooth timeout still needs the course
    CHECK(!LocalObjectiveWin(1, 80.f, kTypeTollbooth, false));
    CHECK(LocalObjectiveWin(1, 100.f, kTypeSprint, true));
}
TEST(race_pursuit_challenge_partner_win_finishes_us_only_after_a_grace) {
    FinishInputs in = Ready();
    in.raceType = kTypePursuitChallenge;
    in.peerWonForMs = 0;      CHECK(!ShouldFinishAsWinner(in));     // the shared stats / escape get there first
    in.peerWonForMs = kPursuitChallengeFinishDelayMs - 1; CHECK(!ShouldFinishAsWinner(in));
    in.peerWonForMs = kPursuitChallengeFinishDelayMs;     CHECK(ShouldFinishAsWinner(in));
    in.alreadyTried = true;   CHECK(!ShouldFinishAsWinner(in));     // once
    in = Ready(); in.raceType = kTypePursuitChallenge; in.peerWonForMs = 60000; in.localFinished = true;
    CHECK(!ShouldFinishAsWinner(in));                                // we got there ourselves
    in = Ready(); in.raceType = 7; in.peerWonForMs = 60000;
    CHECK(!ShouldFinishAsWinner(in));                                // other non-course types: still left alone
}
TEST(race_client_traffic_in_a_pursuit_challenge_is_shared) {
    CHECK(!ClientTrafficAsRace(0, -1));                              // free roam
    CHECK(ClientTrafficAsRace(1, kTypeSprint));
    CHECK(ClientTrafficAsRace(1, kTypeTollbooth));
    CHECK(ClientTrafficAsRace(1, -1));                               // unknown type: as before
    CHECK(!ClientTrafficAsRace(1, kTypePursuitChallenge));           // host's traffic mirrored as in free roam
}

// Joint event start (src/joinutil.h)
static JoinUtil::In JoinIn(JoinUtil::Where w) { JoinUtil::In in{}; in.where = w; in.joinable = true; return in; }

TEST(join_career_event_starts_from_roaming_and_menus) {
    using namespace JoinUtil;
    CHECK(Decide(JoinIn(Where::Roaming)) == Step::Start);
    CHECK(Decide(JoinIn(Where::FrontEnd)) == Step::Start);       // main menu / career menus: LoadTrack
    In in = JoinIn(Where::Roaming); in.menuOpen = true;
    CHECK(Decide(in) == Step::CloseMenusFirst);                   // world map, pause, SMS: backed out first
}

TEST(join_challenge_or_quick_race_from_roaming_quits_to_the_front_end) {
    using namespace JoinUtil;
    In in = JoinIn(Where::Roaming); in.frontEndOnly = true;
    CHECK(Decide(in) == Step::QuitToFrontEndFirst);
    in.menuOpen = true;  CHECK(Decide(in) == Step::CloseMenusFirst);   // menus first, then the quit
    in = JoinIn(Where::FrontEnd); in.frontEndOnly = true;
    CHECK(Decide(in) == Step::Start);
}

TEST(join_never_yanks_a_partner_out_of_another_event) {
    using namespace JoinUtil;
    CHECK(Decide(JoinIn(Where::InEvent)) == Step::Busy);
    In in = JoinIn(Where::InEvent); in.frontEndOnly = true;
    CHECK(Decide(in) == Step::Busy);
    CHECK(Decide(JoinIn(Where::Results)) == Step::LeaveResultsFirst);   // their race is over: Continue
    CHECK(Decide(JoinIn(Where::Loading)) == Step::Wait);
    in = JoinIn(Where::InEvent); in.sameEvent = true;
    CHECK(Decide(in) == Step::AlreadyThere);
    in = JoinIn(Where::Roaming); in.joinable = false;
    CHECK(Decide(in) == Step::Refuse);
}

TEST(join_pending_invite_lives_until_joined_timed_out_or_partner_left) {
    using namespace JoinUtil;
    Pending p; p.hash = 0xABCD; p.since = 1000;
    CHECK(KeepPending(p, 2000, 0xABCD, true, false));
    CHECK(!KeepPending(p, 2000, 0xABCD, true, true));                       // we're in it
    CHECK(!KeepPending(p, 1000 + PENDING_MS + 1, 0xABCD, true, false));     // too old
    CHECK(KeepPending(p, 5000, 0, true, false));                            // they may still be loading
    CHECK(!KeepPending(p, 1000 + PEER_LEFT_GRACE_MS + 1, 0, true, false));  // they quit it
    CHECK(KeepPending(p, 1000 + PEER_LEFT_GRACE_MS + 1, 0, false, false));  // between screens: keep
    Pending none; CHECK(!KeepPending(none, 2000, 0, true, false));
}

TEST(join_sender_sees_a_busy_partner) {
    using namespace JoinUtil;
    CHECK(PeerBusyElsewhere(1, 2, true, false));
    CHECK(!PeerBusyElsewhere(1, 2, true, true));    // on their results: they'll Continue and join
    CHECK(!PeerBusyElsewhere(1, 1, true, false));   // in ours
    CHECK(!PeerBusyElsewhere(1, 0, false, false));  // roaming / menus
    CHECK(!PeerBusyElsewhere(0, 2, true, false));
}

TEST(join_context_travels_and_old_builds_keep_the_old_rule) {
    using namespace JoinUtil;
    for (int c = 0; c <= 3; ++c) CHECK(ContextFromWire(ContextToWire(c)) == c);
    CHECK(ContextToWire(-1) == 0); CHECK(ContextFromWire(0) == -1);
    CHECK(LoadContext(0, false) == 0);    // quick race
    CHECK(LoadContext(2, false) == 2);
    CHECK(LoadContext(-1, true) == 0);    // older sender: Challenge Series
    CHECK(LoadContext(-1, false) == 2);
    CHECK(InviteContext(0) && InviteContext(2) && !InviteContext(1));
}
