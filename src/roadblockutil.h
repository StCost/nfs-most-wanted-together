#pragma once
// Shared police roadblocks: the pure part (wire description, checks, the table of the other player's
// roadblocks). The game side is roadblock.inl.
#include <cstdint>
#include <cmath>
#include <cstring>

namespace Roadblock {
    // pieces a roadblock places besides its cars (AICopManager::CreateRoadBlock 0x43DD30): sawhorses
    // ("XO_Sawhorse_1b_00", smackable key 0x9663AD06) and spike strips ("XO_SpikeBelt_1b_DW_00", 0xCA89EF8F).
    // The game's setups (RoadblockCandidateList 0x8EB5E8, SPIKES_RoadblockCandidateList 0x8EBC68) have at
    // most 3 of them; 4 leaves room.
    enum PieceKind : uint8_t { PIECE_NONE = 0, PIECE_SAWHORSE = 1, PIECE_SPIKES = 2 };
    constexpr int MAX_PIECES = 4;
    constexpr int MAX_PEER = 4;              // the other player's roadblocks we keep at once
    constexpr int MAX_OURS = 4;              // our own roadblocks we announce at once
    constexpr int SETUPS_NORMAL = 15, SETUPS_SPIKES = 9;   // usable entries of the two setup lists
    constexpr float kPlaceRange = 500.f;     // their pieces are built here once we are this close
    constexpr float kSuppressRange = 600.f;  // a live roadblock of theirs this close: our game builds none
    constexpr float kPieceFromCentre = 40.f; // a piece is never farther than this from the centre
    constexpr uint32_t kExpireMs = 5000;     // the owner repeats a live roadblock every second
    constexpr uint32_t kResendMs = 1000;
    constexpr int kRemovalRepeats = 3;

    enum : uint8_t {
        RB_SPIKES = 1,   // picked from the spike strip setups
        RB_HEAVY  = 2,   // heavy support (Rhino SUVs) instead of patrol cars
    };

#pragma pack(push, 1)
    struct Piece {
        uint8_t kind;        // PieceKind
        uint8_t pad[3];
        float   m[16];       // the matrix the owner's game gave IPlaceableScenery::Place (snap to ground on)
    };
    struct Desc {
        uint32_t id;         // the owner's id for it (never 0)
        uint8_t  setup;      // index in its setup list (log / checks)
        uint8_t  flags;      // RB_*
        uint8_t  nPieces;
        uint8_t  nCars;      // cop cars in it (they come as the owner's cops, TrafficEntry flag TE_ROADBLOCK)
        float    centre[3];  // IRoadBlock::GetRoadBlockCentre
        float    dir[3];     // the road's direction there (the side the chased car comes from is -dir)
        Piece    p[MAX_PIECES];
    };
#pragma pack(pop)

    inline bool Finite(const float* v, int n, float lim) {
        for (int i = 0; i < n; ++i) if (!std::isfinite(v[i]) || std::fabs(v[i]) > lim) return false;
        return true;
    }
    inline float Dist3(const float* a, const float* b) {
        float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }
    // A piece's matrix (UMath::Matrix4, 4 rows of 4): rows 0..2 the orientation (unit rows), row 3 the position.
    // The w column is passed on as the owner's game had it.
    inline bool ValidMatrix(const float* m) {
        if (!Finite(m, 16, 1.0e6f)) return false;
        for (int r = 0; r < 3; ++r) {
            float l = m[r * 4] * m[r * 4] + m[r * 4 + 1] * m[r * 4 + 1] + m[r * 4 + 2] * m[r * 4 + 2];
            if (l < 0.8f || l > 1.2f) return false;
        }
        return true;
    }
    inline bool ValidDesc(const Desc& d) {
        if (!d.id || d.nPieces > MAX_PIECES || d.nCars > 8 || (d.flags & ~(RB_SPIKES | RB_HEAVY))) return false;
        if (d.setup >= ((d.flags & RB_SPIKES) ? SETUPS_SPIKES : SETUPS_NORMAL)) return false;
        if (!Finite(d.centre, 3, 1.0e6f) || !Finite(d.dir, 3, 2.f)) return false;
        float dl = d.dir[0] * d.dir[0] + d.dir[1] * d.dir[1] + d.dir[2] * d.dir[2];
        if (dl < 0.5f || dl > 1.5f) return false;
        for (int i = 0; i < d.nPieces; ++i) {
            const Piece& p = d.p[i];
            if (p.kind != PIECE_SAWHORSE && p.kind != PIECE_SPIKES) return false;
            if (!ValidMatrix(p.m) || Dist3(p.m + 12, d.centre) > kPieceFromCentre) return false;
        }
        return d.nPieces > 0 || d.nCars > 0;
    }
    inline int CountKind(const Desc& d, uint8_t kind) {
        int n = 0;
        for (int i = 0; i < d.nPieces && i < MAX_PIECES; ++i) if (d.p[i].kind == kind) ++n;
        return n;
    }

