#pragma once
#include <string>

// MW Together in the game's own menus (speed.exe 1.3; every patch checks the game's bytes first, and if
// any doesn't match nothing is changed - the overlay page stays the way in):
//   - Options (main menu and pause menu): a "Together" icon after Controller opens the game's options list
//     screen (Options.fng) with the Together settings - nickname, Connection (Host / Join), Connect to IP
//     (Join only), port, "Keep connection on restart", map colour and the start / menu options. The last
//     action row is Start Host / Stop Host or Connect / Disconnect, and stays on the screen. Text is typed
//     in the game's own text entry (Enter keeps, Esc cancels). Leaving the screen saves.
//   - Main menu: a "Together" icon (after Customize) opens the same screen with nickname, Connection,
//     Connect to IP (Join only), "Keep connection on restart" and the same Start Host / Stop Host or
//     Connect / Disconnect action. Start Host / Connect saves and starts the session; Stop Host /
//     Disconnect ends it. With Keep connection on restart on, the next start hosts or joins as last time.
// Labels come from a copy of the game's string table with our strings added (the game's own pointer to
// its table is pointed at the copy; the copy is rebuilt when the game loads another language).
namespace NativeUI {
    bool Install();       // first frame, game thread. true = the menus have the Together entries
    bool Active();
    void Tick();          // every frame, game thread
#ifdef MWT_DEVTOOLS
    // test command "native ...": type <text> | enter | esc (the game's text entry, no focus needed),
    // options | main (open the Together screen as if picked there), state
    std::string DebugCommand(const std::string& args);
#endif
}
