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
