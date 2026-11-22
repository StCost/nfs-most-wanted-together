// The "Together" settings page - see together.h. Pure logic (checks, ini lines, key table) is in
// togetherutil.h with tests in tests\test_together.cpp.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <xinput.h>
#include <string>
#include <vector>
#include <cstdio>
#include "together.h"
#include "togetherutil.h"
#include "nativeutil.h"
#include "config.h"
#include "log.h"
#include "net.h"
#include "overlay.h"

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "xinput9_1_0.lib")

using namespace TogetherUtil;

namespace {
    enum Row { R_NAME, R_MODE, R_HOSTIP, R_PORT, R_AUTOCONNECT, R_COLOUR, R_SKIPINTRO, R_DIRECT, R_SAFEHOUSE, R_PURSUITRES,
               R_INTROINPUT, R_PADHOTPLUG, R_OVERLAY, R_NOPURSUITCAM, R_MENUSPEED, R_SAVE, R_CANCEL, R_COUNT };

    std::string g_ini;
    bool     g_open = false;
    bool     g_setupDone = true;
    bool     g_autoConnect = true;     // [Network] AutoConnect as it was at start
    bool     g_autoShown = false;
    bool     g_native = false;         // the game's own menus have the Together entries
    bool     g_planned = false;
    NativeUtil::StartPlan g_plan = { false, false, true };
    bool     g_chosen = false;         // Host / Join picked (main menu or the page) this run
    bool     g_netRunning = false;
    bool     g_netChange = false;
    bool     g_netStop = false;
    bool     g_dirty = true;
    int      g_sel = R_NAME;
    bool     g_padEdit = false;        // pad: up / down change the last character of the selected field
    Settings g_ed, g_cur;              // being edited / as saved
    Settings g_saved; bool g_haveSaved = false;   // last save (start-only options differ from g_cfg until a restart)
    std::string g_msg; bool g_msgWarn = false;
    std::string g_ips;
    int      g_flow = -1;
    bool     g_hint = false;
    DWORD    g_lastRefresh = 0;
    uint8_t  g_dikOpen = 0x3C;         // DIK of [UI] TogetherKey (F2)
    // After the page closes, the key / button that closed it (Esc, Enter, B, START) is still down: the game
    // must not see it (Esc would open the pause menu), so input stays blocked until it is released.
    bool     g_swallowKb = false, g_swallowPad = false;

    bool IsText(int r) { return r == R_NAME || r == R_HOSTIP || r == R_PORT; }
    Field FieldOf(int r) { return r == R_NAME ? Field::Name : r == R_HOSTIP ? Field::Host : Field::Port; }
    std::string& TextOf(int r) { return r == R_NAME ? g_ed.name : r == R_HOSTIP ? g_ed.hostIp : g_ed.port; }
    bool Hidden(int r) { return r == R_HOSTIP && g_ed.host; }

    std::string ReadIni(const char* sec, const char* key, const char* def) {
        char b[256]; GetPrivateProfileStringA(sec, key, def, b, sizeof(b), g_ini.c_str()); return b;
    }

    Settings FromConfig() {
        Settings s;
        s.name = g_cfg.playerName;
        s.host = g_cfg.isHost;
        s.hostIp = ReadIni("Network", "HostIP", "");         // as typed (g_cfg has 127.0.0.1 when it's empty)
        s.port = std::to_string(g_cfg.port);
        s.colour = g_cfg.playerColor & 0xFFFFFF;
        s.skipIntro = g_cfg.skipIntro;
        s.directFreeRoam = g_cfg.directFreeRoam;
        s.skipLeaveSafeHouse = g_cfg.skipLeaveSafeHouseDialog;
        s.skipPursuitResults = g_cfg.skipPursuitResults;
        s.introInput = g_cfg.uiIntroInput;
        s.gamepadHotplug = g_cfg.gamepadHotplug;
        s.overlay = Overlay::IsVisible();
        s.noPursuitCams = g_cfg.noPursuitCams;
        s.menuSpeed = g_cfg.uiMenuSpeed < 1 ? 1 : g_cfg.uiMenuSpeed > 3 ? 3 : g_cfg.uiMenuSpeed;
        s.autoConnect = ReadIni("Network", "AutoConnect", "1") != "0";
        return s;
    }

