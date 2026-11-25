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

TEST(race_pick_racer_duplicate_models_by_slot_and_role) {
    // Big Lou 11.1.2: the rival and the helper are both Eclipse GTs (A2D70BBD). Slot 0 is the local
    // player (not a candidate), 1 the rival, 2 the helper, 3 the other player's car (reserved).
    const uint32_t ecl = 0xA2D70BBDu;
    RacerCand c[3] = { { ecl, true, 2, kRoleHelper }, { ecl, true, 1, kRoleRival }, { 0x5A784095u, false, 3, kRoleUnknown } };
    RacerWant rival{ ecl, 1, kRoleRival }, helper{ ecl, 2, kRoleHelper };
    CHECK_EQ(PickRacer(c, 3, rival, false), 1);       // not the first Eclipse in the list (the helper)
    CHECK_EQ(PickRacer(c, 3, helper, false), 0);
    // a 1.7.9 host sends no slot: the role from our side still keeps a role-less want on the same model
    RacerWant old{ ecl };
    CHECK_EQ(PickRacer(c, 3, old, false), 0);         // model only: the first free one (old behaviour)
    // roles known, slot differs (one game had no helper): role + model wins over a bare slot
    RacerWant rivalOtherSlot{ ecl, 5, kRoleRival };
    CHECK_EQ(PickRacer(c, 3, rivalOtherSlot, false), 1);
    // the slot matches but our racer there has another role: not that one
    RacerWant helperInRivalSlot{ ecl, 1, kRoleHelper };
    CHECK_EQ(PickRacer(c, 3, helperInRivalSlot, false), 0);
    // helper of another model here (no roster): the helper role binds at once, never the rival
    RacerCand d[2] = { { ecl, true, 1, kRoleRival }, { 0x11111111u, true, 2, kRoleHelper } };
    CHECK_EQ(PickRacer(d, 2, helper, false), 1);
    d[1].free = false;                                 // our helper is taken: wait, then...
    CHECK_EQ(PickRacer(d, 2, helper, false), -1);
    CHECK_EQ(PickRacer(d, 2, helper, true), 0);        // ... any free racer rather than none
    // normal races: the same slot even when our game built another model there
    RacerCand e[2] = { { 0x22, true, 2, kRoleNormal }, { 0x33, true, 3, kRoleNormal } };
    RacerWant n3{ 0x22, 3, kRoleNormal };
    CHECK_EQ(PickRacer(e, 2, n3, false), 1);
    RacerWant nNoSlot{ 0x33, -1, kRoleNormal };
    CHECK_EQ(PickRacer(e, 2, nNoSlot, false), 1);
}

TEST(race_racer_role_of_slot) {
    CHECK_EQ(RacerRoleOf(1, true, 2, 3), kRoleRival);
    CHECK_EQ(RacerRoleOf(2, true, 2, 3), kRoleHelper);
    CHECK_EQ(RacerRoleOf(3, true, 2, 3), kRoleUnknown);   // the other player's slot
    CHECK_EQ(RacerRoleOf(1, true, -1, -1), kRoleRival);  // no helper
    CHECK_EQ(RacerRoleOf(4, false, -1, 1), kRoleNormal);
    CHECK_EQ(RacerRoleOf(-1, true, 2, 3), kRoleUnknown);
}

TEST(race_racer_slot_on_the_wire) {
    int slot = 0, role = 0;
    uint16_t f = EncodeRacerSlot(2 | 8, 2, kRoleHelper);
    CHECK((f & 0xFF) == (2 | 8));                      // the old bits stay as they were
    CHECK(DecodeRacerSlot(f, slot, role)); CHECK_EQ(slot, 2); CHECK_EQ(role, kRoleHelper);
    f = EncodeRacerSlot(2, 15, kRoleRival);
    CHECK(DecodeRacerSlot(f, slot, role)); CHECK_EQ(slot, 15); CHECK_EQ(role, kRoleRival);
    f = EncodeRacerSlot(2, 0, kRoleNormal);
    CHECK(DecodeRacerSlot(f, slot, role)); CHECK_EQ(slot, 0); CHECK_EQ(role, kRoleNormal);
    f = EncodeRacerSlot(2, 4, kRoleUnknown);
    CHECK(DecodeRacerSlot(f, slot, role)); CHECK_EQ(slot, 4); CHECK_EQ(role, kRoleUnknown);
    CHECK_EQ(EncodeRacerSlot(2, -1, kRoleRival), 2);   // no slot: nothing added
    CHECK_EQ(EncodeRacerSlot(2, 16, kRoleRival), 2);
    CHECK(!DecodeRacerSlot(2 | 8, slot, role)); CHECK_EQ(slot, -1); CHECK_EQ(role, kRoleUnknown);   // 1.7.9 host
    CHECK(!DecodeRacerSlot(EncodeRacerSlot(2, 3, kRoleRival) & ~2, slot, role));   // not a racer: ignored
    CHECK_EQ(EncodeRacerSlot(0xFF02, 3, kRoleRival) & 0xFF, 0x02);   // stale high bits are replaced
}

