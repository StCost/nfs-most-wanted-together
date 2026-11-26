# NFS MW Together: co-op career for Need for Speed: Most Wanted (2005)

![NFS MW Together](docs/mwt-logo.png)

Play the Most Wanted career with a friend. 2 players, over LAN, a VPN or the internet.

A **co-op mod for Need for Speed: Most Wanted 2005** (PC, v1.3). Two players go through the
**career together**: free roam with traffic, races against the AI, the Blacklist, police chases,
each in their own car. Works over LAN, a VPN (Hamachi, Radmin VPN, ZeroTier), or directly over the internet to the host's public IP.
The mod aims for the best vanilla experience in co-op: the game's own menus, screens and career, played together.

**Get it:** [GitHub releases](https://github.com/StCost/nfs-most-wanted-together/releases/latest) · [Nexus Mods](https://www.nexusmods.com/needforspeedmostwanted2005/mods/366) · [NFSMods](https://nfsmods.xyz/mod/8207)

**Tested by the developers:** the Blacklist up to #10 and the Challenge Series up to #16, all in co-op. Playable and fun, 9/10. Minor sync issues remain on the joining player's side only: physics, traffic and police movement can be slightly off compared to the host. Playtesting and fixes continue until the whole game is completed together.

<table>
<tr><td><img src="docs/screenshots/01_race_start_side_by_side.jpg" alt="Race start together"></td><td><img src="docs/screenshots/02_start_grid_together.jpg" alt="On the grid together"></td></tr>
<tr><td><img src="docs/screenshots/04_chase_partner_and_cop.jpg" alt="Chased together"></td><td><img src="docs/screenshots/08_race_both_names.jpg" alt="Both names in the standings"></td></tr>
</table>

*A.K.A.: NFS MW co-op, NFSMW multiplayer career, Most Wanted 2005 coop campaign, NFS Most
Wanted online free roam with a friend, Need for Speed Most Wanted two player mod.*

<details open><summary><h2>Install</h2></summary>


1. Download [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases) — the x86/Win32 `dinput8.dll` build (e.g. [v10.0.4](https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases/tag/v10.0.4), which we tested) — and put `dinput8.dll` next to `speed.exe`.
2. Download **[MWTogether.zip](https://github.com/StCost/nfs-most-wanted-together/releases/latest/download/MWTogether.zip)** (the latest release) and unpack it into your game folder, next to `speed.exe`.
3. Run `speed.exe` as usual. The game stops at the main menu: pick the **Together** icon there,
   set **Connection** to **Host** or **Join**, and (if joining) enter the host's IP under
   **Connect to IP**. Press **Start Host** or **Connect**. **Stop Host** / **Disconnect** end the session.
4. Do the same on the other PC. The game loads your career into free roam and connects you on its own.
5. To update, unpack the new zip over the old one. Your settings (`scripts\MWTogether.ini`) stay. Leave the existing `dinput8.dll` as is.

You can play the prologue (the opening BMW races) together too: start a new career on both PCs.


</details>

<details open><summary><h2>You need</h2></summary>


- Need for Speed: Most Wanted (2005) for PC, patched to **v1.3**. No game files are included, bring your own copy.
- [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases), the x86/Win32 `dinput8.dll` next to `speed.exe` (v10.0.4 is the build we tested).
- The same version of the mod on both PCs.
- A LAN, a VPN (Hamachi, Radmin VPN, ZeroTier), or a direct connection over the internet.
- If you want to play over a **direct public IP, no VPN**: the host needs a public IP, **UDP port 47555** forwarded to their PC on the router, and UDP 47555 allowed for `speed.exe` in the firewall. The other player joins with the host's public IP. If the host's ISP uses CGNAT (the router's WAN address differs from the IP that sites like whatismyip show), port forwarding won't work; use a VPN instead.
- If one of you can't see the other, a firewall or another VPN (Surfshark, say) is blocking it. Allow UDP 47555 for `speed.exe` on that PC or pause the VPN. The game tells you when only one side gets through.


</details>

<details open><summary><h2>What to expect</h2></summary>


- **Your own cars.** You both drive your own car with your tuning and visuals, damage and brake lights included.
- **Events together.** Start any career event and your partner comes along: same opponents, same start, and you see each other.
- **Shared wins.** If one of you wins, you both win. Whoever is still driving gets finished as the winner right there. Totaled or knocked out before your partner won? Your losing screen closes and you get the winner's screen too, saying who won, with the win, cash and progress counted.
- **Police.** In free roam you share the cops: same heat, you lose them together. Your partner's cops show on your minimap and set off your radar. In an event you're both in (Challenge Series, races with cops) your partner's cops chase you too, and if one of you gets busted, you both are.
- **One world.** Shared traffic when you're close, collisions between your cars, blips on the minimap and the map.
- **Saves.** Each of you keeps your own career save on your own PC, as in the plain game.


</details>

<details open><summary><h2>Known issues</h2></summary>


Things we know about but can't fix yet:

- **Traffic popping in for the joining player.** Sometimes a traffic car appears right in front of the joining player's car, which can cause a sudden collision.
- **Traffic vanishing.** The other way round, a traffic car can disappear right in front of you.
- **Trucks.** Trucks are sometimes not synced and may be missing in one of the two games.


</details>

<details open><summary><h2>Community</h2></summary>


- **[YouTube](https://www.youtube.com/@StCost)** — gameplay videos of the mod.
- **[Discord](https://discord.com/invite/xQwWSFDC8Y)** — the Dreaming Saints server.


</details>

<details><summary><h2>Features and progress (100%)</h2></summary>

<details><summary><b>Connecting</b></summary>

- [x] Direct connection between two PCs (one hosts, one joins), ping in the overlay
- [x] Quitting (Alt+F4 too) tells the other side right away; a game that just disappears times out
- [x] Stays connected while paused, minimized or alt-tabbed, and the game keeps running in the background
- [x] Status overlay on F8 (remembers if you hid it)
- [x] **Together** in the game's own menus: one screen from Main menu > Together, the pause menu, and Options > Together (Connection, Connect to IP when joining, Reconnect on launch, Start Host / Connect); the main menu's profile box shows both profile names (yours and your partner's) while connected. If this `speed.exe` differs, the menus stay as they are and the page below is used
- [x] Main menu logo from `scripts\MWTogether-logo.dds` if it is there (no game files changed); the logo object is made taller so the original lettering stays the same size
- [x] In-game **Together** overlay page (hold BACK on a pad in the menus): host or join, host IP, remaining menu options, your IP address and the connection state. Saving reconnects right away when needed
- [x] Join / leave notices, even with the overlay hidden
- [x] Notices use the game's own message line in the middle of the screen: joins and leaves in green, problems (connection, firewall, prologue) in orange. Long ones come in parts. In cutscenes and menus the EA Trax song banner shows them
- [x] Notices stay up for 10 seconds (`[UI] NoticeSeconds`)
- [x] Joining while the other player is in a race: you drive around freely and join the next event together
- [x] Only the player you connected to can talk to your game; packets from anyone else are ignored
- [x] Event starts and restarts are sent several times, so one lost packet can't split you into different races


</details>
<details><summary><b>Your cars</b></summary>

- [x] Your own car with your full tuning, in free roam and in races
- [x] The other player's car moves smoothly (1-10 mm of jitter)
- [x] Spawn protection: two cars appearing on the same spot don't launch each other
- [x] The other player on the minimap and on the world map (blips use each car's paint colour)
- [x] Car-to-car collisions with real impact: ram your partner and their car gets thrown on their screen too
- [x] Your own names in the race standings
- [x] Your partner's car shows brake lights, damage (dents, broken parts), steering and nitrous


</details>
<details><summary><b>The world</b></summary>

- [x] Shared traffic when you're near each other
- [x] You see the other player's cops (light bars too) and the cops chasing them
- [x] When you're both chased, cops can switch to the other player
- [x] Traffic near the other player keeps full physics
- [x] The other player's traffic, cops and opponents stay on the road on your screen, even where their game has no road loaded under them
- [x] Semi trucks keep their trailers
- [x] Hitting the other player's traffic, cops or opponents pushes the real car on their PC
- [x] The pause menu and world map don't stop time while you're connected
- [x] No speedbreaker in co-op (slow motion can't work for two). The short slow-motion shots (speed traps, finish line, crash cams) play like in the normal game (`[Coop] NoSlowMotion=1` turns them off)
- [x] Smooth traffic handover: drive away from the other player and their traffic around you keeps going as your own. Come back, and their cars only show up where you can't see them appear, or quietly replace one of yours (`[Traffic] SmoothHandover`)
- [x] Shared traffic in races: the client shows the host's traffic with its own cars, so the race scripts keep working. Same positions on both screens, though the model can differ (`[Traffic] SharedInRaces`)
- [x] No cars popping up in front of you: new traffic and cops never appear within ~60 m in front of either player (`[Traffic] NoSpawnNearPlayers`)
- [x] Same weather on both PCs: the client gets the host's rain and wet roads (`[Coop] SyncWeather`)


</details>
<details><summary><b>Events and races</b></summary>

- [x] Starting an event takes the other player along, even from the results screen of the last race
- [x] Same AI opponents on both PCs (cars, tuning, names), and on the client they follow the host's, so the race looks the same on both screens
- [x] Race positions use each player's real progress and update the moment one passes the other
- [x] Joining from far away still gives the other player a spot in the standings (in an AI car's body if their car isn't loaded on your PC yet)
- [x] Separate grid slots, and you see each other from the countdown on (`SeparateGridSlots`)
- [x] Whoever loads first keeps the loading screen up until the other has loaded too, so both intro cutscenes start together; any difference left is evened out on the grid, without a message (`StartTogether`)
- [x] At a start with no grid slot of its own for your partner, their car is shown on a free slot of the grid around you (behind you, or beside that, or beside you), never on top of a rival
- [x] Restart restarts the race for both of you, from the results screen too
- [x] AI catch-up (rubber banding) follows whoever of you is ahead, Blacklist rivals included (`CatchUpLeader`)
- [x] Race intro cutscenes play for both
- [x] A win by either of you counts for both: cash, unlocks and progress in each own career. The one still driving is finished as the winner on the spot, with the payout and the rival's progress; the partner gets the winner's screen too, naming who won, even after losing, totaling or being knocked out (`SharedWins`)
- [x] You see the other player's finish in your standings
- [x] Blacklist rival races: you both race the rival, the client's rival follows the host's (second race of the series too), and you each get the normal rival flow after winning
- [x] In rival races your partner has their own racer slot: a grid slot and a row in the standings (`[Coop] PartnerInRivalRaces`)
- [x] Rival races also get one extra AI racer on the rival's side, with normal AI and catch-up. It drives the car the race intro shows (Ming, #10, Big Lou) when there is one (`[Coop] BossHelper`)
- [x] Rival races and story events (Razor's races, the final pursuit) only take along a partner who's at the same Blacklist rival, so nobody skips ahead. Continue on "race the rival" takes your partner to the next race too
- [x] The final pursuit loads in about a second
- [x] The prologue together: start a new career on both PCs. Each game plays its own prologue, and whoever is ahead waits on the next grid. Your partner races next to you, and the Razor race is still lost for both (`[Coop] Prologue`)
- [x] After beating a rival, each of you picks your own markers and gets your own rewards (pink slip, cash, parts)
- [x] Bounty goes to each player's own career
- [x] Shared milestones and speed traps: when one of you completes one for your current rival, the other gets it too, with the bounty, if they're at the same rival (`[Career] SharedMilestones`)
- [x] Starting an event together: when either of you starts a career race, a Blacklist race, a milestone or pursuit event, a quick race or a Challenge Series event, the other player is pulled into the same one, from free roam, the world map, any menu, the results screens or the main menu. Open menus are closed first. For a Challenge Series event or a quick race, a partner who's roaming quits to the main menu first and then loads the same event (Challenge Series: with its car). A partner who's driving another event isn't pulled out. You both get a notice and they finish their own event


</details>
<details><summary><b>Career</b></summary>

- [x] Each of you keeps your own career; wins are shared, so both move forward together
- [x] A cutscene you both watch is skipped only when both of you press skip
- [x] Story movies and cutscenes outside events: the other player gets a notice and waits in free roam until it's done (`SyncStoryScenes`). The next Blacklist rival's intro and the marker picks after beating a rival are each player's own: nobody waits for them. With `SyncStoryScenes=2` the movie plays on their PC too if their career is that far
- [x] Shared pursuits (`[Coop] SharedPursuits`): you get the same heat, the higher one counts
- [x] The other player's cops show on your minimap and trigger your radar
- [x] Ramming their cop starts a pursuit on you
- [x] Blacklist pursuits (milestones, bounty) started from the Blacklist screen take your partner along to the start (`[Coop] FollowToPursuits`)
- [ ] One of you gets busted in an event you're both in (Challenge Series, a race with cops), you both get busted: the busted cutscene and the event's loss screen on both PCs. In free roam a bust is personal
- [ ] Your partner's cops chase you in events too (Challenge Series tollbooth, races with cops): their chasing cop close to you, their patrol cop you speed past, ramming one of their cops, or their pursuit starting within 250 m of you starts your pursuit
- [x] Escaping: whoever loses the cops first goes into cooldown, but the meter doesn't fill until you're both in cooldown. Then both meters run, a spot restarts the other one's, and you both escape at the same moment. This works in free roam and when you're both in the same event (Challenge Series, career pursuit events, races with cops)
- [x] Police roadblocks are the same for both of you, in free roam and in Challenge Series / career pursuits: the same cars, sawhorses and spike strips in the same spots, so the gap you aim for is there on both screens. Spike strips work on both sides, and a roadblock removed in one game goes in the other too. While your partner's roadblock stands near you, your game doesn't build a second one
- [x] Cost to State and bounty count for both of you: cops either of you immobilizes or damages, roadblocks and spike strips dodged and property smashed while you're both in the pursuit are added to both players' totals, so the HUD, milestones, Challenge Series goals and the rap sheet match. Infractions stay personal
- [x] Challenge Series pursuits: the client sees the host's traffic as in free roam, heat and the chase are shared (one of you chased pulls the other in, their cops can spot you), and a win by either of you counts for both. The partner's goal normally completes from the shared stats or escape; if it hasn't 8 s later, their game finishes the event as won. The other player's cops can spot you and start a pursuit on you there, as in free roam
- [x] On the results / win / Continue screens the pursuit is frozen for you: no bust meter, no new chase
- [x] The pursuit-start camera follows the game's own cinematic camera option in Settings
- [x] `SharedPursuits=0`: busted and escape only affect the player being chased


</details>
<details><summary><b>Quality of life (settings in <code>[Start]</code>, <code>[UI]</code> and <code>[Input]</code>)</b></summary>

- [x] Several profiles: the quick start loads the one you used last (`[Start] LastProfile`); the first time you pick it yourself
- [x] **F1** hides the minimap, heat meter and speedometer, keeping the rear-view mirror. F1 again brings them back (`[UI] HideHudKey`)

- [x] Pads and wheels plugged in mid-game (USB or Bluetooth) work right away, reconnects too, and keep your bindings (`[Input] GamepadHotplug`)
- [x] Your pad keeps its bindings even when the game starts without it (`[Input] KeepPadBindings`)


</details>
<details><summary><b>Test tools (<code>[Debug] TestKeys=1</code>)</b></summary>

- [x] **F5** autopilot (dev builds only): the game's AI drives your car around free roam or along the race route, using nitrous on straights (`AutoPilot`, `AutoPilotNos`). A release ASI ignores `AutoPilot=1`.
- [x] `[Window] StartInBackground=1` (dev builds only): the game starts without stealing focus, can boot in the background and stays muted until you click it. A release ASI ignores this key.
- [x] Car budget log: the game fits 20 cars in the world (both players, traffic, cops). The log says which cars got removed or refused to make room, plus a summary every 30 s
- [x] `[Debug] Perf=1` logs fps, 1% lows, the worst frame and the mod's own cost every 5 seconds (also in the F8 overlay)
- [x] **F7** starts the test event (`TestEvent`) for both players
- [x] **F10 / F11** spawn or remove a copy of the other player's car, **F12** spawns a cop
- [x] Test commands in `scripts\MWTogether.cmd`, like `pursuit`, `uncop`, `ram cop` and `tp x y z`. Full list in `docs/TESTING.md`


</details>


</details>

<details><summary><h2>Configuration</h2></summary>

All options are in `scripts\MWTogether.ini`, with a comment on each. Sections:
`[Network]`, `[Ghost]`, `[Traffic]`, `[Coop]`, `[Start]`, `[UI]`, `[Window]`, `[Debug]`.


</details>

<details><summary><h2>Build from source</h2></summary>

- Visual Studio 2022 with the C++ workload (MSVC x86).
- Run `build.bat`. It produces `build\MWTogether.asi`. When this folder sits inside the game folder
  (`<game>\MWTogether-src\`), the script also installs the plugin into `<game>\scripts\`.
  That is a dev build with the test keys and commands (`docs/TESTING.md`); `set MWT_RELEASE=1` first for a
  release build without them (`tools\release.ps1` and the workflow do this).
- Nothing builds automatically on GitHub. The **Build** workflow under **Actions** runs only when you start it
  by hand: it builds the plugin, runs the tests and packages the zip (the ASI loader is a separate
  required download; see Install). Give it a version number and it also creates the GitHub release.
- Local release, without Actions: `tools\release.ps1 -Version 1.7.16` publishes the release (always a normal
  release marked latest; there are no pre-releases). `-DryRun` only builds the zip in `dist\`.
- After every release, update the mod pages so only the newest version is listed:
  - Nexus Mods: upload the zip as an **Update** of the current main file, with "Archive existing file" ticked, "Update mod
    version" ticked and the changelog filled in; archive any other older file still under Main / Optional / Update.
    Archive, never delete.
  - NFSMods: Edit the mod, set the version, upload the zip (it becomes "Latest Version"). The site has no way to hide or
    archive old files per file, so the older ones stay listed as "Old Version".
- GitHub shows the SHA256 of every release asset, and files built by the workflow carry a build provenance
  attestation: `gh attestation verify MWTogether.zip --repo StCost/nfs-most-wanted-together`.


</details>

<details><summary><h2>Testing</h2></summary>

The network code, the config reader and the remote-car maths have tests that run without the game:

```
tests\build_tests.bat
```

It builds `build\tests.exe` and runs it, then runs the same tests again under AddressSanitizer when your
MSVC supports it. Tests include: packet layouts and protocol version, connecting / leaving / timeouts,
snapshot interpolation and extrapolation, clock sync, traffic / event / roster / restart packets, `MWTogether.ini`
parsing, and fuzz tests that throw several hundred thousand random and damaged datagrams at the packet
handler to make sure nothing a network can deliver crashes the game. Exit code 0 means everything passed.

Everything that needs the game - the test commands, the two-PC scenarios (`tools\coop-test.ps1`) and what a pass looks like - is in [docs/TESTING.md](docs/TESTING.md).

What the tests can't cover is everything inside the game itself (spawning cars, races, cops): that still needs
two PCs. See [CONTRIBUTING.md](CONTRIBUTING.md) for the details and for adding tests.


</details>

<details><summary><h2>How it works (short)</h2></summary>

`MWTogether.asi` is loaded by the ASI loader inside `speed.exe`. It hooks Direct3D 9 `Present` to run once per
frame on the game thread, and it patches game code and vtables at known v1.3 addresses, found by reverse
engineering. Remote cars are real game vehicles (`PVehicle`) whose rigid bodies are set every frame from
interpolated network snapshots. Race opponents and traffic of the other PC are "mirrors" driven the same way.
The code is in `src/`:

| File | What |
|---|---|
| `main.cpp` | frame loop, hotkeys, overlay text, start-up patches |
| `net.*` | UDP protocol (hello, state, car info, traffic, truck trailers, events, roster, restart, keep-alive) |
| `netutil.h` | pure helpers: interpolation maths, traffic ids, race traffic binding, traffic handover rules (camera view, when a car may appear or go), name limits, validation of received numbers |
| `game.cpp` | vehicles, spawning, the other player's car (ghost), race integration, autopilot |
| `traffic.inl` | traffic, cop and race-opponent mirroring |
| `budget.inl` | the game's 20-car budget, logged |
| `event.inl` | shared event start, roster, co-op rules (speedbreaker, no pause, restart sync) |
| `coopwin.inl` | shared wins |
| `bosshelper.inl` | rival races together: the partner's racer slot and the extra AI racer |
| `pursuit.inl` | cops chasing the other player, shared heat / busted / cooldown, their cops on the minimap and radar |
| `follow.inl` | going along to a Blacklist pursuit |
| `milestone.inl` | shared milestones and speed traps |
| `nis.inl`, `story.inl` | cutscene skip voting, story movies and scenes |
| `profile.inl` | the name you go by: your career profile's alias |
| `weather.inl` | the same rain on both PCs |
| `stream.inl` | world streaming around the other player |
| `minimap.inl`, `worldmap.inl` | player blips |
| `hud.inl` | F1: hide the minimap and the gauges |
| `nativeui.cpp` | Together in the game's menus (Options and Main menu entries, the settings list, text entry, the profile box) |
| `logo.cpp` | the main menu logo from `scripts\MWTogether-logo.dds` (taller Together variant, original lettering size kept) |
| `boot.inl` | skip the load-complete screen, last profile, final pursuit load fix |
| `input.*`, `pad.cpp` | DirectInput, gamepad hot-plug and bindings; AutoStart menu automation in dev builds only |
| `overlay.*` | D3D9 hooks and the status overlay |
| `*util.h` | the rules that don't need the game (pursuit, race, ground, milestones, prologue, profiles, notices), covered by `tests\` |


</details>

<details><summary><h2>Contributing</h2></summary>

Pull requests welcome, see [CONTRIBUTING.md](CONTRIBUTING.md).


</details>

<details><summary><h2>License</h2></summary>

MIT - see [LICENSE](LICENSE). Ultimate ASI Loader is a separate required download (also MIT): [THIRD-PARTY-NOTICES.txt](THIRD-PARTY-NOTICES.txt).


</details>

<details><summary><h2>Credits</h2></summary>

- Reverse-engineering references: NFSPluginSDK (MW05 headers), MostWantedTimeTrialGhosts and
  nya-common-nfsmw (MPL-2.0), ExOpts.
- [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) by ThirteenAG.
- Need for Speed and Most Wanted are trademarks of Electronic Arts. This is an unofficial fan project, not affiliated with EA.


</details>
