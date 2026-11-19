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
    bool IsVisible();

    IDirect3DDevice9* Device();
    HWND GameWindow();
    void SetCloseCallback(void (*cb)());   // called when the game window gets a close/destroy message
}
