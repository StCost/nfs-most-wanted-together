#pragma once
#include <cstdint>
#include <string>

// MW Together in the game's own menus (speed.exe 1.3; every patch checks the game's bytes first, and if
// any doesn't match nothing is changed - the overlay page stays the way in):
//   - Main menu: a "Together" icon (after Customize) opens the Connection screen - nickname,
//     Connection (Host / Join), Connect to IP (Join only), "Keep connection on restart" and
//     Start Host / Stop Host or Connect / Disconnect. The action stays on the screen. Start Host /
//     Connect saves and starts the session; Stop Host / Disconnect ends it. With Keep connection
//     on restart on, the next start hosts or joins as last time.
//   - Pause menu: the Together icon (in the pause Options) opens that same Connection screen.
//   - Options > Together (main menu Options): the Together settings - UDP port, map colour, and
//     the start / menu options (including Auto-close pursuit results). If the list ever has more
//     rows than the screen has objects, it scrolls.
// Text is typed in the game's own text entry (Enter keeps, Esc cancels). Leaving either screen saves.
// Labels come from a copy of the game's string table with our strings added (the game's own pointer to
// its table is pointed at the copy; the copy is rebuilt when the game loads another language).
namespace NativeUI {
    bool Install();       // first frame, game thread. true = the menus have the Together entries
    bool Active();
    void Tick();          // every frame, game thread
    // Pause_Options.fng can leave on EXITCOMPLETE without PAD_ACCEPT ever reaching the
    // screen. Eat Enter (and Up / Down at the list edges) in the DirectInput hooks so
    // the Together screen stays and edits in place, like the main menu path.
    void FilterKeyboardState(uint8_t* state256);
    void FilterKeyboardData(void* events, unsigned* count, unsigned eventSize);
#ifdef MWT_DEVTOOLS
    // test command "native ...": type <text> | enter | esc (the game's text entry, no focus needed),
    // options | main | connect | settings (open the Together screen as if picked there), state
    std::string DebugCommand(const std::string& args);
#endif
}
