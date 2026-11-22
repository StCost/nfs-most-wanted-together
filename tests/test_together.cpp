// The in-game "Together" settings page: src/togetherutil.h
#include "mwt_test.h"
#include "../src/togetherutil.h"
using namespace TogetherUtil;

static std::string IniValue(const std::vector<IniLine>& v, const char* sec, const char* key) {
    for (const IniLine& l : v) if (!strcmp(l.section, sec) && !strcmp(l.key, key)) return l.value;
    return "<missing>";
}

TEST(together_defaults_are_valid) {
    Settings s;
    CHECK_STR(Validate(s).c_str(), "");
}

TEST(together_validate) {
    Settings s;
    s.name = "  "; CHECK(!Validate(s).empty());
    s.name = "1234567890123456"; CHECK(!Validate(s).empty());     // 16: too long
    s.name = "123456789012345"; CHECK_STR(Validate(s).c_str(), "");
    s.name = "Bob"; s.port = "0"; CHECK(!Validate(s).empty());
    s.port = "65536"; CHECK(!Validate(s).empty());
    s.port = "47a"; CHECK(!Validate(s).empty());
    s.port = " 47555 "; CHECK_STR(Validate(s).c_str(), "");
    s.host = false; s.hostIp = ""; CHECK(!Validate(s).empty());     // joining needs the host's address
    s.hostIp = "192.168.0.200"; CHECK_STR(Validate(s).c_str(), "");
    s.hostIp = "my-pc.lan"; CHECK_STR(Validate(s).c_str(), "");
    s.hostIp = "1.2.3.4;x"; CHECK(!Validate(s).empty());
    s.hostIp = "127.0.0.1192.168.1.50"; CHECK(!Validate(s).empty());   // two addresses run together
    s.hostIp = "192.168.0.256"; CHECK(!Validate(s).empty());
    s.hostIp = "10.0.0"; CHECK(!Validate(s).empty());
    s.hostIp = "10.0.0.1."; CHECK(!Validate(s).empty());
    CHECK(IsIPv4("0.0.0.0")); CHECK(IsIPv4("255.255.255.255")); CHECK(!IsIPv4("1..2.3"));
    s.host = true; s.hostIp = "junk;"; CHECK_STR(Validate(s).c_str(), "");   // ignored when hosting
}

TEST(together_ini_lines) {
    Settings s; s.name = " Kostya "; s.host = false; s.hostIp = "10.0.0.2"; s.port = "47556"; s.colour = 0xFF8C00;
    s.menuSpeed = 3; s.overlay = true;
    auto v = ToIni(s);
    CHECK_STR(IniValue(v, "Player", "Name").c_str(), "Kostya");
    CHECK_STR(IniValue(v, "Player", "Color").c_str(), "FF8C00");
    CHECK_STR(IniValue(v, "Network", "Mode").c_str(), "client");
    CHECK_STR(IniValue(v, "Network", "HostIP").c_str(), "10.0.0.2");
    CHECK_STR(IniValue(v, "Network", "Port").c_str(), "47556");
    CHECK_STR(IniValue(v, "UI", "MenuSpeed").c_str(), "3");
    CHECK_STR(IniValue(v, "Debug", "Overlay").c_str(), "1");
    CHECK_STR(IniValue(v, "UI", "TogetherSetupDone").c_str(), "1");
    s.host = true; s.colour = 0;
    v = ToIni(s);
    CHECK_STR(IniValue(v, "Network", "Mode").c_str(), "host");
    CHECK_STR(IniValue(v, "Network", "HostIP").c_str(), "<missing>");   // the host keeps the last typed address
    CHECK_STR(IniValue(v, "Player", "Color").c_str(), "");
}

TEST(together_network_changed) {
    Settings a, b;
    CHECK(!NetworkChanged(a, b));
    b.overlay = true; b.menuSpeed = 3; CHECK(!NetworkChanged(a, b));
    b = a; b.name = "X"; CHECK(NetworkChanged(a, b));
    b = a; b.host = false; CHECK(NetworkChanged(a, b));
    b = a; b.hostIp = "1.2.3.4"; CHECK(!NetworkChanged(a, b));            // host: the address isn't used
    a.host = b.host = false; b.hostIp = "1.2.3.4"; CHECK(NetworkChanged(a, b));
    b = a; b.colour = 0x43A047; CHECK(NetworkChanged(a, b));
}

TEST(together_colours) {
    CHECK_EQ(ColourIndex(0), 0);
    CHECK_EQ(ColourIndex(0x123456), -1);
    CHECK_EQ(NextColour(0, 1), kColours[1]);
    CHECK_EQ(NextColour(0, -1), kColours[kColourCount - 1]);
    CHECK_EQ(NextColour(0x123456, 1), kColours[1]);                    // custom -> the list
    CHECK_STR(ColourText(0xFF00FF00u).c_str(), "00FF00");
}