    // IPv4 addresses of the adapters that are up (what the other player types to join), like the launcher
    std::string LocalIps() {
        std::string out;
        ULONG len = 16 * 1024;
        std::vector<unsigned char> buf(len);
        auto* aa = (IP_ADAPTER_ADDRESSES*)buf.data();
        ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
        ULONG r = GetAdaptersAddresses(AF_INET, flags, nullptr, aa, &len);
        if (r == ERROR_BUFFER_OVERFLOW) { buf.resize(len); aa = (IP_ADAPTER_ADDRESSES*)buf.data(); r = GetAdaptersAddresses(AF_INET, flags, nullptr, aa, &len); }
        if (r != NO_ERROR) return "?";
        for (auto* a = aa; a; a = a->Next) {
            if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
                auto* sin = (sockaddr_in*)u->Address.lpSockaddr;
                if (!sin || sin->sin_family != AF_INET) continue;
                const uint8_t* b = (const uint8_t*)&sin->sin_addr;
                if (b[0] == 127 || (b[0] == 169 && b[1] == 254)) continue;   // loopback / self-assigned: nobody can reach them
                char s[20]; sprintf_s(s, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
                if (out.find(s) != std::string::npos) continue;
                if (!out.empty()) out += ", ";
                out += s;
            }
        }
        return out.empty() ? "?" : out;
    }

    void Msg(const std::string& m, bool warn) { g_msg = m; g_msgWarn = warn; g_dirty = true; }

    Settings g_pendingNet;
    void ApplyNetwork(const Settings& s) {
        g_cfg.playerName = Trim(s.name);
        g_cfg.isHost = s.host;
        if (!s.host) g_cfg.peerIp = Trim(s.hostIp);
        g_cfg.port = (int)strtol(Trim(s.port).c_str(), nullptr, 10);
        g_cfg.playerColor = s.colour ? (0xFF000000u | s.colour) : 0;
    }

    void Open() {
        if (g_open) return;
        g_cur = Together::Current();
        g_ed = g_cur;
        g_ips = LocalIps();
        g_sel = R_NAME; g_padEdit = false;
        g_msg = g_setupDone ? "" : "Welcome! Pick a nickname, then Host or Join and Save & Play.";
        g_msgWarn = false;
        g_open = true; g_dirty = true;
        LOG("Together: settings page opened%s", g_setupDone ? "" : " (first start)");
    }
    void Close(const char* why) {
        if (!g_open) return;
        g_open = false; g_padEdit = false; g_dirty = true;
        g_swallowKb = g_swallowPad = true;
        if (!g_netRunning && !g_netChange) {     // opened at start instead of connecting: connect now as saved
            g_pendingNet = g_cur; g_netChange = true; g_chosen = true;
        }
        LOG("Together: settings page closed (%s)", why);
    }

    void KeepWidescreenFixMovies() {
        std::string dir = g_ini.substr(0, g_ini.find_last_of("\\/") + 1);
        std::string f = dir + "NFSMostWanted.WidescreenFix.ini";
        FILE* in = nullptr;
        if (fopen_s(&in, f.c_str(), "rb") || !in) return;
        std::string all; char b[4096]; size_t n;
        while ((n = fread(b, 1, sizeof(b), in)) > 0) all.append(b, n);
        fclose(in);
        bool changed = false; std::string out; size_t pos = 0;
        while (pos < all.size()) {
            size_t e = all.find('\n', pos);
            std::string line = all.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
            bool cr = !line.empty() && line.back() == '\r'; if (cr) line.pop_back();
            if (WidescreenFixLine(line)) changed = true;
            out += line; if (cr) out += '\r'; if (e != std::string::npos) out += '\n';
            pos = e == std::string::npos ? all.size() : e + 1;
        }
        if (!changed) return;
        FILE* o = nullptr;
        if (!fopen_s(&o, f.c_str(), "wb") && o) { fwrite(out.data(), 1, out.size(), o); fclose(o); LOG("Together: Widescreen Fix SkipIntro set to 0 (story movies stay)"); }
    }

