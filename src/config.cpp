#include "config.h"
#include <windows.h>
#include <cstdlib>

Config g_cfg;

static std::string ReadStr(const char* ini, const char* sec, const char* key, const char* def) {
    char buf[1024];
    GetPrivateProfileStringA(sec, key, def, buf, sizeof(buf), ini);
    return buf;
}

static int ReadInt(const char* ini, const char* sec, const char* key, int def) {
    std::string s = ReadStr(ini, sec, key, "");
    if (s.empty()) return def;
    return (int)strtol(s.c_str(), nullptr, 0); // accepts 0x77 or 119
}

void Config::Load(const char* ini) {
    std::string mode = ReadStr(ini, "Network", "Mode", "host");
    isHost       = _stricmp(mode.c_str(), "client") != 0;
    peerIp       = ReadStr(ini, "Network", "HostIP", "127.0.0.1");
    clientIp     = ReadStr(ini, "Network", "ClientIP", "");
    port         = ReadInt(ini, "Network", "Port", 47555);
    sendRateHz   = ReadInt(ini, "Network", "SendRate", 60);
    playerName   = ReadStr(ini, "Player", "Name", "Player");
    autoStartDelayMs    = ReadInt(ini, "AutoStart", "DelayMs", 2500);
    autoStartIntervalMs = ReadInt(ini, "AutoStart", "IntervalMs", 1500);
    autoStartSequence   = ReadStr(ini, "AutoStart", "Sequence", "@WS_MW_LS_Splash.fng,ENTER,@MC_Main.fng,ENTER,@MainMenu.fng,ENTER,@MainMenu_Sub.fng,ENTER,WAIT,WAIT,WAIT,WAIT,ENTER,@Dialog.fng,LEFT,ENTER");
#ifdef MWT_DEVTOOLS
    autoStart = ReadInt(ini, "AutoStart", "Enabled", 0) != 0;
#else
    autoStart = false;   // release ignores [AutoStart] even if a leftover ini still has it
#endif
    hideCursor   = ReadInt(ini, "Window", "HideCursor", 1) != 0;
    showOverlay  = ReadInt(ini, "Debug", "Overlay", 0) != 0;
    testKeys     = ReadInt(ini, "Debug", "TestKeys", 0) != 0;
#ifdef MWT_DEVTOOLS
    testSkipIntro  = ReadInt(ini, "Debug", "TestSkipIntro", 0) != 0;
    testPressStart = ReadInt(ini, "Debug", "TestPressStart", 0) != 0;
#endif
    noSpeedbreaker = ReadInt(ini, "Coop", "NoSpeedbreaker", 1) != 0;
    noPause = ReadInt(ini, "Coop", "NoPause", 1) != 0;
    runInBackground = ReadInt(ini, "Coop", "RunInBackground", 1) != 0;
#ifdef MWT_DEVTOOLS
    autoPilot = ReadInt(ini, "Debug", "AutoPilot", 0) != 0;
    autoPilotNos = ReadInt(ini, "Debug", "AutoPilotNos", 1) != 0;
#else
    autoPilot = false;           // release ignores [Debug] AutoPilot
    autoPilotNos = ReadInt(ini, "Debug", "AutoPilotNos", 1) != 0;
#endif
    syncControls = ReadInt(ini, "Ghost", "SyncControls", 1) != 0;
    syncDamage = ReadInt(ini, "Ghost", "SyncDamage", 1) != 0;
    perf = ReadInt(ini, "Debug", "Perf", 0) != 0;
    bootTrace = ReadInt(ini, "Debug", "BootTrace", 0) != 0;
    saveSync = ReadInt(ini, "Career", "SaveSync", 1) != 0;
    sharedWins = ReadInt(ini, "Career", "SharedWins", 1) != 0;
    sharedMilestones = ReadInt(ini, "Career", "SharedMilestones", 1) != 0;
    startTogether = ReadInt(ini, "Coop", "StartTogether", 1) != 0;
    separateGridSlots = ReadInt(ini, "Coop", "SeparateGridSlots", 1) != 0;
    coopPrologue = ReadInt(ini, "Coop", "Prologue", 1) != 0;
    catchUpLeader = ReadInt(ini, "Coop", "CatchUpLeader", 1) != 0;
    bossHelper = ReadInt(ini, "Coop", "BossHelper", 1) != 0;
#ifdef MWT_DEVTOOLS
    bossHelperSolo = ReadInt(ini, "Debug", "BossHelperSolo", 0) != 0;
#else
    bossHelperSolo = false;
#endif
    noSlowMotion = ReadInt(ini, "Coop", "NoSlowMotion", 0) != 0;
    partnerInRivalRaces = ReadInt(ini, "Coop", "PartnerInRivalRaces", 1) != 0;
    syncWeather = ReadInt(ini, "Coop", "SyncWeather", 1) != 0;
    followToPursuits = ReadInt(ini, "Coop", "FollowToPursuits", 1) != 0;
    restoreOnlyMissing = ReadInt(ini, "Career", "RestoreOnlyMissing", 1) != 0;
    sharedPursuits = ReadInt(ini, "Coop", "SharedPursuits", 1) != 0;
    syncStoryScenes = ReadInt(ini, "Coop", "SyncStoryScenes", 1);
    if (syncStoryScenes < 0 || syncStoryScenes > 2) syncStoryScenes = 1;
#ifdef MWT_DEVTOOLS
    readyDelayMs = ReadInt(ini, "Debug", "ReadyDelayMs", 0);
#else
    readyDelayMs = 0;
#endif
#ifdef MWT_DEVTOOLS
    startInBackground = ReadInt(ini, "Window", "StartInBackground", 0) != 0;
#else
    startInBackground = false;   // release ignores [Window] StartInBackground
#endif
    gameNotices = ReadInt(ini, "UI", "GameNotices", 1) != 0;
    hideHudKey = ReadInt(ini, "UI", "HideHudKey", 0x70);
    noticeSeconds = ReadInt(ini, "UI", "NoticeSeconds", 10);
    if (noticeSeconds < 2) noticeSeconds = 2;
    if (noticeSeconds > 60) noticeSeconds = 60;
    gamepadHotplug = ReadInt(ini, "Input", "GamepadHotplug", 1) != 0;
    keepPadBindings = ReadInt(ini, "Input", "KeepPadBindings", 1) != 0;
    lastProfile = ReadStr(ini, "Start", "LastProfile", "");
    iniFile = ini ? ini : "";
#ifdef MWT_DEVTOOLS
    skipLoadComplete = ReadInt(ini, "Start", "SkipLoadComplete", 0) != 0;
#else
    skipLoadComplete = false;   // release ignores [Start] SkipLoadComplete
#endif
    streamAroundPeer = ReadInt(ini, "Coop", "StreamAroundPeer", 1) != 0;
    autoStartTarget = ReadStr(ini, "AutoStart", "Target", "freeroam");
    testEvent    = ReadStr(ini, "Debug", "TestEvent", "15.1.1");
    trafficSync  = ReadInt(ini, "Traffic", "Sync", 1) != 0;
    trafficMirrorClass = ReadInt(ini, "Traffic", "MirrorClass", 4);
    trafficSharedInRaces = ReadInt(ini, "Traffic", "SharedInRaces", 1) != 0;
    trafficSmoothHandover = ReadInt(ini, "Traffic", "SmoothHandover", 1) != 0;
    trafficNoSpawnNearPlayers = ReadInt(ini, "Traffic", "NoSpawnNearPlayers", 1) != 0;
    toggleKey    = ReadInt(ini, "Debug", "ToggleKey", 0x77);
#ifdef MWT_DEVTOOLS
    netDelayMs = ReadInt(ini, "Debug", "NetDelayMs", 0);
    netJitterMs = ReadInt(ini, "Debug", "NetJitterMs", 0);
    netLossPercent = ReadInt(ini, "Debug", "NetLossPercent", 0);
    netDuplicatePercent = ReadInt(ini, "Debug", "NetDuplicatePercent", 0);
    netReorderPercent = ReadInt(ini, "Debug", "NetReorderPercent", 0);
    if (netDelayMs < 0) netDelayMs = 0;
    if (netDelayMs > 2000) netDelayMs = 2000;
    if (netJitterMs < 0) netJitterMs = 0;
    if (netJitterMs > 1000) netJitterMs = 1000;
    if (netLossPercent < 0) netLossPercent = 0;
    if (netLossPercent > 100) netLossPercent = 100;
    if (netDuplicatePercent < 0) netDuplicatePercent = 0;
    if (netDuplicatePercent > 100) netDuplicatePercent = 100;
    if (netReorderPercent < 0) netReorderPercent = 0;
    if (netReorderPercent > 100) netReorderPercent = 100;
#else
    netDelayMs = netJitterMs = netLossPercent = netDuplicatePercent = netReorderPercent = 0;
#endif
    ghostEnabled = ReadInt(ini, "Ghost", "Enabled", 1) != 0;
    interpDelayMs= ReadInt(ini, "Ghost", "InterpDelayMs", 100);
    ghostCollisions = ReadInt(ini, "Ghost", "Collisions", 1) != 0;
    safeDistance = ReadInt(ini, "Ghost", "SafeDistance", 10);
    if (sendRateHz < 1) sendRateHz = 1;
    if (sendRateHz > 120) sendRateHz = 120;
    if (port < 1 || port > 65535) port = 47555;        // would wrap when cast to a 16-bit port
    if (interpDelayMs < 0) interpDelayMs = 0;          // negative = showing the future (all extrapolation)
    if (interpDelayMs > 1000) interpDelayMs = 1000;
    if (safeDistance < 0) safeDistance = 0;
}
