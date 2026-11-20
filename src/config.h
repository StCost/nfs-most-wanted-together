#pragma once
#include <string>

struct Config {
    bool        isHost = true;        // [Network] Mode = host | client
    std::string peerIp = "127.0.0.1"; // [Network] HostIP (client only)
    std::string clientIp;             // [Network] ClientIP (host only, optional): send to the client too,
                                      // so firewalls on both sides see the traffic as a reply
    int         port = 47555;         // [Network] Port (both sides bind it)
    int         sendRateHz = 60;      // [Network] SendRate
    std::string playerName = "Player";// [Player] Name
    bool        autoStart = true;      // [AutoStart] Enabled - title screen -> career automatically
    int         autoStartDelayMs = 2500;   // [AutoStart] DelayMs - wait in the menu before the first press
    int         autoStartIntervalMs = 1500;// [AutoStart] IntervalMs - between presses
    std::string autoStartSequence = "@WS_MW_LS_Splash.fng,ENTER,@MC_Main.fng,ENTER,@MainMenu.fng,ENTER,@MainMenu_Sub.fng,ENTER,WAIT,WAIT,WAIT,WAIT,ENTER,@Dialog.fng,LEFT,ENTER"; // [AutoStart] Sequence
    bool        hideCursor = true;    // [Window] HideCursor - hide the Windows arrow over the game
    bool        showOverlay = true;   // [Debug] Overlay
    bool        testKeys = false;
    bool        noSpeedbreaker = true;   // [Coop] NoSpeedbreaker
    bool        noPause = true;          // [Coop] NoPause - the world keeps running while a pause menu / map is open (connected only)
    bool        streamAroundPeer = true; // [Coop] StreamAroundPeer - load the world around the other player (full physics there)
    bool        skipIntro = true;        // [Start] SkipIntro - no logos / intro movie / Press START
    bool        directFreeRoam = true;
    bool        skipLoadComplete = true; // [Start] SkipLoadComplete - confirm the "load complete" screen by itself   // [Start] DirectFreeRoam - load career free roam right after the profile
    bool        skipLeaveSafeHouseDialog = true; // [Start] SkipLeaveSafeHouseDialog
    bool        skipPursuitResults = false; // [UI] SkipPursuitResults - close the pursuit result screen in free roam
    bool        startInBackground = false; // [Window] StartInBackground - give focus back on start, game muted until clicked
    bool        sharedWins = true;       // [Career] SharedWins - a win by either player counts for both
    bool        saveSync = true;         // [Career] SaveSync - the host keeps a copy of the client's save
    bool        perf = false;            // [Debug] Perf - log fps / frame times / mod cost every 5 s
    bool        autoPilot = false;       // [Debug] AutoPilot - the AI drives the local car in free roam and races (F5 toggles)
    bool        runInBackground = true;  // [Coop] RunInBackground - keep running when the window loses focus
    bool        uiIntroInput = true;  // [UI] IntroInput - menus accept input while their intro animation plays
    int         uiMenuSpeed = 2;      // [UI] MenuSpeed - 1 = normal, 2 = 2x, 3 = 3x menu animations
    std::string autoStartTarget = "freeroam"; // [AutoStart] Target - freeroam | safehouse | race
    std::string testEvent = "15.1.1"; // [Debug] TestEvent - event F7 starts
    bool        trafficSync = true;   // [Traffic] Sync
    int         trafficMirrorClass = 4; // [Traffic] MirrorClass - driver class for mirror cars (4 = none, 6 = remote)     // [Debug] TestKeys - F10/F11 spawn/remove a test car
    int         toggleKey = 0x77;     // [Debug] ToggleKey (VK code, default F8)
    bool        ghostEnabled = true;  // [Ghost] Enabled
    int         interpDelayMs = 100;  // [Ghost] InterpDelayMs - show the remote car this far in the past
    bool        ghostCollisions = false; // [Ghost] Collisions
    int         safeDistance = 10;    // [Ghost] SafeDistance - metres apart before collisions turn on

    void Load(const char* iniPath);
};

extern Config g_cfg;