    // Write the settings and use them. connect: start / restart the connection now (a Host / Join choice)
    // even if the network settings didn't change. Returns "" or what to fix.
    std::string ApplySettings(const Settings& ed, bool connect, bool* restartNeededOut) {
        std::string err = Validate(ed);
        if (!err.empty()) return err;
        Settings cur = Together::Current();
        for (const IniLine& l : ToIni(ed)) WritePrivateProfileStringA(l.section, l.key, l.value.c_str(), g_ini.c_str());
        if (ed.skipIntro) KeepWidescreenFixMovies();
        bool net = NetworkChanged(cur, ed) || (connect && !g_netRunning);
        if (!g_netRunning && !connect) net = false;     // not connected yet (waiting for Host / Join): just remember
        g_pendingNet = ed;    // name / colour / mode / address / port: applied with the reconnect (TakeNetworkChange)
        // skip intros, quick safe house, menu input / speed are patched at start: g_cfg keeps what runs now
        g_cfg.directFreeRoam = ed.directFreeRoam;
        g_cfg.skipPursuitResults = ed.skipPursuitResults;
        g_cfg.gamepadHotplug = ed.gamepadHotplug;
        g_cfg.showOverlay = ed.overlay; Overlay::SetVisible(ed.overlay);
        g_cfg.noPursuitCams = ed.noPursuitCams;
        bool restartNeeded = cur.skipIntro != ed.skipIntro || cur.skipLeaveSafeHouse != ed.skipLeaveSafeHouse
                          || cur.introInput != ed.introInput || cur.menuSpeed != ed.menuSpeed;
        if (net) g_netChange = true;
        else ApplyNetwork(ed);
        if (connect) g_chosen = true;
        g_setupDone = true;
        LOG("Together: saved - name %s, %s%s, port %s%s%s%s", Trim(ed.name).c_str(), ed.host ? "host" : "join ",
            ed.host ? "" : Trim(ed.hostIp).c_str(), Trim(ed.port).c_str(), ed.autoConnect ? ", keep connection on restart" : "",
            net ? ", connecting" : "", restartNeeded ? ", some options apply at the next start" : "");
        g_saved = ed; g_haveSaved = true;
        if (restartNeededOut) *restartNeededOut = restartNeeded;
        return "";
    }

    bool Save() {
        bool restartNeeded = false;
        std::string err = ApplySettings(g_ed, true, &restartNeeded);
        if (!err.empty()) { Msg(err, true); return false; }
        Msg(restartNeeded ? "Saved. Some options apply at the next start." : "Saved.", false);
        g_cur = g_ed;
        return true;
    }

    void Move(int dir) {
        int r = g_sel;
        for (int i = 0; i < R_COUNT; ++i) { r = (r + dir + R_COUNT) % R_COUNT; if (!Hidden(r)) break; }
        g_sel = r; g_padEdit = false; g_dirty = true;
    }

    void Edited() { if (g_msgWarn) { g_msg.clear(); g_msgWarn = false; } g_dirty = true; }

    void Change(int dir) {
        if (g_sel != R_SAVE && g_sel != R_CANCEL) Edited();
        switch (g_sel) {
        case R_MODE: g_ed.host = !g_ed.host; break;
        case R_COLOUR: g_ed.colour = NextColour(g_ed.colour, dir); break;
        case R_SKIPINTRO: g_ed.skipIntro = !g_ed.skipIntro; break;
        case R_DIRECT: g_ed.directFreeRoam = !g_ed.directFreeRoam; break;
        case R_SAFEHOUSE: g_ed.skipLeaveSafeHouse = !g_ed.skipLeaveSafeHouse; break;
        case R_PURSUITRES: g_ed.skipPursuitResults = !g_ed.skipPursuitResults; break;
        case R_INTROINPUT: g_ed.introInput = !g_ed.introInput; break;
        case R_PADHOTPLUG: g_ed.gamepadHotplug = !g_ed.gamepadHotplug; break;
        case R_OVERLAY: g_ed.overlay = !g_ed.overlay; break;
        case R_NOPURSUITCAM: g_ed.noPursuitCams = !g_ed.noPursuitCams; break;
        case R_MENUSPEED: g_ed.menuSpeed = (g_ed.menuSpeed - 1 + dir + 3) % 3 + 1; break;
        case R_AUTOCONNECT: g_ed.autoConnect = !g_ed.autoConnect; break;
        case R_SAVE: case R_CANCEL: g_sel = g_sel == R_SAVE ? R_CANCEL : R_SAVE; break;
        default:
            if (IsText(g_sel) && g_padEdit) {       // pad: right adds a character, left removes one
                std::string& t = TextOf(g_sel);
                if (dir > 0 && (int)t.size() < MaxLen(FieldOf(g_sel))) t += StepChar(FieldOf(g_sel), 0, 1);
                else if (dir < 0 && !t.empty()) t.pop_back();
            }
            break;
        }
        g_dirty = true;
    }

    void Accept(bool pad) {
        if (IsText(g_sel)) {
            if (pad) g_padEdit = !g_padEdit;
            else Move(1);
        } else if (g_sel == R_SAVE) {
            if (Save()) Close("saved");
        } else if (g_sel == R_CANCEL) {
            Close("cancelled");
        } else Change(1);
        g_dirty = true;
    }

