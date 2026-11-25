#pragma once
// Blacklist rival race in co-op: both players must be ready for it (src/rivalgate.inl).
// CarState::pad[1] bit 0 says "co-op allowed here" (older builds test pad[1] != 0). Builds that know
// the rival gate add, only with bit 0 set:
//   bit 1  the rival state below is known
//   bit 2  the rival race of our current Blacklist rival is open for us (races, milestones, bounty done)
//   bits 3..5  which of the rival's races is open (GRaceBin boss race index)
#include <cstdint>

namespace RivalGate {

constexpr uint8_t kAllowed = 0x01, kKnown = 0x02, kReady = 0x04, kIdxShift = 3, kIdxMask = 0x38;

inline uint8_t Encode(bool allowed, bool known, bool ready, int idx) {
    if (!allowed) return 0;
    uint8_t v = kAllowed;
    if (known) {
        v |= kKnown;
        if (ready) v |= (uint8_t)(kReady | (((idx < 0 ? 0 : idx > 7 ? 7 : idx) << kIdxShift) & kIdxMask));
    }
    return v;
}

// "co-op allowed here" from the other player's pad[1] (no state yet: allowed)
inline bool PeerAllowed(bool valid, uint8_t pad1) { return !valid || (pad1 & kAllowed) != 0; }

struct Peer { bool known = false, ready = false; int idx = 0; };
inline Peer Decode(bool valid, uint8_t pad1) {
    Peer p;
    if (!valid || !(pad1 & kAllowed) || !(pad1 & kKnown)) return p;   // no state / an older build
    p.known = true;
    p.ready = (pad1 & kReady) != 0;
    p.idx = p.ready ? (pad1 & kIdxMask) >> kIdxShift : 0;
    return p;
}

// Our rival race waits for the other player: co-op is on, the rival race is open for us, and they are
// not there yet - on an earlier rival (Blacklist numbers count down: a higher number is behind us), or on
// ours without it open (or an earlier race of the rival than ours). Never towards a partner we know
// nothing about (an older build) or one already past this rival.
inline bool Gated(bool linked, int myBin, bool myReady, int myIdx, int peerBin, const Peer& peer) {
    if (!linked || !myReady || !peer.known) return false;
    if (myBin < 1 || myBin > 15 || peerBin < 1 || peerBin > 15) return false;
    if (peerBin < myBin) return false;            // they are past this rival already
    if (peerBin > myBin) return true;             // still on an earlier rival
    return !peer.ready || peer.idx < myIdx;
}

// The other side of Gated: the other player's rival race waits for us.
inline bool PartnerWaits(bool linked, int myBin, bool myReady, int myIdx, int peerBin, const Peer& peer) {
    Peer me; me.known = true; me.ready = myReady; me.idx = myIdx;
    return peer.known && Gated(linked, peerBin, peer.ready, peer.idx, myBin, me);
}

}  // namespace RivalGate
