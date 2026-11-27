#pragma once
#include <d3d9.h>
#include <string>
#include <cstdint>

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

    // The friend marker (marker.inl sets it every frame): a tinted chevron over the friend's car (onScreen:
    // x / y = where, as fractions of the screen, pointing down), or at the screen edge pointing along
    // (dirX, dirY) (screen fractions, y down). tex = the game's texture (IDirect3DTexture9*), or null for a drawn one.
    struct Marker { bool show = false, onScreen = false; float x = 0, y = 0, dirX = 0, dirY = 0, scale = 1;
                    uint32_t color = 0; void* tex = nullptr; };
    void SetMarker(const Marker& m);

    IDirect3DDevice9* Device();
    HWND GameWindow();
    void SetCloseCallback(void (*cb)());   // called when the game window gets a close/destroy message
}
