#pragma once
// Notices in the centre of the screen (tests\test_notice.cpp). The game's HUD message holds at most 63
// characters (it copies 64 bytes, 0x568099), so a longer notice is shown in parts, one after another.
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

namespace Notice {
    constexpr size_t MaxChars = 63;

    // Split at " - " first (our notices are "what - what to do"), packing pieces back together while they
    // fit; a piece still too long is split between words (a word longer than the limit is cut). Parts after
    // the first start with a capital letter.
    inline std::vector<std::string> Split(const std::string& text, size_t maxChars = MaxChars) {
        std::vector<std::string> out;
        if (text.size() <= maxChars) { if (!text.empty()) out.push_back(text); return out; }
        std::vector<std::string> pieces;
        for (size_t p = 0;;) {
            size_t q = text.find(" - ", p);
            pieces.push_back(text.substr(p, q == std::string::npos ? std::string::npos : q - p));
            if (q == std::string::npos) break;
            p = q + 3;
        }
        std::string cur;
        auto flush = [&]() { if (!cur.empty()) { out.push_back(cur); cur.clear(); } };
        for (std::string piece : pieces) {
            if (!cur.empty() && cur.size() + 3 + piece.size() <= maxChars) { cur += " - " + piece; continue; }
            flush();
            while (piece.size() > maxChars) {
                size_t cut = piece.rfind(' ', maxChars);
                if (cut == std::string::npos || cut == 0) cut = maxChars;
                out.push_back(piece.substr(0, cut));
                piece = piece.substr(cut);
                while (!piece.empty() && piece[0] == ' ') piece.erase(0, 1);
            }
            cur = piece;
        }
        flush();
        for (size_t i = 1; i < out.size(); ++i) out[i][0] = (char)toupper((unsigned char)out[i][0]);
        return out;
    }

    // "<name> joined": one notice, with the other player's name. The connection comes up with their
    // first packet; their name comes with their hello a moment later, so the notice waits for it (up to
    // JoinNameWaitMs, then a generic one). Lost before the name came: no join notice.
    constexpr uint32_t JoinNameWaitMs = 3000;
    struct JoinNotice { bool wasConnected = false; bool pending = false; uint32_t since = 0; };
    // Every frame. Returns the notice to show now, or "".
    inline std::string JoinTick(JoinNotice& j, bool connected, const std::string& name, uint32_t now,
                                uint32_t waitMs = JoinNameWaitMs) {
        if (connected && !j.wasConnected) { j.pending = true; j.since = now; }
        if (!connected) j.pending = false;
        j.wasConnected = connected;
        if (!j.pending) return {};
        if (!name.empty()) { j.pending = false; return name + " joined"; }
        if (now - j.since >= waitMs) { j.pending = false; return "The other player joined"; }
        return {};
    }
}

