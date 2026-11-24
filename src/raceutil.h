#pragma once
// Races in co-op: the decisions that don't need the game, so the test suite covers them
// (tests\test_race.cpp). coopwin.inl and traffic.inl read the game, ask these, and do what they say.
#include <cstdint>
#include <cmath>

namespace CoopRace {
    constexpr int kTypeSprint = 0, kTypeCircuit = 1, kTypeDrag = 2, kTypeKnockout = 3,
                  kTypeTollbooth = 4, kTypeSpeedtrap = 5, kTypePursuitChallenge = 8;
    // GRaceParameters race type (0x5FAA20): 0 sprint, 1 circuit, 2 drag, 3 lap knockout, 4 tollbooth,
    // 5 speedtrap. These end at the finish line through the basic race script; the others (pursuit
    // challenges, ...) end some other way and are left alone.
    inline bool FinishLineRaceType(int type) { return type >= 0 && type <= 5; }

    // The other player crossed the line first: finish our racer as the winner right away, the way the
    // race script finishes a racer at the finish line (see coopwin.inl).
    struct FinishInputs {
        bool sharedWins;      // [Career] SharedWins
        bool peerWon;         // they finished 1st in this run of the event we are racing
        bool racing;          // GRaceStatus play mode 1 (racing)
        bool haveLocalRacer;  // our GRacerInfo exists
        bool localFinished;   // ... and already finished (by itself or by us)
        bool timerRunning;    // our race clock runs: the countdown is over (the race script is in "racing")
        bool restartPending;  // a restart is on its way
        bool cutscene;        // a cutscene plays over the race
        int  raceType;        // GRaceParameters type
        bool alreadyTried;    // we finished it once for this win (never twice)
        int  peerWonForMs;    // how long ago their win came in
    };
    // Pursuit challenge: the shared counters and the shared escape normally complete our goal on
    // their own within a second; only if they haven't after this long is our racer finished for us.
    constexpr int kPursuitChallengeFinishDelayMs = 8000;
    inline bool ShouldFinishAsWinner(const FinishInputs& in) {
        bool typeOk = FinishLineRaceType(in.raceType) ||
                      (in.raceType == kTypePursuitChallenge && in.peerWonForMs >= kPursuitChallengeFinishDelayMs);
        return in.sharedWins && in.peerWon && in.racing && in.haveLocalRacer && !in.localFinished &&
               in.timerRunning && !in.restartPending && !in.cutscene && typeOk && !in.alreadyTried;
    }

    // The race script asks CalculateRanking for a racer's place when it crosses the line and passes the
    // answer on (NotifyRacePlacement: 1 = the winning flow, else the losing one). Our own racer's place
    // is 1 once the other player has won this event; everyone else keeps the game's answer.
    inline int PlacingForScript(bool isLocalRacer, bool peerWon, int gamePlacing) {
        return (isLocalRacer && peerWon) ? 1 : gamePlacing;
    }

    // Racer mirrors (client): each opponent of the host drives one of our own AI racers. The same model
    // first; when none of that model is free for a while (our game built another car for that opponent,
    // e.g. a different rival), any free racer, so nobody races on their own AI next to the host's.
    constexpr double ANY_MODEL_AFTER_MS = 1500.0;
    struct RacerCand { uint32_t model; bool free; };
    inline int PickRacer(const RacerCand* c, int n, uint32_t model, bool anyModel) {
        for (int i = 0; i < n; ++i) if (c[i].free && c[i].model == model) return i;
        if (anyModel) for (int i = 0; i < n; ++i) if (c[i].free) return i;
        return -1;
    }

