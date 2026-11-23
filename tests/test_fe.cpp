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