TEST(race_roster_role_byte) {
    CHECK_EQ(DecodeRosterRole(0), kRoleUnknown);       // 1.7.9 and older: padding, always 0
    for (int r = kRoleNormal; r <= kRoleHelper; ++r) CHECK_EQ(DecodeRosterRole(EncodeRosterRole(r)), r);
    CHECK_EQ(EncodeRosterRole(kRoleUnknown), 0);
    CHECK_EQ(EncodeRosterRole(7), 0);
    CHECK_EQ(DecodeRosterRole(0x80 | 5), kRoleUnknown);  // a role we don't know
    CHECK_EQ(DecodeRosterRole(0x02), kRoleUnknown);      // no "present" bit
    int marked[3] = { kRoleRival, kRoleHelper, kRoleNormal };
    CHECK_EQ(RosterHelperEntry(marked, 3, 1), 1);
    int marked2[2] = { kRoleHelper, kRoleRival };
    CHECK_EQ(RosterHelperEntry(marked2, 2, 1), 0);       // the marked entry, not the index
    int noHelper[1] = { kRoleRival };
    CHECK_EQ(RosterHelperEntry(noHelper, 1, 1), -1);     // marked roster without a helper: none
    int old[2] = { kRoleUnknown, kRoleUnknown };
    CHECK_EQ(RosterHelperEntry(old, 2, 1), 1);           // 1.7.9 sender: by opponent index
    CHECK_EQ(RosterHelperEntry(old, 1, 1), -1);
    CHECK_EQ(RosterHelperEntry(old, 0, 0), -1);
}

TEST(race_helper_always_gets_a_parts_record) {
    const uint32_t ecl = 0xA2D70BBDu, pick = 0x12345678u, wrx = 0x0BADF00Du;
    // Big Lou: the game picked another car, the rival is an Eclipse with its record: the rival's parts
    HelperIn in{ ecl, pick, ecl, true, 0, false };
    HelperOut o = ChooseHelperCar(in);
    CHECK_EQ(o.model, ecl); CHECK_EQ(o.record, kHelperRivalRecord);
    // the game already picked the list model: its own record
    in = HelperIn{ ecl, ecl, ecl, true, 0, false };
    o = ChooseHelperCar(in); CHECK_EQ(o.model, ecl); CHECK_EQ(o.record, kHelperGameRecord);
    // Ming: the list model is not the rival's: no record for it here - the game's pick stays
    in = HelperIn{ wrx, pick, ecl, true, 0, false };
    o = ChooseHelperCar(in); CHECK_EQ(o.model, pick); CHECK_EQ(o.record, kHelperGameRecord);
    // rival without a record: never build the list model bare
    in = HelperIn{ ecl, pick, ecl, false, 0, false };
    o = ChooseHelperCar(in); CHECK_EQ(o.model, pick); CHECK_EQ(o.record, kHelperGameRecord);
    // no list model for this rival: the game's pick
    in = HelperIn{ 0, pick, ecl, true, 0, false };
    o = ChooseHelperCar(in); CHECK_EQ(o.model, pick); CHECK_EQ(o.record, kHelperGameRecord);
    // joining: the other game's helper with its record wins
    in = HelperIn{ ecl, pick, ecl, true, wrx, true };
    o = ChooseHelperCar(in); CHECK_EQ(o.model, wrx); CHECK_EQ(o.record, kHelperPeerRecord);
    // a bare entry from a 1.7.9 game (the bug): its model with the rival's record when that fits ...
    in = HelperIn{ 0, pick, ecl, true, ecl, false };
    o = ChooseHelperCar(in); CHECK_EQ(o.model, ecl); CHECK_EQ(o.record, kHelperRivalRecord);
    // ... the game's own record when the game picked that model ...
    in = HelperIn{ 0, ecl, ecl, true, ecl, false };
    o = ChooseHelperCar(in); CHECK_EQ(o.model, ecl); CHECK_EQ(o.record, kHelperGameRecord);
    // ... else the game's pick, never a bare car
    in = HelperIn{ ecl, pick, ecl, true, wrx, false };
    o = ChooseHelperCar(in); CHECK_EQ(o.model, pick); CHECK_EQ(o.record, kHelperGameRecord);
}

