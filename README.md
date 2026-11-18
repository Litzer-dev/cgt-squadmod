# SquadMod – AI squadmates for Conflict: Global Terror co-op

Conflict: Global Terror (PC, 2005) co-op only gives you soldiers for the human players. With two players, half of the four-man squad is missing.

**SquadMod fills every empty squad slot with an AI squadmate**, just like the campaign:

- AI squadmates spawn in co-op. The host simulates them and every client sees them.
- **Every player can give squad orders:** follow, hold, fire at will / hold fire, run to, hit the dirt, shield and interact. This includes clients.
- Squad orders and the highlight only affect AI squadmates, never the other human players.
- AI squadmates can be downed and revived.
- **Character switching:** press **Up / Down arrow** to take over the previous / next AI squadmate. The body you leave becomes AI. Works for the host and for clients.
- Mission 1 backpacks are placed for the AI squadmates too.
- **Play over Steam:** invite friends through Steam. No port forwarding and no IP addresses needed. Your Steam name is used as your in-game name.
- **Proper display settings:** any resolution up to your desktop size, windowed or borderless fullscreen, alt-tab safe, and remembered between sessions.
- **No more dead GameSpy buttons:** INTERNET and GAMESPY ID LOGIN are removed, and LAN becomes **STEAM CO-OP** when Steam play is on.
- **New in-game screens** under **OPTIONS**, in the game's own style, with keyboard and mouse:
  - **STEAM:** invite friends, choose Steam name or profile name, see who is in your lobby.
  - **GRAPHICS:** anti-aliasing, texture filtering and frame rate limit (dgVoodoo settings).

> Tested: LAN co-op with two copies on one PC, mission 1; Steam login, lobby, invite dialog and all menus on one PC. Real two-PC play over Steam, other missions and more than two human players are not tested yet. Reports are welcome.

## Install

1. Copy everything in `release/` (`dinput8.dll`, `squadmod.ini`, `steam_api.dll`) into the game folder, next to `ConflictGlobal.exe`.
2. **Every player** needs the mod installed.
3. Play over Steam (below), or on a real LAN set `Steam=0` and host a LAN co-op game as usual.

To uninstall, delete `dinput8.dll` (and `squadmod.ini`). If you used the GRAPHICS screen, rename `dgVoodoo.conf.squadmod.bak` back to `dgVoodoo.conf` to get your original dgVoodoo settings.

The mod is a `dinput8.dll` proxy. It loads the real system DirectInput and patches the game in memory, so no game files are modified. It works alongside dgVoodoo (`D3D9.dll`), which this game copy already uses. The only file outside `squadmod.ini` that the mod ever writes is `dgVoodoo.conf`, and only when you press APPLY on the GRAPHICS screen.

## In-game menus

### MAIN MENU and MULTIPLAYER

The dead GameSpy entries are removed, because GameSpy's servers shut down years ago:

- **Main menu:** GAMESPY ID LOGIN is hidden. The menu is now TRAINING, SINGLE PLAYER, MULTIPLAYER, EDIT PROFILE, OPTIONS, EXIT TO WINDOWS.
- **MULTIPLAYER:**
  - INTERNET is hidden.
  - With Steam on, LAN is renamed **STEAM CO-OP**. The screen it opens (SEARCH FOR MATCH / CREATE MATCH) gets the same title.
  - The bottom notice shows your Steam lobby (`STEAM LOBBY (N): names`) and how to invite friends.
  - With `Steam=0`, or if Steam failed to start, the button and screen keep the name **LAN** and the original notice.

### OPTIONS

The OPTIONS screen lists: **GAMMA CORRECTION, STEAM, GRAPHICS, VIDEO, AUDIO**.

STEAM and GRAPHICS sit between GAMMA and VIDEO because the game lays the rows out in its own internal order; moving them makes the game quit (see the technical notes).

### VIDEO

The game's own VIDEO screen, extended:

| Row | What it does |
|---|---|
| RESOLUTION | Common sizes (800×600, 1024×768, 1280×720, 1280×1024, 1366×768, 1600×900, 1920×1080) plus your desktop size; nothing larger than the desktop. The game's list holds at most 8 entries. |
| COLOR DEPTH | Unchanged. |
| WINDOWED | Hidden in the original PC release; the mod shows it again. **NO** = borderless window covering the whole monitor. **YES** = normal window of the chosen size, centred. |
| APPLY | Applies both and saves them to `squadmod.ini` (`[Display]`). |

- The game itself always starts at 800×600. The mod re-applies your saved size about 6 seconds after the menus appear, and closes the "keep these settings?" countdown for you.
- When you open VIDEO, the RESOLUTION row shows the size the game is really running at.
- Under the hood Direct3D always runs windowed, so alt-tab never minimises or loses the game.

