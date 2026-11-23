#pragma once
// Dev-only UDP network simulator (delay, jitter, loss, optional duplicate / reorder).
// Pure helpers so the tests cover the decisions; net.cpp applies them to send and receive
// under MWT_DEVTOOLS. Release builds do not compile that path and ignore the ini keys.
#include <cstdint>
#include <algorithm>

namespace NetSimUtil {

struct Settings {
    int delayMs = 0;
    int jitterMs = 0;
    int lossPercent = 0;
    int duplicatePercent = 0;
    int reorderPercent = 0;
};

inline Settings ClampSettings(Settings s) {
    auto clamp = [](int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); };
    s.delayMs = clamp(s.delayMs, 0, 2000);
    s.jitterMs = clamp(s.jitterMs, 0, 1000);
    s.lossPercent = clamp(s.lossPercent, 0, 100);
    s.duplicatePercent = clamp(s.duplicatePercent, 0, 100);
    s.reorderPercent = clamp(s.reorderPercent, 0, 100);
    return s;
}

inline bool Enabled(const Settings& s) {
    return s.delayMs > 0 || s.jitterMs > 0 || s.lossPercent > 0
        || s.duplicatePercent > 0 || s.reorderPercent > 0;
}

// Delay in ms: delayMs + uniform jitter in [-jitterMs, +jitterMs], not below 0.
// randU32 is any 32-bit value (the caller supplies the RNG).
inline int ComputeDelay(const Settings& s, uint32_t randU32) {
    int d = s.delayMs;
    if (s.jitterMs > 0) {
        int span = s.jitterMs * 2 + 1;
        int j = (int)(randU32 % (uint32_t)span) - s.jitterMs;
        d += j;
    }
    return d < 0 ? 0 : d;
}

struct Decision {
    bool drop = false;
    int  copies = 1;       // 0 if dropped, 1 normally, 2 if duplicated
    int  delayMs[2] = {0, 0};
};

// rLoss / rDup / rReorder / rJitter / rJitter2 are independent random words.
// Percentages compare rand % 100 against the configured rate (0-99).
inline Decision Decide(const Settings& s, uint32_t rLoss, uint32_t rDup, uint32_t rReorder,
                       uint32_t rJitter, uint32_t rJitter2) {
    Decision d;
    if (s.lossPercent > 0 && (int)(rLoss % 100u) < s.lossPercent) {
        d.drop = true;
        d.copies = 0;
        return d;
    }
    d.delayMs[0] = ComputeDelay(s, rJitter);
    if (s.reorderPercent > 0 && (int)(rReorder % 100u) < s.reorderPercent) {
        int extra = s.delayMs > 0 ? (s.delayMs / 2 + s.jitterMs + 1) : 30;
        d.delayMs[0] += extra;
    }
    if (s.duplicatePercent > 0 && (int)(rDup % 100u) < s.duplicatePercent) {
        d.copies = 2;
        d.delayMs[1] = ComputeDelay(s, rJitter2);
        if (d.delayMs[1] <= d.delayMs[0]) d.delayMs[1] = d.delayMs[0] + 1;
    }
    return d;
}

}  // namespace NetSimUtil
