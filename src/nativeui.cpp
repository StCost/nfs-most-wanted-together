// MW Together in the game's own menus - see nativeui.h. Addresses are speed.exe 1.3; names from the
// game's symbols. Every code / vtable patch is checked against the game's bytes before it is made.
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "nativeui.h"
#include "nativeutil.h"
#include "together.h"
#include "net.h"
#include "config.h"
#include "log.h"
#include "mem.h"

using namespace TogetherUtil;
using NativeUtil::StringRec;

namespace {
    // ---- the game ----
    constexpr uintptr_t kFEDatabase = 0x91CF90;        // cFrontendDatabase*; +0x10 options settings (+0x24 current category), +0x12C game mode bits
    constexpr uintptr_t kFEng = 0x91CADC;              // cFEng::mInstance
    constexpr uintptr_t kRecordTable = 0x91CF80, kNumRecords = 0x91CF78;   // the string table SearchForString reads
    constexpr uintptr_t kKeyboard = 0x91D4B0;          // gKeyboardManager (KeyboardEditString); +0x414 text being edited

    // FE messages
    constexpr uint32_t BUTTON_PRESSED = 0x0C407210, EXITCOMPLETE = 0xE1FDE1D1, PAD_BACK = 0x911AB364, PAD_LEFT = 0x9120409E,
                       PAD_RIGHT = 0xB5971BF1, PAD_START = 0xB5AF2461, PAD_BUTTON5 = 0xC519BFC4, ACCEPT_EDITED = 0xDA5B8712,
                       DECLINE_EDITED = 0xC9D30688, LEAVE_SCREEN = 0x587C018B, PC_DISPLAY = 0x9CDD83CA;
    constexpr uint32_t PAD_ACCEPT = 0x406415E3;
    constexpr uint32_t PROFILE_NAME = 0xB514E2D8;          // MainMenu.fng: the profile box
    constexpr uint32_t HEADER_TEXT = 0x42ADB44C, HEADER_ICON = 0x08007B4C;
    constexpr uint32_t LANG_ON = 0x417B2604, LANG_OFF = 0x70DFE5C2;        // the game's own "On" / "Off"
    constexpr uint32_t TEX_ONLINE = 0x5A5E5137, TEX_PLAYER = 0xD708EFEF, TEX_LAN = 0xD13A2E66;
    // the game's package names (its own strings: QueuePackageSwitch keeps the pointer)
    const char* const kOptionsFng = (const char*)0x89E55C, *const kPauseOptionsFng = (const char*)0x897450,
              *const kPauseMainFng = (const char*)0x897464, *const kMainMenuFng = (const char*)0x89AE54,
              *const kMainMenuSubFng = (const char*)0x89D1F0, *const kOnlineMainFng = (const char*)0x89B044;

    // categories of the options list screen that are ours (the game's are 0..9; it ignores others)
    constexpr int CAT_SETTINGS = 0x40, CAT_CONNECT = 0x41;

    using Alloc_t = void*(__cdecl*)(size_t);
    using IconCtor_t = void*(__thiscall*)(void*, uint32_t, uint32_t, uint32_t);
    using AddOption_t = void(__thiscall*)(void*, void*);
    using ToggleCtor_t = void*(__thiscall*)(void*, bool);
    using AddToggle_t = uint32_t(__thiscall*)(void*, void*, bool);
    using SetInitial_t = void(__thiscall*)(void*, int);
    using Refresh_t = void(__thiscall*)(void*);
    using SetLangPkg_t = void(__cdecl*)(const char*, uint32_t, uint32_t);
    using SetLangStr_t = void(__cdecl*)(void*, uint32_t);
    using QueueSwitch_t = void(__thiscall*)(void*, const char*, int, uint32_t, bool);
    using QueueMessage_t = void(__thiscall*)(void*, uint32_t, const char*, void*, uint32_t);
    using IsPushed_t = bool(__thiscall*)(void*, const char*);
    using Notify_t = void(__thiscall*)(void*, uint32_t, void*, uint32_t, uint32_t);
    using Setup_t = void(__thiscall*)(void*);
    using BeginText_t = void(__thiscall*)(void*, uint32_t, uint32_t, const char*, const char*, uint32_t);
    using EditedText_t = const char*(__thiscall*)(void*);
    using GetString_t = void*(__thiscall*)(void*, const char*);
    using FindPackage_t = void*(__thiscall*)(void*, const char*);
    using FindObject_t = void*(__thiscall*)(void*, uint32_t);
    using ToggleUpdate_t = bool(__thiscall*)(void*, uint32_t);
    using NotifyChar_t = void(__thiscall*)(void*, int);

    const auto GameAlloc = (Alloc_t)0x652AD0;              // FastMem::Alloc (the game's operator new)
    const auto IconOptionCtor = (IconCtor_t)0x586FA0;      // IconOption(tex, name, desc)
    const auto AddOption = (AddOption_t)0x573960;          // IconScrollerMenu::AddOption
    const auto ToggleCtor = (ToggleCtor_t)0x589300;        // FEToggleWidget(enabled)
    const auto AddToggleOption = (AddToggle_t)0x588570;    // UIWidgetMenu::AddToggleOption(widget, arrows)
    const auto SetInitialOption = (SetInitial_t)0x5A5EA0;
    const auto RefreshWidgets = (Refresh_t)0x588D20;
    const auto SetLangPkg = (SetLangPkg_t)0x525220;        // FEngSetLanguageHash(pkg, object, string)
    const auto SetLangStr = (SetLangStr_t)0x515C00;        // FEngSetLanguageHash(FEString*, string)
    const auto QueuePackageSwitch = (QueueSwitch_t)0x525940;
    const auto QueueMessage = (QueueMessage_t)0x516BE0;
    const auto IsPackagePushed = (IsPushed_t)0x516B50;
    const auto WidgetMenuNotify = (Notify_t)0x5A96C0;      // UIWidgetMenu::NotificationMessage
    const auto BeginTextInput = (BeginText_t)0x5865E0;     // MenuScreen::FEngBeginTextInput(object, mode, text, title, max)
    const auto EditedText = (EditedText_t)0x586550;        // MenuScreen::FEngGetEditedString
    const auto GetCurrentFEString = (GetString_t)0x573CC0, GetCurrentFEImage = (GetString_t)0x573D10,
               GetCurrentFEObject = (GetString_t)0x573D80;
    const auto FindPackage = (FindPackage_t)0x516AF0;
    const auto FindObjectByHash = (FindObject_t)0x5B8550;
    const auto ToggleUpdate = (ToggleUpdate_t)0x50E640;    // FEToggleWidget::Update (blinks the arrow)
    const auto NotifyChar = (NotifyChar_t)0x5A8700;        // KeyboardEditString::NotifyChar (WM_CHAR)