    // The other player's car must never be borrowed as a racer or traffic mirror (same class of
    // mix-up as a racer also listed as traffic): that swapped the ghost onto an AI racer and showed
    // the wrong model at their position, while the standings still named them correctly.
    inline bool MayBindAsRaceMirror(bool isLocalPlayer, bool isGhost, bool isPeerRacer) {
        return !isLocalPlayer && !isGhost && !isPeerRacer;
    }
    // A ghost may only be the factory-built peer racer, or a spawned copy, of THEIR model.
    // An AI racer body of another model must not stand in for them (not even to keep a standings slot).
    inline bool GhostModelIsPeers(uint32_t vehicleModel, uint32_t peerModel, bool peerModelKnown) {
        if (!peerModelKnown || peerModel == 0 || vehicleModel == 0) return true;
        return vehicleModel == peerModel;
    }
    inline bool MayAcquireGhostRacer(bool isFactoryPeerRacer, uint32_t vehicleModel, uint32_t peerModel,
                                     bool peerModelKnown) {
        return isFactoryPeerRacer && GhostModelIsPeers(vehicleModel, peerModel, peerModelKnown);
    }
    // If constructing their model as a racer failed, do not keep the original AI body as "their" racer.
    inline bool AcceptReplacedRacer(bool builtAsPeerModel) { return builtAsPeerModel; }

    // Rival races together: the helper's car for Blacklist rival #bin - a car the rival's race intro shows
    // besides the rival's own; null = none: the game's own pick. Seen by starting each rival's first race
    // and logging the cars its intro cutscene builds: all others build only traffic / police props
    // (traf*, copmidsize_nis, log trucks) or nothing.
    inline const char* BossHelperCar(int bin) {
        switch (bin) {
        case 6:  return "imprezawrx";   // Ming (6.1.1: imprezawrx, eclipsegt)
        case 10: return "eclipsegt";    // 10.2.1
        case 11: return "eclipsegt";    // Big Lou (11.1.2)
        default: return nullptr;
        }
    }

    // The other player's win counts for us only in a race we drove together: they were racing (not finished)
    // while our own race was running (clock on, not finished) for RacedTogetherMs. `since` keeps the start
    // of the current overlap (0 = none). True once it has lasted long enough.
    constexpr unsigned long RacedTogetherMs = 3000;
    inline bool RacedTogether(bool peerRacing, bool weRacing, unsigned long& since, unsigned long now) {
        if (!peerRacing || !weRacing) { since = 0; return false; }
        if (!since) { since = now ? now : 1; return false; }
        return now - since >= RacedTogetherMs;
    }

    // ---- when a finish is actually a win (career "event won") ----
    // CarState::finished is the sender's rank at the finish (0 = still racing). Only 1 means they won.
    // Rank 1 is not enough on its own: a Tollbooth loss (timer expired) leaves the leader in 1st
    // with the course unfinished. Reporting that as 1 made the host's career mark the race
    // COMPLETED after a shared loss; the client, often 2nd, was left un-marked.
    // Losing, restarting or quitting must never count as a win, and a win of event A must never
    // complete event B.
    constexpr float kCourseCompletePct = 99.f;   // GRacerInfo +0x18 is 0-100

    inline bool CourseCompleted(float racePct) { return racePct >= kCourseCompletePct; }

    // The client's traffic in an event: a race keeps its own traffic (race scripts own some of it, or
    // SharedInRaces binds it to the host's); a pursuit challenge is open-world driving with cops and no
    // opponents, so the host's traffic is mirrored there as in free roam (1.7.4).
    inline bool ClientTrafficAsRace(int playMode, int raceType) {
        return playMode == 1 && raceType != kTypePursuitChallenge;
    }
    // Lap knockout can be won mid-lap when the last opponent is eliminated. Every other finish-line
    // type (and an unknown type) needs the course actually completed; rank 1 at a timeout is a loss.
    // A Challenge Series pursuit (type 8) has no course: the game finishes the racer when the goal is
    // met (cost to state, bounty, cops immobilized, escape ...), so rank 1 there is the win unless we
    // were busted in this run (1.7.4: these wins were reported as 2 and never reached the partner).
    inline bool RankOneNeedsCourseComplete(int raceType) { return raceType != kTypeKnockout && raceType != kTypePursuitChallenge; }

