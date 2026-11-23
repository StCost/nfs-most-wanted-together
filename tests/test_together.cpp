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
    Settings s; s.name = " Kostya "; s.host = false; s.hostIp = "10.0.0.2"; s.port = "47556";
    s.menuSpeed = 3; s.overlay = true;
    auto v = ToIni(s);
    CHECK_STR(IniValue(v, "Player", "Name").c_str(), "Kostya");
    CHECK_STR(IniValue(v, "Player", "Color").c_str(), "<missing>");
    CHECK_STR(IniValue(v, "Start", "SkipIntro").c_str(), "<missing>");
    CHECK_STR(IniValue(v, "Start", "DirectFreeRoam").c_str(), "<missing>");
    CHECK_STR(IniValue(v, "Start", "SkipLeaveSafeHouseDialog").c_str(), "<missing>");
    CHECK_STR(IniValue(v, "UI", "SkipPursuitResults").c_str(), "<missing>");
    CHECK_STR(IniValue(v, "Coop", "NoPursuitCams").c_str(), "<missing>");
    CHECK_STR(IniValue(v, "Network", "Mode").c_str(), "client");
    CHECK_STR(IniValue(v, "Network", "HostIP").c_str(), "10.0.0.2");
    CHECK_STR(IniValue(v, "Network", "Port").c_str(), "47556");
    CHECK_STR(IniValue(v, "UI", "MenuSpeed").c_str(), "3");
    CHECK_STR(IniValue(v, "Debug", "Overlay").c_str(), "1");
    CHECK_STR(IniValue(v, "UI", "TogetherSetupDone").c_str(), "1");
    s.host = true;
    v = ToIni(s);
    CHECK_STR(IniValue(v, "Network", "Mode").c_str(), "host");
    CHECK_STR(IniValue(v, "Network", "HostIP").c_str(), "<missing>");   // the host keeps the last typed address
}