### STEAM

| Row | What it does |
|---|---|
| INVITE FRIENDS | Opens Steam's *Choose Friend to Invite* overlay (same as **F7**). Shows *(CONNECTING...)* until the lobby exists, or *(STEAM OFF)* when `Steam=0`. |
| PLAYER NAME: STEAM NAME / PROFILE NAME | Enter switches which name other players see. Saved as `SteamName` in `squadmod.ini`. Takes effect the next time the name is sent (joining or hosting a game). |
| LOBBY (N): names | Everyone in your Steam lobby, refreshed twice a second. |

ESC goes back to OPTIONS.

### GRAPHICS

These are **dgVoodoo** settings. dgVoodoo reads them when the game starts, so they take effect **after a restart**.

| Row | Values | `dgVoodoo.conf` key |
|---|---|---|
| ANTI-ALIASING | OFF, 2X, 4X, 8X | `[DirectX] Antialiasing` |
| TEXTURE FILTERING | GAME DEFAULT, BILINEAR, TRILINEAR, ANISOTROPIC 4X / 8X / 16X | `[DirectX] Filtering` (`appdriven`, `bilinear`, `trilinear`, `4`, `8`, `16`) |
| FRAME RATE LIMIT | 30, 60, 120, 144, UNLIMITED | `[GeneralExt] FPSLimit` (UNLIMITED = `0`) |

- Left / right arrows change a value. The screen always opens showing what is in `dgVoodoo.conf` right now.
- **APPLY (AFTER RESTART)** writes the three values and changes to *SAVED - RESTART THE GAME TO USE* (or *COULD NOT SAVE DGVOODOO.CONF*).
- Only those three lines change; everything else in the file, including the `[Glide]` section, is left alone.
- The first save copies your original file to `dgVoodoo.conf.squadmod.bak` (never overwritten afterwards).
- A value that isn't in the list (for example set by hand in dgVoodooCpl) shows as the first entry until you change it.

## Playing over Steam

With `Steam=1` (the shipped setting), the mod connects to Steam when the game starts and creates a **friends-only Steam lobby** (max 4). Everyone in the same lobby is on one virtual LAN. The game's normal LAN screens work as if you were all on one network, and the traffic goes through Steam's relay servers.

1. **Everyone starts Steam and then the game.** The window title shows `Steam lobby (N players) - F7 invite friends`.
2. **The host invites friends:** **OPTIONS > STEAM > INVITE FRIENDS**, or press **F7** in game. If the Steam overlay doesn't appear, right-click the friend in the Steam friends list and choose *Invite to game*.
3. **Friends join:** accept the invite, or right-click the host in the Steam friends list and choose *Join game*. The game must already be running. The player count in the title and on the STEAM screen goes up.
4. The host goes to **MULTIPLAYER > STEAM CO-OP > CREATE MATCH**. Friends go to **MULTIPLAYER > STEAM CO-OP > SEARCH FOR MATCH** and join it. This is the game's normal LAN screen, renamed.

Notes:

- The mod uses Steam App ID **480** (Valve's free test app "Spacewar"), because the game isn't sold on Steam. Steam shows you as playing *Spacewar*. `steam_appid.txt` is created automatically.
- `steam_api.dll` is Valve's redistributable from the Steamworks SDK. It must be next to `ConflictGlobal.exe`.
- Your Steam name is cut to 15 plain ASCII characters, because the game's font has nothing else.
- If Steam isn't running, the log says `steam: init failed` and the game falls back to plain LAN.
- **Dropped from the lobby:** if Steam drops you from the lobby (a network or Steam hiccup), the mod notices within a second and rejoins the same lobby. If that lobby no longer exists, it creates a new one, and you invite your friends again. Meanwhile OPTIONS > STEAM shows *CONNECTION LOST - REJOINING...*.
- **A friend drops out:** their connection is kept open for 90 seconds, so a short drop can recover when they come back into the lobby. Whether the match itself survives depends on the game's own timeout.
- **Joining a match already in progress isn't possible.** The game only lets players in before the mission loads. After a disconnect, the host goes back to STEAM CO-OP and creates a new match, and everyone joins it.

### Firewall (LAN without Steam)

Besides the game's own ports (UDP 10010 discovery, TCP and UDP 4658), the mod uses **UDP port 4659** to send client orders and character switches to the host. Allow it on the host. Over Steam, no ports need to be opened.

## Settings (`squadmod.ini`)

### `[SquadMod]`

| Key | Default | Meaning |
|---|---|---|
| `Enabled` | 1 | Fill empty squad slots with AI in multiplayer. |
| `FillCount` | 4 | Total squad size (1–4). |
| `HostOwnsAI` | 1 | The host simulates the AI and clients mirror it. Leave at 1. |
| `Log` | 1 | Write `squadmod_N.log` in the game folder. |
| `LoopbackTest` | 0 | 1 = run **two copies of the game on one PC** for testing (copy 1 = 127.0.0.1, copy 2 = 127.0.0.2). Keep 0 for real PCs. |
| `FreeMouse` | 1 | Don't lock the mouse to the game window. **Also turns on the display handling** (windowed / borderless, saved resolution). With 0 the game runs as it originally did. |
| `Debug` | 0 | Extra diagnostic logging, including the reason if the game ever closes by itself. |
| `Steam` | 1 in the shipped ini (0 if the key is missing) | Play over Steam (friends-only lobby = virtual LAN). 0 = plain LAN. |
| `SteamName` | 1 | 1 = other players see your Steam name, 0 = your game profile name. Set from OPTIONS > STEAM. |
| `TunnelTest` | 0 | Developer test: with `LoopbackTest=1`, the two copies talk through the same tunnel Steam uses, but over local UDP. |

### `[Display]` (written by the VIDEO screen's APPLY)

| Key | Meaning |
|---|---|
| `Windowed` | 1 = normal window, 0 = borderless fullscreen. |
| `Width`, `Height` | Resolution re-applied at startup. Delete them to start at the game's own 800×600. |

### Testing on one PC

Set `LoopbackTest=1`, start the game, wait about 20 seconds, then start a second copy. In this mode the two copies always open as normal windows side by side (copy 1 on the left, copy 2 on the right), and dgVoodoo should be set to `FullScreenMode=false`.

## Build

Requires [Zig](https://ziglang.org/) (used as a C compiler for 32-bit Windows).

```bat
build.bat
```

which runs:

```
zig cc -target x86-windows-gnu -shared -O2 -o ../release/dinput8.dll squadmod.c tunnel.c dinput8.def -lwsock32 -luser32
```

## Releasing an update (ConflictLauncher)

ConflictLauncher checks `https://api.github.com/repos/Litzer-dev/cgt-squadmod/releases/latest` on startup. It compares `tag_name` with `mod_version.txt` in the game folder, then extracts the release's first `.zip` asset into the game folder. Every update goes out with:

```powershell
.\release.ps1 1.0.1            # build + dist\cgt-squadmod-v1.0.1.zip, nothing uploaded
.\release.ps1 1.0.1 -Publish   # also push tag v1.0.1 and create the GitHub release with the zip
```

Rules the launcher depends on:
- **Tags:** `vX.Y.Z`, always higher than the last one. It must be a full release, not a draft or pre-release, because `/releases/latest` skips those.
- **One `.zip` asset**, with the files at its root: `dinput8.dll`, `steam_api.dll`, `README.txt` (installed as `SquadMod_README.txt`).
- **No `squadmod.ini` or `dgVoodoo.conf` in the zip.** The launcher only keeps the player's `Steam=` value, so shipping them would reset their `[Display]`, `SteamName` and GRAPHICS settings. Keys missing from an older ini use the mod's built-in defaults. The launcher's first install already writes both files.
- **The repo must stay public.** For a private repo, GitHub's API answers 404 to the launcher, so it never sees an update.

## Known issues

- You can still pick the other human player as a single order target. They ignore the order.
- No joining a mission in progress (the game's session flow: lobby → load level → ready → start). After a disconnect, create a new match.
- Client orders and switches use plain UDP with no retry. If a key press is lost, press it again.
- Steam play has been tested on one PC so far (login, lobby, invite overlay, menus). The two-PC test is pending.
- Missions 2–15 are untested. Some mission scripts may treat AI squadmates differently from players.
- GRAPHICS changes need a game restart (dgVoodoo only reads its config at startup).
- STEAM and GRAPHICS appear between GAMMA and VIDEO in OPTIONS, not at the bottom.
- The help text on the CREATE MATCH settings screen still says "Create a LAN game."
- In the menus the mouse cursor uses relative movement, as in the original game; the keyboard is the most reliable way to navigate.
- After the game is killed, it may start on *CHOOSE A PROFILE* or show its launcher dialog first. That is the game, not the mod.

## How it works

See [docs/TECHNICAL_NOTES.md](docs/TECHNICAL_NOTES.md) for the reverse-engineering notes: game addresses, data layouts, the network order relay, the Steam tunnel, the menu/UI engine (including the GameSpy clean-up), the display handling, the mission-script fixes and how each problem was solved.

## Disclaimer

This is a fan-made mod, not affiliated with the game's developer or publisher. You need your own copy of the game; no game files are included.