    void Back() {
        if (g_padEdit) { g_padEdit = false; g_dirty = true; return; }
        Close("cancelled");
    }

    void TypeChar(char c) {
        if (!IsText(g_sel) || !Accepts(FieldOf(g_sel), c)) return;
        std::string& t = TextOf(g_sel);
        if ((int)t.size() < MaxLen(FieldOf(g_sel))) { t += c; Edited(); }
    }
    void Backspace() {
        if (!IsText(g_sel)) return;
        std::string& t = TextOf(g_sel);
        if (!t.empty()) { t.pop_back(); Edited(); }
    }
    void PadStepChar(int dir) {
        std::string& t = TextOf(g_sel);
        if (t.empty()) t += StepChar(FieldOf(g_sel), 0, dir > 0 ? 1 : -1);
        else t.back() = StepChar(FieldOf(g_sel), t.back(), dir);
        Edited();
    }

#ifdef MWT_DEVTOOLS
    bool g_debugBg = false;     // test: background key injection counts (the game has no focus)
#else
    constexpr bool g_debugBg = false;
#endif
    bool Focused() { HWND w = Overlay::GameWindow(); return w && (g_debugBg || GetForegroundWindow() == w); }

    // ---- keyboard (the game's own polled state) ----
    uint8_t g_prev[256];
    DWORD   g_heldAt[256], g_repAt[256];
    bool Pressed(const uint8_t* k, int d, bool repeat) {
        bool down = (k[d] & 0x80) != 0, was = (g_prev[d] & 0x80) != 0;
        DWORD now = GetTickCount();
        if (down && !was) { g_heldAt[d] = now; g_repAt[d] = now; return true; }
        if (down && repeat && now - g_heldAt[d] > 400 && now - g_repAt[d] > 60) { g_repAt[d] = now; return true; }
        return false;
    }

    // ---- pad (XInput) ----
    WORD  g_padPrev = 0;
    DWORD g_backAt = 0, g_padHeldAt = 0, g_padRepAt = 0;
    bool PadPressed(WORD now, WORD bit, bool repeat) {
        DWORD t = GetTickCount();
        if ((now & bit) && !(g_padPrev & bit)) { g_padHeldAt = t; g_padRepAt = t; return true; }
        if ((now & bit) && repeat && t - g_padHeldAt > 400 && t - g_padRepAt > 90) { g_padRepAt = t; return true; }
        return false;
    }
    void PadTick() {
        XINPUT_STATE st{}; WORD b = 0; bool any = false;
        for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) if (XInputGetState(i, &st) == ERROR_SUCCESS) { b = st.Gamepad.wButtons; any = true; break; }
        if (!any || !b) g_swallowPad = false;
        if (!any) { g_padPrev = 0; g_backAt = 0; return; }
        if (!g_open) {   // hold BACK for a second to open
            if (b & XINPUT_GAMEPAD_BACK) { if (!g_backAt) g_backAt = GetTickCount(); else if (GetTickCount() - g_backAt > 1000) { g_backAt = 0; Open(); } }
            else g_backAt = 0;
            g_padPrev = b; return;
        }
        if (PadPressed(b, XINPUT_GAMEPAD_DPAD_UP, true))   { if (g_padEdit) PadStepChar(1); else Move(-1); }
        if (PadPressed(b, XINPUT_GAMEPAD_DPAD_DOWN, true)) { if (g_padEdit) PadStepChar(-1); else Move(1); }
        if (PadPressed(b, XINPUT_GAMEPAD_DPAD_LEFT, true)) Change(-1);
        if (PadPressed(b, XINPUT_GAMEPAD_DPAD_RIGHT, true)) Change(1);
        if (PadPressed(b, XINPUT_GAMEPAD_A, false)) Accept(true);
        if (PadPressed(b, XINPUT_GAMEPAD_B, false)) Back();
        if (PadPressed(b, XINPUT_GAMEPAD_START, false)) { if (Save()) Close("saved"); }
        g_padPrev = b;
    }

    std::string ConnText() {
        const NetStats& st = Net::Stats();
        char b[160];
        if (st.connected) sprintf_s(b, "Connected to %s", st.peerName.empty() ? "the other player" : st.peerName.c_str());
        else if (g_cfg.isHost) sprintf_s(b, "Hosting: waiting for the other player (UDP %d)", g_cfg.port);
        else sprintf_s(b, "Joining %s:%d - no answer yet", g_cfg.peerIp.c_str(), g_cfg.port);
        std::string s = b;
        if (st.connected && st.pingMs >= 0) s += " (" + std::to_string(st.pingMs) + " ms)";
        return s;
    }
}

