#pragma once
#include <d3d9.h>
#include <string>

namespace Overlay {
    // Installs the Direct3DCreate9 IAT hook. Call from DllMain/early init.
    bool InstallHooks();

    // Called once per frame from EndScene, on the game's render thread.
    using FrameCallback = void(*)(IDirect3DDevice9* dev);
    void SetFrameCallback(FrameCallback cb);

    // Text to show in the top-left corner (multi-line, '\n' separated).
    void SetText(const std::string& text);
    void SetVisible(bool v);
    void RequestStack();                      // test: log the game's return addresses at the next frame
    void Screenshot(const std::string& path, bool withOverlay = false);   // save the next frame as a BMP (test tool)   // draw even when hidden (notifications)
    bool IsVisible();

    // The other player's pursuit bar, drawn above our own (bottom centre). kind 0 hides it, 1 the chase
    // bar (value -100 busted .. +100 evade), 2 the cooldown meter (0 .. 100). y / width: the bar's centre
    // and width as fractions of the screen height.
    struct PeerBar { int kind = 0; int value = 0; std::string name; float y = 0.76f, width = 0.454f; };
    void SetPeerBar(const PeerBar& b);

    IDirect3DDevice9* Device();
    HWND GameWindow();
    void SetCloseCallback(void (*cb)());   // called when the game window gets a close/destroy message
}
