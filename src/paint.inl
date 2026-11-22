// Included from game.cpp (before minimap.inl).
// ---------------------------------------------------------------------------
// Map / minimap blip colour from the car's BASE_PAINT part.
//  InstalledPartIndices[76] -> CarPartDatabase_GetCarPartByIndex (_CarPartDB)
//  -> CarPart RED/GREEN/BLUE attributes (v1.3 addresses, prologue-checked).
// ---------------------------------------------------------------------------
#include "paintutil.h"

namespace {
    constexpr uintptr_t kCarPartDB = 0x9B26A8;
    constexpr uintptr_t kGetCarPartByIndex = 0x747C00;
    constexpr uintptr_t kGetAppliedIParam  = 0x747AC0;
    constexpr uintptr_t kHasAppliedAttr    = 0x747A50;
    constexpr uintptr_t kBStringHash       = 0x460BF0;

    const uint8_t kGetByIndexSig[] = { 0x8B, 0x44, 0x24, 0x04, 0x85, 0xC0, 0x56, 0x7C, 0x16 };
    const uint8_t kIParamSig[]     = { 0x8B, 0x44, 0x24, 0x04, 0x6A, 0x00, 0x50, 0xE8 };
    const uint8_t kHasSig[]        = { 0x8B, 0x44, 0x24, 0x04, 0x6A, 0x00, 0x50, 0xE8 };
    const uint8_t kHashSig[]       = { 0x8B, 0x54, 0x24, 0x04, 0x8A, 0x0A, 0x83, 0xC8, 0xFF };

    bool SigOk(uintptr_t addr, const uint8_t* sig, size_t n) {
        uint8_t got[16] = {};
        if (n > sizeof(got) || !Mem::SafeRead((const void*)addr, got, n)) return false;
        return memcmp(got, sig, n) == 0;
    }

    bool PaintFnsOk() {
        static int state = 0; // 0 unknown, 1 ok, -1 no
        if (state) return state == 1;
        bool ok = SigOk(kGetCarPartByIndex, kGetByIndexSig, sizeof(kGetByIndexSig))
               && SigOk(kGetAppliedIParam,  kIParamSig,     sizeof(kIParamSig))
               && SigOk(kHasAppliedAttr,    kHasSig,        sizeof(kHasSig))
               && SigOk(kBStringHash,       kHashSig,       sizeof(kHashSig));
        state = ok ? 1 : -1;
        LOG("Paint: CarPart RGB lookup %s", ok ? "ready (runtime swatch)" : "NOT hooked (prologue mismatch)");
        return ok;
    }

    uint32_t AttrHash(const char* s) {
        char buf[16] = {};
        strncpy_s(buf, s, _TRUNCATE);
        return ((uint32_t(__cdecl*)(char*))kBStringHash)(buf);
    }

    bool RgbFromPartIndex(int16_t index, int* r, int* g, int* b) {
        if (!PaintFnsOk() || index < 0 || !r || !g || !b) return false;
        void* part = ((void*(__thiscall*)(void*, int))kGetCarPartByIndex)((void*)kCarPartDB, (int)index);
        if (!part) return false;
        uint32_t hR = AttrHash("RED"), hG = AttrHash("GREEN"), hB = AttrHash("BLUE");
        auto has = (bool(__thiscall*)(void*, uint32_t))kHasAppliedAttr;
        auto ip  = (unsigned(__thiscall*)(void*, uint32_t, int))kGetAppliedIParam;
        if (!has(part, hR) || !has(part, hG) || !has(part, hB)) return false;
        unsigned rv = ip(part, hR, 0), gv = ip(part, hG, 0), bv = ip(part, hB, 0);
        auto to8 = [](unsigned v) -> int {
            if (v <= 255) return (int)v;
            float f;
            memcpy(&f, &v, 4);
            if (f >= 0.f && f <= 1.f) return (int)(f * 255.f + 0.5f);
            return 255;
        };
        *r = to8(rv); *g = to8(gv); *b = to8(bv);
        return true;
    }