    // ---- patch sites (all checked before use) ----
    constexpr uintptr_t kCallOptionsAdd = 0x52909C;      // UIOptionsMain::Setup: AddOption(Controller)
    constexpr uintptr_t kCallMainAdd = 0x5450BF;         // UIMain::Setup: AddOption(Customize)
    constexpr uintptr_t kCallOptionsScreenSetup = 0x54FE4E;   // UIOptionsScreen ctor: Setup()
    constexpr uintptr_t kSlotOptionsScreenSetup = 0x89CF6C, kSlotOptionsScreenNotify = 0x89CF64;
    constexpr uintptr_t kSlotOptionsMainNotify = 0x89E530, kSlotMainNotify = 0x89EAD8;
    constexpr uintptr_t kOrigOptionsScreenSetup = 0x545C50, kOrigOptionsScreenNotify = 0x545750,
                        kOrigOptionsMainNotify = 0x545310, kOrigMainNotify = 0x559FC0;
    constexpr uintptr_t kVtGODamage = 0x89BE68;          // a toggle widget's vtable (15 slots), copied for ours

    bool g_active = false;

    // ---- strings ----
    constexpr uint32_t kStrBase = 0x6D775400;            // our string hashes ("mwT" + n)
    enum : uint32_t { S_OPT_NAME = kStrBase, S_OPT_DESC, S_MAIN_NAME, S_MAIN_DESC, S_HEADER, S_HEADER_CONNECT,
                      S_TITLE = kStrBase + 0x20, S_VALUE = kStrBase + 0x40 };
    using NativeUtil::W_NAME; using NativeUtil::W_MODE; using NativeUtil::W_HOSTIP; using NativeUtil::W_PORT;
    using NativeUtil::W_AUTO; using NativeUtil::W_COLOUR; using NativeUtil::W_DIRECT; using NativeUtil::W_SKIPINTRO;
    using NativeUtil::W_SAFEHOUSE; using NativeUtil::W_PURSUITRES; using NativeUtil::W_INTROINPUT;
    using NativeUtil::W_PADHOTPLUG; using NativeUtil::W_NOPURSUITCAM; using NativeUtil::W_MENUSPEED;
    using NativeUtil::W_START; using NativeUtil::W_COUNT;
    char g_title[W_COUNT][48], g_value[W_COUNT][96];
    std::vector<StringRec> g_ours;
    StringRec* g_table = nullptr; int g_tableN = 0;      // our merged copy (what RecordTable points at)
    const StringRec* g_gameTable = nullptr;              // the game's table it was made from

    void InitStrings() {
        g_ours = {
            { S_OPT_NAME, "Together" }, { S_OPT_DESC, "MW Together co-op: nickname, host or join, start options" },
            { S_MAIN_NAME, "Together" }, { S_MAIN_DESC, "Play together: host or join" },
            { S_HEADER, "Together" }, { S_HEADER_CONNECT, "Together" },
        };
        for (int r = 0; r < W_COUNT; ++r) {
            g_title[r][0] = g_value[r][0] = 0;
            g_ours.push_back({ S_TITLE + r, g_title[r] });
            g_ours.push_back({ S_VALUE + r, g_value[r] });
        }
    }

    // Point the game's string table at a copy with our strings (again whenever the game loads its own)
    void StringsTick() {
        StringRec* cur = nullptr; int n = 0;
        if (!Mem::Read(kRecordTable, cur) || !Mem::Read(kNumRecords, n) || !cur || n <= 0 || n > 200000) return;
        if (cur == g_table) return;
        if (!Mem::IsReadable(cur, (size_t)n * sizeof(StringRec))) return;
        std::vector<StringRec> v = NativeUtil::MergeStrings(cur, n, g_ours);
        StringRec* t = (StringRec*)HeapAlloc(GetProcessHeap(), 0, v.size() * sizeof(StringRec));
        if (!t) return;
        memcpy(t, v.data(), v.size() * sizeof(StringRec));
        StringRec* old = g_table;
        g_gameTable = cur;
        g_table = t; g_tableN = (int)v.size();
        Mem::Write(kRecordTable, t);
        Mem::Write(kNumRecords, g_tableN);
        if (old) HeapFree(GetProcessHeap(), 0, old);   // the game replaced it with its own: nobody reads it any more
        LOG("Native: string table %p (%d) -> %p (%d)", cur, n, t, g_tableN);
    }

    // ---- the front end ----
    uint8_t* Db() { uint8_t* d = nullptr; Mem::Read(kFEDatabase, d); return d; }
    int* CategoryPtr() {
        uint8_t* d = Db(); if (!d) return nullptr;
        uint8_t* o = nullptr; if (!Mem::Read((uintptr_t)d + 0x10, o) || !o) return nullptr;
        return (int*)(o + 0x24);
    }
    int Category() { int* p = CategoryPtr(); return p ? *p : -1; }
    void SetCategory(int c) { if (int* p = CategoryPtr()) *p = c; }
    uint32_t GameModeBits() { uint8_t* d = Db(); return d ? *(uint32_t*)(d + 0x12C) : 0; }
    void* FE() { void* f = nullptr; Mem::Read(kFEng, f); return f; }
    const char* PkgName(uint8_t* screen) { return *(const char**)(screen + 0x10); }
    bool Capturing() { void* t = nullptr; Mem::Read(kKeyboard + 0x414, t); return t != nullptr; }

