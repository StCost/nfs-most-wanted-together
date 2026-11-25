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

    // Racer mirrors (client): each opponent of the host drives one of our own AI racers.
    // Racer roles in a rival race: the rival(s), our extra helper racer, or a normal racer. Both games
    // build their racers in the same order (humans first, then the event's opponents, then the helper,
    // then the other player's slot), so a racer's GRacerInfo slot and role match on both machines.
    // Matching by model alone swapped the rival and the helper when both are the same car (Big Lou
    // and his helper are both Eclipse GTs in 11.1.2).
    enum RacerRole : int { kRoleUnknown = -1, kRoleNormal = 0, kRoleRival = 1, kRoleHelper = 2 };
    inline bool RolesCompatible(int a, int b) { return a == kRoleUnknown || b == kRoleUnknown || a == b; }
    // Binding order: the same slot (and a role that doesn't contradict it), then the same role and
    // model, then the same model (role not contradicting), then - for the one-of-a-kind roles (rival,
    // helper) - the same role in any model; when none of that is free for a while (our game built
    // another car for that opponent), any free racer of a compatible role, then any free racer, so
    // nobody races on their own AI next to the host's.
    constexpr double ANY_MODEL_AFTER_MS = 1500.0;
    struct RacerCand { uint32_t model; bool free; int slot = -1; int role = kRoleUnknown; };
    struct RacerWant { uint32_t model; int slot = -1; int role = kRoleUnknown; };
    inline int PickRacer(const RacerCand* c, int n, const RacerWant& w, bool anyModel) {
        if (w.slot >= 0)
            for (int i = 0; i < n; ++i) if (c[i].free && c[i].slot == w.slot && RolesCompatible(c[i].role, w.role)) return i;
        if (w.role != kRoleUnknown)
            for (int i = 0; i < n; ++i) if (c[i].free && c[i].role == w.role && c[i].model == w.model) return i;
        for (int i = 0; i < n; ++i) if (c[i].free && c[i].model == w.model && RolesCompatible(c[i].role, w.role)) return i;
        if (w.role == kRoleRival || w.role == kRoleHelper)
            for (int i = 0; i < n; ++i) if (c[i].free && c[i].role == w.role) return i;
        if (anyModel) {
            for (int i = 0; i < n; ++i) if (c[i].free && RolesCompatible(c[i].role, w.role)) return i;
            for (int i = 0; i < n; ++i) if (c[i].free) return i;
        }
        return -1;
    }
    inline int PickRacer(const RacerCand* c, int n, uint32_t model, bool anyModel) {
        RacerWant w{ model };
        return PickRacer(c, n, w, anyModel);
    }
    // The host's racers drive ours only while the host is in our event. Starting a rival's next race from
    // the results screen, one game is in the new race while the other still shows the last one's results
    // and sends that race's racers: bound to them, our new racers were driven to the old race's spots
    // (and stayed taken when the host's racers of this race came). No state from the host: no change.
    inline bool HostRacersAreOurs(bool peerStateValid, uint32_t peerEvent, uint32_t ourEvent) {
        return !peerStateValid || (ourEvent != 0 && peerEvent == ourEvent);
    }
    // Role of an AI racer by its GRacerInfo slot: `helperSlot` / `partnerSlot` as this race load set
    // them (-1 = none), `bossRace` = a Blacklist rival race. The other player's slot is no AI racer.
    inline int RacerRoleOf(int slot, bool bossRace, int helperSlot, int partnerSlot) {
        if (slot < 0) return kRoleUnknown;
        if (helperSlot >= 0 && slot == helperSlot) return kRoleHelper;
        if (partnerSlot >= 0 && slot == partnerSlot) return kRoleUnknown;
        return bossRace ? kRoleRival : kRoleNormal;
    }
    // Racer slot and role on the wire, in TrafficEntry::flags of a racer (bit 1) from the host:
    // bit 15 = present (1.8+ hosts; older builds leave bits 8-15 at 0 and ignore them),
    // bits 8-12 = GRacerInfo slot (0-15), bits 13-14 = role (0 normal, 1 rival, 2 helper, 3 unknown).
    constexpr uint16_t kRacerSlotPresent = 0x8000;
    inline uint16_t EncodeRacerSlot(uint16_t flags, int slot, int role) {
        flags = (uint16_t)(flags & 0x00FF);
        if (slot < 0 || slot > 15) return flags;
        int r = (role == kRoleNormal || role == kRoleRival || role == kRoleHelper) ? role : 3;
        return (uint16_t)(flags | kRacerSlotPresent | ((slot & 0x1F) << 8) | (r << 13));
    }
    inline bool DecodeRacerSlot(uint16_t flags, int& slot, int& role) {
        slot = -1; role = kRoleUnknown;
        if (!(flags & kRacerSlotPresent) || !(flags & 2)) return false;
        int s = (flags >> 8) & 0x1F, r = (flags >> 13) & 3;
        if (s > 15) return false;
        slot = s; role = r == 3 ? kRoleUnknown : r;
        return true;
    }
    // Racer role in RosterPacket's spare byte (was padding, always 0 from 1.7.9 and older):
    // bit 7 = present, bits 0-1 = role (0 normal, 1 rival, 2 helper).
    constexpr uint8_t kRosterRolePresent = 0x80;
    inline uint8_t EncodeRosterRole(int role) {
        return (role == kRoleNormal || role == kRoleRival || role == kRoleHelper) ? (uint8_t)(kRosterRolePresent | role) : 0;
    }
    inline int DecodeRosterRole(uint8_t b) {
        if (!(b & kRosterRolePresent)) return kRoleUnknown;
        int r = b & 0x7F;
        return (r == kRoleNormal || r == kRoleRival || r == kRoleHelper) ? r : kRoleUnknown;
    }
    // The roster entry of the helper: the one the sender marked as the helper, else (a 1.7.9 sender)
    // the entry at the helper's opponent index. -1 = none.
    inline int RosterHelperEntry(const int* roles, int n, int helperOpp) {
        bool anyRole = false;
        for (int i = 0; i < n; ++i) { if (roles[i] == kRoleHelper) return i; if (roles[i] != kRoleUnknown) anyRole = true; }
        if (anyRole) return -1;   // a roster with roles but no helper: the sender had none
        return (helperOpp >= 0 && helperOpp < n) ? helperOpp : -1;
    }

    // The helper's car. A racer built with no customization record (the model swapped in, the
    // record left null) came out with only its wheels: no body mesh. So the helper always gets a
    // full record of its own model: the other game's helper with its record, else the list model
    // (BossHelperCar) with the game's own record when the game already picked that model, or the
    // rival's record when the rival is that model (both built from the event, so the same on both
    // machines), else the game's pick as it is.
    enum HelperRecord : int { kHelperGameRecord = 0, kHelperRivalRecord = 1, kHelperPeerRecord = 2 };
    struct HelperIn {
        uint32_t want;            // BossHelperCar model key, 0 = none for this rival
        uint32_t gameModel;       // what the game picked for the helper
        uint32_t rivalModel;      // the rival built just before it (0 = unknown)
        bool     rivalHasRecord;
        uint32_t peerModel;       // the other game's helper from its roster (0 = none)
        bool     peerHasRecord;
    };
    struct HelperOut { uint32_t model; int record; };
    inline HelperOut ChooseHelperCar(const HelperIn& in) {
        if (in.peerModel && in.peerHasRecord) return { in.peerModel, kHelperPeerRecord };
        uint32_t want = in.peerModel ? in.peerModel : in.want;   // a bare entry (1.7.9): its model, a record from here
        if (want && want != in.gameModel && in.rivalModel == want && in.rivalHasRecord) return { want, kHelperRivalRecord };
        return { in.gameModel, kHelperGameRecord };
    }
    // Joining the other player's rival race: wait a little for their roster (it comes with the
    // invitation, repeated every 1.5 s) so both games build the same helper.
    constexpr unsigned long kRosterWaitMs = 3000;
    inline bool WaitForRoster(bool bossRace, bool helperOn, bool rosterFresh, unsigned long waitedMs) {
        return bossRace && helperOn && !rosterFresh && waitedMs < kRosterWaitMs;
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

    // Start together, on the grid: we are on it first and hold the start for the other player. A slower load
    // gets 20 s. While they are still loading - unloading the last race's world or loading this one (a game
    // flow other than in game / the menus), or already in this event but not on its grid yet (its intro
    // cutscene, their loading screen held) - we keep holding, and the 20 s count from when they were last
    // seen loading: the client that starts a rival's next race while the host still leaves the last one's
    // results through a full world load no longer goes 20 s before the host. Never longer than 2 minutes.
    constexpr int kStartHoldMs = 20000;
    constexpr int kStartHoldLoadingMaxMs = 120000;
    inline bool PeerLoadingToStart(uint32_t event, uint32_t peerEvent, int peerFlow, uint8_t peerStage) {
        if (peerFlow == 6) return event && peerEvent == event && !OnGridStage(peerStage);
        return peerFlow != 0 && peerFlow != 3;      // 3: the menus (not on the way), 0: no game yet
    }
    inline bool StartHoldTimedOut(int heldMs, int sincePeerLoadingMs) {
        return heldMs >= kStartHoldLoadingMaxMs || sincePeerLoadingMs >= kStartHoldMs;
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

    // A win we only have because the other player won (event `teamWinEvent`) shows the winner's photo too, its line
    // under the photo naming them ("<name> won the race"; coopwin.inl hk_PhotoSetup).
    inline bool TeamWinPhoto(uint32_t event, uint32_t teamWinEvent) {
        return event && event == teamWinEvent;
    }
    constexpr uint32_t kFeAccept = 0x406415E3u, kFeExitComplete = 0xE1FDE1D1u;
    // The photo's Continue (coopwin.inl hk_PhotoNotify). outWinEvent: the event whose win was counted for us after we
    // were out - the race script is in "raceover" and waits for MNotifyRaceAbandoned, not the photo's
    // MFlowReadyForOutro: a career race then ends the way the results list ends it. Anywhere else the photo's own.
    enum TeamPhotoStep { kPhotoAsIs = 0, kPhotoEndOut = 1 };
    inline int TeamPhotoAction(uint32_t msg, uint32_t event, uint32_t outWinEvent, bool racePhoto, bool career,
                               bool photoReady) {
        if (msg != kFeAccept || !event || event != outWinEvent) return kPhotoAsIs;
        if (!racePhoto || !career || !photoReady) return kPhotoAsIs;   // a quick race quits to the menu as always
        return kPhotoEndOut;
    }
    // What the results list does with a message (coopwin.inl hk_ResultsNotify). swapEvent: the event whose losing
    // list is closing for the winner's photo - its Continue / exit must not end the race then. A shared win's
    // Continue skips the "you did not win" dialog of a Blacklist race (our racer may not be 1st here).
    enum TeamResultsStep { kResultsAsIs = 0, kResultsNoDialog = 1, kResultsHold = 2 };
    inline int TeamResultsAction(uint32_t msg, uint32_t event, uint32_t teamWinEvent, uint32_t swapEvent, bool resultsUp) {
        if (!event || !resultsUp) return kResultsAsIs;
        if (event == swapEvent && (msg == kFeAccept || msg == kFeExitComplete)) return kResultsHold;
        if (msg == kFeAccept && event == teamWinEvent) return kResultsNoDialog;
        return kResultsAsIs;
    }

    // Out of the event ("raceover": totaled, knocked out, beaten), not waiting for a knockout, the race still
    // running here, and then the other player wins (once per run): what happens to the screen up.
    // The screen up closes and the winner's photo comes instead, the win counted (coopwin.inl LoseScreenWinTickImpl):
    //  ViaPauseScreen - the race-over Restart / Quit screen
    //  ViaResults     - the losing results list
    //  None           - nothing yet (the "race over" message: ShowLosingPostRaceScreen decides), or we left
    enum class OutWin { None, ViaResults, ViaPauseScreen };
    inline OutWin OutOfRaceWin(bool knockoutHeld, bool peerWon, bool racing, bool localOut,
                               bool localWon, bool doneThisRun, bool pauseScreenUp, bool resultsUp, bool photoUp) {
        if (knockoutHeld || !peerWon || !racing || !localOut || localWon || doneThisRun) return OutWin::None;
        if (pauseScreenUp) return OutWin::ViaPauseScreen;
        if (resultsUp && !photoUp) return OutWin::ViaResults;
        return OutWin::None;
    }
    // ShowLosingPostRaceScreen with their win already in (they won while "race over" was shown): the win is
    // counted and the winner's photo comes instead of the losing screen
    inline bool CreditBeforeLoseScreen(bool peerWon, bool racing, bool localOut, bool localWon, bool doneThisRun) {
        return peerWon && racing && localOut && !localWon && !doneThisRun;
    }
}