    // Prefer a vivid BASE_PAINT swatch. If that is missing, black or near-white (a vinyl
    // often carries the colour you actually see), pick the most saturated vinyl colour.
    int SwatchChroma(int r, int g, int b) {
        int mx = r; if (g > mx) mx = g; if (b > mx) mx = b;
        int mn = r; if (g < mn) mn = g; if (b < mn) mn = b;
        return mx - mn;
    }
    uint32_t ColorFromCustom(const uint8_t* rec, int16_t* usedIdx, const char** how) {
        if (usedIdx) *usedIdx = -1;
        if (how) *how = "";
        auto read_idx = [&](int16_t idx, int* r, int* g, int* b) -> bool {
            if (idx < 0 || !RgbFromPartIndex(idx, r, g, b)) return false;
            return true;
        };
        int br = 0, bg = 0, bb = 0;
        int16_t bidx = Paint::BasePaintIndex(rec, CUSTOM_RECORD_SIZE);
        if (read_idx(bidx, &br, &bg, &bb) && SwatchChroma(br, bg, bb) >= 30) {
            if (usedIdx) *usedIdx = bidx;
            if (how) *how = "base";
            return Paint::PackArgb(br, bg, bb);
        }
        static const int kVinyl[] = { 79, 80, 81, 82 };
        int bestC = -1, bestR = 0, bestG = 0, bestB = 0; int16_t bestIdx = -1;
        for (int slot : kVinyl) {
            int r = 0, g = 0, b = 0;
            int16_t idx = Paint::SlotIndex(rec, CUSTOM_RECORD_SIZE, slot);
            if (!read_idx(idx, &r, &g, &b)) continue;
            int ch = SwatchChroma(r, g, b);
            if (ch > bestC) { bestC = ch; bestR = r; bestG = g; bestB = b; bestIdx = idx; }
        }
        if (bestC >= 30) {
            if (usedIdx) *usedIdx = bestIdx;
            if (how) *how = "vinyl";
            return Paint::PackArgb(bestR, bestG, bestB);
        }
        // last resort: a non-black base, even if dull
        if (br || bg || bb) {
            if (usedIdx) *usedIdx = bidx;
            if (how) *how = "base";
            return Paint::PackArgb(br, bg, bb);
        }
        return 0;
    }

    uint32_t g_blipLocal = 0, g_blipPeer = 0;
    uint32_t g_blipLocalHash = 0, g_blipPeerHash = 0;
    void*    g_debugFriend = nullptr;
    CarInfo  g_debugFriendCar{};
    bool     g_haveDebugFriend = false;

    uint32_t CustomHashOf(const uint8_t* rec, uint32_t carKey) {
        uint32_t h = 2166136261u;
        for (int i = 0; i < CUSTOM_RECORD_SIZE; ++i) { h ^= rec[i]; h *= 16777619u; }
        return h ^ carKey;
    }
}

uint32_t Game::LocalBlipColor() { return g_blipLocal; }
uint32_t Game::PeerBlipColor()  { return g_blipPeer; }