    // ---- the settings being edited ----
    Settings g_ed;
    int      g_cat = 0;
    uint8_t* g_screen = nullptr;          // the options list screen showing our rows
    int      g_editRow = -1;
    // Options.fng / Pause_Options.fng treat ACCEPT as "keep and go back" (a package response).
    // Enter on a Together row must stay: we don't forward ACCEPT to the widget menu, and we drop
    // LEAVE_SCREEN for this package while g_stayOnAccept is set. Reopen is only a fallback.
    DWORD    g_acceptAt = 0;              // when ACCEPT reached the screen
    bool     g_stayOnAccept = false;      // this ACCEPT is an in-place row action: do not leave
    int      g_reopenRow = -1;            // fallback: open again on this row if the package still left
    bool     g_reopening = false;         // ... keeping the edits not saved yet
    DWORD    g_startAt = 0;               // unused for Start now (the action stays on the screen)
    // Pause_Options.fng may leave first and send ACCEPT after: where to go is decided a moment later
    uint8_t* g_leftScreen = nullptr;      // the screen that has left (it may still get that ACCEPT)
    DWORD    g_leftAt = 0;                // 0 = nothing pending
    int      g_leftCat = 0;
    bool     g_leftPause = false;
    int      g_editOnOpen = -1;           // after opening again: type into this row
    DWORD    g_builtAt = 0;
    DWORD    g_editEnd = 0;               // when the text entry ended (its Enter / Esc mustn't act again)
    std::string g_err; int g_errRow = -1; bool g_backWarned = false;
    bool     g_mainPicked = false;        // main menu: our icon was picked

    bool IsText(int r) { return r == W_NAME || r == W_HOSTIP || r == W_PORT; }
    bool IsOnOff(int r) { return r == W_AUTO || (r >= W_DIRECT && r <= W_NOPURSUITCAM); }
    bool* Flag(int r) {
        switch (r) {
        case W_AUTO: return &g_ed.autoConnect; case W_DIRECT: return &g_ed.directFreeRoam; case W_SKIPINTRO: return &g_ed.skipIntro;
        case W_SAFEHOUSE: return &g_ed.skipLeaveSafeHouse; case W_PURSUITRES: return &g_ed.skipPursuitResults;
        case W_INTROINPUT: return &g_ed.introInput; case W_PADHOTPLUG: return &g_ed.gamepadHotplug; case W_NOPURSUITCAM: return &g_ed.noPursuitCams;
        }
        return nullptr;
    }
    std::string& Text(int r) { return r == W_NAME ? g_ed.name : r == W_HOSTIP ? g_ed.hostIp : g_ed.port; }
    Field FieldOf(int r) { return r == W_NAME ? Field::Name : r == W_HOSTIP ? Field::Host : Field::Port; }

    const char* TitleOf(int r) {
        switch (r) {
        case W_NAME: return "Nickname"; case W_MODE: return "Connection";
        case W_HOSTIP: return "Connect to IP";
        case W_PORT: return "UDP port"; case W_AUTO: return "Keep connection on restart"; case W_COLOUR: return "Map colour";
        case W_DIRECT: return "Quick career start"; case W_SKIPINTRO: return "Skip intros (next start)";
        case W_SAFEHOUSE: return "Quick safe house exit (next start)"; case W_PURSUITRES: return "Auto-close pursuit results";
        case W_INTROINPUT: return "Fast menus (next start)"; case W_PADHOTPLUG: return "Detect new gamepads";
        case W_NOPURSUITCAM: return "No pursuit start camera"; case W_MENUSPEED: return "Menu speed (next start)";
        case W_START: return NativeUtil::ConnectionActionTitle(g_ed.host, Together::SessionActive());
        }
        return "";
    }
    std::string ValueOf(int r) {
        if (r == g_errRow && !g_err.empty()) return g_err;
        switch (r) {
        case W_NAME: return g_ed.name.empty() ? "(type a nickname)" : g_ed.name;
        case W_MODE: return g_ed.host ? "Host" : "Join";
        case W_HOSTIP: return Trim(g_ed.hostIp).empty() ? "(type the host's IP)" : g_ed.hostIp;
        case W_PORT: return g_ed.port;
        case W_COLOUR: { int i = ColourIndex(g_ed.colour); return i < 0 ? "Custom " + ColourText(g_ed.colour) : kColourNames[i]; }
        case W_MENUSPEED: return std::to_string(g_ed.menuSpeed) + "x";
        case W_START: {
            if (!Together::SessionActive()) return g_ed.host ? std::string() : (Trim(g_ed.hostIp).empty() ? "..." : Trim(g_ed.hostIp));
            if (Together::Connected()) {
                std::string n = Net::Stats().peerName;
                return n.empty() ? "Connected" : n;
            }
            return g_ed.host ? "Running" : "Connecting...";
        }
        }
        return "";
    }

    // ---- our option rows: FEToggleWidget objects with our own Act / Draw ----
    constexpr size_t kWidgetSize = 0x5C;               // sizeof(FEToggleWidget); our row number follows
    void* g_widgetVt[15];

    int RowOf(uint8_t* w) { return *(int*)(w + kWidgetSize); }

