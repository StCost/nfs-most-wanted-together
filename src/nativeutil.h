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

// Rows on the Together screen (main menu and Options > Together). The join IP row is only shown
// when Connection = Join. The last action row's label depends on whether a session is running.
enum Row {
    W_NAME, W_MODE, W_HOSTIP, W_PORT, W_AUTO, W_COLOUR, W_DIRECT, W_SKIPINTRO, W_SAFEHOUSE, W_PURSUITRES,
    W_INTROINPUT, W_PADHOTPLUG, W_NOPURSUITCAM, W_MENUSPEED, W_START, W_COUNT
};
enum class Screen { Settings, Connect };

inline std::vector<int> TogetherRows(Screen screen, bool host) {
    std::vector<int> v;
    v.push_back(W_NAME);
    v.push_back(W_MODE);
    if (!host) v.push_back(W_HOSTIP);
    if (screen == Screen::Settings) {
        v.push_back(W_PORT);
        v.push_back(W_AUTO);
        v.push_back(W_START);
        v.push_back(W_COLOUR);
        v.push_back(W_DIRECT);
        v.push_back(W_SKIPINTRO);
        v.push_back(W_SAFEHOUSE);
        v.push_back(W_PURSUITRES);
        v.push_back(W_INTROINPUT);
        v.push_back(W_PADHOTPLUG);
        v.push_back(W_NOPURSUITCAM);
        v.push_back(W_MENUSPEED);
    } else {
        v.push_back(W_AUTO);
        v.push_back(W_START);
    }
    return v;
}

// The action row: Host mode uses Start Host / Stop Host; Join mode uses Connect / Disconnect.
inline const char* ConnectionActionTitle(bool host, bool running) {
    if (host) return running ? "Stop Host" : "Start Host";
    return running ? "Disconnect" : "Connect";
}

}  // namespace NativeUtil
