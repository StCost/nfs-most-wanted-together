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

#include "../src/exitutil.h"

TEST(exit_message_pump_never_treats_empty_queue_as_quit) {
    using namespace ExitUtil;
    CHECK(IsStaleQuit(false, WmQuit, PumpPeekReturn));
    CHECK(!IsStaleQuit(true, WmQuit, PumpPeekReturn));    // a real WM_QUIT must still quit
    CHECK(!IsStaleQuit(false, 0x100, PumpPeekReturn));
    CHECK(!IsStaleQuit(false, WmQuit, PumpPeekReturn + 1)); // no change to another caller's MSG
    for (uint32_t m = 0; m < 0x500; ++m)
        CHECK_EQ(IsStaleQuit(false, m, PumpPeekReturn), m == WmQuit);
}

TEST(exit_remote_event_does_not_drive_boot_screens) {
    using namespace ExitUtil;
    CHECK(!RemoteEventCanAct(0, false, false));
    CHECK(!RemoteEventCanAct(1, false, false));
    CHECK(!RemoteEventCanAct(3, true, true));       // title / splash / profile screen: no event, no Accept/Back
    CHECK(!RemoteEventCanAct(3, false, false));
    CHECK(RemoteEventCanAct(3, false, true));       // actual main menu: quick-race/Challenge Series joining stays supported
    CHECK(RemoteEventCanAct(6, false, true));       // ordinary shared event in free roam unchanged
    CHECK(RemoteEventCanAct(4, false, true));       // in-progress world load unchanged
}

TEST(exit_partner_already_in_event_notice_once_and_again_after_disconnect) {
    using namespace ExitUtil;
    BusyNotice b;
    CHECK(!PeerBusyInMenuTick(b, false, 3, true, 6, 123, true));
    CHECK(!PeerBusyInMenuTick(b, true, 1, true, 6, 123, true));
    CHECK(!PeerBusyInMenuTick(b, true, 3, false, 6, 123, true));
    CHECK(!PeerBusyInMenuTick(b, true, 3, true, 3, 123, true));
    CHECK(!PeerBusyInMenuTick(b, true, 3, true, 6, 0, true));
    CHECK(!PeerBusyInMenuTick(b, true, 3, true, 6, 123, false));
    CHECK(PeerBusyInMenuTick(b, true, 3, true, 6, 123, true));
    CHECK(!PeerBusyInMenuTick(b, true, 3, true, 6, 123, true));
    CHECK(PeerBusyInMenuTick(b, true, 3, true, 6, 124, true));
    CHECK(!PeerBusyInMenuTick(b, false, 3, false, 0, 0, false));
    CHECK(PeerBusyInMenuTick(b, true, 3, true, 6, 124, true));
    CHECK(!PeerBusyInMenuTick(b, true, 3, true, 6, 0, false));
    CHECK(PeerBusyInMenuTick(b, true, 3, true, 6, 124, true));
}

TEST(exit_all_five_flag_writers_and_formatting) {
    using namespace ExitUtil;
    int n; const Site* s = Sites(&n);
    CHECK_EQ(n, 5);
    CHECK_EQ(s[0].at, 0x5A4C87); CHECK_EQ(s[1].at, 0x55BE9B);
    CHECK_EQ(s[2].at, 0x57E667); CHECK_EQ(s[3].at, 0x6C2206); CHECK_EQ(s[4].at, 0x6DB7D4);
    CHECK_EQ(SetFlagBytes()[0], 0xC7); CHECK_EQ(SetFlagBytes()[1], 5);
    uint32_t dst = 0, val = 0;
    memcpy(&dst, SetFlagBytes() + 2, 4); memcpy(&val, SetFlagBytes() + 6, 4);
    CHECK_EQ(dst, ExitFlag); CHECK_EQ(val, 1);
    CHECK_STR(SiteName(-1), "an unknown place"); CHECK_STR(SiteName(5), "an unknown place");
    char a[80];
    FormatAddress(a, sizeof(a), "MWTogether.asi", 0x10000000, 0x10002345, false); CHECK_STR(a, "MWTogether.asi+2345");
    FormatAddress(a, sizeof(a), "speed.exe", 0x400000, 0x6C2206, true); CHECK_STR(a, "006C2206");
    CHECK_STR(BaseName("E:\\Game\\scripts\\other.asi"), "other.asi");
    CHECK_STR(BaseName("other.asi"), "other.asi"); CHECK_STR(BaseName(nullptr), "");
}

TEST(exit_stack_call_recognition) {
    using namespace ExitUtil;
    const uint8_t a[][7] = {
        {0x90,0x90,0xE8,0,0,0,0},                      // rel32
        {0x90,0x90,0x90,0x90,0x90,0xFF,0xD0},         // eax
        {0x90,0xFF,0x15,0x34,0x03,0x89,0x00},         // [IAT]
        {0x90,0x90,0x90,0x90,0xFF,0x50,0x10},         // [eax+10]
        {0x90,0xFF,0x90,0x24,0x01,0,0},              // [eax+124]
    };
    for (auto& x : a) CHECK(LooksAfterCall(x));
    uint8_t none[7] = {}; CHECK(!LooksAfterCall(none));
    uint8_t jump[7] = {0x90,0x90,0x90,0x90,0x90,0xFF,0xE0}; CHECK(!LooksAfterCall(jump));
}