    inline bool LocalObjectiveWin(int rank, float racePct, int raceType, bool pursuitFailed = false) {
        if (rank != 1) return false;
        if (raceType == kTypePursuitChallenge && pursuitFailed) return false;
        if (RankOneNeedsCourseComplete(raceType)) return CourseCompleted(racePct);
        return true;
    }

    // What CarState::finished should carry once GRacerInfo +0x21 is set.
    // 0 = still racing (flag not set). 1 = won. 2+ = finished and did not win (their rank, or 2
    // when they were leading but did not complete the course — a Tollbooth timeout).
    inline uint8_t FinishRankToReport(uint8_t finishedFlag, int rank, float racePct, int raceType, bool pursuitFailed = false) {
        if (!finishedFlag) return 0;
        if (rank < 1 || rank > 255) return 255;   // unreadable: never a win
        if (rank == 1 && !LocalObjectiveWin(rank, racePct, raceType, pursuitFailed)) return 2;
        return (uint8_t)rank;
    }

    // The other player's reported finish is a shared win only for this same event, after we raced
    // it together, and only if they reported rank 1 (a real win, not a loss / quit leftover).
    inline bool PeerReportedWin(uint32_t localEvent, uint32_t peerEvent, bool peerInRace,
                                uint8_t peerFinished, bool racedTogether, bool scriptedLoss,
                                bool restartPending) {
        if (scriptedLoss || restartPending) return false;
        if (!localEvent || !peerEvent || localEvent != peerEvent) return false;
        if (!peerInRace || !racedTogether) return false;
        return peerFinished == 1;
    }

    // A remembered peer win still applies only to the event we are in now, never a previous one.
    inline bool LatchedPeerWin(uint32_t localEvent, uint32_t latchedEvent, bool scriptedLoss) {
        return !scriptedLoss && localEvent && latchedEvent && localEvent == latchedEvent;
    }

    // Start together, at the loading screen. Each game finishes loading an event at its own speed; the one
    // that is done first keeps its loading screen up (the game's "finished loading" step waits) until the
    // other one is done too, so both intro cutscenes start together. CarState::pad[0] says so meanwhile.
    constexpr uint8_t kStageLoadHeld = 4;           // loaded, holding the loading screen for the other player
    constexpr int kLoadHoldMaxMs = 30000;           // never longer
    constexpr int kLoadHoldElsewhereMs = 3000;      // they drive in another event / free roam: not coming
    inline bool OnGridStage(uint8_t stage) { return stage == 2 || stage == 3; }
    struct LoadHoldInputs {
        bool     enabled;      // linked and [Coop] StartTogether
        uint32_t event;        // event being loaded (0: free roam - never held)
        bool     prologue;     // the prologue's starts have their own wait on the grid
        bool     peerValid;
        bool     peerComing;   // in this event, or invited to it / inviting us
        uint32_t peerEvent;
        int      peerFlow;     // their game flow state (6 = in game)
        uint8_t  peerStage;    // their CarState::pad[0]
        bool     peerElsewhere;// driving (flow 6, a car) in another event or in free roam
        int      heldMs;       // how long we hold already
    };
    inline bool HoldLoadingScreen(const LoadHoldInputs& in) {
        if (!in.enabled || !in.event || in.prologue || !in.peerValid || !in.peerComing) return false;
        if (in.heldMs >= kLoadHoldMaxMs) return false;
        if (in.peerEvent == in.event && (in.peerStage == kStageLoadHeld || (in.peerFlow == 6 && in.peerStage >= 1)))
            return false;                           // they're loaded too (holding, or already past it)
        if (in.peerElsewhere && in.heldMs >= kLoadHoldElsewhereMs) return false;
        return true;
    }