TEST(together_network_changed) {
    Settings a, b;
    CHECK(!NetworkChanged(a, b));
    b.overlay = true; b.menuSpeed = 3; CHECK(!NetworkChanged(a, b));
    b = a; b.name = "X"; CHECK(NetworkChanged(a, b));
    b = a; b.host = false; CHECK(NetworkChanged(a, b));
    b = a; b.hostIp = "1.2.3.4"; CHECK(!NetworkChanged(a, b));            // host: the address isn't used
    a.host = b.host = false; b.hostIp = "1.2.3.4"; CHECK(NetworkChanged(a, b));
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

TEST(native_together_rows) {
    auto hostSet = TogetherRows(Screen::Settings, true);
    auto joinSet = TogetherRows(Screen::Settings, false);
    auto hostCon = TogetherRows(Screen::Connect, true);
    auto joinCon = TogetherRows(Screen::Connect, false);
    // One Together list for every entry (main, pause, Options): join IP only when joining
    CHECK(std::find(hostCon.begin(), hostCon.end(), W_HOSTIP) == hostCon.end());
    CHECK(std::find(joinCon.begin(), joinCon.end(), W_HOSTIP) != joinCon.end());
    CHECK(std::find(hostCon.begin(), hostCon.end(), W_START) != hostCon.end());
    CHECK(std::find(joinCon.begin(), joinCon.end(), W_START) != joinCon.end());
    CHECK(std::find(hostCon.begin(), hostCon.end(), W_NAME) != hostCon.end());
    CHECK(std::find(hostCon.begin(), hostCon.end(), W_AUTO) != hostCon.end());
    CHECK(std::find(hostCon.begin(), hostCon.end(), W_INTROINPUT) != hostCon.end());
    CHECK(std::find(hostCon.begin(), hostCon.end(), W_PADHOTPLUG) != hostCon.end());
    CHECK(std::find(hostCon.begin(), hostCon.end(), W_MENUSPEED) != hostCon.end());
    CHECK_EQ((int)hostCon.size(), 7);
    CHECK_EQ((int)joinCon.size(), 8);
    CHECK((int)hostCon.size() <= 9);
    CHECK((int)joinCon.size() <= 9);
    // Options > Together is the same list
    CHECK_EQ((int)hostSet.size(), (int)hostCon.size());
    CHECK_EQ((int)joinSet.size(), (int)joinCon.size());
    CHECK(hostSet == hostCon);
    CHECK(joinSet == joinCon);
}

TEST(native_scroll_window) {
    // Settings is 10 rows today; the window still has to move if a list grows past the slots
    auto v = NativeUtil::MoveScroll(15, 10, 0, 9, 1);
    CHECK(v.moved);
    CHECK_EQ(v.focus, 10);
    CHECK_EQ(v.scroll, 1);
    CHECK_EQ(NativeUtil::VisibleIndex(15, 10, v.scroll, 10), 9);
    v = NativeUtil::MoveScroll(15, 10, 5, 14, 1);
    CHECK(v.moved);
    CHECK_EQ(v.focus, 0);
    CHECK_EQ(v.scroll, 0);
    v = NativeUtil::MoveScroll(15, 10, 0, 0, -1);
    CHECK(v.moved);
    CHECK_EQ(v.focus, 14);
    CHECK_EQ(v.scroll, 5);
    v = NativeUtil::MoveScroll(15, 10, 0, 3, 1);
    CHECK(!v.moved);
    CHECK_EQ(v.focus, 4);
    CHECK_EQ(v.scroll, 0);
    v = NativeUtil::MoveScroll(4, 10, 0, 0, 1);
    CHECK(!v.moved);
    CHECK_EQ(v.scroll, 0);
    CHECK_EQ(NativeUtil::ClampScroll(15, 10, 9), 5);
    CHECK_EQ(NativeUtil::ClampScroll(4, 10, 2), 0);
    auto set = TogetherRows(Screen::Settings, false);
    CHECK_EQ((int)set.size(), 8);
    CHECK(NativeUtil::VisibleIndex((int)set.size(), 9, 0, 0) == 0);
    int speed = -1;
    for (int i = 0; i < (int)set.size(); ++i) if (set[i] == W_MENUSPEED) speed = i;
    CHECK(speed >= 0);
    CHECK(NativeUtil::VisibleIndex((int)set.size(), 9, 0, speed) >= 0);
}

TEST(native_connection_action_title) {
    CHECK_STR(ConnectionActionTitle(true, false), "Start Host");
    CHECK_STR(ConnectionActionTitle(true, true), "Stop Host");
    CHECK_STR(ConnectionActionTitle(false, false), "Connect");
    CHECK_STR(ConnectionActionTitle(false, true), "Disconnect");
}

TEST(native_soft_edit_package) {
    CHECK(SoftEditPackage("Options.fng"));
    CHECK(SoftEditPackage("Pause_Options.fng"));
    CHECK(SoftEditPackage("options.fng"));
    CHECK(SoftEditPackage("PAUSE_OPTIONS.FNG"));
    CHECK(!SoftEditPackage("MainMenu.fng"));
    CHECK(!SoftEditPackage("SMS_Message.fng"));
    CHECK(!SoftEditPackage(""));
    CHECK(!SoftEditPackage(nullptr));
    CHECK(!BeginEditOnBlockedLeave());
}

TEST(native_soft_wait_release) {
    CHECK(SoftWaitReleaseDone(false, 0));
    CHECK(!SoftWaitReleaseDone(true, 0));
    CHECK(!SoftWaitReleaseDone(true, 249));
    CHECK(SoftWaitReleaseDone(true, 250));
    CHECK(SoftWaitReleaseDone(true, 400));
    CHECK(SoftWaitReleaseDone(false, 10));
    // Esc is not modeled here; the UI always treats Esc as cancel during the wait.
}


// The logo replacement's .dds reader: src/ddsutil.h
#include "../src/ddsutil.h"
static std::vector<uint8_t> MakeDds(uint32_t w, uint32_t h, uint32_t mips, bool dxt5, size_t cut = 0) {
    std::vector<uint8_t> f(128, 0);
    auto put = [&](size_t o, uint32_t v) { memcpy(&f[o], &v, 4); };
    memcpy(&f[0], "DDS ", 4); put(4, 124); put(8, 0x1007 | (mips > 1 ? 0x20000 : 0)); put(12, h); put(16, w); put(28, mips);
    put(76, 32);
    if (dxt5) { put(80, 4); put(84, DdsUtil::FMT_DXT5); }
    else { put(80, 0x41); put(88, 32); put(92, 0x00FF0000); put(96, 0x0000FF00); put(100, 0x000000FF); put(104, 0xFF000000); }
    size_t data = 0;
    uint32_t fmt = dxt5 ? DdsUtil::FMT_DXT5 : DdsUtil::FMT_A8R8G8B8;
    for (uint32_t l = 0; l < mips; ++l) data += DdsUtil::LevelBytes(fmt, DdsUtil::Mip(w, l), DdsUtil::Mip(h, l));
    f.resize(128 + data - cut, 0x11);
    return f;
}

TEST(logo_dds_parse) {
    DdsUtil::Info i; std::string err;
    std::vector<uint8_t> a = MakeDds(512, 128, 1, false);
    CHECK(DdsUtil::Parse(a.data(), a.size(), i, err));
    CHECK(i.width == 512 && i.height == 128 && i.mips == 1 && i.format == DdsUtil::FMT_A8R8G8B8);
    CHECK(a.size() == 128 + 512 * 128 * 4);
    std::vector<uint8_t> b = MakeDds(1024, 256, 11, true);
    CHECK(DdsUtil::Parse(b.data(), b.size(), i, err));
    CHECK(i.mips == 11 && i.format == DdsUtil::FMT_DXT5);
    CHECK(DdsUtil::LevelBytes(DdsUtil::FMT_DXT5, 1, 1) == 16);
    CHECK(DdsUtil::LevelBytes(DdsUtil::FMT_DXT1, 512, 128) == 512 * 128 / 2);
    std::vector<uint8_t> c = MakeDds(512, 128, 1, false, 1);           // one byte short
    CHECK(!DdsUtil::Parse(c.data(), c.size(), i, err));
    std::vector<uint8_t> d = a; memcpy(&d[0], "PNG ", 4);
    CHECK(!DdsUtil::Parse(d.data(), d.size(), i, err));
    CHECK(!DdsUtil::Parse(nullptr, 0, i, err));
}
