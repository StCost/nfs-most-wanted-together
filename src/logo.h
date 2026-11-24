#pragma once

// The main menu's logo (MW_LOGO in GLOBAL\GLOBALA.BUN, 512 x 128 A8R8G8B8) can come from
// scripts\MWTogether-logo.dds instead - no game file changes: the game's own texture stays loaded and
// only the Direct3D texture it draws with is swapped (any size with the same 4:1 shape; DXT1/3/5 or
// 32-bit ARGB). No file = nothing happens.
namespace Logo {
    void Tick(const char* scriptsDir);    // game thread, once per frame (cheap: it checks once a second)
}