    void WidgetDrawImpl(uint8_t* w) {
            int r = RowOf(w);
            if (r < 0 || r >= W_COUNT) return;
            void* title = *(void**)(w + 0x2C);
            void* data = *(void**)(w + 0x30);
            strncpy_s(g_title[r], TitleOf(r), _TRUNCATE);
            if (title) SetLangStr(title, S_TITLE + r);
            if (!data) return;
            if (r == g_editRow) return;     // the game's text entry owns this string while typing
            if (IsOnOff(r) && !(r == g_errRow && !g_err.empty())) { SetLangStr(data, *Flag(r) ? LANG_ON : LANG_OFF); return; }
            strncpy_s(g_value[r], ValueOf(r).c_str(), _TRUNCATE);
            SetLangStr(data, S_VALUE + r);
    }
    void __fastcall WidgetDraw(uint8_t* w, void*) {
        __try { WidgetDrawImpl(w); } __except (EXCEPTION_EXECUTE_HANDLER) { LOG("Native: exception drawing a row"); }
    }

    void Refresh() { if (g_screen) __try { RefreshWidgets(g_screen); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
    void ClearError() { if (g_errRow >= 0) { g_err.clear(); g_errRow = -1; g_backWarned = false; } }

    void ShowError(const std::string& e) {
        g_err = e;
        g_errRow = e.find("nickname") != std::string::npos ? W_NAME : e.find("port") != std::string::npos ? W_PORT
                 : (e.find("host") != std::string::npos || e.find("IP") != std::string::npos) ? W_HOSTIP : W_NAME;
        Refresh();
    }

    void RebuildRows();   // defined after BuildScreen

    void Change(int r, int dir) {
        ClearError();
        switch (r) {
        case W_MODE: g_ed.host = !g_ed.host; RebuildRows(); return;
        case W_COLOUR: g_ed.colour = NextColour(g_ed.colour, dir); break;
        case W_MENUSPEED: g_ed.menuSpeed = (g_ed.menuSpeed - 1 + dir + 3) % 3 + 1; break;
        default: if (bool* f = Flag(r)) *f = !*f; break;
        }
        Refresh();
    }

    void BeginEdit(uint8_t* w, int r) {
        if (!g_screen) return;
        void* data = *(void**)(w + 0x30);
        if (!data) return;
        uint32_t objHash = *(uint32_t*)((uint8_t*)data + 0x10);
        ClearError();
        std::string start = Text(r);
        strncpy_s(g_value[r], start.c_str(), _TRUNCATE);
        SetLangStr(data, S_VALUE + r);
        // edit modes of the game's text entry: 0 = any key, 4 = letters, digits, '.', '@', '_'
        BeginTextInput(g_screen, objHash, r == W_NAME ? 0u : 4u, start.c_str(), TitleOf(r), (uint32_t)MaxLen(FieldOf(r)));
        *(g_screen + 0x120) = 0;     // bAllowScroll: no row changes while typing (as the game's own text rows)
        g_editRow = r;
        LOG("Native: typing %s", TitleOf(r));
    }

    void EndEdit(bool keep) {
        int r = g_editRow; g_editRow = -1; g_editEnd = GetTickCount();
        if (g_screen) *(g_screen + 0x120) = 1;
        if (r < 0) return;
        if (keep && g_screen) {
            const char* t = EditedText(g_screen);
            if (t) {
                std::string v = NativeUtil::CleanText(FieldOf(r), t);
                if (r == W_HOSTIP && !v.empty()) g_ed.host = false;      // typing an address means joining it
                if (!(r == W_HOSTIP && v.empty() && g_ed.host)) Text(r) = v;
                LOG("Native: %s = %s", TitleOf(r), v.c_str());
            }
        }
        Refresh();
    }

    void Start() {
        if (Together::SessionActive()) {
            Together::StopSession();
            LOG("Native: %s stopped", g_ed.host ? "host" : "join");
            Refresh();
            return;
        }
        std::string err = Together::Apply(g_ed, true);
        if (!err.empty()) { ShowError(err); return; }
        LOG("Native: %s from the Together screen", g_ed.host ? "hosting" : "joining");
        Refresh();
    }

    void __fastcall WidgetAct(uint8_t* w, void*, const char* /*pkg*/, uint32_t data) {
        __try {
            int r = RowOf(w);
#ifdef MWT_DEVTOOLS
            if (g_cfg.testKeys) LOG("Native: row %d act %08X", r, data);
#endif
            if (r < 0 || r >= W_COUNT || Capturing() || GetTickCount() - g_editEnd < 300) return;
            if (data == PAD_LEFT || data == PAD_RIGHT) {
                if (IsText(r) || r == W_START) return;
                Change(r, data == PAD_RIGHT ? 1 : -1);
                // Change(W_MODE) rebuilds the rows, so the widget pointer is no longer valid
                if (r != W_MODE) ToggleUpdate(w, data);
            } else if (data == BUTTON_PRESSED) {
                if (IsText(r)) BeginEdit(w, r);
                else if (r == W_START) Start();
                else Change(r, 1);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { LOG("Native: exception in a row"); }
    }

    // ---- the options list screen (Options.fng / Pause_Options.fng) ----
    bool RowFits(uint8_t* s) {
        // the screen has a fixed number of row objects: stop before one is missing
        // (title, data, -, left / right arrow; the backing is optional: Options.fng has none)
        static const int kNames[] = { 0xB0, 0xB4, 0xBC, 0xC0, 0xC4 };
        for (int i = 0; i < 5; ++i) {
            if (i == 2) continue;
            const char* name = *(const char**)(s + kNames[i]);
            void* o = !name ? nullptr : i == 2 ? GetCurrentFEObject(s, name) : i >= 3 ? GetCurrentFEImage(s, name) : GetCurrentFEString(s, name);
            if (!o) {
                LOG("Native: row %d: no %s object (%s)", *(int*)(s + 0x104), i == 0 ? "title" : i == 1 ? "data" : i == 2 ? "backing" : "arrow",
                    name ? name : "-");
                return false;
            }
        }
        return true;
    }

    void BuildScreen(uint8_t* s, int cat) {
        g_screen = s; g_cat = cat; g_builtAt = GetTickCount();
        if (g_leftScreen == s) g_leftScreen = nullptr;      // a new screen where the old one was
        if (!g_reopening) g_ed = Together::Current();
        g_editRow = -1; g_backWarned = false;
        if (!g_reopening) { g_err.clear(); g_errRow = -1; }
        const char* pkg = PkgName(s);
        SetLangPkg(pkg, HEADER_TEXT, cat == CAT_CONNECT ? S_HEADER_CONNECT : S_HEADER);
        if (void* fe = FE())        // the header icon (as SetupPlayer does)
            if (void* p = FindPackage(fe, pkg))
                if (uint8_t* o = (uint8_t*)FindObjectByHash(p, HEADER_ICON))
                    if (*(int*)(o + 0x18) == 1) { *(uint32_t*)(o + 0x24) = TEX_PLAYER; *(uint32_t*)(o + 0x1C) |= 0x2400000; }
        std::vector<int> rows = NativeUtil::TogetherRows(
            cat == CAT_CONNECT ? NativeUtil::Screen::Connect : NativeUtil::Screen::Settings, g_ed.host);
        int added = 0;
        for (int r : rows) {
            if (!RowFits(s)) { LOG("Native: the screen has room for %d rows", added); break; }
            uint8_t* w = (uint8_t*)GameAlloc(kWidgetSize + 4);
            if (!w) break;
            ToggleCtor(w, true);
            *(void**)w = g_widgetVt;
            *(int*)(w + kWidgetSize) = r;
            AddToggleOption(s, w, true);
            ++added;
        }
        int first = 0;
        for (int i = 0; i < added; ++i) if (g_reopening && rows[i] == g_reopenRow) first = i;
        g_reopening = false; g_reopenRow = -1;
        SetInitialOption(s, first + 1);   // 1-based (0 = the first enabled row)
        LOG("Native: Together screen (%s, %d rows)", cat == CAT_CONNECT ? "main menu" : "options", added);
    }

    void RebuildRows() {
        if (!g_screen) return;
        int keep = W_MODE;
        uint8_t* w = *(uint8_t**)(g_screen + 0x34);
        if (w && *(void**)w == g_widgetVt) {
            int r = RowOf(w);
            if (r != W_HOSTIP || !g_ed.host) keep = r;
        }
        g_reopening = true;
        g_reopenRow = keep;
        ((Setup_t)kOrigOptionsScreenSetup)(g_screen);   // clears the rows; adds nothing for our categories
        BuildScreen(g_screen, g_cat);
    }

    void BuildScreenSafe(uint8_t* s, int cat) {
        __try { BuildScreen(s, cat); } __except (EXCEPTION_EXECUTE_HANDLER) { LOG("Native: exception building the Together screen"); }
    }
    void __fastcall hk_OptionsScreenSetup(uint8_t* s, void*) {
        ((Setup_t)kOrigOptionsScreenSetup)(s);      // clears the rows; adds nothing for our categories
        int cat = Category();
        if (cat != CAT_SETTINGS && cat != CAT_CONNECT) return;
        BuildScreenSafe(s, cat);
    }

    // Back on the settings screen: save (the game's own options apply as they change). Something to fix:
    // shown on its row; Back again leaves without saving.
    bool SaveOnLeave() {
        std::string err = Together::Apply(g_ed, false);
        if (err.empty()) return true;
        if (g_backWarned) { LOG("Native: left without saving (%s)", err.c_str()); return true; }
        g_backWarned = true;
        ShowError(err);
        return false;
    }

    void SaveQuietly() { std::string e = Together::Apply(g_ed, false); if (!e.empty()) LOG("Native: not saved (%s)", e.c_str()); }

    void ExitTo(bool pause, int cat) {
        void* fe = FE(); if (!fe) return;
        if (cat == CAT_CONNECT) { QueuePackageSwitch(fe, kMainMenuFng, 0, 0, false); return; }
        if (pause) QueuePackageSwitch(fe, kPauseMainFng, 1, 0, false);
        else if (GameModeBits() & 8) QueuePackageSwitch(fe, kOnlineMainFng, 0, 0, false);
        else QueuePackageSwitch(fe, kMainMenuSubFng, 0, 0, false);
    }

    // ACCEPT reaching a screen that has left (Pause_Options.fng fallback): do the row's action, then open again
    void AcceptAfterLeaving(uint8_t* s) {
        uint8_t* w = *(uint8_t**)(s + 0x34);
        if (!w || *(void**)w != g_widgetVt || Capturing() || GetTickCount() - g_editEnd < 300) return;
        int r = RowOf(w);
        if (r < 0 || r >= W_COUNT) return;
        g_acceptAt = GetTickCount();
        if (r == W_START) { Start(); g_reopenRow = W_START; return; }
        if (IsText(r)) g_editOnOpen = r; else Change(r, 1);
        g_reopenRow = r;
    }

    void LeftTick() {
        if (!g_leftAt || GetTickCount() - g_leftAt < 100) return;     // (its ACCEPT comes in the same frame)
        g_leftAt = 0;
        void* fe = FE(); if (!fe) return;
        if (g_reopenRow >= 0 && GetTickCount() - g_acceptAt < 1500) {
            g_reopening = true;
            QueuePackageSwitch(fe, g_leftPause ? kPauseOptionsFng : kOptionsFng, g_leftPause ? 1 : 0, 0, false);
        } else {
            g_reopenRow = -1; g_editOnOpen = -1;
            if (g_leftCat == CAT_SETTINGS && !g_startAt && GetTickCount() - g_acceptAt < 1500) SaveQuietly();   // ACCEPT = keep
            ExitTo(g_leftPause, g_leftCat);
        }
        g_startAt = 0;
    }

    void EditOnOpenTick() {
        if (g_editOnOpen < 0 || !g_screen || Capturing() || GetTickCount() - g_builtAt < 700) return;   // after its intro
        int r = g_editOnOpen; g_editOnOpen = -1;
        uint8_t* w = *(uint8_t**)(g_screen + 0x34);
        if (w && *(void**)w == g_widgetVt && RowOf(w) == r) BeginEdit(w, r);
    }

    void __fastcall hk_OptionsScreenNotify(uint8_t* s, void*, uint32_t msg, void* obj, uint32_t p1, uint32_t p2) {
        int cat = Category();
        if (cat != CAT_SETTINGS && cat != CAT_CONNECT) { ((Notify_t)kOrigOptionsScreenNotify)(s, msg, obj, p1, p2); return; }
        __try {
            if (s == g_leftScreen) {                    // gone already
                if (msg == PAD_ACCEPT && g_leftAt) AcceptAfterLeaving(s);
                else WidgetMenuNotify(s, msg, obj, p1, p2);
                return;
            }
            g_screen = s;
#ifdef MWT_DEVTOOLS
            if (g_cfg.testKeys && msg != 0xC98356BA) LOG("Native: screen message %08X obj %p", msg, obj);
#endif
            switch (msg) {
            case ACCEPT_EDITED: EndEdit(true); return;
            case DECLINE_EDITED: EndEdit(false); return;
            case PAD_BUTTON5: case PC_DISPLAY: return;       // no "restore defaults" / display toggles here
            case PAD_BACK:
                if (Capturing() || GetTickCount() - g_editEnd < 300) return;
                g_stayOnAccept = false;
                WidgetMenuNotify(s, msg, obj, p1, p2);
                if (cat == CAT_CONNECT || SaveOnLeave())
                    if (void* fe = FE()) QueueMessage(fe, LEAVE_SCREEN, PkgName(s), (void*)(intptr_t)-4, 0xFF);
                return;
            case PAD_START:
                if (cat == CAT_SETTINGS && *(s + 0x124)) { SaveQuietly(); ((Notify_t)kOrigOptionsScreenNotify)(s, msg, obj, p1, p2); return; }
                break;
            case PAD_ACCEPT: {                              // the rows' own action; stay on this screen
                g_acceptAt = GetTickCount(); g_reopenRow = -1; g_stayOnAccept = true;
                uint8_t* w = *(uint8_t**)(s + 0x34);          // pCurrentOption
                if (w && *(void**)w == g_widgetVt) WidgetAct(w, nullptr, PkgName(s), BUTTON_PRESSED);
                return; }
            case EXITCOMPLETE:
                if (g_stayOnAccept && GetTickCount() - g_acceptAt < 1500) {
                    // the package still tried to leave: don't pop; typing / the new value stays here
                    LOG("Native: stayed on the Together screen after ACCEPT");
                    return;
                }
                WidgetMenuNotify(s, msg, obj, p1, p2);
                g_leftScreen = s; g_leftAt = GetTickCount(); if (!g_leftAt) g_leftAt = 1;
                g_leftCat = cat; g_leftPause = *(s + 0x124) != 0;
                g_stayOnAccept = false;
                g_screen = nullptr;
                return;     // LeftTick: open again, or back to the menu it came from
            }
            WidgetMenuNotify(s, msg, obj, p1, p2);
        } __except (EXCEPTION_EXECUTE_HANDLER) { LOG("Native: exception in the Together screen (%08X)", msg); }
    }

    // ---- the icons ----
    void* g_optionsIconVt[2];
    void* g_mainIconVt[2];

    void __fastcall OptionsIconReact(void*, void*, const char*, uint32_t data, void*, uint32_t, uint32_t) {
        if (data == BUTTON_PRESSED) SetCategory(CAT_SETTINGS);
    }
    void __fastcall MainIconReact(void*, void*, const char*, uint32_t data, void*, uint32_t, uint32_t) {
        if (data == BUTTON_PRESSED) g_mainPicked = true;
    }

    void AddIcon(void* menu, bool mainMenu) {
        __try {
            void* o = GameAlloc(0x4C);
            if (!o) return;
            if (mainMenu) IconOptionCtor(o, TEX_LAN, S_MAIN_NAME, S_MAIN_DESC);
            else IconOptionCtor(o, TEX_ONLINE, S_OPT_NAME, S_OPT_DESC);
            *(void**)o = mainMenu ? g_mainIconVt : g_optionsIconVt;
            AddOption(menu, o);
        } __except (EXCEPTION_EXECUTE_HANDLER) { LOG("Native: exception adding the Together icon"); }
    }
    void __fastcall hk_OptionsAdd(void* menu, void*, void* opt) { AddOption(menu, opt); AddIcon(menu, false); }
    void __fastcall hk_MainAdd(void* menu, void*, void* opt) { g_mainPicked = false; AddOption(menu, opt); AddIcon(menu, true); }

    // the options icon menu: after its exit animation, open the list screen for our icon
    void __fastcall hk_OptionsMainNotify(uint8_t* s, void*, uint32_t msg, void* obj, uint32_t p1, uint32_t p2) {
        ((Notify_t)kOrigOptionsMainNotify)(s, msg, obj, p1, p2);
        __try {
            if (msg != EXITCOMPLETE || *(uint32_t*)(s + 0x138) != BUTTON_PRESSED || Category() != CAT_SETTINGS) return;
            void* fe = FE(); if (!fe) return;
            uint32_t mode = GameModeBits();
            bool pause = *(s + 0x148) && !(mode & 8) && !(mode & 0x40);
            QueuePackageSwitch(fe, pause ? kPauseOptionsFng : kOptionsFng, pause ? 1 : 0, 0, false);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    // the main menu: the same for its icon (no game mode set = none of the game's own icons)
    void __fastcall hk_MainNotify(uint8_t* s, void*, uint32_t msg, void* obj, uint32_t p1, uint32_t p2) {
        ((Notify_t)kOrigMainNotify)(s, msg, obj, p1, p2);
        __try {
            if (msg != EXITCOMPLETE || *(uint32_t*)(s + 0x138) != BUTTON_PRESSED || !g_mainPicked) return;
            g_mainPicked = false;
            if (GameModeBits() & 0x1FF) return;
            void* fe = FE(); if (!fe) return;
            SetCategory(CAT_CONNECT);
            QueuePackageSwitch(fe, kOptionsFng, 0, 0, false);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    // ---- install ----
    // ---- the main menu's profile box: "Me & Friend" while connected ----
    constexpr uintptr_t kFEPrintf = 0x5252D0;
    typedef int(__cdecl* FEPrintf_t)(const char* pkg, uint32_t hash, const char* fmt, ...);
    std::string g_profileShown;           // what we last put there ("" = the game's own text)
    std::string g_fakePeer;               // dev: a pretend friend for screenshots

    void ProfileTick() {
        static DWORD last = 0;
        DWORD now = GetTickCount();
        if (now - last < 250) return;
        last = now;
        void* fe = FE();
        if (!fe || !IsPackagePushed(fe, kMainMenuFng)) { g_profileShown.clear(); return; }
        uint8_t* db = Db();
        const char* me = db ? *(const char**)(db + 0x10) : nullptr;   // UserProfile*: the name comes first
        if (!me) return;
        bool connected = Together::Connected();
        std::string peer = connected ? Net::Stats().peerName : std::string();
        if (!g_fakePeer.empty()) { connected = true; peer = g_fakePeer; }
        std::string label = NativeUtil::ProfileLabel(me, peer, connected);
        static DWORD written = 0;
        if (label == me && g_profileShown.empty()) return;            // the game's text is right
        if (label == g_profileShown && now - written < 2000) return;  // (now and then again: the menu may redo it)
        written = now;
        ((FEPrintf_t)kFEPrintf)(kMainMenuFng, PROFILE_NAME, "%s", label.c_str());
        g_profileShown = label == me ? std::string() : label;
    }

    // Drop LEAVE_SCREEN while Enter on a Together row should stay on this screen.
    constexpr uintptr_t kQueueMessageAddr = 0x516BE0;
    const uint8_t kQueueMessageHead[6] = { 0x8B, 0x44, 0x24, 0x08, 0x85, 0xC0 };
    void* g_qmTramp = nullptr;

    void __fastcall hk_QueueMessage(void* fe, void*, uint32_t msg, const char* pkg, void* obj, uint32_t p) {
        if (g_stayOnAccept && msg == LEAVE_SCREEN && g_screen && pkg) {
            const char* ours = PkgName(g_screen);
            if (ours && _stricmp(pkg, ours) == 0) return;
        }
        using Fn = void(__thiscall*)(void*, uint32_t, const char*, void*, uint32_t);
        ((Fn)g_qmTramp)(fe, msg, pkg, obj, p);
    }

    bool HookQueueMessage() {
        uint8_t have[6] = {};
        if (!Mem::SafeRead((const void*)kQueueMessageAddr, have, 6) || memcmp(have, kQueueMessageHead, 6) != 0) return false;
        void* tr = VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!tr) return false;
        memcpy(tr, kQueueMessageHead, 6);
        uint8_t* back = (uint8_t*)tr + 6;
        back[0] = 0xE9;
        *(int32_t*)(back + 1) = (int32_t)((kQueueMessageAddr + 6) - ((uintptr_t)back + 5));
        uint8_t hook[6] = { 0xE9, 0, 0, 0, 0, 0x90 };
        *(int32_t*)(hook + 1) = (int32_t)((uintptr_t)&hk_QueueMessage - (kQueueMessageAddr + 5));
        if (!Mem::PatchBytes(kQueueMessageAddr, kQueueMessageHead, hook, 6)) {
            VirtualFree(tr, 0, MEM_RELEASE);
            return false;
        }
        FlushInstructionCache(GetCurrentProcess(), (void*)kQueueMessageAddr, 6);
        g_qmTramp = tr;
        return true;
    }

    bool CallTargets(uintptr_t at, uintptr_t target) {
        uint8_t b[5];
        if (!Mem::SafeRead((const void*)at, b, 5) || b[0] != 0xE8) return false;
        return at + 5 + *(int32_t*)(b + 1) == target;
    }
    bool SlotHolds(uintptr_t slot, uintptr_t v) { uintptr_t cur = 0; return Mem::Read(slot, cur) && cur == v; }
    bool PatchCallTo(uintptr_t at, uintptr_t expectTarget, void* to) {
        uint8_t expect[5], code[5];
        expect[0] = 0xE8; *(int32_t*)(expect + 1) = (int32_t)(expectTarget - (at + 5));
        code[0] = 0xE8; *(int32_t*)(code + 1) = (int32_t)((uintptr_t)to - (at + 5));
        return Mem::PatchBytes(at, expect, code, 5);
    }
}

bool NativeUI::Install() {
    if (g_active) return true;
    // check everything first: all or nothing
    bool ok = CallTargets(kCallOptionsAdd, (uintptr_t)AddOption) && CallTargets(kCallMainAdd, (uintptr_t)AddOption)
           && CallTargets(kCallOptionsScreenSetup, kOrigOptionsScreenSetup)
           && SlotHolds(kSlotOptionsScreenSetup, kOrigOptionsScreenSetup) && SlotHolds(kSlotOptionsScreenNotify, kOrigOptionsScreenNotify)
           && SlotHolds(kSlotOptionsMainNotify, kOrigOptionsMainNotify) && SlotHolds(kSlotMainNotify, kOrigMainNotify)
           && SlotHolds(kVtGODamage, 0x5286D0) && SlotHolds(kVtGODamage + 4, 0x510050) && SlotHolds(kVtGODamage + 12, 0x51C180)
           && SlotHolds(0x89B8C8, 0x51A500);
    // the main menu's profile box (UIMain::UpdateProfileData: FEPrintf(pkg, 0xB514E2D8, "%s", profile name))
    uint8_t nameHash[5] = {};
    ok = ok && CallTargets(0x528D27, kFEPrintf) && Mem::SafeRead((const void*)0x528D21, nameHash, 5)
            && nameHash[0] == 0x68 && *(uint32_t*)(nameHash + 1) == PROFILE_NAME;
    uint8_t ctorHead[3] = {}, qmHead[6] = {};
    ok = ok && Mem::SafeRead((const void*)0x586FA0, ctorHead, 3) && ctorHead[0] == 0x8B && ctorHead[1] == 0x54;   // IconOption ctor
    ok = ok && Mem::SafeRead((const void*)kQueueMessageAddr, qmHead, 6) && memcmp(qmHead, kQueueMessageHead, 6) == 0;
    if (!ok) { LOG("Native: this speed.exe isn't the one the menu entries are made for - the game's menus stay as they are (F2 page)"); return false; }

    InitStrings();
    memcpy(g_widgetVt, (const void*)kVtGODamage, sizeof(g_widgetVt));
    g_widgetVt[1] = (void*)&WidgetAct;
    g_widgetVt[3] = (void*)&WidgetDraw;
    g_optionsIconVt[0] = g_mainIconVt[0] = (void*)0x51A500;    // IconOption's deleting destructor
    g_optionsIconVt[1] = (void*)&OptionsIconReact;
    g_mainIconVt[1] = (void*)&MainIconReact;

    StringsTick();
    bool done = PatchCallTo(kCallOptionsScreenSetup, kOrigOptionsScreenSetup, (void*)&hk_OptionsScreenSetup);
    done = done && Mem::PatchPointer((void*)kSlotOptionsScreenSetup, (void*)&hk_OptionsScreenSetup);
    done = done && Mem::PatchPointer((void*)kSlotOptionsScreenNotify, (void*)&hk_OptionsScreenNotify);
    done = done && Mem::PatchPointer((void*)kSlotOptionsMainNotify, (void*)&hk_OptionsMainNotify);
    done = done && Mem::PatchPointer((void*)kSlotMainNotify, (void*)&hk_MainNotify);
    // the icons last: nothing can open our screens before everything else is in place
    done = done && PatchCallTo(kCallOptionsAdd, (uintptr_t)AddOption, (void*)&hk_OptionsAdd);
    done = done && PatchCallTo(kCallMainAdd, (uintptr_t)AddOption, (void*)&hk_MainAdd);
    done = done && HookQueueMessage();
    if (!done) { LOG("Native: a menu patch failed half way"); return false; }
    g_active = true;
    LOG("Native: Together in the main menu and in Options");
    return true;
}

bool NativeUI::Active() { return g_active; }

void NativeUI::Tick() {
    if (!g_active) return;
    __try {
        StringsTick();
        ProfileTick();
        LeftTick();
        EditOnOpenTick();
        // our category lives on in the game's options settings: back to Audio once our screen is gone
        int cat = Category();
        if (cat == CAT_SETTINGS || cat == CAT_CONNECT) {
            void* fe = FE();
            static DWORD goneSince = 0;
            bool up = fe && (IsPackagePushed(fe, kOptionsFng) || IsPackagePushed(fe, kPauseOptionsFng)
                             || IsPackagePushed(fe, kMainMenuSubFng) || IsPackagePushed(fe, "Pause_Main.fng") || IsPackagePushed(fe, kMainMenuFng));
            if (up) goneSince = 0;
            else if (!goneSince) goneSince = GetTickCount();
            else if (GetTickCount() - goneSince > 2000) { SetCategory(0); goneSince = 0; }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

#ifdef MWT_DEVTOOLS
std::string NativeUI::DebugCommand(const std::string& a) {
    if (!g_active) return "native menus off";
    if (a.rfind("type ", 0) == 0) {
        if (!Capturing()) return "no text entry open";
        for (char c : a.substr(5)) NotifyChar((void*)kKeyboard, (unsigned char)c);
        return "typed";
    }
    if (a == "enter") { if (!Capturing()) return "no text entry open"; NotifyChar((void*)kKeyboard, 13); return "enter"; }
    if (a == "esc") { if (!Capturing()) return "no text entry open"; NotifyChar((void*)kKeyboard, 27); return "esc"; }
    if (a == "options" || a == "main") {
        void* fe = FE(); if (!fe) return "no front end";
        if (IsPackagePushed(fe, kOptionsFng) || IsPackagePushed(fe, kPauseOptionsFng)) return "an options screen is already open";
        SetCategory(a == "main" ? CAT_CONNECT : CAT_SETTINGS);
        bool pause = a == "options" && IsPackagePushed(fe, kPauseMainFng);     // as the pause menu's Options icon
        QueuePackageSwitch(fe, pause ? kPauseOptionsFng : kOptionsFng, pause ? 1 : 0, 0, false);
        return "opening the Together screen";
    }
    if (a == "act") {
        if (!g_screen) return "no screen";
        uint8_t* w = *(uint8_t**)(g_screen + 0x34);
        if (!w || *(void**)w != g_widgetVt) return "current row is not ours";
        WidgetAct(w, nullptr, PkgName(g_screen), BUTTON_PRESSED);
        return "row activated";
    }
    if (a.rfind("peer", 0) == 0) { g_fakePeer = a.size() > 5 ? a.substr(5) : std::string(); return g_fakePeer.empty() ? "pretend friend gone" : "pretend friend set"; }
    if (a == "state") {
        char b[200];
        sprintf_s(b, "category %d, screen %p, editing %d, capturing %d, name %s, %s %s, auto %d", Category(), g_screen, g_editRow,
                  (int)Capturing(), g_ed.name.c_str(), g_ed.host ? "host" : "join", g_ed.hostIp.c_str(), (int)g_ed.autoConnect);
        return b;
    }
    return "usage: native type <text> | enter | esc | options | main | state";
}
#endif