TEST(together_keys) {
    CHECK_EQ(DikToChar(0x1E, false), 'a');
    CHECK_EQ(DikToChar(0x1E, true), 'A');
    CHECK_EQ(DikToChar(0x0B, false), '0');
    CHECK_EQ(DikToChar(0x34, false), '.');
    CHECK_EQ(DikToChar(0x0C, true), '_');
    CHECK_EQ(DikToChar(0x4F, false), '1');
    CHECK_EQ(DikToChar(0x1C, false), 0);                               // Enter isn't a character
    CHECK(Accepts(Field::Port, '5')); CHECK(!Accepts(Field::Port, '.'));
    CHECK(Accepts(Field::Host, '.')); CHECK(!Accepts(Field::Host, ' '));
    CHECK(Accepts(Field::Name, ' '));
    CHECK_EQ(StepChar(Field::Port, '9', 1), '0');
    CHECK_EQ(StepChar(Field::Port, '0', -1), '9');
    CHECK_EQ(StepChar(Field::Host, 0, 1), '0');
}

TEST(together_widescreen_fix_line) {
    std::string l = "SkipIntro = 1 ; skips all movies";
    CHECK(WidescreenFixLine(l)); CHECK_STR(l.c_str(), "SkipIntro = 0 ; skips all movies");
    l = "skipintro=1"; CHECK(WidescreenFixLine(l)); CHECK_STR(l.c_str(), "skipintro=0");
    l = "SkipIntro = 0"; CHECK(!WidescreenFixLine(l));
    l = "SkipIntroMovies = 1"; CHECK(!WidescreenFixLine(l));
    l = "; SkipIntro = 1"; CHECK(!WidescreenFixLine(l));
}

// ---- the game's own menus (src/nativeutil.h) ----
#include "../src/nativeutil.h"
using namespace NativeUtil;

TEST(native_merge_strings) {
    StringRec game[] = { { 5, "five" }, { 10, "ten" }, { 0xF0000000u, "big" } };
    std::vector<StringRec> ours = { { 7, "seven" }, { 10, "mine" }, { 1, "one" }, { 0x80000001u, "high" } };
    auto v = MergeStrings(game, 3, ours);
    CHECK_EQ((int)v.size(), 6);
    for (size_t i = 1; i < v.size(); ++i) CHECK(v[i - 1].hash < v[i].hash);   // sorted (unsigned)
    CHECK_STR(v[0].str, "one");
    CHECK_STR(v[3].str, "ten");             // the game's string wins
    CHECK_STR(v[4].str, "high");
    CHECK_EQ((int)MergeStrings(nullptr, 0, ours).size(), 4);
}

TEST(native_clean_text) {
    CHECK_STR(CleanText(Field::Host, "192.168.0.10 ").c_str(), "192.168.0.10");
    CHECK_STR(CleanText(Field::Port, "47a555").c_str(), "47555");
    CHECK_STR(CleanText(Field::Name, "  Kostya\xC3\xA9  ").c_str(), "Kostya");
    CHECK_STR(CleanText(Field::Name, "1234567890123456789").c_str(), "123456789012345");
}

TEST(native_profile_label) {
    CHECK_STR(ProfileLabel("StCost", "Friend", true).c_str(), "StCost & Friend");
    CHECK_STR(ProfileLabel("StCost", "Friend", false).c_str(), "StCost");
    CHECK_STR(ProfileLabel("StCost", "", true).c_str(), "StCost");
    CHECK_STR(ProfileLabel("StCost", " Fr\xC3\xA9nd ", true).c_str(), "StCost & Frnd");
    CHECK_STR(ProfileLabel("", "Friend", true).c_str(), "Friend");
    CHECK_STR(ProfileLabel("StCost", "12345678901234567890", true).c_str(), "StCost & 123456789012345");
}

TEST(native_start_plan) {
    StartPlan p = PlanStart(true, true, true);
    CHECK(!p.askAtMenu); CHECK(!p.openPage); CHECK(p.netAtStart);
    p = PlanStart(true, false, true);
    CHECK(p.askAtMenu); CHECK(!p.openPage); CHECK(!p.netAtStart);
    p = PlanStart(false, true, true);                     // first start
    CHECK(p.askAtMenu); CHECK(!p.netAtStart);
    p = PlanStart(false, true, false);                    // the menus couldn't be extended: the page
    CHECK(!p.askAtMenu); CHECK(p.openPage); CHECK(!p.netAtStart);
}

TEST(native_auto_connect_ini) {
    Settings s; s.autoConnect = false;
    CHECK_STR(IniValue(ToIni(s), "Network", "AutoConnect").c_str(), "0");
}
