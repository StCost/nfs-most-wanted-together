// In-game FE overlays closed before a remote event start or teleport (src/feutil.h).
#include "mwt_test.h"
#include "../src/feutil.h"

using namespace FeClose;

TEST(fe_close_covers_pause_map_sms_blacklist_and_dialogs) {
    const char* need[] = {
        "Pause_Main.fng", "Pause_Options.fng", "WorldMapMain.fng",
        "SMS_Message.fng", "SMS_Mailboxes.fng", "RapSheetMain.fng",
        "InGameRivalChallenge.fng", "InGameRivalBio.fng", "InGameBounty.fng",
        "InGameDialog.fng", "EngageEventDialog.fng", "GarageMain.fng",
        "PerformanceTuning.fng",
    };
    for (const char* n : need) CHECK(ShouldCloseBeforeRemoteMove(n));
}

TEST(fe_close_leaves_hud_movies_loading_and_results) {
    const char* keep[] = {
        "HUD_SingleRace.fng", "HUD.fng", "HUD_Drag.fng",
        "InGameAnyMovie.fng", "WS_InGameAnyMovie.fng", "FEAnyMovie.fng", "WS_FEAnyMovie.fng",
        "Loading.fng", "WS_Loading.fng", "FadeScreenNoLoadingBar.fng",
        "PostRace_Results.fng", "InGamePhotoMaster.fng", "PostRace_MilestoneRewards.fng",
        "MainMenu.fng", "MC_Main.fng", "WS_MW_LS_Splash.fng",
    };
    for (const char* n : keep) CHECK(!ShouldCloseBeforeRemoteMove(n));
}

TEST(fe_close_peels_stacked_menus_from_the_top) {
    const char* stacked[] = { "Pause_Main.fng", "Pause_Options.fng", "SMS_Message.fng" };
    CHECK_STR(NextToClose(stacked, 3), "SMS_Message.fng");
    const char* pause[] = { "Pause_Main.fng", "Pause_Options.fng" };
    CHECK_STR(NextToClose(pause, 2), "Pause_Options.fng");
    const char* only[] = { "WorldMapMain.fng" };
    CHECK_STR(NextToClose(only, 1), "WorldMapMain.fng");
    CHECK(!NextToClose(nullptr, 0));
    const char* hud[] = { "HUD_SingleRace.fng" };
    CHECK(!NextToClose(hud, 1));
    CHECK(!AnyOpen(hud, 1));
    CHECK(AnyOpen(only, 1));
}

TEST(fe_close_unknown_in_game_overlay_is_closed) {
    CHECK(ShouldCloseBeforeRemoteMove("SomeNewBlacklistSheet.fng"));
    CHECK(!ShouldCloseBeforeRemoteMove("HUD_NewWidget.fng"));
    CHECK(!ShouldCloseBeforeRemoteMove(""));
    CHECK(!ShouldCloseBeforeRemoteMove(nullptr));
}

// ---- DirectInput device hooks, once per vtable (src/hookutil.h) ----
#include "../src/hookutil.h"

namespace {
    int OrigA(int x) { return x + 1; }
    int OrigB(int x) { return x + 2; }
    int Hook(int x) { return x; }
    using Fn = int(*)(int);
}

TEST(hook_vtables_keyboard_with_its_own_vtable_is_hooked_too) {
    // the PC case: the mouse and the keyboard each have their own vtable
    HookUtil::VtableOriginals<Fn> h;
    int mouseVt = 0, keyboardVt = 0;
    CHECK(h.Add(&mouseVt, &OrigA, &Hook));
    CHECK(h.Add(&keyboardVt, &OrigB, &Hook));
    CHECK_EQ(h.count, 2);
    CHECK(h.Find(&mouseVt) == &OrigA);
    CHECK(h.Find(&keyboardVt) == &OrigB);        // each call goes on to its own original
    CHECK_EQ(h.Find(&keyboardVt)(10), 12);
}

TEST(hook_vtables_shared_vtable_hooked_once) {
    // the usual case: keyboard and mouse share one vtable - the second device adds nothing
    HookUtil::VtableOriginals<Fn> h;
    int vt = 0;
    CHECK(h.Add(&vt, &OrigA, &Hook));
    CHECK(!h.Add(&vt, &Hook, &Hook));            // its slot already points at the hook
    CHECK(!h.Add(&vt, &OrigB, &Hook));
    CHECK_EQ(h.count, 1);
    CHECK(h.Find(&vt) == &OrigA);
}

TEST(hook_vtables_unknown_null_and_full) {
    HookUtil::VtableOriginals<Fn, 2> h;
    int a = 0, b = 0, c = 0;
    CHECK(h.Find(&a) == nullptr);                // never hooked: no original to call
    CHECK(h.Find(nullptr) == nullptr);
    CHECK(!h.Add(nullptr, &OrigA, &Hook));
    CHECK(!h.Add(&a, nullptr, &Hook));
    CHECK(!h.Add(&a, &Hook, &Hook));             // the hook is never its own original
    CHECK(h.Add(&a, &OrigA, &Hook));
    CHECK(h.Add(&b, &OrigB, &Hook));
    CHECK(!h.Add(&c, &OrigA, &Hook));            // no room: left unhooked rather than overwrite
    CHECK(!h.Has(&c));
    CHECK(h.Has(&a) && h.Has(&b));
}