void Together::Init(const char* iniPath) {
    g_ini = iniPath;
    char b[16]; GetPrivateProfileStringA("UI", "TogetherSetupDone", "0", b, sizeof(b), iniPath);
    g_setupDone = atoi(b) != 0;
    GetPrivateProfileStringA("Network", "AutoConnect", "1", b, sizeof(b), iniPath);
    g_autoConnect = atoi(b) != 0;
    GetPrivateProfileStringA("UI", "TogetherKey", "0x71", b, sizeof(b), iniPath);   // VK, default F2
    UINT vk = (UINT)strtoul(b, nullptr, 0);
    g_dikOpen = vk ? (uint8_t)MapVirtualKeyA(vk, MAPVK_VK_TO_VSC) : 0;   // DirectInput codes are scan codes
#ifdef MWT_DEVTOOLS
    if (g_cfg.testKeys) g_setupDone = true;     // scripted test runs never wait for the first-start setup
#endif
    memset(g_prev, 0, sizeof(g_prev));
    LOG("Together: settings page on %s%s%s", vk ? "the key / holding BACK on a pad" : "holding BACK on a pad (no key)",
        g_setupDone ? "" : " - first start", g_autoConnect ? "" : " - asks Host / Join at start");
}

void Together::OnKeyboard(const uint8_t* k) {
    if (g_swallowKb) {      // until every key that was down when the page closed is up
        bool held = false;
        for (int d = 0; d < 256; ++d) if ((g_prev[d] & 0x80) && (k[d] & 0x80)) { held = true; break; }
        if (!held) g_swallowKb = false;
    }
    if (!Focused()) { memcpy(g_prev, k, 256); return; }
    if (!g_open) {
        if (g_swallowKb) return;                    // g_prev keeps the keys that closed the page
        if (g_dikOpen && Pressed(k, g_dikOpen, false)) Open();
        g_prev[g_dikOpen] = k[g_dikOpen];          // only the open key is looked at while the page is closed
        if (g_open) memcpy(g_prev, k, 256);         // keys already held don't act on the page
        return;
    }
    bool shift = (k[0x2A] & 0x80) || (k[0x36] & 0x80);
    if (Pressed(k, 0xC8, true)) Move(-1);                                   // up
    if (Pressed(k, 0xD0, true) || Pressed(k, 0x0F, true)) Move(1);           // down, tab
    if (Pressed(k, 0xCB, true)) Change(-1);                                 // left
    if (Pressed(k, 0xCD, true)) Change(1);                                  // right
    if (Pressed(k, 0x1C, false) || Pressed(k, 0x9C, false)) Accept(false);  // enter
    if (Pressed(k, 0x01, false)) Back();                                    // esc
    if (Pressed(k, 0x0E, true)) Backspace();
    if (g_open && IsText(g_sel))
        for (int d = 0; d < 0x60; ++d) {        // character keys (only while a text field is selected)
            char c = DikToChar((uint8_t)d, shift);
            if (c && Pressed(k, d, true)) TypeChar(c);
        }
    memcpy(g_prev, k, 256);
}

void Together::Tick(int flow) {
    if (flow != g_flow) { g_flow = flow; g_dirty = true; }
    if (g_planned && g_plan.openPage && !g_autoShown && flow == 3 && Overlay::GameWindow()) { g_autoShown = true; Open(); }
    bool hint = !g_open && flow == 3 && !g_native;
    if (hint != g_hint) { g_hint = hint; g_dirty = true; }
    if (Focused()) PadTick(); else { g_padPrev = 0; g_backAt = 0; g_swallowPad = false; }
    if (g_open && GetTickCount() - g_lastRefresh > 1000) { g_lastRefresh = GetTickCount(); g_dirty = true; }
}

