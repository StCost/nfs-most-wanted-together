// Included from game.cpp (after traffic.inl).
// ---------------------------------------------------------------------------
// Minimap markers for remote players
//  The HUD minimap draws AI-racer icons in Minimap::DrawRacers (0x57A110, thiscall, called once from
//  Minimap::Update at 0x59DD04). It reads VEHICLE_LIST 7 (REMOTE) only in online sessions, else list 3
//  (AIRACERS). Our spawned remote-player cars (DRIVER_REMOTE) are in list 7, so offline they never got
//  an icon. This replacement draws list 3 and then list 7 into the same 8 icon slots
//  (Minimap+0xF0..+0x10C = MMICON_AIRACER_0..7), with remote players pinned to the radar's edge.
//  Calls (same as the original): IVehicle vtable +0x88 IsActive, +0x4 GetSimable;
//  0x579840 cdecl(bVector2* pos, bVector2* dir, ISimable*); 0x5675D0 Minimap::DrawIcon
//  thiscall(pos*, dir*, FEObject* icon, bool pinToEdge); 0x514C70 cdecl(FEObject*) hides an icon.
// ---------------------------------------------------------------------------
namespace {
    constexpr uintptr_t MinimapRacersCall = 0x59DD04, MinimapRacersFn = 0x57A110;

    // The other player's blip in the colour they picked ([Player] Color on their PC); the 8 icon slots
    // are shared with AI racers, so every other use of a slot gets the slot's own colour back.
    void* g_slotIcon[8] = {}; uint32_t g_slotOrig[8] = {};
    void PaintSlot(int slot, void* icon, bool isPeer) {
        if (g_slotIcon[slot] != icon) {                     // first sight of this icon: remember its colour
            int c[4] = {};
            ((int* (__cdecl*)(int*, void*))0x515830)(c, icon);                      // FEngGetColor
            g_slotOrig[slot] = ((uint32_t(__thiscall*)(int*))0x5B2D80)(c);         // FEColor -> ARGB
            g_slotIcon[slot] = icon;
        }
        uint32_t want = (isPeer && Net::Stats().peerColor) ? Net::Stats().peerColor : g_slotOrig[slot];
        ((void(__cdecl*)(void*, uint32_t))0x5157E0)(icon, want);                    // FEngSetColor
    }

    void __fastcall hk_MinimapRacers(void* minimap, void*) {
        int slot = 0;
        const int lists[2] = { (int)VL_AIRACERS, 7 /*REMOTE*/ };
        for (int list : lists) {
            void** b = nullptr;
            int n = ListSize(list, &b);
            for (int i = 0; i < n && slot < 8; ++i) {
                void* iv = ListGet(list, i);
                if (!IsVehicle(iv)) continue;
                if (!VCall<bool(__thiscall*)(void*)>(iv, 34)(iv)) continue;      // IsActive
                void* sim = VCall<void* (__thiscall*)(void*)>(iv, 1)(iv);        // GetSimable
                if (!sim) continue;
                float pos[2], dir[2];
                ((void(__cdecl*)(float*, float*, void*))0x579840)(pos, dir, sim);
                void* icon = *(void**)((uint8_t*)minimap + 0xF0 + slot * 4);
                if (!icon) continue;
                ((void(__thiscall*)(void*, float*, float*, void*, bool))0x5675D0)(minimap, pos, dir, icon, list == 7);
                PaintSlot(slot, icon, iv == g_ghost);
                ++slot;
            }
        }
        // the other player's car, wherever the game keeps it (spawned car, or a racer in a race)
        bool ghostDrawn = false;
        {
            void** b = nullptr;
            int n7 = ListSize(7, &b);
            for (int i = 0; i < n7; ++i) if (ListGet(7, i) == g_ghost) ghostDrawn = true;
            if (ListContains(VL_AIRACERS, g_ghost)) ghostDrawn = true;
            static DWORD nextLog = 0;
            if (GetTickCount() >= nextLog) {
                nextLog = GetTickCount() + 10000;
                LOG("Minimap: remote list %d car(s), ghost %p %s", n7, g_ghost, ghostDrawn ? "in a drawn list" : "drawn directly");
            }
        }
        if (!ghostDrawn && slot < 8 && IsVehicle(g_ghost) && ListContains(-1, g_ghost)) {
            void* sim = VCall<void* (__thiscall*)(void*)>(g_ghost, 1)(g_ghost);
            void* icon = *(void**)((uint8_t*)minimap + 0xF0 + slot * 4);
            if (sim && icon) {
                float pos[2], dir[2];
                ((void(__cdecl*)(float*, float*, void*))0x579840)(pos, dir, sim);
                ((void(__thiscall*)(void*, float*, float*, void*, bool))0x5675D0)(minimap, pos, dir, icon, true);
                PaintSlot(slot, icon, true);
                ++slot;
            }
        }
        for (; slot < 8; ++slot) {
            void* icon = *(void**)((uint8_t*)minimap + 0xF0 + slot * 4);
            if (icon) ((void(__cdecl*)(void*))0x514C70)(icon);
        }
    }
}

// Test command 'icons': logs every minimap icon drawn in the next second (position, pinned to the edge,
// call site), e.g. to find the GPS target of a story drive. The call sites of Minimap::DrawIcon are
// patched on first use only.
namespace {
    constexpr uintptr_t kDrawIconSites[] = { 0x56787A, 0x579FA7, 0x579FC1, 0x57A012, 0x57A19C, 0x57A260, 0x57A2D4, 0x594EF4 };
    DWORD g_iconsUntil = 0;
    template <int I>
    void __fastcall hk_DrawIconLog(void* mm, void*, float* pos, float* dir, void* icon, bool pin) {
        if (GetTickCount() < g_iconsUntil && pos)
            LOG("Icons: site %06X icon %p pos %.1f %.1f pin %d", (unsigned)kDrawIconSites[I], icon, pos[0], pos[1], (int)pin);
        ((void(__thiscall*)(void*, float*, float*, void*, bool))0x5675D0)(mm, pos, dir, icon, pin);
    }
}
std::string Game::DebugIcons() {
    static bool installed = false, ok = true;
    if (!installed) {
        installed = true;
        void* const hooks[] = { (void*)&hk_DrawIconLog<0>, (void*)&hk_DrawIconLog<1>, (void*)&hk_DrawIconLog<2>, (void*)&hk_DrawIconLog<3>,
                                (void*)&hk_DrawIconLog<4>, (void*)&hk_DrawIconLog<5>, (void*)&hk_DrawIconLog<6>, (void*)&hk_DrawIconLog<7> };
        for (int i = 0; i < 8; ++i) ok &= PatchCallChecked(kDrawIconSites[i], 0x5675D0, hooks[i]);
    }
    g_iconsUntil = GetTickCount() + 1000;
    return ok ? "logging minimap icons for 1 s" : "logging minimap icons for 1 s (some call sites didn't match)";
}

bool Game::InstallMinimapHook() {
    bool ok = PatchCallChecked(MinimapRacersCall, MinimapRacersFn, (void*)&hk_MinimapRacers);
    LOG("Minimap: remote-player icons %s", ok ? "hooked" : "NOT hooked (call site mismatch)");
    return ok;
}
