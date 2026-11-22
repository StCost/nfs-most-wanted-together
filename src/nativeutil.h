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
    W_NAME, W_MODE, W_HOSTIP, W_AUTO, W_START, W_INTROINPUT, W_PADHOTPLUG, W_MENUSPEED, W_COUNT
};
enum class Screen { Settings, Connect };

inline std::vector<int> TogetherRows(Screen /*screen*/, bool host) {
    std::vector<int> v;
    v.push_back(W_NAME);
    v.push_back(W_MODE);
    if (!host) v.push_back(W_HOSTIP);
    v.push_back(W_AUTO);
    v.push_back(W_START);
    v.push_back(W_INTROINPUT);
    v.push_back(W_PADHOTPLUG);
    v.push_back(W_MENUSPEED);
    return v;
}

// The action row: Host mode uses Start Host / Stop Host; Join mode uses Connect / Disconnect.
inline const char* ConnectionActionTitle(bool host, bool running) {
    if (host) return running ? "Stop Host" : "Start Host";
    return running ? "Disconnect" : "Connect";
}

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