    // ---- the other player's roadblocks ---------------------------------------------------------
    struct PeerEntry {
        bool     used = false;
        bool     placed = false;   // its pieces are built here (the game layer keeps the objects)
        uint32_t lastMs = 0;       // when the owner last said it is there
        Desc     d{};
        bool     haveSide = false; // our car's last place along the road, seen from it (PassedRoadblock)
        float    lastAlong = 0.f;
        bool     dodged = false;   // we drove through it: counted once
        bool     ownerDodged = false; // the owner's player drove through it first: their game counted it (shared)
    };

    // ---- driving through one of theirs counts as a roadblock dodged here too --------------------
    // The game counts a dodge for its own roadblocks only (the chased car crosses the line through the
    // centre, AICopManager::UpdateRoadBlocks 0x43A040); theirs is not in our cop manager. Same rule for
    // theirs: our car crosses that line (either way) close to the centre.
    constexpr float kPassHalfWidth = 30.f;   // across the road from the centre
    constexpr float kPassHeight = 8.f;
    constexpr float kPassMaxStep = 40.f;     // a jump farther than this between two ticks is a reset, not a pass
    // along: signed distance past the line (+dir side); across / up: from the centre
    inline void RoadblockFrame(const Desc& d, const float* p, float* along, float* across, float* up) {
        float rx = p[0] - d.centre[0], ry = p[1] - d.centre[1], rz = p[2] - d.centre[2];
        float hx = d.dir[0], hz = d.dir[2];
        float hl = std::sqrt(hx * hx + hz * hz);
        if (!(hl > 1e-4f)) { hx = 1.f; hz = 0.f; hl = 1.f; }
        hx /= hl; hz /= hl;
        *along = rx * hx + rz * hz;
        *across = -rx * hz + rz * hx;
        *up = ry;
    }
    inline bool PassedRoadblock(float prevAlong, float along, float across, float up) {
        if (!std::isfinite(prevAlong) || !std::isfinite(along) || !std::isfinite(across) || !std::isfinite(up)) return false;
        if ((prevAlong < 0.f) == (along < 0.f)) return false;               // still on the same side
        if (std::fabs(along - prevAlong) > kPassMaxStep) return false;
        return std::fabs(across) <= kPassHalfWidth && std::fabs(up) <= kPassHeight;
    }
    struct PeerTable {
        PeerEntry e[MAX_PEER];
        int Find(uint32_t id) const {
            for (int i = 0; i < MAX_PEER; ++i) if (e[i].used && e[i].d.id == id) return i;
            return -1;
        }
        // A slot for a new one: a free one, else the one heard of longest ago (*evicted = true: the game
        // layer removes what it built for that one first).
        int Alloc(bool* evicted) const {
            if (evicted) *evicted = false;
            int oldest = 0;
            for (int i = 0; i < MAX_PEER; ++i) {
                if (!e[i].used) return i;
                if ((int32_t)(e[i].lastMs - e[oldest].lastMs) < 0) oldest = i;
            }
            if (evicted) *evicted = true;
            return oldest;
        }
        int Live() const { int n = 0; for (const auto& x : e) n += x.used ? 1 : 0; return n; }
    };
    inline bool Expired(const PeerEntry& x, uint32_t nowMs) { return x.used && nowMs - x.lastMs > kExpireMs; }
    inline bool InRange(const PeerEntry& x, const float* ourPos, float range) {
        return x.used && ourPos && Dist3(x.d.centre, ourPos) <= range;
    }
    // Our game wants to build a roadblock: refused while one of theirs is live near us (both games
    // would block the same road twice, differently). The game asks again later; once theirs is gone
    // ours goes up (and is shown on their side).
    inline bool SuppressOurs(const PeerTable& t, const float* ourPos, uint32_t nowMs, bool linked) {
        if (!linked || !ourPos) return false;
        for (const auto& x : t.e)
            if (x.used && !Expired(x, nowMs) && InRange(x, ourPos, kSuppressRange)) return true;
        return false;
    }
}
