#pragma once
// The game's own menus for MW Together (src/nativeui.cpp): the parts that don't need the game, so the
// tests cover them - the merged string table, typed-text cleanup and what happens at start.
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>
#include "togetherutil.h"

namespace NativeUtil {

// One entry of the game's string table (RecordTable, sorted by hash; binary-searched by SearchForString)
struct StringRec { uint32_t hash; const char* str; };

// The game's table plus ours, sorted by hash. A hash the game already has keeps the game's string.
inline std::vector<StringRec> MergeStrings(const StringRec* game, int n, const std::vector<StringRec>& ours) {
    std::vector<StringRec> v(game, game + (n > 0 ? n : 0));
    std::vector<uint32_t> have; have.reserve(v.size());
    for (const StringRec& r : v) have.push_back(r.hash);
    std::sort(have.begin(), have.end());
    for (const StringRec& r : ours)
        if (!std::binary_search(have.begin(), have.end(), r.hash)) v.push_back(r);
    std::stable_sort(v.begin(), v.end(), [](const StringRec& a, const StringRec& b) { return a.hash < b.hash; });
    return v;
}

// Text typed in the game's own text entry: only what the field takes, at most its length
inline std::string CleanText(TogetherUtil::Field f, const std::string& in) {
    std::string out;
    for (char c : in) {
        if (!TogetherUtil::Accepts(f, c)) continue;
        if ((int)out.size() >= TogetherUtil::MaxLen(f)) break;
        out += c;
    }
    return f == TogetherUtil::Field::Name ? TogetherUtil::Trim(out) : out;
}

// What the start does.
//   askAtMenu:  the quick start stops at the main menu until Host / Join is picked there
//   openPage:   the overlay page opens by itself (the game's menus couldn't be extended)
//   netAtStart: host / join right away with the saved settings
struct StartPlan { bool askAtMenu; bool openPage; bool netAtStart; };
inline StartPlan PlanStart(bool setupDone, bool autoConnect, bool nativeMenus) {
    bool ask = !setupDone || !autoConnect;
    return { ask && nativeMenus, ask && !nativeMenus, !ask };
}

// The main menu's profile box: "Me & Friend" while connected, else just the profile name.
// Only characters the menu font has (printable ASCII); each name at most 15.
inline std::string ProfileLabel(const std::string& me, const std::string& peer, bool connected) {
    auto clean = [](const std::string& in) {
        std::string o;
        for (unsigned char c : in) if (c >= 0x20 && c < 0x7F && o.size() < 15) o += (char)c;
        while (!o.empty() && o.back() == ' ') o.pop_back();
        size_t a = o.find_first_not_of(' ');
        return a == std::string::npos ? std::string() : o.substr(a);
    };
    std::string m = clean(me), f = clean(peer);
    if (!connected || f.empty()) return m;
    return m.empty() ? f : m + " & " + f;
}

// One Together screen (main menu Together, pause Together, and Options > Together):
// nickname, Connection, Connect to IP (Join only), Reconnect on launch, the start /
// stop action, then the remaining settings. The list fits in the 9 visible row slots.
enum Row {
    W_NAME, W_MODE, W_HOSTIP, W_AUTO, W_START, W_PADHOTPLUG, W_COUNT
};
enum class Screen { Settings, Connect };

inline std::vector<int> TogetherRows(Screen /*screen*/, bool host) {
    std::vector<int> v;
    v.push_back(W_NAME);
    v.push_back(W_MODE);
    if (!host) v.push_back(W_HOSTIP);
    v.push_back(W_AUTO);
    v.push_back(W_START);
    v.push_back(W_PADHOTPLUG);
    return v;
}

// The action row: Host mode uses Start Host / Stop Host; Join mode uses Connect / Disconnect.
inline const char* ConnectionActionTitle(bool host, bool running) {
    if (host) return running ? "Stop Host" : "Start Host";
    return running ? "Disconnect" : "Connect";
}

// Type into the visible row (do not open the game's text-entry page). The game's
// BeginTextInput hides the value on Pause_Options.fng and, on a mouse click, can
// open a blank page on Options.fng too. Esc ends the edit; Back then leaves.
inline bool SoftEditPackage(const char* pkg) {
    if (!pkg || !pkg[0]) return false;
    auto eq = [](const char* a, const char* b) {
        while (*a && *b) {
            unsigned char ca = (unsigned char)*a++, cb = (unsigned char)*b++;
            if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca + 32);
            if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb + 32);
            if (ca != cb) return false;
        }
        return !*a && !*b;
    };
    return eq(pkg, "Pause_Options.fng") || eq(pkg, "Options.fng");
}

