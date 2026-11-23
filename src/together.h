#pragma once
#include <cstdint>
#include <string>
#include "togetherutil.h"

// The "Together" settings: nickname, host / join, host address, start and menu options.
//   - normally in the game's own menus (nativeui.cpp): Options > Together, and Main menu > Together to
//     host or join. Until Host / Join is picked ([UI] TogetherSetupDone=0, or [Network] AutoConnect=0)
//     AutoStart stops at the main menu; with AutoConnect=1 it connects as last time.
//   - the overlay page (F2 or holding BACK on a pad) has the same rows; it opens by itself on the
//     first start only when the game's menus couldn't get their entries
//   - Save & Play writes scripts\MWTogether.ini (comments kept) and applies the settings; a changed
//     name / colour / mode / address / port reconnects (Net::Restart)
// Input: the keys the game itself just polled from its keyboard (input.cpp), and XInput for a pad - both
// only while the game window is focused. While the page is open the game gets no keys / pad buttons.
namespace Together {
    void Init(const char* iniPath);       // after g_cfg.Load (DllMain)
    void Tick(int gameFlowState);         // once per frame, game thread
    void OnKeyboard(const uint8_t* dik256);   // the game's keyboard state, from its GetDeviceState call
    bool CapturesInput();                 // the page is open: the game gets no input
    bool IsOpen();
    bool HoldStart();                     // AutoStart must wait (the page is open)
    bool WaitAtMenu();                    // AutoStart stops at the main menu: Host / Join isn't picked yet

    // The game's own menus (nativeui.cpp). SetNativeMenus once at the first frame (did the menus get their
    // Together entries?) - it decides what the start does; NetAtStart: connect right away.
    void SetNativeMenus(bool ok);
    bool NetAtStart();
    TogetherUtil::Settings Current();     // as saved
    // Save and use the settings; connect = a Host / Join choice (connects now). "" or what to fix.
    std::string Apply(const TogetherUtil::Settings& s, bool connect);
    std::string LocalAddresses();         // this PC's IPv4 addresses, for the other player to join
    bool Connected();
    bool SessionActive();                 // hosting, or a join is in progress / connected
    std::string StopSession();            // bye + shut the session down (Stop Host / Disconnect)
    bool TakeNetworkChange();             // true once after a save that needs a new connection
    bool TakeNetworkStop();               // true once: the game should SendBye + Shutdown

    // Drawing (overlay.cpp): Visible() = something to draw (the page, or the front-end hint);
    // Paint fills an A8R8G8B8 image (top-down, pitch in pixels) when Dirty(); Centered() = the page.
    bool Visible();
    bool Dirty();
    bool Paint(uint32_t* pixels, int pitch, int maxW, int maxH, int* w, int* h);
    bool Centered();

#ifdef MWT_DEVTOOLS
    // test command "together ...": open | close | bg 0/1 (keys count without focus) | a list of page actions
    // (up down left right enter esc bksp a b start, t:<text>)
    std::string DebugCommand(const std::string& args);
#endif
}
