# SquadMod – AI squadmates for Conflict: Global Terror co-op

Conflict: Global Terror (PC, 2005) co-op only gives you soldiers for the human players. With two players, half of the four-man squad is missing.

**SquadMod fills every empty squad slot with an AI squadmate**, just like the campaign:

- AI squadmates spawn in co-op. The host simulates them and every client sees them.
- **Every player can give squad orders:** follow, hold, fire at will / hold fire, run to, hit the dirt, shield and interact. This includes clients.
- Squad orders and the highlight only affect AI squadmates, never the other human players.
- AI squadmates can be downed and revived.
- **Character switching:** press **Up / Down arrow** to take over the previous / next AI squadmate. The body you leave becomes AI. Works for the host and for clients.
- Mission 1 backpacks are placed for the AI squadmates too.

> Tested: LAN co-op, two players, mission 1. Other missions and more than two human players are not tested yet. Reports are welcome.

## Install

1. Copy `release/dinput8.dll` and `release/squadmod.ini` into the game folder, next to `ConflictGlobal.exe`.
2. **Every player** needs the mod installed.
3. Host a LAN co-op game as usual.

To uninstall, delete `dinput8.dll` (and `squadmod.ini`).

The mod is a `dinput8.dll` proxy. It loads the real system DirectInput and patches the game in memory, so no game files are modified. It works alongside dgVoodoo (`D3D9.dll`).

### Firewall

Besides the game's own ports, the mod uses **UDP port 4659** to send client orders and character switches to the host. Allow it on the host.

## Settings (`squadmod.ini`)

| Key | Default | Meaning |
|---|---|---|
| `Enabled` | 1 | Fill empty squad slots with AI in multiplayer. |
| `FillCount` | 4 | Total squad size (1–4). |
| `HostOwnsAI` | 1 | The host simulates the AI and clients mirror it. Leave at 1. |
| `Log` | 1 | Write `squadmod_N.log` in the game folder. |
| `LoopbackTest` | 0 | 1 = run **two copies of the game on one PC** for testing (copy 1 = 127.0.0.1, copy 2 = 127.0.0.2). Keep 0 for real PCs. |
| `FreeMouse` | 1 | Don't lock the mouse to the game window. |
| `Debug` | 0 | Extra diagnostic logging. |

### Testing on one PC

Set `LoopbackTest=1`, start the game, wait about 20 seconds, then start a second copy. Run the game windowed: the mod forces windowed mode, and dgVoodoo should be set to `FullScreenMode=false`.

## Build

Requires [Zig](https://ziglang.org/) (used as a C compiler for 32-bit Windows).

```bat
build.bat
```

which runs:

```
zig cc -target x86-windows-gnu -shared -O2 -o ../release/dinput8.dll squadmod.c dinput8.def -lwsock32 -luser32
```

## Known issues

- You can still pick the other human player as a single order target. They ignore the order.
- Client orders and switches use plain UDP with no retry. If a key press is lost, press it again.
- Missions 2–15 are untested. Some mission scripts may treat AI squadmates differently from players.

## How it works

See [docs/TECHNICAL_NOTES.md](docs/TECHNICAL_NOTES.md) for the reverse-engineering notes: game addresses, data layouts, the network order relay, the mission-script fixes and how each problem was solved.

## Disclaimer

This is a fan-made mod, not affiliated with the game's developer or publisher. You need your own copy of the game; no game files are included.
