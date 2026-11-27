# Testing NFS MW Together

There are three layers. Use all three before a release, and the first one before every commit.

| Layer | What it covers | Needs |
|---|---|---|
| 1. Automated tests | network code, packets, config, save transfer, maths | nothing but the compiler |
| 2. One PC | start-up, solo racing, test tools, performance | the game |
| 3. Two PCs | everything co-op | the game on two PCs on the same network |

## 1. Automated tests (no game)

```
tests\build_tests.bat            build and run (also an AddressSanitizer pass if your MSVC has it)
tests\build_tests.bat noasan     faster
```

Exit code 0 and `N test(s) run, 0 failed` is a pass. Protocol changes must update `tests\test_protocol.cpp`
(struct sizes, offsets, `COOP_VERSION`) in the same commit: the tests fail on purpose until you do.

## 2. Test tools inside the mod

Set `[Debug] TestKeys=1` in `scripts\MWTogether.ini`. Nothing below works without it, and only in a dev build
(`build.bat` without `MWT_RELEASE=1`): release builds leave the test keys and commands out.

Hidden `[Debug]` keys, also **dev builds only** (compiled out of a release ASI, ignored even if they remain
in the ini, and not shown in the Together menus). They are the test-only stand-in for the old user-facing
Skip intros / Quick career start, so an automated launch is not left on the intro or the "Press START" /
"press to continue" screen:

```
[Debug]
TestSkipIntro=1
TestPressStart=1
```

`TestSkipIntro` drops the EA logo, PSA and attract movie from the boot table. `TestPressStart` presses
START on `WS_MW_LS_Splash.fng` / `MW_LS_Splash.fng`, including when the game is unfocused. After that,
`[Start] SkipLoadComplete=1` (dev builds only; a release ASI ignores it) can confirm the profile load
screen. Career free roam is started with the `freeroam` command, not a user option.

`tools\ingame.ps1` is the shared launch helper: it presses START if the splash is still up, then sends
`freeroam`. `tools\coop-test.ps1` uses it from `Start-Game`. Dot-source that helper from any new in-game
script; do not wait blindly after starting `speed.exe`.

**Keys** (only while the game window is focused): `F5` autopilot on/off, `F7` start the test event
(`[Debug] TestEvent`), `F10`/`F11` spawn/remove a copy of the other player's car, `F12` spawn a cop.

**Command file.** Write one command per line into `scripts\MWTogether.cmd`; the mod reads it within half a
second, deletes it and logs `Command '<text>': <result>`. This works with the game in the background.