#ifdef MWT_DEVTOOLS
std::string Together::DebugCommand(const std::string& args) {
    if (args == "open") { Open(); return "page open"; }
    if (args == "close") { Close("test"); return "page closed"; }
    if (args.rfind("bg ", 0) == 0) { g_debugBg = args.substr(3) == "1"; return g_debugBg ? "background keys count" : "focus needed"; }
    if (!g_open) return "page not open";
    size_t pos = 0; int n = 0;
    while (pos < args.size()) {
        size_t e = args.find(' ', pos); if (e == std::string::npos) e = args.size();
        std::string t = args.substr(pos, e - pos); pos = e + 1;
        if (t.empty()) continue;
        ++n;
        if (t == "up") Move(-1); else if (t == "down") Move(1); else if (t == "left") Change(-1); else if (t == "right") Change(1);
        else if (t == "enter") Accept(false); else if (t == "esc") Back(); else if (t == "bksp") Backspace();
        else if (t == "a") Accept(true); else if (t == "b") Back(); else if (t == "start") { if (Save()) Close("saved"); }
        else if (t.rfind("t:", 0) == 0) for (char c : t.substr(2)) TypeChar(c);
        else return "unknown action " + t;
        if (!g_open) break;
    }
    return std::to_string(n) + " action(s), page " + (g_open ? "open" : "closed") + (g_msg.empty() ? "" : " - " + g_msg);
}
#endif

bool Together::CapturesInput() { return g_open || g_swallowKb || g_swallowPad; }
bool Together::IsOpen() { return g_open; }
bool Together::HoldStart() { return g_open || !g_planned || (g_plan.openPage && !g_autoShown); }
bool Together::WaitAtMenu() { return g_planned && g_plan.askAtMenu && !g_chosen; }

void Together::SetNativeMenus(bool ok) {
    g_native = ok;
    g_plan = NativeUtil::PlanStart(g_setupDone, g_autoConnect, ok);
    g_planned = true;
    g_netRunning = g_plan.netAtStart;
    LOG("Together: %s%s", ok ? "Host / Join and settings in the game's menus" : "the game's menus are unchanged - settings on the overlay page",
        g_plan.askAtMenu ? " - waiting for Host / Join in the main menu" : g_plan.openPage ? " - the page opens on the title screen" : "");
}
bool Together::NetAtStart() { return !g_planned || g_plan.netAtStart; }

Settings Together::Current() {
    Settings s = FromConfig();
    if (g_haveSaved) { bool ov = s.overlay; s = g_saved; s.overlay = ov; }   // F8 may have changed the overlay
    return s;
}
std::string Together::Apply(const Settings& s, bool connect) { return ApplySettings(s, connect, nullptr); }
std::string Together::LocalAddresses() { return LocalIps(); }
bool Together::Connected() { return g_netRunning && Net::Stats().connected; }
bool Together::SessionActive() { return g_netRunning || g_netChange; }

std::string Together::StopSession() {
    if (!g_netRunning && !g_netChange) return "";
    g_netChange = false;
    if (g_netRunning) g_netStop = true;
    g_netRunning = false;
    LOG("Together: session stopped");
    return "";
}

bool Together::TakeNetworkChange() {
    if (!g_netChange) return false;
    g_netChange = false;
    g_netStop = false;
    ApplyNetwork(g_pendingNet);     // right before the reconnect, so the old connection never sees the new settings
    g_netRunning = true;
    return true;
}
bool Together::TakeNetworkStop() {
    if (!g_netStop) return false;
    g_netStop = false;
    return true;
}
bool Together::Visible() { return g_open || g_hint; }
bool Together::Dirty() { return g_dirty; }
bool Together::Centered() { return g_open; }

// ---- drawing (GDI into a DIB, copied into the overlay's texture) ----
namespace {
    const COLORREF kBack = RGB(14, 16, 18), kRow = RGB(28, 32, 36), kSel = RGB(64, 62, 56), kField = RGB(40, 45, 50),
                   kText = RGB(225, 228, 230), kDim = RGB(140, 148, 155), kAccent = RGB(242, 169, 0), kWarn = RGB(255, 120, 60);

    void Fill(HDC dc, int x, int y, int w, int h, COLORREF c) { RECT r{ x, y, x + w, y + h }; HBRUSH b = CreateSolidBrush(c); FillRect(dc, &r, b); DeleteObject(b); }
    void Text(HDC dc, HFONT f, int x, int y, int w, int h, const std::string& s, COLORREF c, UINT fmt) {
        SelectObject(dc, f); SetTextColor(dc, c); RECT r{ x, y, x + w, y + h };
        DrawTextA(dc, s.c_str(), -1, &r, fmt | DT_NOPREFIX | DT_SINGLELINE | DT_VCENTER);
    }
    void Brackets(HDC dc, int x, int y, int w, int h) {
        HPEN p = CreatePen(PS_SOLID, 3, kAccent); HGDIOBJ o = SelectObject(dc, p);
        int L = 10, x1 = x + w - 2, y1 = y + h - 2; x += 1; y += 1;
        POINT a[3] = { { x, y + L }, { x, y }, { x + L, y } }; Polyline(dc, a, 3);
        POINT b[3] = { { x1 - L, y }, { x1, y }, { x1, y + L } }; Polyline(dc, b, 3);
        POINT c[3] = { { x, y1 - L }, { x, y1 }, { x + L, y1 } }; Polyline(dc, c, 3);
        POINT d[3] = { { x1 - L, y1 }, { x1, y1 }, { x1, y1 - L } }; Polyline(dc, d, 3);
        SelectObject(dc, o); DeleteObject(p);
    }
    HFONT Font(int h, int weight, const char* face) {
        return CreateFontA(h, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, face);
    }