void Game::RefreshBlipColors() {
    __try {
        CarInfo me{};
        if (ReadLocalCarInfo(me) && me.hasCustom) {
            if (me.customHash != g_blipLocalHash || !g_blipLocal) {
                int16_t idx = -1; const char* how = "";
                uint32_t c = ColorFromCustom(me.custom, &idx, &how);
                if (c != g_blipLocal || me.customHash != g_blipLocalHash) {
                    g_blipLocal = c;
                    g_blipLocalHash = me.customHash;
                    int sr, sg, sb;
                    char extra[160] = "";
                    int n = 0;
                    auto append = [&](const char* tag, int16_t si) {
                        if (si < 0 || !RgbFromPartIndex(si, &sr, &sg, &sb)) return;
                        n += sprintf_s(extra + n, sizeof(extra) - n, " %s %d=%d,%d,%d", tag, (int)si, sr, sg, sb);
                    };
                    append("base", Paint::BasePaintIndex(me.custom, CUSTOM_RECORD_SIZE));
                    append("v0", Paint::SlotIndex(me.custom, CUSTOM_RECORD_SIZE, 79));
                    append("v1", Paint::SlotIndex(me.custom, CUSTOM_RECORD_SIZE, 80));
                    append("v2", Paint::SlotIndex(me.custom, CUSTOM_RECORD_SIZE, 81));
                    append("v3", Paint::SlotIndex(me.custom, CUSTOM_RECORD_SIZE, 82));
                    LOG("Blip: local paint %06X (index %d, %s)%s", c & 0xFFFFFF, (int)idx, how && *how ? how : "none", extra);
                }
            }
        }
        const uint8_t* rec = nullptr;
        uint32_t hash = 0;
        if (g_haveDebugFriend && g_debugFriendCar.hasCustom) {
            rec = g_debugFriendCar.custom;
            hash = g_debugFriendCar.customHash;
        } else if (const CarInfo* p = Net::PeerCar()) {
            if (p->hasCustom) { rec = p->custom; hash = p->customHash; }
        }
        if (rec && (hash != g_blipPeerHash || !g_blipPeer)) {
            int16_t idx = -1; const char* how = "";
            uint32_t c = ColorFromCustom(rec, &idx, &how);
            if (c != g_blipPeer || hash != g_blipPeerHash) {
                g_blipPeer = c;
                g_blipPeerHash = hash;
                LOG("Blip: friend paint %06X (index %d, %s)", c & 0xFFFFFF, (int)idx, how && *how ? how : "none");
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

#ifdef MWT_DEVTOOLS
static void KillDebugFriend() {
    if (!g_debugFriend) return;
    __try { KillVehicle(g_debugFriend); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    g_debugFriend = nullptr;
}

static bool DebugFakeFriendImpl(char* out, size_t n) {
    __try {
        CarInfo me{};
        if (!Game::ReadLocalCarInfo(me) || !me.hasCustom) {
            strcpy_s(out, n, "no local customization record");
            return false;
        }
        if (!PaintFnsOk()) {
            strcpy_s(out, n, "paint lookup not available on this exe");
            return false;
        }

        int bestScore = -1, bestR = 0, bestG = 0, bestB = 0;
        int16_t bestIdx = -1;
        for (int i = 0; i < 20000; ++i) {
            void* part = ((void*(__thiscall*)(void*, int))kGetCarPartByIndex)((void*)kCarPartDB, i);
            if (!part) break;
            int r = 0, g = 0, b = 0;
            if (!RgbFromPartIndex((int16_t)i, &r, &g, &b)) continue;
            int sc = Paint::PurpleScore(r, g, b);
            if (sc > bestScore) { bestScore = sc; bestIdx = (int16_t)i; bestR = r; bestG = g; bestB = b; }
        }
        if (bestIdx < 0 || bestScore < 0) {
            strcpy_s(out, n, "no purple BASE_PAINT swatch in the car-part database");
            return false;
        }

        KillDebugFriend();

        CarInfo fake = me;
        Paint::SetBasePaintIndex(fake.custom, CUSTOM_RECORD_SIZE, bestIdx);
        fake.customHash = CustomHashOf(fake.custom, fake.carKey);

        void* lrb = RigidBodyOf(LocalVehicle());
        if (!lrb) { strcpy_s(out, n, "no local car"); return false; }
        const float* p = RB_GetVec(lrb, RB_GetPosition);
        float fwd[3] = { 1, 0, 0 };
        float pos[3] = { p[0] + 8.f, p[1], p[2] + 1.f };
        void* iv = SpawnVehicle(fake.carKey, fake.custom, pos, fwd, DRV_REMOTE);
        if (!iv) {
            sprintf_s(out, n, "spawn failed (key %08X, purple index %d)", fake.carKey, (int)bestIdx);
            return false;
        }
        g_debugFriend = iv;
        g_debugFriendCar = fake;
        g_haveDebugFriend = true;
        g_blipPeer = 0;
        g_blipPeerHash = 0;
        Game::RefreshBlipColors();

        int lr = 0, lg = 0, lb = 0;
        int16_t lidx = Paint::BasePaintIndex(me.custom, CUSTOM_RECORD_SIZE);
        RgbFromPartIndex(lidx, &lr, &lg, &lb);
        sprintf_s(out, n, "fake friend purple index %d rgb %d %d %d (blip %06X); local index %d rgb %d %d %d (blip %06X)",
                  (int)bestIdx, bestR, bestG, bestB, Game::PeerBlipColor() & 0xFFFFFF,
                  (int)lidx, lr, lg, lb, Game::LocalBlipColor() & 0xFFFFFF);
        LOG("DebugFakeFriend: %s", out);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        strcpy_s(out, n, "exception while spawning fake friend");
        return false;
    }
}

std::string Game::DebugFakeFriend() {
    if (!g_ok || GameFlowState() != 6) return "not in game";
    char buf[240];
    DebugFakeFriendImpl(buf, sizeof(buf));
    return buf;
}
#endif
