#pragma once
#include "net.h"
#include <string>

// Everything that touches speed.exe internals lives behind this interface.
namespace Game {
    bool Init();                 // verify exe version / addresses
    int  GameFlowState();        // 6 = in game (driving), -1 = unknown

    // Fill 'out' with the local player's car state. Returns false if no car (menus/loading).
    bool ReadLocalCar(CarState& out);

    // Drive the "ghost" car to the remote player's state. Called every frame while connected.
    // Returns a short status string for the overlay.
    std::string UpdateGhost(const Net::Snapshot& latest, const Net::Snapshot& prev, bool enabled);

    // Called when the peer disconnects / ghost disabled, to give the car back to the game.
    void ReleaseGhost();

    std::string DebugInfo();     // extra lines for the overlay

    bool InstallVehicleFactoryHook(); // races: build one AI racer as the peer's car
    void OnFlow(int flow);            // call every frame with the game flow state
    bool ReadLocalCarInfo(CarInfo& out); // my car model + customization

    // Traffic sync (host sends its traffic, client mirrors it when near the host)
    bool InstallTrafficHooks();
    float SimTimeSec();           // game simulation clock (s)

    // Shared event start
    bool InstallEventSync();
    bool InstallMinimapHook();     // remote players get minimap icons
    bool InstallWorldMapHook();    // ...and a world map icon
    void EventTick(bool connected);
    const char* EventStatus();
    void CoopRulesTick(bool connected);
    void PauseRuleTick(bool connected);
    std::string ToggleAutopilot();
    std::string DebugRestart();
    std::string FEAccept(const char* package);   // test: send PAD_ACCEPT to a front-end screen         // test: restart the current race
    void AutopilotTick();               // [Debug] AutoPilot: AI drives in free roam / races      // test key: the game's AI drives the local car   // no pause while connected + race restart sync
    bool InstallRestartSync();
    bool InstallStreamHook();
    void InstallBootPatches();          // call from DllMain (before the front end starts)
    void BootTick();
    void PursuitResultTick();
    bool InstallNisHook();              // cutscene skip voting
    void NisTick(bool connected);
    uint32_t NisSceneHash();
    const char* NisStatus();           // [UI] SkipPursuitResults                    // [Start] DirectFreeRoam
    bool DirectStartActive();   // co-op rules (no speedbreaker)
    std::string DebugStartEvent(const char* eventId);   // F7 test
    void TrafficTick(bool connected, const Net::Snapshot& peer);
    void PursuitTick(bool connected, const Net::Snapshot& peer);   // cops may chase the other player
    int16_t LocalPursuitHeat();
    int  TrafficCount();          // host: cars sent last tick; client: mirror cars alive
    bool TrafficSuppressed();     // client: own traffic switched off

    // Test: spawn a copy of the local car (same model + tuning) nearby / remove it.
    std::string DebugSpawnClone();
    std::string DebugKillClone();
    std::string DebugSpawnCop();   // F12 (TestKeys=1): spawn a patrol cop
}
