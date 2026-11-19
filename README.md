# CoopMod: co-op career for Need for Speed: Most Wanted (2005)

Play the Most Wanted career together. Two players share one world: each drives their own
car with its own tuning, sees the other player's car, traffic and cops, and they start events
together. The host's game is the reference world, and the client mirrors it.

> **Status: alpha, 2 players.** Free roam, shared events and races work and have been tested on two
> PCs over LAN. Career progress sharing, cutscene sync and more than 2 players are still in progress;
> see [ROADMAP.md](ROADMAP.md).

## Requirements
- NFS Most Wanted (2005), PC, **v1.3** `speed.exe`. Other versions are detected and the mod
  stays inactive.
- [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) (`dinput8.dll`, included in releases).
- Both players need the same CoopMod version: the protocol version is checked on connect.
- A network path between the PCs: LAN, a VPN such as Radmin VPN or ZeroTier, or UDP port 47555
  forwarded to the host.

This repository contains **only the mod**. It does not include game files or saves. You need your
own copy of the game.

## Install
1. Download the latest release zip from the **Releases** page.
2. Copy `dinput8.dll` and the `scripts` folder next to `speed.exe`. If you already have an ASI loader,
   keep yours and copy only `scripts\CoopMod.asi` and `scripts\CoopMod.ini`.
3. Edit `scripts\CoopMod.ini`:
   - **Host:** `Mode=host`, `Name=<you>`. `ClientIP=<client IP>` is optional; set it to skip firewall rules on a LAN.
   - **Client:** `Mode=client`, `HostIP=<host IP>`, `Name=<you>`.
4. Start the game on both PCs. The game skips the intro and loads career free roam by itself, and the
   two games connect automatically.

## Features

### Connection
- Direct UDP peer-to-peer (host/client) on port 47555 with ping, a timeout, and a "bye" message on quit (Alt+F4 included).
- Stays connected while a game is paused, minimized or alt-tabbed (keep-alive thread). The game keeps running in the background.
- Status overlay (F8 toggles it; the choice is saved).

### Players
- **Each player drives their own car, model and full tuning**, which the other game spawns.
- Smooth movement: snapshot interpolation on the game's simulation clock, measured at about 1 to 10 mm jitter.
- Collisions between players, with spawn protection so cars that spawn on the same spot don't fling each other.
- Player blips on the minimap and the world map, which stay live while the map is open.
- Player names in the race standings.

### World
- **Shared traffic:** near each other, the client's traffic is the host's traffic.
- **Cops:** each player has their own heat and pursuit. Both players see each other's pursuit cops, and
  a cop may switch to chasing the other player when both are in a pursuit.
- **Full physics around both players:** the host also streams the world around the client, so AI
  cars there keep collisions instead of the cheap "floating" simulation.
- The pause menu and world map don't stop the world while connected.
- No speedbreaker in co-op: slowing time can't work for two players.

### Events and races
- **Shared event start:** start an event and the other player is teleported into the same event.
- **Same opponents on both PCs:** the starting game sends its AI roster (models and tuning). The client's
  AI racers follow the host's racers, so positions and names match.
- The other player is a real racer in the standings, with their real race progress.
- **Restarts sync:** restarting a race restarts it for both players.
- Cutscenes are left to the game, with no network interference during race intros.

### Quality of life (settings in `[Start]` / `[UI]`)
- Skip the intro logos and movie, then load career free roam directly after the profile (`SkipIntro`, `DirectFreeRoam`).
- No "leave the safe house?" question (`SkipLeaveSafeHouseDialog`).
- Menus accept input during their intro animations, and menu animations run 2x faster (`IntroInput`, `MenuSpeed`).

### Test tools (`[Debug] TestKeys=1`)
- **F5:** autopilot. The game's AI drives your car: it cruises in free roam and follows the route in races.
  `AutoPilot=1` switches it on automatically.
- **F7:** start the test event (`TestEvent`) for both players.
- **F10 / F11:** spawn or remove a test copy of the other player's car. **F12:** spawn a test cop.

## Configuration
Every option is in `scripts\CoopMod.ini`, which comments each one. Main sections:
`[Network]`, `[Player]`, `[Ghost]`, `[Traffic]`, `[Coop]`, `[Start]`, `[AutoStart]`, `[UI]`, `[Window]`, `[Debug]`.

## Build from source
- Visual Studio 2022 with the C++ workload (MSVC x86).
- Run `build.bat`. It produces `build\CoopMod.asi`. When this folder sits inside the game folder
  (`<game>\CoopMod-src\`), the script also installs the plugin into `<game>\scripts\`.
- CI: every push builds the plugin (see **Actions**). To publish a release, run the **Build** workflow
  manually with a version number. It builds, packages the zip with the ASI loader, and creates the GitHub release.

## How it works (short)
`CoopMod.asi` is loaded by the ASI loader inside `speed.exe`. It hooks Direct3D 9 `Present` to run once per
frame on the game thread, and it patches game code and vtables at known v1.3 addresses, found by reverse
engineering. Remote cars are real game vehicles (`PVehicle`) whose rigid bodies are set every frame from
interpolated network snapshots. Race opponents and traffic of the other PC are "mirrors" driven the same way.
The code is in `src/`:

| File | What |
|---|---|
| `main.cpp` | frame loop, hotkeys, overlay text, start-up patches |
| `net.*` | UDP protocol (hello, state, car info, traffic, events, roster, restart, keep-alive) |
| `game.cpp` | vehicles, spawning, the other player's car (ghost), race integration, autopilot |
| `traffic.inl` | traffic, cop and race-opponent mirroring |
| `event.inl` | shared event start, roster, co-op rules (speedbreaker, no pause, restart sync) |
| `pursuit.inl` | cops chasing the other player |
| `stream.inl` | world streaming around the other player |
| `minimap.inl`, `worldmap.inl` | player blips |
| `boot.inl` | fast start (skip intro, direct free roam, no safe-house dialog) |
| `input.*` | DirectInput observation, menu automation (AutoStart) |
| `overlay.*` | D3D9 hooks and the status overlay |

## Contributing
Contributions are welcome. See [CONTRIBUTING.md](CONTRIBUTING.md).

## Credits
- Reverse-engineering references: NFSPluginSDK (MW05 headers), MostWantedTimeTrialGhosts and
  nya-common-nfsmw (MPL-2.0), ExOpts.
- [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) by ThirteenAG.
- Need for Speed and Most Wanted are trademarks of Electronic Arts. This is an unofficial fan project, not affiliated with EA.
