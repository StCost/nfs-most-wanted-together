#pragma once
// Joint event start (1.7.4): when one player starts an event, the other is pulled into the same
// one from wherever they are. The decisions that don't need the game, so the test suite covers
// them (tests\test_race.cpp). event.inl reads the game, asks these, and does what they say.
#include <cstdint>

namespace JoinUtil {
    enum class Where {
        Roaming,    // free roam in career (flow 6, no event)
        FrontEnd,   // the menus (flow 3): main menu, career menus, quick race, Challenge Series
        InEvent,    // driving another event
        Results,    // on the results / win / Continue screens of an event
        Loading,    // anything in between
    };
    enum class Step {
        Start,               // start it here now (free roam: GManager; menus: LoadTrack)
        CloseMenusFirst,     // a menu over the world (map, pause, SMS ...): back out of it, then retry
        LeaveResultsFirst,   // Continue past the results, then retry
        QuitToFrontEndFirst, // the event needs the front end (Challenge Series, quick race): quit there, then retry
        Wait,                // loading: retry in a moment
        Busy,                // in another event: never yanked out mid-race - tell both players, don't join
        Refuse,              // not an event we may join (another Blacklist rival's, the prologue ...)
        AlreadyThere,
    };
    struct In {
        Where where;
        bool  menuOpen;        // roaming with a menu over the world
        bool  frontEndOnly;    // started from the menus (context 0: Challenge Series / quick race)
        bool  sameEvent;       // we're already in (or loading) that event
        bool  joinable;        // rival / prologue checks passed
    };
    inline Step Decide(const In& in) {
        if (in.sameEvent) return Step::AlreadyThere;
        if (!in.joinable) return Step::Refuse;
        switch (in.where) {
        case Where::InEvent:  return Step::Busy;
        case Where::Results:  return Step::LeaveResultsFirst;
        case Where::Loading:  return Step::Wait;
        case Where::FrontEnd: return Step::Start;
        case Where::Roaming:
            if (in.menuOpen) return Step::CloseMenusFirst;
            return in.frontEndOnly ? Step::QuitToFrontEndFirst : Step::Start;
        }
        return Step::Wait;
    }

    // The joining side keeps the invitation itself (the sender repeats it only ~12 s, and quitting to
    // the front end and loading takes longer). It is dropped when it's done, too old, or the other
    // player is no longer in that event (they quit it or it's over).
    constexpr uint32_t PENDING_MS = 60000;            // the whole way: results -> free roam -> front end -> load
    constexpr uint32_t PEER_LEFT_GRACE_MS = 15000;    // they may still be loading it when we look
    struct Pending { uint32_t hash = 0; uint32_t since = 0; int context = -1; };
    inline bool KeepPending(const Pending& p, uint32_t now, uint32_t peerEventHash, bool peerInGame, bool weAreThere) {
        if (!p.hash || weAreThere) return false;
        uint32_t age = now - p.since;
        if (age > PENDING_MS) return false;
        if (age > PEER_LEFT_GRACE_MS && peerInGame && peerEventHash != p.hash) return false;   // they left it
        return true;
    }

    // The starting side: the other player is driving another event (not its results), so they won't
    // come - say so once instead of inviting into nothing.
    inline bool PeerBusyElsewhere(uint32_t ourEvent, uint32_t peerEvent, bool peerInRace, bool peerFinished) {
        return ourEvent && peerEvent && peerEvent != ourEvent && peerInRace && !peerFinished;
    }

    // The race context travels in the last byte of EventPacket::eventId (after the ID's NUL, so older
    // builds read the same ID): 0x80 | context, 0 = not sent (an older build).
    inline uint8_t ContextToWire(int context) { return (context >= 0 && context <= 3) ? (uint8_t)(0x80 | context) : 0; }
    inline int ContextFromWire(uint8_t b) { return (b & 0x80) && (b & 0x7F) <= 3 ? (b & 0x7F) : -1; }
    // Which context the joining side loads it with from the menus: the sender's if known, else
    // as before (a Challenge Series event 0, anything else career 2).
    inline int LoadContext(int sentContext, bool isChallenge) {
        if (sentContext == 0 || sentContext == 2) return sentContext;
        return isChallenge ? 0 : 2;
    }
    // Events we invite the other player into: career (2) and front-end starts (0: Challenge Series,
    // quick race). Not online (1).
    inline bool InviteContext(int context) { return context == 0 || context == 2; }
}