| Command | What it does |
|---|---|
| `event <id>` | start a career event for both players, e.g. `event 15.1.1` |
| `events` | list every event in the log: id, type, `BOSS`, preset car |
| `restart` | restart the current race |
| `slowai` | AI opponents stop driving (so a player wins) |
| `cash` | log career cash and whether the current event counts as won |
| `autopilot` | toggle the autopilot |
| `skipnis` | press "skip" on the current cutscene |
| `story` / `story <movie>` | log the story-scene state (movie, scene hash, the other player's scene, hold, rival number) / play a story movie over the world here as the career would, e.g. `story storyfmv_pin11`, `story blacklist_15` |
| `pursuit` / `uncop` | start a pursuit on you / remove all police cars |
| `roadblock [plain\|spikes\|list]` | while chased (`pursuit`): build a roadblock for your pursuit now, plain or with spike strips (needs a straight road 250 m ahead and a few cops); `list` logs your roadblocks and the other player's |
| `ram` | from far away: the first call loads the world next to the other car, the second (5 s later) parks 25 m from it, the third (5 s later) hits it at 90 km/h. Closer than 30 m: hit at once |
| `ram cop` | the same against the nearest of the other player's police cars shown here |
| `pos` | log own position, speed and damage |
| `peerbar chase <-100..100>` / `peerbar off` | show the partner's pursuit strip with a fixed value (no partner needed, the HUD must be up) / back to the partner's own |
| `tp <x> <y> <z>` | move your car there (numbers as `pos` prints them). The first call loads the world at that spot; send the same `tp` again about 5 s later to move. Handy to reach a marker the autopilot won't drive to |
| `clone` / `unclone` / `cop` | spawn/remove a copy of the other player's car, spawn a cop |
| `fakefriend` | spawn a nearby car with a purple BASE_PAINT and treat it as the friend for map / minimap blips (one-PC checks) |
| `notice <text>` / `notice warn <text>` | show a notice in the centre of the screen, green / orange (in a cutscene or menu: the song banner) |
| `careerbin <n>` | TEST PROFILES ONLY: set this career's current Blacklist rival (profile +0xB0) to n in memory - the next autosave keeps it. E.g. `careerbin 1` on both PCs, then `event 1.8.1` (the final pursuit) or Razor's races (1.1.2, 1.2.3, 1.5.2, 1.7.3): rival events are only joined at the same rival |
| `ground` | log every car here (ours, the other player's, AI racers, cops, traffic): physics mode (1 full, 2 emulated = height from the road path, may float), height above our ground (cars ~0.7 m, trucks ~1.8 m), copies marked `copy`; replies the counts. In co-op the client also logs every 10 s `Traffic: N frame(s) of copies put on the ground (largest correction X m)` while it corrects floating copies |
| `milestones` | log our rival's open and done milestones / speed traps (key, rival, value) and print the counts |
| `milestone` / `speedtrap` | complete the first open milestone / speed trap of our rival here, through the game's own award (bounty included), as if we had earned it. With co-op on it's then sent to the other player |
| `shot <file.bmp>` | save the next frame (without the overlay) |
| `accept <Screen.fng>` | send "accept" to a menu screen |
| `fe <pad> <Screen.fng>` | send a pad message (accept, back, left, right, up, down) to a menu screen without focus. Doesn't reach Yes/No dialogs (Dialog.fng) |
| `watchload <s>` | log once a second what an in-world loading waits for: the movie / NIS gates, the race status's "loaded" checks (racers, vehicles, the cop manager's queued spawns and its no-new-cops timer), and the code that raises that timer (hardware write watch). Found the final pursuit's 2-minute wait |
| `fixedlists` | log every fixed-capacity table in the game's data (capacity, fill): which tables would overflow past the 20-car limit |
| `stack` | the game's return addresses at the next frame (which loop draws it) |
| `screens` | log every menu screen that is open now (all screen names in the exe, not only the known ones) |
| `freeroam` | from the front end (main menu, or the safe house after a story movie) load career free roam, no focus needed |
| `hud` / `hudscan` | toggle the F1 HUD hide / log every object of the HUD package with its position (to pick what F1 hides) |
| `icons` | log every minimap icon drawn in the next second (position, pinned to the edge, call site): finds the GPS target of a story drive, e.g. Mia's meeting point after the car lot |

**Useful ini switches:** `[Debug] AutoPilot=1` (the AI drives in free roam and races), `[Window]
StartInBackground=1` (start without taking focus, muted), `[Debug] Perf=1` (fps and the mod's cost every
5 s), `[Debug] ReadyDelayMs=6000` (pretend to load slowly), `[Debug] BootTrace=1` (log the boot screens).

**Useful event ids:** `15.1.1` circuit, `15.1.2` rival race (Razor), `14.4.1` tollbooth, `13.5.1` speedtrap,
`12.7.3` drag, `15.3.1` lap knockout, `1.8.1` final pursuit, `19.8.32` Challenge Series tollbooth (preset car). `events` lists all 208.

**The log** is `scripts\MWTogether.log`, rewritten on every start. The game keeps it open, so read it with
shared access (plain `Get-Content` works; some tools need "open for read, allow read/write").

## 3. Scenarios (`tools\coop-test.ps1`)

Run on the **host**. On the other PC just start the game and let it connect; for scenarios that restart
the host, restart the client too when it sees the host leave. Both PCs: same build, `TestKeys=1`,
and on the host a dev build with `TestSkipIntro=1` and `TestPressStart=1` (see section 2) so
`Start-Game` is not left on the intro or Press START screen. `AutoPilot=1` unless the scenario says "parked".

```
tools\coop-test.ps1 -Scenario list
tools\coop-test.ps1 -Scenario race
tools\coop-test.ps1 -Scenario boss -Event 15.1.2
tools\coop-test.ps1 -Scenario solo -Solo
```

| Scenario | Pass looks like (H = host log, C = client log) |
|---|---|
| `smoke` | H+C `Connected to peer`, `Ghost acquired: spawned peer car`, a `joined` notice. No `CRASH`. |
| `race` | H+C `standings use the other player's own progress hooked` at start-up. C `Event: using the peer's opponents`, `our racer ... follows the host's opponent`, `the other player's grid slot x y z`, H `Start: the client starts on the slot our race gave their racer ... - telling them`, C `Net: the host's grid slot for us ...`, then on the grid C `Start: we stood on the host's grid slot - moved to the slot the host's race gave our racer (N m)` (N = a few metres) and the client stands on the slot the host's intro showed it on - no empty slot on either screen; from then on neither log shows `spawn protection` freezing the car and both cars drive away at the go. The other player's name is in the standings during the intro and countdown. While racing `Race: peer pct A (sent B) ...`: A is a little above B at speed, and the rank flips within a car length when one passes the other (drive side by side, then let the other pass). After the finish C `the other player finished (rank 1 there, 1 here)`, `the other player won - counted as our win too`; C's `cash` goes up; C returns to free roam and its save file is rewritten. |
| `boss` | H `sent 1 opponent(s)`; H and C `Race: rival race - rival built as model XXXXXXXX (ok)` with the same model, C `rival race - the event's own rival car, not the partner's roster`, `our racer ... now follows the host's opponent`, `Ghost acquired: spawned peer car` (the partner is not a racer here). Standings on each PC: that player + the rival + the helper racer: H and C `Race: rival race together - 1 opponent(s) + 1 helper racer`, `helper racer = opponent 1, racer slot N`, `helper racer built as model XXXXXXXX, ...` with the same model on both (C: `the host's helper`). With `[Coop] BossHelper=0`: no helper. Solo check of the helper alone: `[Debug] BossHelperSolo=1`, `event 1.1.2` (3 rows in the standings). See Shared finish below for the end of the race. |
| `restart` | H `restarted here - telling the peer`, C `peer restarted the race - restarting here too`; C is back on the grid with a new countdown, logs `Start: we stood on the host's grid slot - moved to the slot the host's race gave our racer` again (the same slot) and the two cars stand apart. |
| `cutscene` | One vote: H `voted to skip ... waiting`, intro runs its full ~7.7 s on both. Both vote (`skipnis` on C too): `both voted - skipping`, intro ~3 s. |
| `pursuit` | The PC that is NOT chased logs `Cops: showing the other player's police car` and `Pursuit: mine no ... theirs heat 1.x, 1 of their cops shown here`. A parked car is busted in ~30 s: with `SharedPursuits=1` (default) the other player is busted right after (see Shared pursuits below); with 0 that player lands in the garage and the other keeps playing. |
| `ram` (client parked, `AutoPilot=0`) | H `hit the other player, lost ~60 km/h - sent to them`, C `hit by the other player, pushed by ~35 km/h`, C's `pos` moves ~4 m. No hit line the other way round. |
| `together` (client has `ReadyDelayMs=6000`) | H `on the grid first - waiting for <name> to load...` then `go (both ready, waited 6.x s)`. |
| `results` | C stays on its results screen after 15.1.1; H leaves its own and starts 15.1.2. C logs `invited while we're on the results screen - leaving it to join`, then `starting 15.1.2`; both race 15.1.2 together. |
| `kinds` | All four events run 70 s with no `CRASH` on either PC. C logs `Traffic: in a race - mirroring the host's traffic with our own traffic cars` (with `SharedInRaces=0`: `in a race - keeping our own traffic`), then `mirroring the host's traffic (N cars, M of the host's without a car here)` with N > 0 in tollbooth / drag / knockout; after the `in a race` line C logs no `KillVehicle` until `race over` (only mirrors of the host's cops may go). Drag (12.7.3) used to crash at ~28 s when the client removed its own traffic. |
| `solo` | Race alone: no `Start: on the grid` hold, no `Collision` lines, the win pays out as in the unmodded game. |

## Checks without a script

| Check | How | Pass |
|---|---|---|
| Join from far away | one player drives > 500 m away, the other starts an event | the joining PC logs `keeping our copy of the other player's car until their racer is built`, then `AI racer built as peer car` (or, if the model still isn't loaded, `their racer keeps this AI car's model`) and `Ghost acquired: racer built as peer car`; on both PCs the other player is in the standings with their name from the countdown on |
| Save restore | client: close the game, move its save away, start again | C `restored save '<profile>' from the host (we had none)` before `pressing START`; the career loads |
| Background start | `StartInBackground=1`, keep another window focused | free roam reached, `Window: in the world - muted until clicked` |
| Smoothness | one car parked, the other on autopilot | `Ghost stats ... jitter` mostly under 20 mm |
| Performance | `Perf=1` | `mod` under about 0.2 ms/frame |
| Visual state | watch the partner's car | brake lights when it stops; `Ghost: damage zones now X` after it crashes |
| Join / leave | start and quit one game | centre of the screen: ONE green `<name> joined` (never a `Player joined` first; `The other player joined` only if no name came within 3 s) / orange `left` on the other |
| Shared race traffic | both PCs `AutoPilot=1`, start 14.4.1 / 12.7.3 / 15.3.1 with `event`, cars close together | C `our traffic car ... now follows the host's car N (same model ...)`; traffic around you is in the same places and models on both screens (a few extra cars on C are its spare own traffic, they keep their own AI); hitting a traffic car on C logs `Collision: hit the other player's car` and pushes it on H. After the finish C `race over`, `released N of our traffic cars`, and 8 s later its own traffic is replaced by mirrors as in free roam |
| Smooth traffic handover | `SmoothHandover=1` (default). Free roam, H parked in a busy street, C drives with traffic around it. (1) C drives straight away from H past 450 m and watches the cars around it. (2) C turns back and drives towards H, looking ahead | (1) C `Traffic: away from the host (... m) - using own traffic (the host's cars around us drive on as ours)`, then `the host's car N (...) drives on as our own traffic car ... (traffic AI, the game culls it)` and `left the host's traffic - N of the host's cars drive on as our traffic (M taken over by the game's traffic manager)`; no car around C disappears, they keep driving in their lanes (not stopped dead, not off the road); later `KillVehicle` lines only for cars out of sight. (2) C `near the host ... (our cars stay until out of view, host cars appear out of view)`, then every ~5 s `handover - X of our cars still in view, Y host cars waiting for a spot out of view, Z shown by our own cars ...` with X falling to 0; `our traffic car ... now shows the host's car N (... in its spot / nearby, in view ...)` when one of C's cars takes a host car's place (it glides there over ~1.5 s). No car appears or vanishes in front of C; no two cars inside each other. Rerun with `SmoothHandover=0`: the old instant switch (cars vanish / pop in) |
| No cars popping up in front | `NoSpawnNearPlayers=1` (default) on both. Start-up: both `Traffic: new traffic / cops never right in front of the other player - spot checks hooked: traffic ok, cops 4/4`. (1) Free roam, both drive side by side through busy streets, then 100-300 m apart (each with its own traffic / patrols, the client in a race with `SharedInRaces=0`). (2) A pursuit on H with C driving ahead of H. (3) C drives towards H's traffic looking ahead (near the 350 m switch) | (1)(2) now and then `Traffic: a new traffic car / police car at x y z would appear N m in front of the other player - the game picks another spot` (first few), and every ~5 s on change `Traffic: no-spawn - T traffic / P police spots refused in front of the other player so far, W of their cars wait until out of our view`; no new car or cop appears within ~60 m in front of either player; traffic density and pursuits otherwise look normal (cops keep coming, from other places). (3) C's `W` goes up while a host car's spot is in view and back to 0 once it is behind or far; no car pops up in front of C. No `CRASH`. Rerun with `NoSpawnNearPlayers=0`: no such lines |
| Truck trailers | both PCs this build. Free roam, H and C together on a road with semi trucks (industrial area / highway; `ground` lists cars). C watches H's trucks pass, overtakes one, drives away past 450 m and back. Then the same with C's build older than H's (or H's older than C's) | H: `Traffic: 1 truck trailer(s) sent with the traffic` when a truck is near C (`0 ...` when none). C: `Traffic: the host's truck N (...): its trailer ... follows the host's trailer (hitched there 1, here 1)`, every 10 s `Traffic: trailers - X frame(s) moved with the host's trailer data, 0 frame(s) pulled by our own hitch, 0 re-hitched, 0 put back ...`. Every truck on C pulls its trailer through turns, no trailer lying on the road, none left behind when a truck is removed (`KillVehicle` of the truck only). Hitting a trailer on C does not separate it. Mixed builds: the old side ignores the trailer packets; with an old host C logs `the trailer ... put back at the hitch (the truck was just placed; no trailer data from the host - an older build?)` once per truck and `pulled by our own hitch` frames |
| Rival catch-up | rival race (`boss -Event 15.1.2`), one player drives well ahead, the other slowly | the rival's PC (H) logs every 15 s `Race: catch-up measured against A% (ours B%, partner C%, rival race)` with A = the larger of B and C; the rival stays near the leading player on both screens, it never drops back to the slower one. If H ever logs `catch-up measured against us only (...)` while both race, the values in it say why the partner didn't count |

## Shared roadblocks

Both PCs linked, close together. Start-up: both logs `Roadblock: shared roadblocks on (create 2/2, setup ok, pieces ok/ok, place ok)`.
H: `pursuit`, drive (autopilot) down a long straight, then `roadblock spikes`. H logs `Roadblock: our game built #N (spike setup S): ...`;
C logs `Roadblock: the other player's game built roadblock #N ...` and, within 500 m, `Roadblock: showing the other player's roadblock #N ...: k of k piece(s) built`.
Both screens show the same cars, sawhorses and spike strips in the same spots (screenshot both). Driving C over the strip pops C's tyres.
While it stands, C's own requests log `Roadblock: the other player's roadblock stands near us - our game builds none of its own`.
`uncop` on H: H logs `our roadblock #N is gone`, C `... was removed in their game - removed here`. Repeat with `roadblock plain` and from C's side.

## Shared pursuits (`SharedPursuits`)

Both PCs in free roam, linked, within ~300 m of each other, `TestKeys=1`, `AutoPilot=0` (parked unless a step
says to drive), `SharedPursuits=1` on both. At start-up both logs show
`Pursuit: shared pursuits on (minimap cops hooked, radar
detector hooked)`. While either player is chased, both logs print a status line every 5 s:
`Pursuit: shared - here <state> [seen], <s> s left, heat <h> | there <state> [seen], <s> s left, heat <h>`.

| Check | How | Pass |
|---|---|---|
| Heat | H: `pursuit` | H `Pursuit: ours none -> chase`. Within a second C logs `Pursuit: heat a -> b (the other player's heat)` with b = H's heat; as H's heat climbs, C follows. C's heat never goes down because of H. Same the other way round. |

| Radar / minimap | H: `cop` (a patrol cop 25 m from H), C drives within 300 m of it | C shows the cop (`Cops: showing the other player's police car`), C logs `Radar: 1 of the other player's cops in the radar detector` and the radar detector beeps. H: `pursuit`: C logs `Minimap: N of the other player's cops shown` and the cops blink red / blue on C's minimap, also while C is not chased. |
| Ram their cop | H: `cop`. C: `ram cop`, 5 s later `ram cop` again | C `Collision: hit the other player's car <id>`, then `Pursuit: hit the other player's cop <id> - starting a pursuit on us` and `Pursuit: ours none -> chase`. (H's own game may start a chase on H too: its cop was hit) |
| Their patrol cop spots us | H: `cop` (a patrol cop 25 m from H). C drives past it at over 100 km/h, in front of it or within 15 m | C `Pursuit: the other player's patrol cop <id> saw us speeding (<d> m, <v> km/h) - starting a pursuit on us`, `Pursuit: ours none -> chase`; H is pulled in (`the other player is chased - starting the same pursuit on us`). Driving past it under 100 km/h, or more than 15 m behind it: nothing. |
| Cops go after the closer player | both chased (H: `pursuit`), side by side, then C drives 30-50 m away from H with H's cops around | H logs `Pursuit: cop <p> goes after the other player (<d> m from us, closer to them)` for each of H's cops clearly closer to C, and on C's screen those cops chase and ram C; `back on the local player` when C gets away (> 90 m) or H is clearly closer again; H's status line `(<n> cops, <k> on the other player, ...)` never has k = n. At start-up both log `Pursuit: cops chasing the other player no longer count as seeing us`. H hides from its own cops while some of them chase C: H still enters cooldown. |
| Remote car never a perp | both in a tollbooth with cops (`event 19.8.37`), or free roam: C speeds past one of H's cops | At start-up both log `Pursuit: the other player's car is never a pursuit target here (new-pursuit check ok/ok)`; the first time a cop could have taken it: `Pursuit: the other player's car here (<p>) is never a pursuit target here`. In the tollbooth each player gets their own pursuit during the run (`Pursuit: ours none -> chase` before `Race: we finished`, pursuit HUD and heat on screen), not only after the finish. |
| Partner's cops on the map in events | In a Challenge Series event (e.g. tollbooth 19.8.37) and in free roam, both players | The other game's chasing cops show on the minimap and radar detector in the same event too, and while you are chased all of their cops show (as vanilla shows every cop during a pursuit). Their patrol cop seeing you over 105 km/h, or at heat 4+ at any speed, starts your pursuit (free roam, pursuit challenges, and any event both are in: `Pursuit: the other player's cops can start a pursuit on us in this event too`, then `the other player's patrol cop <id> saw us` / `cop <id> spotted us` / `hit the other player's cop <id> - starting a pursuit on us`) |
| Shared hiding spot and cooldown | Free roam and a Challenge Series pursuit, both players chased, then both in cooldown; one drives into a hiding spot, the other stays in the open (out of sight) | The hider's log has their own state with "hiding" in the 5 s status line; the other one logs "the other player is in a hiding spot - our cooldown runs at the hiding-spot speed too" and their bar fills as fast as the hider's. Both bars show the same time left ("cooldown caught up with the other player's" when one was ahead) and both escape together. Not hiding-shared when either is spotted (both drop back to the chase); a cop driving by still spots the player who is not really hidden. In a race with cops the vanilla rule stays (no escape before the finish) |
| Busted together (events) | both in a Challenge Series tollbooth with cops (`event 19.8.37`); H gets chased and parks | H logs `Pursuit: we were busted (count 1) - in an event the other player is busted with us`; C logs `Pursuit: the other player was busted in event <hash> - busting us too (the event is lost for both)` (not chased: `no pursuit here, starting one to bust us too`) and `Pursuit: busted here too, as the other player was`; both get the busted cutscene and the event's loss screen. A player already finished / on the results screen is not busted. On the race-over menu after the bust neither game is busted again and the partner's cops start nothing (`results screen - pursuit and bust meter frozen for us`) until a restart. In free roam a bust busts the partner too while both are chased (`the other player was busted in our shared pursuit - busting us too`); a partner no longer chased is not affected (`not in a pursuit with us, nothing happens here`). |
| Busted together (free roam) | both chased in free roam (H: `pursuit`), H parks, C keeps driving (`autopilot`) | H busted; C logs `Pursuit: the other player was busted in our shared pursuit - busting us too` and is busted within a second. If C had already escaped (no pursuit): `not in a pursuit with us, nothing happens here`. Both need COOP_VERSION 21. |
| Cooldown waits | both: `pursuit`. H: `uncop` (H's cops are gone, H is out of sight), C stays chased | H `Pursuit: our evade bar is full - waiting at the evade limit until the other player's is full too`; H's status line stays `chase` (no `cooldown`) for as long as C is seen; H's evade bar stays full. On C's screen H's strip (under C's own) is full to the EVADE side. |
| Escape together | as above, then C: `uncop` | both `Pursuit: ours chase -> cooldown` within ~1 s, then both `Pursuit: ours cooldown -> escaped` within ~1 s of each other. If a new cop spots C during the cooldown, both go back: H logs `Pursuit: the other player is not in the cooldown (spotted) - we leave it too, back to the chase` (also from a hiding spot) and H's state returns to `chase`. The safe house opens for both in the cooldown (as in the game). Shared decisions use the partner's ready state held for 0.7 s, and for 1 s after entering the cooldown a partner sighting doesn't pull you back (no flapping). |
| Partner's bar | both chased in free roam, then apart; or one PC: `peerbar chase -70`, `peerbar chase 70`, `peerbar off` | One BUSTED / EVADE panel (ours) with two strips: ours, and the partner's right under it (a copy of the game's own strip). Our backup timer (backup coming) moves down under both strips. The partner's strip shows what their own meter shows (the game's TimeUntilBusted), easing between packets. Not chased ourselves: their whole meter in our meter's place. In the cooldown only ours (it is shared). Log: `Pursuit: partner bar - cloned the game's meter (...)` once per HUD, then `Pursuit: the other player's bar shown (chase)` / `hidden`. Hidden over menus, the map, cutscenes and while you are busted. A race or restart after it shows: no crash, the bar comes back. |
| Traffic models | C near H in free roam, both driving (`autopilot`) for a few minutes | Host cars here have the host's model and colour (a model has one colour). When one of our cars of another model stood in for a host car, C logs `host car <id> (model ...) was shown by our <name> - out of view now, building the right model` and, every 10 s, `Traffic: other model shown <n> frame(s), <k> swapped for the right one`. Never right behind you (mirror) or in a race. |
| Car pool / evictions | any session with MaxCars > 20 | At start `Budget: car model pool 50000 KB -> 82768 KB`. Every eviction logs `Budget: evicted: <name>(copy/ours, cop) ... - car models <used> KB of <pool> KB`; with the bigger pool few or none in free roam. |
| Partner marker | both in free roam: C `autopilot`, H watches; then H drives ahead | A see-through chevron in C's paint colour over C's car; when C is off screen or behind, the chevron at the screen edge pointing at C, up to the very edges. None when the cars are on top of each other, in menus, cutscenes, F1, or while busted. Log: `Marker: shown (chevron texture found)` / `Marker: hidden - <why>`. `FriendMarker=0`: none. |
| Off | `SharedPursuits=0` on both | `Pursuit: SharedPursuits=0 - each player has their own heat and pursuit`; none of the lines above; busting or escaping affects only the player chased. |
| In events | start an event with `event 15.1.1` while chased | no heat / bust / cooldown lines while either player is in the event. |
## Notices and game controllers

One PC is enough. Keep the log open (`scripts\MWTogether.log`).

| Check | How | Pass |
|---|---|---|
| Notice length | free roam, `notice hello` in `MWTogether.cmd` (or a join / leave) | the centre line appears once, stays about `NoticeSeconds` (10 s), then fades; a two-part notice (the client's `No answer from the host`, 20 s after free roam without a host) shows each part ~5 s. In a cutscene: the song banner, log `Notice: in the game banner (no HUD here)` and `holding the game banner for 10 s` |
| Notice fallback | as above | no `banner timer object not found` / `exception while holding` lines. If one shows up, the banner has its old length (about 6 s) and the corner line still stays 10 s |
| Song banner | wait for a new song in free roam | the game's own song banner looks and times as before (we only hold our own notices) |
| Pad plugged in | start the game with no pad, get to free roam, plug a USB pad in | within about 2 s: `Pad: controller connected: <name> -> added to the game`; the pad drives the car. In the controller options the pad's bindings are shown |
| Bluetooth pad | as above, switch a paired Bluetooth pad on while playing | same as USB |
| Unplug + replug | pad connected at start, pull it out, wait 3 s, plug it back | `Pad: controller 0 disconnected (<name>) - removed from the game's list`, then `controller connected ... added`; the pad works again with the same bindings |
| Wheel force feedback | a force feedback wheel plugged in while playing | `Pad: force feedback slot 0: <name> (format 00000000, coop 00000000)`; the wheel's centering force works in a race |
| Bindings kept, no pad at start | change a pad binding in Options > Controller, save, quit. Start the game WITHOUT the pad, play until an autosave, quit. Start with the pad | log at the pad-less start `Pad: no controller attached - keeping the profile's controller bindings (made for XXXXXXXX)`; at the last start your changed binding is still there, no `reset to this pad's defaults` line |
| Bindings kept, pad added later | as above, but plug the pad in during the pad-less session | `Pad: keeping the controller bindings (made for X, this pad X)`; your bindings work at once |
| Restore defaults still works | Options > Controller > restore defaults | the bindings go back to the defaults, log `controller bindings set to the defaults ... (called from 0051D707)` |
| Unknown reset | play normally (pause menu, options, reset car, restart a race) | if `Pad: blocked a reset of the controller bindings (called from ...)` shows up, note what you just did: that is a reset the game would have done and the mod stopped |
| Switches off | `GamepadHotplug=0` | no `Pad: controller ...` lines, a pad plugged in later is ignored (the game's own behaviour) |

## Shared milestones (`[Career] SharedMilestones`)

Both PCs in free roam, linked, `TestKeys=1`, both careers at the **same** Blacklist rival (the overlay / `story`
shows the rival number; `milestones` prints it). At start-up both logs show
`Milestones: shared with the other player (same rival only)`.

| Check | How | Pass |
|---|---|---|
| Lists readable | both: `milestones` | `Command 'milestones': rival N: milestones M (a open, b done), speed traps T (c open, d done)` with the same N on both, plus one `Milestones: milestone XXXXXXXX rival N open/done ...` line per entry of that rival |
| Milestone shared | H: `milestone` | H `Command 'milestone': milestone K (rival N): applied, state now 4`, then `Milestones: we completed milestone K (rival N, ...) - telling the other player`; H's game shows the "milestone reached" popup. C: `Milestones: the other player completed milestone K`, then `Milestones: the other player's milestone K (rival N) completed here (state now 4)`; banner `<H> completed a milestone - it counts for you too`; C's bounty goes up by the milestone's reward; C's Blacklist / rap sheet shows K done. C sends nothing back. `milestones` on C: one more done. |
| Speed trap shared | C: `speedtrap` | the same lines the other way round with `speed trap`; on H the trap is done in the Blacklist screen |
| Real play | both: drive through one of the rival's speed traps (C only), or get a pursuit milestone (pursuit length, cost to state...) on H and escape | the same log lines as above within ~1 s of the game paying it on the PC that earned it (a pursuit milestone is paid when the pursuit ends, not when it's reached) |
| Already done | repeat a check for an entry the other PC already has | `... - already done here (our rival N)`, no notice, no second bounty |
| Different rivals | one career a rival further on | `... - not available here (another rival)` or `another rival here`; nothing changes on that PC |
| In a race | C starts `event 15.1.1`, then H: `milestone` | C logs `the other player completed ...` at once and applies it only after the race, back in free roam |
| Loading | quit to the menu and load the career again | `Milestones: the lists changed (career loaded?) ...` at most; no `we completed` lines for old progress |
| Off | `SharedMilestones=0` on both | `Milestones: SharedMilestones=0 - each player keeps their own`; no `Milestones:` sharing lines |

## Shared finish (`[Career] SharedWins`)

Both PCs linked, `TestKeys=1`, the same career event. At start-up both logs show
`Career: shared wins ok/ok/ok, winner's place for the race script ok`. Use `slowai` on both so the AI can't win.

| Check | How | Pass |
|---|---|---|
| Partner wins, you still drive | normal race (`15.1.1`), H drives to the line, C stays far behind | H: normal winner's screen. C within a second of H's finish: `Race: the other player won while we still drive - finishing our racer as the winner (race type N)`, `Race: our racer finished 1 (rank 1 here), placement 1 posted - the race script goes on to the winner's screen`, then `the other player finished (rank 1 there, 2 here)`; the game takes C's car (AI drives it), C gets the winner's photo too, its line under the photo saying `<H's name> won the race` (log `Race: the winner's photo for the other player's win`; never "you finished 2nd" or the losing screen) and its Continue works; C's `cash` went up; both return to free roam. |
| Swap roles | the same with C winning | the same lines on H |
| Their car crossed your line first | H wins, C drives right behind H's car over the line before H's packet arrives (or set a long `InterpDelay` on C) | C either finishes as above, or crosses itself and logs `Race: we crossed the line 2. - the other player won, so the race script gets place 1`; winner's screen either way |
| Rival race | `15.1.2` (rival race), then the second rival race from the "race the rival" screen | each race: the PC that did not cross the line logs the two `Race: ... finishing our racer as the winner` lines; both see the winner's screen; after the second race both get the normal rival flow (rival beaten, pink slip, markers) in their own career |
| Out, then partner wins | drag `12.7.3` (or any race): C totals / blows the engine (or stays last in a knockout) and sits on the losing results list, H then wins | C: `Race: the other player won while we were out - the results list closes for the winner's photo`, `Race: the other player won after we were out (the results list) - counted for us too, placement 1 posted`, the list closes and the winner's photo shows `<H's name> won the race`; its Continue: `Race: photo closed - the race ends as after the results list (we were out)`, back to free roam with control; C's `cash` went up and `current event won flag 1`. The same if H wins while C's "race over" message still shows (`before the losing screen`) or from the Restart / Quit screen |
| Nobody wins | let the AI win (no `slowai`) | no `finishing our racer` line; losing screens as in the plain game |
| Off | `SharedWins=0` on both | no `finishing our racer` / `place 1` lines |

If C logs `activity key ... does not lead back to this race - not posting the placement`, the game's AI drives C's car
to the line and the race ends there (the winner's screen follows, only later): report that log.

**Rival race opponents.** C logs `Race: rival race - rival built as model XXXXXXXX (ok)` (and, if a roster came,
`the event's own rival car, not the partner's roster`), H the same model. Then C `our racer ... now follows the host's
opponent (mirror id N)`; if the models still differ, after 1.5 s `... now follows the host's opponent N of model
XXXXXXXX - no free racer of that model here`, and the rival drives exactly where H's rival drives. In the second rival
race C logs `the racer that followed host opponent N is a new car now - binding again` (or a fresh `now follows`)
and `AI: ... is a new car with a new AI` at most once; the rival never drives on its own AI next to H's rival.

## Story scenes (`SyncStoryScenes`)

Both PCs in free roam, linked, `TestKeys=1`, `AutoPilot=0`. `story <movie>` stands in for the career
playing a movie (getting a real one means reaching that point of the career).

| Check | How | Pass |
|---|---|---|
| Wait | `SyncStoryScenes=1` on both. H: `story storyfmv_pin11` | H plays the movie. C: banner `<name> is watching a story scene - your game waits`, log `Story: the other player is watching a story scene (...) - holding the world`; C's car, traffic and cops stand still. When H's movie ends or is skipped: C `hold released (scene over, N s)`, banner `<name> finished the story scene`, C drives again. |
| Hold limits | as above, but C is in a race / the Safe House / loading | no hold on C, only the notice (`is watching a story scene`) |
| Watch along | `SyncStoryScenes=2` on C. H: `story blacklist_15` (C's career must have reached rival 15, i.e. any normal career) | C `playing blacklist_15 here too`, the movie runs on both. `story` on C shows `in-game 1`. |
| Not reached yet | `SyncStoryScenes=2` on C, H: `story blacklist_01` while C is not at rival 1 | no movie on C (spoiler rule), C holds instead |
| Skip vote | both watching the same movie (watch along). Press skip (Enter) on H only | H `Story: voted to skip ... waiting for the other player`, overlay `Cutscene: skip voted`, movie keeps playing on both. Then skip on C: `both voted - skipping` on both, both movies close. |
| Second vote ends it | while both watch, C votes first, then H presses skip | H `both voted - skipping`; C closes its movie within about a second (`both voted` or `the other player is past it`) |
| Real career | play until a Blacklist movie / story movie / Safe House rival movie comes up | as `Wait`; for a Safe House movie the other PC waits while it roams |
| World pauses | H watches `story storyfmv_pin11` alone (C disconnected or `SyncStoryScenes=0`) | as in the unmodded game. Connected with `SyncStoryScenes=1`: H's world also stops under the movie (log `Coop: pause menu pauses again`) |

## Prologue and new profiles

The prologue (career bin 16, profile +0xB0 == 0x10) is a fixed chain of scripted events the game loads by
itself one after the other - seen on a fresh career: 16.1.0, 16.2.2, 16.2.3, 16.1.1, 16.8.1 (a ~30 s cutscene
step), 16.2.1 (Razor, lost on purpose), then the car lot and free roam with Mia's "I'll meet you there" SMS,
still in the prologue until you meet her. Story movies play over the world at some starts; no free roam
before the car lot. With
`[Coop] Prologue=1` (default) and the same build on both PCs, two careers in the prologue are linked:
each game runs its own chain (no `Event: sent ... to peer` for 16.x), the partner's car is shown next to
the race (not as a racer: the scripted opponents stay), and the start-together hold waits without the
20 s limit while the partner is still on the way to the same event (`CarState.prologue`: in the prologue,
prologue events done). A career in the prologue and one past it are not linked. An older build on the
other side: co-op starts after the prologue, as before (`CarState.pad[1]` = 0 in the prologue).

Making a fresh profile without losing the real one:

1. Close the game. Copy `Documents\NFS Most Wanted\<profile>` to a backup folder, then move it out of
   `Documents\NFS Most Wanted`. The game now finds no profile.
2. Start the game focused, create the profile by hand ("Create a new alias?" Yes, the autosave notices,
   the name, "save the alias?" Yes), then Career > Start Career. The first race (16.1.0) loads at once.
3. Afterwards close the game, delete the test profile folder and move the real one back. Check the real
   save's time/hash is what it was.

`tools\saves.ps1 fresh` / `real` does steps 1 and 3 by renaming folders (nothing deleted; `status` shows
which is active, `snapshot-fresh` keeps the current save as the template for the next `fresh`).

A pass looks like this (both PCs on fresh careers, linked):

| Step | Expected |
|---|---|
| Career started | log `Prologue: in the prologue (career started, prologue events done 0)`; both: `Co-op on - <name> is in your world now` |
| First race 16.1.0 | `Event: prologue event - not sent (each game runs its own prologue ...)`; the partner's car beside the grid, no `AI racer built as peer car` |
| One finishes first | the next race loads by itself; `Prologue: ... prologue events done 1`; on its grid `Start: on the grid first - waiting for <name> to catch up` and the notice `waiting for <name> to catch up`; the world stands still |
| The other arrives | `Start: go (both ready, ...)` on both, the race starts together |
| Razor race 16.2.1 | the car breaks on both; no `the other player won while we still drive` |
| Story movie at a start (16.1.0, 16.2.1) | no grid hold while it plays: the hold starts after the movie |
| Car lot, drive to Mia | still `in the prologue ... done 5` on both and linked: both drive in free roam together |
| After meeting Mia | `Prologue: not in the prologue (career started, ...)`; while only one has met her the two are not linked for a moment |
| One career in the prologue, one past it | no link; the one past it gets `<name> is still in the prologue ...`, the other `<name> is past the prologue ...` |
| Normal career profile | `Prologue: not in the prologue (career started, prologue events done 0)`; ghost, events, traffic as usual |

## Before a release

1. `tests\build_tests.bat` passes.
2. `solo`, `smoke`, `race`, `results`, `boss`, `restart`, `cutscene`, `pursuit`, `ram`, `together`, `kinds` pass on two PCs.
3. One real session by two people with `TestKeys=0`: free roam, a race started from the game's own menus
   (not `event`), a pursuit, a rival race.
5. Install the release zip into a copy of the game **without** the Widescreen Fix (and once with it). The
   zip does not include `dinput8.dll`: put Ultimate ASI Loader's x86 `dinput8.dll` next to `speed.exe`
   first (already there if the copy was a 1.7.2 or earlier install). Start
   `speed.exe` as usual: the plain game's title screen is `MW_LS_Splash.fng`, the fixed
   game's `WS_MW_LS_Splash.fng`. A release ASI does not honour `TestSkipIntro` / `TestPressStart`; press
   START yourself. AutoStart, SkipLoadComplete, StartInBackground, AutoPilot, and TestSkipIntro / TestPressStart exist only in dev builds (see section 2).
   and should not stall at intro or Press START.
6. Update the checklist in `README.md`: a new feature gets `(?)` until it was seen working on two PCs.

## Known limits of the test tools

- The autopilot doesn't drive Challenge Series events (it stays on the start line).
- `event` starts any event regardless of career progress; real play goes through the game's menus.
- A parked car in a pursuit is busted in about 30 seconds, so pursuit scenarios are short.
- Only one copy of the game can run per PC, so co-op always needs two PCs.