// A blocked EXITCOMPLETE must not start or finish a text edit. A mouse click on
// an Options row can raise EXITCOMPLETE without PAD_ACCEPT; treating that as
// Enter opened a soft-edit and left the player stuck on a blank screen.
inline bool BeginEditOnBlockedLeave() { return false; }

// After soft-edit starts we wait for the opening Enter (or a gamepad-mapped
// Accept that Widescreen Fix may hold) to go up, so that key does not also
// commit the field. Esc always cancels, even during this wait. If the key
// stays down (sticky Accept), stop waiting after SoftWaitReleaseMs.
constexpr unsigned SoftWaitReleaseMs = 250;
inline bool SoftWaitReleaseDone(bool enterDown, unsigned waitedMs,
                                unsigned timeoutMs = SoftWaitReleaseMs) {
    return !enterDown || waitedMs >= timeoutMs;
}

// One key on the Together screen (Enter, Esc), polled each time the game reads its keyboard.
// It acts once per press, on the down edge; a press that acted (or that ended a text edit)
// stays hidden from the game until it is let go, so one press can't also confirm, reopen
// the field or make the screen leave. A press that did not act passes to the game.
struct KeyLatch { bool prev = false; bool eating = false; };
enum class KeyStep { Pass, Act, Swallow };
inline KeyStep StepKey(KeyLatch& k, bool down) {
    bool edge = down && !k.prev;
    k.prev = down;
    if (!down) { k.eating = false; return KeyStep::Pass; }
    if (k.eating) return KeyStep::Swallow;
    return edge ? KeyStep::Act : KeyStep::Pass;
}
// The key was handled somewhere else this poll (the text edit): eat it while it stays down.
inline void HoldKey(KeyLatch& k, bool down) { k.prev = down; k.eating = down; }

// The same for the pad's buttons (XInput bits): a button that was already down when the
// edit started (the A that opened it) has to go up before it counts.
inline bool PadEdge(uint16_t now, uint16_t prev, uint16_t bit) { return (now & bit) && !(prev & bit); }

// The row while it is being typed: the text and a cursor, so it is plain that typing goes there.
inline std::string SoftEditShown(const std::string& text) { return text + "_"; }

// A window of `slots` rows over `count` rows. MoveScroll steps `focus` by `dir` and
// shifts `scroll` only when the new focus would leave the window (including wrap).
// When every row fits, moved is false so the game can change the highlight itself.
struct ScrollView { int scroll; int focus; bool moved; };

inline int ClampScroll(int count, int slots, int scroll) {
    if (slots <= 0 || count <= slots) return 0;
    if (scroll < 0) return 0;
    if (scroll > count - slots) return count - slots;
    return scroll;
}

inline int VisibleIndex(int count, int slots, int scroll, int logical) {
    int sc = ClampScroll(count, slots, scroll);
    int vis = (slots > 0 && count > slots) ? slots : count;
    if (logical < sc || logical >= sc + vis) return -1;
    return logical - sc;
}

inline ScrollView MoveScroll(int count, int slots, int scroll, int focus, int dir) {
    ScrollView o{ scroll, focus, false };
    if (count <= 0) return o;
    if (slots <= 0) slots = count;
    int next = focus + dir;
    if (next < 0) next = count - 1;
    else if (next >= count) next = 0;
    o.focus = next;
    if (count <= slots) { o.scroll = 0; return o; }
    int sc = scroll;
    if (next < sc) sc = next;
    else if (next >= sc + slots) sc = next - slots + 1;
    sc = ClampScroll(count, slots, sc);
    o.scroll = sc;
    o.moved = (sc != scroll);
    return o;
}

}  // namespace NativeUtil