TEST(race_join_waits_for_the_roster_in_rival_races) {
    CHECK(WaitForRoster(true, true, false, 0));
    CHECK(WaitForRoster(true, true, false, kRosterWaitMs - 1));
    CHECK(!WaitForRoster(true, true, false, kRosterWaitMs));   // not forever: our own helper then
    CHECK(!WaitForRoster(true, true, true, 0));                // it's here
    CHECK(!WaitForRoster(false, true, false, 0));              // not a rival race
    CHECK(!WaitForRoster(true, false, false, 0));              // no helper
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

TEST(race_lose_screen_gives_way_to_the_results_list) {
    // (koHeld, peerWon, racing, localOut, localWon, doneThisRun, pauseUp, resultsUp, photoUp)
    CHECK(OutOfRaceWin(false, true, true, true, false, false, true, false, false) == OutWin::ViaPauseScreen);
    CHECK(OutOfRaceWin(true, true, true, true, false, false, true, false, false) == OutWin::None);    // knockout hold: its own release
    CHECK(OutOfRaceWin(false, false, true, true, false, false, true, false, false) == OutWin::None);  // they haven't won
    CHECK(OutOfRaceWin(false, true, true, true, false, false, false, false, false) == OutWin::None);  // "race over" message / left
    CHECK(OutOfRaceWin(false, true, true, false, false, false, true, false, false) == OutWin::None);  // we still race (a plain pause)
    CHECK(OutOfRaceWin(false, true, true, true, true, false, true, false, false) == OutWin::None);    // we won ourselves
    CHECK(OutOfRaceWin(false, true, true, true, false, true, true, false, false) == OutWin::None);    // once per run
}

TEST(race_out_player_gets_the_team_win) {
    // totaled / knocked out with the losing results list up, then they win: the list closes for the winner's photo
    CHECK(OutOfRaceWin(false, true, true, true, false, false, false, true, false) == OutWin::ViaResults);
    CHECK(OutOfRaceWin(false, true, true, true, false, false, false, true, true) == OutWin::None);    // the photo, not ours
    CHECK(OutOfRaceWin(false, true, true, true, false, true, false, true, false) == OutWin::None);    // already counted
    // quit / continued off the losing screen before they won: the race is over here - not counted
    CHECK(OutOfRaceWin(false, true, false, true, false, false, false, false, false) == OutWin::None);
    CHECK(OutOfRaceWin(false, true, false, true, false, false, false, true, false) == OutWin::None);
    CHECK(OutOfRaceWin(false, true, false, true, false, false, true, false, false) == OutWin::None);
    // they won while "race over" was shown: the winner's photo instead of the losing screen
    CHECK(CreditBeforeLoseScreen(true, true, true, false, false));
    CHECK(!CreditBeforeLoseScreen(false, true, true, false, false));  // they haven't won (yet)
    CHECK(!CreditBeforeLoseScreen(true, false, true, false, false));  // the race is over here
    CHECK(!CreditBeforeLoseScreen(true, true, false, false, false));  // we still race
    CHECK(!CreditBeforeLoseScreen(true, true, true, true, false));    // we won ourselves
    CHECK(!CreditBeforeLoseScreen(true, true, true, false, true));    // already counted
}

// ---- start together at the loading screen (1.7.5) ------------------------------------------------
static LoadHoldInputs Loading() {
    LoadHoldInputs in{};
    in.enabled = true; in.event = 0x1234; in.peerValid = true; in.peerComing = true;
    in.peerEvent = 0x1234; in.peerFlow = 5; in.peerStage = 1;
    return in;
}
TEST(race_load_hold_waits_for_a_partner_still_loading) {
    CHECK(HoldLoadingScreen(Loading()));
    LoadHoldInputs in = Loading(); in.peerEvent = 0; CHECK(HoldLoadingScreen(in));   // invited, not there yet
}
TEST(race_load_hold_goes_once_they_are_loaded) {
    LoadHoldInputs in = Loading(); in.peerStage = kStageLoadHeld; CHECK(!HoldLoadingScreen(in));   // both held: go
    in = Loading(); in.peerFlow = 6; CHECK(!HoldLoadingScreen(in));                  // already in the intro
    in = Loading(); in.peerFlow = 6; in.peerStage = 2; CHECK(!HoldLoadingScreen(in));  // on the grid
    in = Loading(); in.peerStage = kStageLoadHeld; in.peerEvent = 0x999; in.peerComing = true;
    CHECK(HoldLoadingScreen(in));                                                     // held in another event: not ours
}
TEST(race_load_hold_never_without_reason) {
    LoadHoldInputs in = Loading(); in.enabled = false; CHECK(!HoldLoadingScreen(in));
    in = Loading(); in.event = 0;        CHECK(!HoldLoadingScreen(in));   // free roam loading
    in = Loading(); in.prologue = true;  CHECK(!HoldLoadingScreen(in));
    in = Loading(); in.peerValid = false; CHECK(!HoldLoadingScreen(in));
    in = Loading(); in.peerComing = false; CHECK(!HoldLoadingScreen(in));
    in = Loading(); in.heldMs = kLoadHoldMaxMs; CHECK(!HoldLoadingScreen(in));
    in = Loading(); in.peerElsewhere = true; in.heldMs = 1000; CHECK(HoldLoadingScreen(in));
    in.heldMs = kLoadHoldElsewhereMs; CHECK(!HoldLoadingScreen(in));
}
TEST(race_grid_stage_is_2_or_3_only) {
    CHECK(!OnGridStage(0) && !OnGridStage(1) && OnGridStage(2) && OnGridStage(3) && !OnGridStage(kStageLoadHeld));
}

// ---- the start hold waits while the other player still loads (1.7.14) ---------------------------
TEST(race_start_hold_peer_loading_states) {
    const uint32_t ev = 0xD9464514, other = 0xFE9D9378;
    CHECK(PeerLoadingToStart(ev, other, 7, 0));             // unloading the last race's world (results left)
    CHECK(PeerLoadingToStart(ev, 0, 8, 0));
    CHECK(PeerLoadingToStart(ev, 0, 4, 0));                 // loading this one
    CHECK(PeerLoadingToStart(ev, ev, 5, 0));
    CHECK(PeerLoadingToStart(ev, ev, 6, 1));                // in this event, intro cutscene
    CHECK(PeerLoadingToStart(ev, ev, 6, kStageLoadHeld));   // holding their loading screen
    CHECK(!PeerLoadingToStart(ev, ev, 6, 2));               // on the grid: ready, not loading
    CHECK(!PeerLoadingToStart(ev, ev, 6, 3));
    CHECK(!PeerLoadingToStart(ev, other, 6, 1));            // driving in another event / its results
    CHECK(!PeerLoadingToStart(ev, 0, 6, 0));                // free roam
    CHECK(!PeerLoadingToStart(ev, 0, 3, 0));                // the menus: not on the way
    CHECK(!PeerLoadingToStart(ev, 0, 0, 0));
    CHECK(!PeerLoadingToStart(0, 0, 6, 1));
}
TEST(race_start_hold_waits_for_a_full_world_load) {
    // the client holds on the grid; the host leaves the last race's results through a full load (flow 7, 8,
    // 4, 5), then its intro: 1 s steps, last seen loading at 30 s - no go at 20 s (the old limit)
    int lastLoading = 0;
    for (int t = 0; t <= 30000; t += 1000) {
        lastLoading = t;                         // still loading the whole time
        CHECK(!StartHoldTimedOut(t, t - lastLoading));
    }
    // done loading but never on the grid: the usual 20 s from then
    CHECK(!StartHoldTimedOut(30000 + kStartHoldMs - 1, kStartHoldMs - 1));
    CHECK(StartHoldTimedOut(30000 + kStartHoldMs, kStartHoldMs));
}
TEST(race_start_hold_limits) {
    CHECK(!StartHoldTimedOut(0, 0));
    CHECK(!StartHoldTimedOut(kStartHoldMs - 1, kStartHoldMs - 1));   // a slower load, not loading any more
    CHECK(StartHoldTimedOut(kStartHoldMs, kStartHoldMs));            // the old 20 s when they are not loading
    CHECK(!StartHoldTimedOut(kStartHoldLoadingMaxMs - 1, 0));         // loading for nearly 2 minutes
    CHECK(StartHoldTimedOut(kStartHoldLoadingMaxMs, 0));              // never longer than 2 minutes
}

// ---- a grid slot for the other player (1.7.5) ----------------------------------------------------
TEST(race_grid_slot_nothing_around_is_behind_us) {
    float a = 0, s = 0;
    CHECK(GridSlotFor(nullptr, 0, a, s));
    CHECK(a == -kGridDefaultRow && s == 0.f);
}
TEST(race_grid_slot_skips_a_rival_behind_us) {
    // a 2-wide grid: a car beside us (4.5 m right), one straight behind (7 m), one behind-right
    GridCar cars[] = { { 0.f, 4.5f }, { -7.f, 0.f }, { -7.f, 4.5f } };
    float a = 0, s = 0;
    CHECK(GridSlotFor(cars, 3, a, s));
    CHECK(fabsf(a + 7.f) < 0.01f && fabsf(s + 4.5f) < 0.01f);    // behind-left: the free one of that row
}
TEST(race_grid_slot_reads_the_row_spacing) {
    GridCar cars[] = { { 9.f, 0.f } };          // the car ahead of us, 9 m: rows are 9 m apart
    float a = 0, s = 0;
    CHECK(GridSlotFor(cars, 1, a, s));
    CHECK(fabsf(a + 9.f) < 0.01f && s == 0.f);
}
TEST(race_grid_slot_no_side_slots_without_a_wide_grid) {
    GridCar cars[] = { { -6.f, 0.f } };         // single file (a tollbooth / a narrow start): only behind
    float a = 0, s = 0;
    CHECK(GridSlotFor(cars, 1, a, s));
    CHECK(fabsf(a + 12.f) < 0.01f && s == 0.f); // two rows back, not beside the road
}
TEST(race_grid_slot_staggered_grid) {
    GridCar cars[] = { { 5.f, 4.f }, { -5.f, 4.f } };   // diagonal neighbours only
    float a = 0, s = 0;
    CHECK(GridSlotFor(cars, 2, a, s));
    CHECK(fabsf(a + 5.f) < 0.01f && s == 0.f);           // straight behind, one row (5 m) back, is free
}
TEST(race_grid_slot_all_taken_falls_back) {
    GridCar cars[] = { { 0.f, 4.f }, { 0.f, -4.f }, { -6.f, 0.f }, { -6.f, 4.f }, { -6.f, -4.f }, { -12.f, 0.f }, { -12.f, 4.f } };
    float a = 0, s = 0;
    CHECK(!GridSlotFor(cars, 7, a, s));
    CHECK(a == -kGridDefaultRow && s == 0.f);
}

// ---- a shared win shows the winner's photo, naming who won ---------------------------------------
TEST(race_team_win_shows_the_photo) {
    CHECK(TeamWinPhoto(0x1234, 0x1234));      // our win came from the other player: the photo names them
    CHECK(!TeamWinPhoto(0x1234, 0));          // we won ourselves: the photo as the game shows it
    CHECK(!TeamWinPhoto(0x1234, 0x999));      // their win in an earlier event
    CHECK(!TeamWinPhoto(0, 0));
}
TEST(race_team_photo_after_out_ends_like_the_results_list) {
    // (msg, event, outWinEvent, racePhoto, career, photoReady)
    CHECK(TeamPhotoAction(kFeAccept, 0x1234, 0x1234, true, true, true) == kPhotoEndOut);
    CHECK(TeamPhotoAction(kFeAccept, 0x1234, 0, true, true, true) == kPhotoAsIs);         // "postrace": the photo's own
    CHECK(TeamPhotoAction(kFeAccept, 0x1234, 0x999, true, true, true) == kPhotoAsIs);     // another event's win
    CHECK(TeamPhotoAction(kFeAccept, 0, 0, true, true, true) == kPhotoAsIs);
    CHECK(TeamPhotoAction(0xE1A57D51u, 0x1234, 0x1234, true, true, true) == kPhotoAsIs);  // Restart: as the game does
    CHECK(TeamPhotoAction(kFeExitComplete, 0x1234, 0x1234, true, true, true) == kPhotoAsIs);
    CHECK(TeamPhotoAction(kFeAccept, 0x1234, 0x1234, false, true, true) == kPhotoAsIs);   // a speedtrap / pursuit photo
    CHECK(TeamPhotoAction(kFeAccept, 0x1234, 0x1234, true, false, true) == kPhotoAsIs);   // quick race: quits to the menu
    CHECK(TeamPhotoAction(kFeAccept, 0x1234, 0x1234, true, true, false) == kPhotoAsIs);   // pictures still coming in
}
TEST(race_team_results_list) {
    // (msg, event, teamWinEvent, swapEvent, resultsUp)
    CHECK(TeamResultsAction(kFeAccept, 0x1234, 0x1234, 0, true) == kResultsNoDialog);       // shared win: no "you did not win"
    CHECK(TeamResultsAction(kFeExitComplete, 0x1234, 0x1234, 0, true) == kResultsAsIs);     // its exit as the game does
    CHECK(TeamResultsAction(0xE1A57D51u, 0x1234, 0x1234, 0, true) == kResultsAsIs);         // Restart
    CHECK(TeamResultsAction(kFeAccept, 0x1234, 0, 0, true) == kResultsAsIs);                // a lost race
    CHECK(TeamResultsAction(kFeAccept, 0x1234, 0x999, 0, true) == kResultsAsIs);
    CHECK(TeamResultsAction(kFeAccept, 0x1234, 0x1234, 0, false) == kResultsAsIs);
    CHECK(TeamResultsAction(kFeAccept, 0, 0, 0, true) == kResultsAsIs);
    // the losing list closing for the winner's photo: it must not end the race on its way out
    CHECK(TeamResultsAction(kFeExitComplete, 0x1234, 0, 0x1234, true) == kResultsHold);
    CHECK(TeamResultsAction(kFeAccept, 0x1234, 0, 0x1234, true) == kResultsHold);
    CHECK(TeamResultsAction(kFeExitComplete, 0x1234, 0, 0x999, true) == kResultsAsIs);
    CHECK(TeamResultsAction(0xE1A57D51u, 0x1234, 0, 0x1234, true) == kResultsAsIs);
}

TEST(race_host_racers_only_in_our_event) {
    CHECK(HostRacersAreOurs(true, 0xD9464514u, 0xD9464514u));
    CHECK(!HostRacersAreOurs(true, 0xFE9D9378u, 0xD9464514u));   // host still on the last race's results
    CHECK(!HostRacersAreOurs(true, 0u, 0xD9464514u));            // host back in free roam
    CHECK(!HostRacersAreOurs(true, 0xD9464514u, 0u));            // we left the race
    CHECK(HostRacersAreOurs(false, 0u, 0xD9464514u));            // no state from the host: leave as is
}

TEST(race_knockout_wait_for_partner_result) {
    using namespace CoopRace;
    CHECK(KnockoutRelease(true, false, true, 0, false) == KoRelease::Hold);          // they still race
    CHECK(KnockoutRelease(false, true, true, 0, false) == KoRelease::Won);           // they won
    CHECK(KnockoutRelease(true, true, true, 0, false) == KoRelease::Won);
    CHECK(KnockoutRelease(false, false, true, 0, false) == KoRelease::Hold);         // stopped: their result may lag
    CHECK(KnockoutRelease(false, false, true, kKoResultWaitMs - 1, false) == KoRelease::Hold);
    CHECK(KnockoutRelease(false, false, true, kKoResultWaitMs, false) == KoRelease::Lost);
    CHECK(KnockoutRelease(false, false, false, 0, false) == KoRelease::Lost);        // they left the event
    CHECK(KnockoutRelease(true, false, true, 0, true) == KoRelease::Lost);           // 15 min: give up
}

TEST(race_out_is_never_reported_as_a_win) {
    using namespace CoopRace;
    CHECK_EQ((int)FinishRankAfterOut(1, true), 2);
    CHECK_EQ((int)FinishRankAfterOut(1, false), 1);
    CHECK_EQ((int)FinishRankAfterOut(3, true), 3);
    CHECK_EQ((int)FinishRankAfterOut(0, true), 0);
}