    // An event start with no grid slot for the other player (their racer isn't one of ours, or both games
    // put the two cars on one spot): where to show them. The grid is read from the cars around us - how far
    // apart a row's cars stand side by side, and how far the rows are apart - and the first free slot is
    // taken: straight behind us, then the slots beside that one, then the free one beside us in our row,
    // then two rows back. Only slots the grid shows exist are used (no car beside anyone: no side slots), so
    // they are not off the road; with nothing to read, one car length straight behind us, as before.
    struct GridCar { float along, side; };          // another car, relative to us: metres ahead (+) / right (+)
    constexpr float kGridSlotFree = 3.0f;           // no car's centre within this of a slot
    constexpr float kGridDefaultRow = 6.0f;
    inline bool GridSlotFor(const GridCar* cars, int n, float& along, float& side) {
        float row = 0.f, width = 0.f;
        for (int i = 0; i < n; ++i) {
            float a = fabsf(cars[i].along), w = fabsf(cars[i].side);
            if (!(a < 40.f) || !(w < 40.f)) continue;
            if (a < 2.f && w >= 2.5f && w <= 8.f && (width == 0.f || w < width)) width = w;   // beside us
            if (w < 2.f && a >= 4.f && a <= 15.f && (row == 0.f || a < row)) row = a;       // in our lane
        }
        // a staggered grid: the nearest car of the next row stands diagonally - its offsets still give both
        if (width == 0.f || row == 0.f)
            for (int i = 0; i < n; ++i) {
                float a = fabsf(cars[i].along), w = fabsf(cars[i].side);
                if (a >= 2.f && a <= 15.f && w >= 2.5f && w <= 8.f) {
                    if (width == 0.f) width = w;
                    if (row == 0.f) row = a;
                }
            }
        if (row == 0.f) row = kGridDefaultRow;
        struct Cand { float a, s; bool beside; };
        Cand c[7] = { { -row, 0.f, false }, { -row, width, true }, { -row, -width, true }, { 0.f, width, true },
                      { 0.f, -width, true }, { -2.f * row, 0.f, false }, { -2.f * row, width, true } };
        for (const Cand& k : c) {
            if (k.beside && width == 0.f) continue;
            bool free = true;
            for (int i = 0; i < n && free; ++i) {
                float da = cars[i].along - k.a, ds = cars[i].side - k.s;
                if (da * da + ds * ds < kGridSlotFree * kGridSlotFree) free = false;
            }
            if (free) { along = k.a; side = k.s; return true; }
        }
        along = -kGridDefaultRow; side = 0.f;      // all taken: behind us as before
        return false;
    }

    // The winner's photo is for whoever really won: a win we only have because the other player won
    // (event `teamWinEvent`) shows the results list instead (coopwin.inl ShowPostRaceCoop).
    inline bool TeamWinResults(bool won, uint32_t event, uint32_t teamWinEvent) {
        return won && event && event == teamWinEvent;
    }

    // What the results list does with a message while it stands in for the winner's photo in the race
    // script's "postrace" state (outroEvent: the event it was shown for there; coopwin.inl hk_ResultsNotify).
    // That state only moves on with the photo's Continue (MFlowReadyForOutro); the list's own Continue posts
    // MNotifyRaceAbandoned, which only "raceover" answers - the race never ended and nothing responded.
    enum TeamResultsStep { kResultsAsIs = 0, kResultsNoDialog = 1, kResultsOutro = 2 };
    constexpr uint32_t kFeAccept = 0x406415E3u, kFeExitComplete = 0xE1FDE1D1u;
    inline int TeamResultsAction(uint32_t msg, uint32_t event, uint32_t outroEvent, bool resultsUp, bool photoUp) {
        if (!event || event != outroEvent || !resultsUp || photoUp) return kResultsAsIs;
        if (msg == kFeExitComplete) return kResultsOutro;     // the list has gone after Continue: the photo's Continue
        if (msg == kFeAccept) return kResultsNoDialog;        // Continue: no "you did not win" dialog for a counted win
        return kResultsAsIs;
    }
}
