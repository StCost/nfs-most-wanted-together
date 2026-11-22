#pragma once
// The "Together" settings page (src/together.cpp): everything that doesn't need the game, so the tests
// cover it - the settings and their checks, the ini lines they are saved as, the keys the page reads and
// the Widescreen Fix line it rewrites.
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace TogetherUtil {

constexpr int kMaxName = 15;      // the hello packet carries 15 characters + the colour bytes
constexpr int kMaxHost = 63;

// Map colours the page cycles through (RRGGBB; 0 = the game's own colour). A colour from the ini that
// isn't in the list is kept and shown as "custom".
static const uint32_t kColours[] = { 0, 0xFF8C00, 0xE53935, 0x43A047, 0x1E88E5, 0x00BCD4, 0xD81B60, 0xFDD835, 0xFFFFFF };
static const char* const kColourNames[] = { "Game", "Orange", "Red", "Green", "Blue", "Cyan", "Pink", "Yellow", "White" };
constexpr int kColourCount = (int)(sizeof(kColours) / sizeof(kColours[0]));

struct Settings {
    std::string name = "Player";
    bool        host = true;
    std::string hostIp;
    std::string port = "47555";        // text while editing; checked on save
    uint32_t    colour = 0;            // RRGGBB, 0 = game colour
    bool        skipIntro = false;
    bool        directFreeRoam = true;
    bool        skipLeaveSafeHouse = true;
    bool        skipPursuitResults = false;
    bool        introInput = true;
    bool        gamepadHotplug = true;
    bool        overlay = false;
    bool        noPursuitCams = true;
    int         menuSpeed = 2;          // 1..3
};

inline std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

// a.b.c.d, each 0-255
inline bool IsIPv4(const std::string& h) {
    int parts = 0; size_t i = 0;
    while (i <= h.size()) {
        size_t e = h.find('.', i); if (e == std::string::npos) e = h.size();
        std::string p = h.substr(i, e - i);
        if (p.empty() || p.size() > 3 || p.find_first_not_of("0123456789") != std::string::npos || atoi(p.c_str()) > 255) return false;
        ++parts; i = e + 1;
    }
    return parts == 4;
}

// "" when the settings can be saved, else what to fix (shown on the page)
inline std::string Validate(const Settings& s) {
    std::string n = Trim(s.name);
    if (n.empty()) return "Enter your nickname.";
    if ((int)n.size() > kMaxName) return "The nickname is too long (15 characters at most).";
    for (char c : n) if ((unsigned char)c < 32 || (unsigned char)c > 126) return "The nickname may only use plain letters, digits and - _ .";
    std::string p = Trim(s.port);
    if (p.empty() || p.size() > 5) return "The port must be a number from 1 to 65535.";
    for (char c : p) if (c < '0' || c > '9') return "The port must be a number from 1 to 65535.";
    long v = strtol(p.c_str(), nullptr, 10);
    if (v < 1 || v > 65535) return "The port must be a number from 1 to 65535.";
    if (!s.host) {
        std::string h = Trim(s.hostIp);
        if (h.empty()) return "Enter the host's IP address to join.";
        if ((int)h.size() > kMaxHost) return "The host's address is too long.";
        for (char c : h) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '-'))
            return "The host's address may only use digits, letters, '.' and '-'.";
        if (h.find_first_not_of("0123456789.") == std::string::npos && !IsIPv4(h))   // all digits: must be a.b.c.d
            return "That IP address isn't valid (like 192.168.0.10).";
    }
    if (s.menuSpeed < 1 || s.menuSpeed > 3) return "Menu speed must be 1x, 2x or 3x.";
    return "";
}

inline std::string ColourText(uint32_t rgb) {
    if (!rgb) return "";
    char b[8]; snprintf(b, sizeof(b), "%06X", (unsigned)(rgb & 0xFFFFFF)); return b;
}
inline int ColourIndex(uint32_t rgb) {
    for (int i = 0; i < kColourCount; ++i) if (kColours[i] == (rgb & 0xFFFFFF)) return i;
    return -1;   // custom
}
inline uint32_t NextColour(uint32_t rgb, int dir) {
    int i = ColourIndex(rgb);
    if (i < 0) i = 0;
    i = ((i + dir) % kColourCount + kColourCount) % kColourCount;
    return kColours[i];
}

