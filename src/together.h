#pragma once
#include <cstdint>

// The "Together" page: every launcher setting inside the game (nickname, host / join, host address, port,
// map colour, start and menu options), drawn by the overlay over the game's own screens.
//   - opens with [UI] TogetherKey (F2) or by holding BACK on a pad, and by itself on the first start
//     ([UI] TogetherSetupDone=0) - the quick start waits until it is closed
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
    bool HoldStart();                     // the quick start / AutoStart must wait
    bool TakeNetworkChange();             // true once after a save that needs a new connection

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