    const char* RowLabel(int r) {
        switch (r) {
        case R_NAME: return "Nickname"; case R_MODE: return "Connection"; case R_HOSTIP: return "Connect to IP"; case R_PORT: return "UDP port";
        case R_AUTOCONNECT: return "Keep connection on restart";
        case R_COLOUR: return "My colour on the map"; case R_SKIPINTRO: return "Skip intros (next start)";
        case R_DIRECT: return "Quick career start"; case R_SAFEHOUSE: return "Quick leave the safe house (next start)";
        case R_PURSUITRES: return "Auto-close pursuit results in free roam"; case R_INTROINPUT: return "Menus respond during animations (next start)";
        case R_PADHOTPLUG: return "Detect gamepads connected while playing"; case R_OVERLAY: return "Show status overlay (F8)";
        case R_NOPURSUITCAM: return "No pursuit start camera"; case R_MENUSPEED: return "Menu animation speed (next start)";
        }
        return "";
    }
}

bool Together::Paint(uint32_t* pixels, int pitch, int maxW, int maxH, int* outW, int* outH) {
    g_dirty = false;
    if (!g_open && !g_hint) return false;
    const int W = g_open ? 620 : 430, H = g_open ? 706 : 34;
    if (W > maxW || H > maxH) return false;
    HDC dc = CreateCompatibleDC(nullptr);
    BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP bmp = dc ? CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    if (!bmp || !bits) { if (bmp) DeleteObject(bmp); if (dc) DeleteDC(dc); return false; }
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    SetBkMode(dc, TRANSPARENT);
    HFONT fTitle = Font(30, FW_HEAVY, "Arial Black"), fLabel = Font(16, FW_BOLD, "Arial"), fSmall = Font(14, FW_NORMAL, "Arial"),
          fButton = Font(18, FW_BOLD, "Arial");
    HGDIOBJ oldFont = SelectObject(dc, fLabel);

    if (!g_open) {   // front-end hint
        Fill(dc, 0, 0, W, H, kBack);
        Fill(dc, 0, 0, 4, H, kAccent);
        char key[32] = "", hint[128];
        if (g_dikOpen && GetKeyNameTextA((LONG)g_dikOpen << 16, key, sizeof(key)) <= 0) sprintf_s(key, "key %u", g_dikOpen);
        sprintf_s(hint, "%s%shold BACK on the pad: MW Together settings", key, key[0] ? " or " : "");
        Text(dc, fSmall, 12, 0, W - 16, H, hint, kText, DT_LEFT);
    } else {
        Fill(dc, 0, 0, W, H, kBack);
        // header: stripes like the game's panels
        Fill(dc, 0, 0, W, 64, RGB(20, 22, 24));
        HPEN sp = CreatePen(PS_SOLID, 6, RGB(32, 34, 36)); HGDIOBJ op = SelectObject(dc, sp);
        for (int x = -64; x < W + 64; x += 18) { MoveToEx(dc, x, 64, nullptr); LineTo(dc, x + 64, 0); }
        SelectObject(dc, op); DeleteObject(sp);
        Fill(dc, 0, 62, W, 2, kAccent);
        Text(dc, fTitle, 20, 4, W - 40, 40, "together", RGB(255, 255, 255), DT_LEFT);
        Text(dc, fSmall, 22, 40, W - 40, 20, "NFS MW Together co-op settings", kDim, DT_LEFT);

        int y = 74;
        const int rowH = 30, lx = 20, vx = 360, vw = W - vx - 20;
        for (int r = R_NAME; r <= R_MENUSPEED; ++r) {
            bool sel = g_sel == r, hid = Hidden(r);
            Fill(dc, 12, y, W - 24, rowH - 3, sel ? kSel : kRow);
            if (sel) Brackets(dc, 12, y, W - 24, rowH - 3);
            Text(dc, fLabel, lx, y, vx - lx - 8, rowH - 3, RowLabel(r), hid ? kDim : (sel ? RGB(255, 255, 255) : kText), DT_LEFT);
            std::string v; COLORREF vc = kText;
            if (IsText(r)) {
                Fill(dc, vx, y + 3, vw, rowH - 9, kField);
                v = hid ? "only when joining" : TextOf(r);
                if (sel && !hid) v += (g_padEdit ? "<" : "_");
                vc = hid ? kDim : kText;
                Text(dc, fLabel, vx + 6, y, vw - 8, rowH - 3, v, vc, DT_LEFT);
                y += rowH; continue;
            }
            switch (r) {
            case R_MODE: v = g_ed.host ? "<  Host LAN  >" : "<  Join LAN  >"; vc = kAccent; break;
            case R_COLOUR: {
                int i = ColourIndex(g_ed.colour);
                v = std::string("<  ") + (i < 0 ? "Custom " + ColourText(g_ed.colour) : kColourNames[i]) + "  >";
                if (g_ed.colour) Fill(dc, vx + vw - 22, y + 6, 16, rowH - 15, RGB((g_ed.colour >> 16) & 255, (g_ed.colour >> 8) & 255, g_ed.colour & 255));
                break; }
            case R_MENUSPEED: v = "<  " + std::to_string(g_ed.menuSpeed) + "x  >"; break;
            default: {
                bool on = r == R_SKIPINTRO ? g_ed.skipIntro : r == R_DIRECT ? g_ed.directFreeRoam : r == R_SAFEHOUSE ? g_ed.skipLeaveSafeHouse
                        : r == R_PURSUITRES ? g_ed.skipPursuitResults : r == R_INTROINPUT ? g_ed.introInput : r == R_PADHOTPLUG ? g_ed.gamepadHotplug
                        : r == R_OVERLAY ? g_ed.overlay : r == R_AUTOCONNECT ? g_ed.autoConnect : g_ed.noPursuitCams;
                v = on ? "ON" : "OFF"; vc = on ? kAccent : kDim; break; }
            }
            Text(dc, fLabel, vx, y, vw - 26, rowH - 3, v, vc, DT_LEFT);
            y += rowH;
        }
        // buttons
        y += 6;
        const int bw = (W - 24 - 10) / 2;
        for (int i = 0; i < 2; ++i) {
            int r = i == 0 ? R_SAVE : R_CANCEL, x = 12 + i * (bw + 10);
            bool sel = g_sel == r;
            Fill(dc, x, y, bw, 36, sel ? kSel : kRow);
            if (sel) Brackets(dc, x, y, bw, 36);
            Text(dc, fButton, x, y, bw, 36, i == 0 ? "SAVE & PLAY" : "CANCEL", sel ? RGB(255, 255, 255) : kDim, DT_CENTER);
        }
        y += 44;
        Text(dc, fSmall, 20, y, W - 40, 18, "Your IP address: " + g_ips + "   (the other player types this to join you)", kText, DT_LEFT); y += 19;
        Text(dc, fSmall, 20, y, W - 40, 18, ConnText(), kDim, DT_LEFT); y += 19;
        Text(dc, fSmall, 20, y, W - 40, 18, g_msg, g_msgWarn ? kWarn : kAccent, DT_LEFT); y += 22;
        Fill(dc, 12, y, W - 24, 1, RGB(60, 60, 60)); y += 4;
        Text(dc, fSmall, 20, y, W - 40, 18, "Keys: Up/Down select, Left/Right change, type to edit, Enter, Esc cancel", kDim, DT_LEFT); y += 18;
        Text(dc, fSmall, 20, y, W - 40, 18, g_padEdit ? "Pad: Up/Down letter, Right add, Left delete, A or B done"
                                                     : "Pad: D-pad, A select / edit text, B cancel, START save & play", kDim, DT_LEFT);
    }
    GdiFlush();
    const uint32_t* src = (const uint32_t*)bits;
    const uint32_t alpha = g_open ? 0xF2000000u : 0xE0000000u;
    for (int yy = 0; yy < H; ++yy)
        for (int xx = 0; xx < W; ++xx) pixels[yy * pitch + xx] = (src[yy * W + xx] & 0x00FFFFFF) | alpha;
    SelectObject(dc, oldFont); SelectObject(dc, oldBmp);
    DeleteObject(fTitle); DeleteObject(fLabel); DeleteObject(fSmall); DeleteObject(fButton);
    DeleteObject(bmp); DeleteDC(dc);
    *outW = W; *outH = H;
    return true;
}
