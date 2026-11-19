# Contributing to CoopMod

Thanks for helping! This is a reverse-engineering-heavy project, so a few rules keep it stable.

## Ground rules
- **Never commit game files**, saves or anything from the game install. Only our own source, docs and tools.
- **v1.3 only.** Every address is for `speed.exe` v1.3. Patch only after checking the expected bytes
  (`Mem::PatchBytes`) or the expected old call target or vtable value (`PatchCallChecked`). If it doesn't
  match, log it and leave the feature off. Never crash.
- **Document every address.** Put a comment block at the top of the feature file with each address, its
  calling convention and what it does, and how you found it (constructor, string reference, call site).
- **Game thread only.** Game calls happen from the frame callback. The only other thread is the network
  keep-alive, which touches nothing in the game.
- **Guard game calls** with `__try/__except`. Keep `std::string` and other objects with destructors out of
  `__try` functions (C2712): split into a small "Safe" helper.
- **Both players see the same world.** A feature isn't done until it is tested on two PCs. Describe the test
  in the pull request: what you did on each PC and the relevant `scripts\CoopMod.log` lines.
- **Protocol changes:** bump `COOP_VERSION` in `src/net.h` whenever a packet layout changes. Add new
  packet types at the end, with a length check in the receiver.

## Workflow
1. Fork or branch from `main`: `feature/<name>` or `fix/<name>`.
2. Build with `build.bat`. CI builds every pull request.
3. Test on two PCs (LAN or VPN), host and client.
4. Open a pull request with what changed, how you tested it, and any new addresses with their source.

## Reporting bugs
Open an issue and include:
- `scripts\CoopMod.log` from **both** PCs,
- which PC was host and which was client,
- what you did, what you expected and what happened instead.

## Good first areas
See [ROADMAP.md](ROADMAP.md): career progress sharing, cutscene skip sync, connect/disconnect
notifications in the game UI, a launcher, and up to 4 players.
