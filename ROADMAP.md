# CoopMod roadmap: co-op career for NFS Most Wanted (2005, v1.3)

## Goal
The whole career is playable together: every Blacklist race, every challenge, free roam,
pursuits, cutscenes and mission intros. Up to 4 players. The **host runs the game**: they own
the save file, decide the world state (traffic, cops, race results) and keep the progress.
Every client is treated as a second "host-player": anyone can start the next event, and starting
an event teleports everyone into it.

## Core model
- **Host-authoritative.** The host's game is the real one: career progress, which race is running,
  heat level, cops, traffic. Clients send their own car's driving state and requests ("start this event").
  The host validates them and broadcasts the result.
- **Every player drives their own car locally** (no input lag for yourself). Other players' cars are
  remote-controlled vehicles in your world, smoothed with prediction.
- **Game engine leftovers we can reuse.** MW shipped with online racing, and the exe still has the
  `DRIVER_REMOTE` driver class, `VEHICLE_REMOTE` / `PLAYER_REMOTE` lists and `RBRemote` (a rigid-body
  mode for networked cars). The long-term goal is to put remote players there instead of hijacking AI racers.

## Milestones

| # | Milestone | Status |
|---|-----------|--------|
| 1 | Plugin loads, overlay, UDP direct-IP connection, ping | **done, verified on 2 PCs** |
| 2 | Ghost car: peer's car drives an AI racer slot in Quick Race | **done, verified on 2 PCs** |
| 2b | **Own car per player**: model + full customization sent over the network (PKT_CAR); races build one AI racer slot as the peer's car (factory hook 0x92C534) | **done, verified (laptop raced the PC's IS300)** |
| 2c | AutoStart: screen-aware menu automation (`@Screen.fng` steps, key retry) to free roam or a Quick Race | **done, verified** |
| 3 | Smooth movement + collisions between players | basic smoothing done; collisions experimental |
| 4 | Up to 4 players (host relays states), player IDs | |
| 5 | Remote cars in **free roam**: spawn the peer's own car (PVehicle::Construct + our IVehicleCache so the budget check evicts traffic for unloaded models) | **done, verified both ways on 2 PCs** (model key = hash of model name; remote body vtable 0x8AD178 accepted by function check) |
| 3b | **Smooth motion**: snapshot interpolation on the game's simulation clock (0x9885D8), 60 Hz, gliding clock offset | **done, verified: ≤10 mm jitter moving, ~0.2 mm parked** (was up to 420 mm) |
| 6 | Shared **event start**: anyone requests, host approves, everyone teleports + same race loads | |
| 7 | Race results with multiple humans (finish order, who "won" for the career) | |
| 8 | **Traffic sync**: host sends its traffic (20 Hz, nearest 20 to the client); client within 350 m switches its own traffic off (ComputeDensity call 0x439BBD) and drives mirror cars; far away it keeps its own; lights synced (IVehicle+0x9C) | **done, verified on 2 PCs** |
| 9a | **Cops mirrored**: host cops included in the traffic stream; client cop spawns suppressed near host (calls 0x4315CF, 0x42ED7C, 0x42ED9B); light bars synced | **done, verified (cops with light bars on the laptop)** |
| 9b | **Pursuit state** (heat, who is chased, busted/escaped, bounty) | next |
| 10 | **Cutscenes (NIS / FMV) and mission intros** played together, with skip voting | |
| 11 | **Career / save**: host save is the truth; client progress stored by host and applied on connect | |
| 12 | Blacklist rival races, milestones, challenge series, bounty | |
| 13 | Steam transport (Spacewar 480) for invites + NAT punch-through (optional, keeps direct IP) | optional |

### 3. Smooth movement and collisions
- Packet rate 30 Hz → interpolation buffer and a physics-based correction (velocity steering, already started).
- Real collisions: both sides own their car. On contact, each side applies the impact to its own car,
  so nobody gets "pushed by a wall".
- **Spawn protection** (done in v0.1 as an option): collisions stay off until players have been more than
  `SafeDistance` metres apart once. The same rule applies after every teleport, reset and race start.
  Also ghost the car (semi-transparent / no collision) for a few seconds after a reset.

### 5. Remote cars in free roam
- `PVehicle::Construct` (0x689820) can create a vehicle. We need a safe way to call it in free roam
  with the right car model + customisation (the peer's tuned car), and to destroy it on disconnect.
- Mark it `DRIVER_REMOTE`, so cops and AI treat it as a player but no AI drives it.

### 6. Events: anyone can start, everyone teleports
- Client walks into a race marker / picks from the map → sends `REQUEST_EVENT(id)` to the host.
- Host checks the event is available in **its** career, then broadcasts `START_EVENT(id, seed, grid)`.
- Every game loads the same event, and players take the grid slots (AI racers fill the rest).
- Everyone waits at "loading" until all are ready → synchronised countdown.

### 9. Cops and traffic
- Only the host runs cop/traffic AI. Clients disable their own spawning and mirror the host's cars
  (positions, sirens, roadblocks, spike strips, helicopter).
- Pursuit state (heat, bounty, cooldown, busted/escaped) comes from the host. Getting busted means
  busted for that player. Whether the pursuit ends for everyone is a design choice we can make later.

### 10. Cutscenes and intros
- Host triggers NIS / FMV playback → `PLAY_CUTSCENE(id)`; clients play the same one.
- Skipping: host decides, or majority vote. Race starts only after everyone has finished or skipped.

### 11. Saves and progress
- The **host's save** is the career everyone plays.
- Each client's own progress (cash, cars, rap sheet) is stored **by the host**, in a separate file
  per player name / ID, next to the host save.
- On connect: host sends the client their stored profile (car, upgrades, cash), and the client applies it
  in memory (never overwriting the client's own single-player save unless they opt in).
- After each event: host updates everyone's stored progress and autosaves.
- This needs reverse engineering of the career / profile structures (`UserProfile`, `FEPlayerCarDB`),
  which nya-common-nfsmw already partly documents.

## Known hard parts / risks
- No existing mod does free-roam spawning or cop sync, so this is original reverse engineering.
- AI is disabled for *all* racers while the ghost is active (v0.1 shortcut). Multi-human races need per-car control.
- The game's simulation isn't deterministic across PCs, so we sync states, not inputs.
- Compatibility: ExOpts and NFSMW Limit Adjuster patch the same areas; the plugin checks before patching.
