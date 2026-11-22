#pragma once
// FECustomizationRecord is a table of 16-bit part indices (Unlimiter / MW v1.3).
// Body colour is CARSLOTID_BASE_PAINT = 0x4C (slot 76). The RGB itself lives on
// that CarPart as the RED / GREEN / BLUE attributes and is read at runtime.
#include <cstdint>
#include <cstring>

namespace Paint {
    constexpr int kNumCarSlots = 139;
    constexpr int kBasePaintSlot = 76;
    constexpr int kBasePaintOff = kBasePaintSlot * 2; // 0x98

    inline int16_t SlotIndex(const uint8_t* rec, int size, int slot) {
        if (!rec || slot < 0 || size < (slot + 1) * 2) return -1;
        int16_t idx = 0;
        memcpy(&idx, rec + slot * 2, sizeof(idx));
        return idx;
    }

    inline int16_t BasePaintIndex(const uint8_t* rec, int size) {
        return SlotIndex(rec, size, kBasePaintSlot);
    }

    inline void SetBasePaintIndex(uint8_t* rec, int size, int16_t idx) {
        if (!rec || size < kBasePaintOff + 2) return;
        memcpy(rec + kBasePaintOff, &idx, sizeof(idx));
    }

    inline uint32_t PackArgb(int r, int g, int b) {
        auto clip = [](int x) -> uint32_t {
            if (x < 0) return 0;
            if (x > 255) return 255;
            return (uint32_t)x;
        };
        return 0xFF000000u | (clip(r) << 16) | (clip(g) << 8) | clip(b);
    }

    // Higher is more like a vivid purple (high blue, some red, little green).
    // Returns -1 when the swatch is clearly not purple.
    inline int PurpleScore(int r, int g, int b) {
        if (b < 70 || b <= g || r <= g) return -1;
        return b * 2 + r - g * 3;
    }
}
