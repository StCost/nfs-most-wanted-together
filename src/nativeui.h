#pragma once
#include <cstdint>
#include <string>

// MW Together in the game's own menus (speed.exe 1.3; every patch checks the game's bytes first, and if
// any doesn't match nothing is changed - the overlay page stays the way in):
//   - Main menu Together, pause Together, and Options > Together all open the same screen:
//     nickname, Connection (Host / Join), Connect to IP (Join only), Reconnect on launch,
//     Start Host / Stop Host or Connect / Disconnect, then Fast menus, Auto-detect gamepads
//     and Menu speed. The action stays on the screen (no extra info in the value column).
//     Start Host / Connect saves and starts the session; Stop Host / Disconnect ends it.
//     With Reconnect on launch on, the next start hosts or joins as last time.
// Nickname and Connect to IP are typed in the visible row (soft edit) on both the main-menu
// and pause Together screens, with a cursor (_) after the text. Enter or pad A keeps, Esc or
// pad B cancels the edit; Back then leaves. Leaving
// the screen saves. The game's own text-entry page is not used (it can open blank on a click).
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
    // A Together field is being typed (or the pad button that ended it is still down): the game
    // gets no pad buttons, so A keeps / B cancels the field and nothing else.
    bool CapturesPad();
#ifdef MWT_DEVTOOLS
    // test command "native ...": type <text> | enter | esc (the game's text entry, no focus needed),
    // options | main | connect | settings (open the Together screen as if picked there), state
    std::string DebugCommand(const std::string& args);
#endif
}