struct IniLine { const char* section; const char* key; std::string value; };
// What a save writes (the launcher wrote the same keys). HostIP only when joining, like the launcher:
// the host keeps whatever address was typed last time.
inline std::vector<IniLine> ToIni(const Settings& s) {
    std::vector<IniLine> v;
    v.push_back({ "Player", "Name", Trim(s.name) });
    v.push_back({ "Player", "Color", ColourText(s.colour) });
    v.push_back({ "Network", "Mode", s.host ? "host" : "client" });
    if (!s.host) v.push_back({ "Network", "HostIP", Trim(s.hostIp) });
    v.push_back({ "Network", "Port", std::to_string(strtol(Trim(s.port).c_str(), nullptr, 10)) });
    v.push_back({ "Start", "SkipIntro", s.skipIntro ? "1" : "0" });
    v.push_back({ "Start", "DirectFreeRoam", s.directFreeRoam ? "1" : "0" });
    v.push_back({ "Start", "SkipLeaveSafeHouseDialog", s.skipLeaveSafeHouse ? "1" : "0" });
    v.push_back({ "UI", "SkipPursuitResults", s.skipPursuitResults ? "1" : "0" });
    v.push_back({ "UI", "IntroInput", s.introInput ? "1" : "0" });
    v.push_back({ "Input", "GamepadHotplug", s.gamepadHotplug ? "1" : "0" });
    v.push_back({ "UI", "MenuSpeed", std::to_string(s.menuSpeed) });
    v.push_back({ "Debug", "Overlay", s.overlay ? "1" : "0" });
    v.push_back({ "Coop", "NoPursuitCams", s.noPursuitCams ? "1" : "0" });
    v.push_back({ "UI", "TogetherSetupDone", "1" });
    return v;
}

// Changes that need a new connection (name and colour travel in the hello packet)
inline bool NetworkChanged(const Settings& a, const Settings& b) {
    return Trim(a.name) != Trim(b.name) || a.colour != b.colour || a.host != b.host || Trim(a.port) != Trim(b.port)
        || (!b.host && Trim(a.hostIp) != Trim(b.hostIp));
}

// DirectInput key code -> character for the text fields (US layout, whatever the Windows layout is: the
// nickname goes over the network as plain ASCII). 0 = not a character key.
inline char DikToChar(uint8_t dik, bool shift) {
    static const char row1[] = "1234567890";                       // DIK_1 0x02 .. DIK_0 0x0B
    if (dik >= 0x02 && dik <= 0x0B) return row1[dik - 0x02];
    static const char q[] = "qwertyuiop";                          // 0x10..0x19
    static const char a[] = "asdfghjkl";                           // 0x1E..0x26
    static const char z[] = "zxcvbnm";                             // 0x2C..0x32
    char c = 0;
    if (dik >= 0x10 && dik <= 0x19) c = q[dik - 0x10];
    else if (dik >= 0x1E && dik <= 0x26) c = a[dik - 0x1E];
    else if (dik >= 0x2C && dik <= 0x32) c = z[dik - 0x2C];
    if (c) return shift ? (char)(c - 'a' + 'A') : c;
    switch (dik) {
    case 0x0C: return shift ? '_' : '-';   // DIK_MINUS
    case 0x34: return '.';                 // DIK_PERIOD
    case 0x53: return '.';                 // DIK_DECIMAL
    case 0x39: return ' ';                 // DIK_SPACE
    case 0x4A: return '-';                 // DIK_SUBTRACT
    case 0x52: return '0'; case 0x4F: return '1'; case 0x50: return '2'; case 0x51: return '3';   // numpad
    case 0x4B: return '4'; case 0x4C: return '5'; case 0x4D: return '6';
    case 0x47: return '7'; case 0x48: return '8'; case 0x49: return '9';
    }
    return 0;
}
// Which characters a field takes
enum class Field { Name, Host, Port };
inline bool Accepts(Field f, char c) {
    if (f == Field::Port) return c >= '0' && c <= '9';
    if (f == Field::Host) return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '-';
    return c >= 32 && c <= 126;
}
inline int MaxLen(Field f) { return f == Field::Name ? kMaxName : f == Field::Host ? kMaxHost : 5; }
// Gamepad text entry: up / down step the last character through this list
inline char StepChar(Field f, char c, int dir) {
    static const char name[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_. ";
    static const char host[] = "0123456789.abcdefghijklmnopqrstuvwxyz-";
    static const char port[] = "0123456789";
    const char* set = f == Field::Name ? name : f == Field::Host ? host : port;
    int n = (int)strlen(set);
    const char* p = strchr(set, c);
    int i = p && c ? (int)(p - set) : (dir > 0 ? -1 : 0);
    i = ((i + dir) % n + n) % n;
    return set[i];
}

// The Widescreen Fix's own SkipIntro = 1 skips every movie, the career's story movies too: set it to 0
// (keeping the rest of the line). Returns true if the line changed.
inline bool WidescreenFixLine(std::string& line) {
    size_t i = line.find_first_not_of(" \t");
    if (i == std::string::npos || line.size() - i < 9) return false;
    std::string head = line.substr(i, 9);
    for (char& c : head) c = (char)tolower((unsigned char)c);
    if (head != "skipintro") return false;
    size_t j = line.find_first_not_of(" \t", i + 9);
    if (j == std::string::npos || line[j] != '=') return false;
    size_t k = line.find_first_not_of(" \t", j + 1);
    if (k == std::string::npos || line[k] < '0' || line[k] > '9') return false;
    size_t e = k; while (e < line.size() && line[e] >= '0' && line[e] <= '9') ++e;
    if (line.substr(k, e - k) == "0") return false;
    line = line.substr(0, k) + "0" + line.substr(e);
    return true;
}

}  // namespace TogetherUtil
