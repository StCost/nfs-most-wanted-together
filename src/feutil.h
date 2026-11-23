#pragma once
#include <cstring>

// In-game front-end overlays that must be closed (PAD_BACK, as if Back was pressed) before a
// remote-started race / event or a teleport. The driving HUD, loading screens, movies and the
// post-race results (those have their own leave path) are left alone.
namespace FeClose {
    inline int CmpFng(const char* a, const char* b) {
        if (!a || !b) return a ? 1 : b ? -1 : 0;
        for (;;) {
            unsigned char ca = (unsigned char)*a++, cb = (unsigned char)*b++;
            if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca + 32);
            if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb + 32);
            if (ca != cb || !ca) return (int)ca - (int)cb;
        }
    }
    inline bool EqFng(const char* a, const char* b) { return CmpFng(a, b) == 0; }

    inline bool HasPrefix(const char* s, const char* pre) {
        if (!s || !pre) return false;
        while (*pre) {
            unsigned char ca = (unsigned char)*s++, cb = (unsigned char)*pre++;
            if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca + 32);
            if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb + 32);
            if (ca != cb) return false;
        }
        return true;
    }
    inline bool HasNeedle(const char* s, const char* n) {
        if (!s || !n || !*n) return false;
        for (const char* p = s; *p; ++p) {
            const char* a = p, *b = n;
            bool ok = true;
            while (*b) {
                if (!*a) { ok = false; break; }
                unsigned char ca = (unsigned char)*a++, cb = (unsigned char)*b++;
                if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca + 32);
                if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb + 32);
                if (ca != cb) { ok = false; break; }
            }
            if (ok) return true;
        }
        return false;
    }

    // Driving HUD, fades, movies, boot / main-menu, results: never PAD_BACK these here.
    inline bool IsProtectedPackage(const char* name) {
        if (!name || !name[0]) return true;
        if (HasPrefix(name, "HUD")) return true;
        if (HasPrefix(name, "Loading") || HasPrefix(name, "loading") || HasPrefix(name, "WS_Loading") ||
            HasPrefix(name, "PC_Loading") || HasPrefix(name, "FadeScreen")) return true;
        if (HasNeedle(name, "Movie") || HasNeedle(name, "AttractFMV")) return true;
        if (HasPrefix(name, "PostRace_") || EqFng(name, "InGamePhotoMaster.fng")) return true;
        if (HasPrefix(name, "PressStart") || HasNeedle(name, "Splash") || HasPrefix(name, "MC_Bootup") ||
            EqFng(name, "MC_Main.fng") || EqFng(name, "MC_List.fng") || EqFng(name, "LS_LangSelect.fng") ||
            HasPrefix(name, "LS_EA") || EqFng(name, "LS_PSA.fng") || EqFng(name, "EA_Trax.fng")) return true;
        if (HasPrefix(name, "MainMenu") || EqFng(name, "UI_QRModeOptions.fng") || HasPrefix(name, "Track_") ||
            EqFng(name, "Car_Select.fng") || EqFng(name, "Quick_Race_Brief.fng")) return true;
        return false;
    }

    // Most-specific first, so a stacked menu is peeled from the top (SMS message before the
    // mailbox, Options before Pause, rival bio before the rap sheet).
    inline const char* const* CloseOrder(int* count) {
        static const char* const k[] = {
            "SMS_Message.fng", "SMS_Mailboxes.fng", "SMS_MailBoxes.fng",
            "InGameDialog.fng", "EngageEventDialog.fng", "GenericDialog_MED.fng",
            "GenericDialog_ThreeButton.fng", "Dialog.fng",
            "Pause_PCDisplayOptions.fng", "Options_PC_Controller.fng",
            "Pause_Options.fng", "Options.fng", "Pause_Main.fng",
            "WorldMapMain.fng",
            "InGameRivalBio.fng", "InGameRivalChallenge.fng", "InGameRaceSheet.fng",
            "RapSheetMain.fng", "InGameBounty.fng", "Infractions.fng",
            "SafeHouseRaceSheet.fng", "SafehouseRaceSheet.fng",
            "GarageMain.fng", "MyCarsManager.fng",
            "PerformanceTuning.fng", "VisualTuning.fng",
            "InGame_MC_Main.fng", "InGame_MC_Main_GC.fng",
        };
        if (count) *count = (int)(sizeof(k) / sizeof(k[0]));
        return k;
    }

    inline bool InCloseOrder(const char* name) {
        int n = 0;
        const char* const* k = CloseOrder(&n);
        for (int i = 0; i < n; ++i) if (EqFng(k[i], name)) return true;
        return false;
    }

    // Close every in-game overlay, including a screen we do not list by name (Performance
    // Tuning, a new Blacklist sheet, ...), as long as it is not a protected package.
    inline bool ShouldCloseBeforeRemoteMove(const char* name) {
        if (!name || !name[0] || IsProtectedPackage(name)) return false;
        if (InCloseOrder(name)) return true;
        // unknown .fng: treat as an in-game overlay if it looks like a package name
        const char* dot = nullptr;
        for (const char* p = name; *p; ++p) if (*p == '.') dot = p;
        return dot && (EqFng(dot, ".fng"));
    }

    // First package to close among those currently pushed (CloseOrder, then any other closeable).
    inline const char* NextToClose(const char* const* pushed, int n) {
        if (!pushed || n <= 0) return nullptr;
        int orderN = 0;
        const char* const* order = CloseOrder(&orderN);
        for (int i = 0; i < orderN; ++i)
            for (int j = 0; j < n; ++j)
                if (EqFng(pushed[j], order[i]) && ShouldCloseBeforeRemoteMove(pushed[j]))
                    return pushed[j];
        for (int j = 0; j < n; ++j)
            if (ShouldCloseBeforeRemoteMove(pushed[j])) return pushed[j];
        return nullptr;
    }

    inline bool AnyOpen(const char* const* pushed, int n) { return NextToClose(pushed, n) != nullptr; }
}
