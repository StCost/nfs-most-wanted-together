#pragma once
// Spectate (1.7.4): out of an event (busted, totaled, knocked out, lost) while the other player still
// races, the Restart / Quit screen also offers Spectate. The camera then follows their car until
// they're done: their win counts for both (shared wins) and we get the winner's flow; if they lose
// too, our Restart / Quit screen comes back. The decisions that don't need the game (tests\test_race.cpp).
#include <cstdint>

namespace Spectate {
    struct Peer {
        bool     connected;
        bool     sameEvent;     // they're in our event (same hash)
        bool     inRace;
        uint8_t  finished;      // CarState::finished: 0 racing, 1 won, 2+ finished and not won
        bool     out;           // CarState::pad[0] == 3: out of the race (busted / totaled / knocked out)
        bool     racedHere;     // seen racing in this run of the event (an old "finished" doesn't count)
    };
    inline bool PeerStillRacing(const Peer& p) {
        return p.connected && p.sameEvent && p.inRace && !p.finished && !p.out && p.racedHere;
    }
    // the Restart / Quit screen shows Spectate
    inline bool Offer(bool linked, bool localOut, const Peer& p) { return linked && localOut && PeerStillRacing(p); }

    enum class Next { Keep, EndWon, EndLost, EndQuiet };
    // while watching: what happens now
    inline Next Tick(const Peer& p, bool restarted, bool eventChanged, bool peerWonLatched) {
        if (restarted || eventChanged) return Next::EndQuiet;      // the race restarted / we left: nothing to show
        if (peerWonLatched || (p.connected && p.sameEvent && p.finished == 1)) return Next::EndWon;
        if (PeerStillRacing(p)) return Next::Keep;
        return Next::EndLost;                                      // they lost too, quit, or left: our Restart / Quit
    }

    // Out of the event ("raceover": totaled, knocked out, beaten), not spectating / waiting for a knockout, the
    // race still running here, and then the other player wins (once per run): what happens to the screen up.
    //  ViaPauseScreen - the race-over Restart / Quit screen: it goes, the win is counted, the results list
    //  CreditHere     - the losing results list: the win is counted, the list stays (its Continue ends the race)
    //  None           - nothing yet (the "race over" message: ShowLosingPostRaceScreen decides), or we left
    enum class OutWin { None, CreditHere, ViaPauseScreen };
    inline OutWin OutOfRaceWin(bool spectating, bool knockoutHeld, bool peerWon, bool racing, bool localOut,
                               bool localWon, bool doneThisRun, bool pauseScreenUp, bool resultsUp, bool photoUp) {
        if (spectating || knockoutHeld || !peerWon || !racing || !localOut || localWon || doneThisRun) return OutWin::None;
        if (pauseScreenUp) return OutWin::ViaPauseScreen;
        if (resultsUp && !photoUp) return OutWin::CreditHere;
        return OutWin::None;
    }
    // ShowLosingPostRaceScreen with their win already in (they won while "race over" was shown): the win is
    // counted and the results list comes instead of the losing screen
    inline bool CreditBeforeLoseScreen(bool spectating, bool peerWon, bool racing, bool localOut, bool localWon,
                                       bool doneThisRun) {
        return !spectating && peerWon && racing && localOut && !localWon && !doneThisRun;
    }
}
