# SquadMod – AI squadmates for Conflict: Global Terror co-op

This mod gives online/LAN co-op in **Conflict: Global Terror** (PC, 2005) the same four-man squad as the campaign. Any squad slot that no human uses is filled by an AI squadmate. The AI follows orders from every player, gets its mission kit, can be revived, and any player can switch into it, like in single player.

On top of that it adds online play through Steam (a virtual LAN tunnelled over Steam's relays), proper windowed / borderless display handling with a saved resolution, and two new in-game screens (STEAM and GRAPHICS) built from unused screens of the game's own UI.

These notes cover what was found by reverse-engineering `ConflictGlobal.exe` and the mission DLLs, what the mod changes, and why. All addresses are for the `ConflictGlobal.exe` in this install (cracked SecuROM build with an `.rld` section). The mod checks the original bytes at every patch site and skips that patch, with a log line, if they differ.

---

## 1. Status

| Feature | State |
|---|---|
| AI squadmates spawn in empty co-op slots | ✅ working |
| AI move/fight, simulated by the host, seen correctly on the client | ✅ working |
| Host gives squad orders (follow, hold, hold fire, fire at will, hit the dirt, go to, shield, interact) | ✅ working |
| Client gives squad orders (all of the above) | ✅ working |
| Client's squad-wide "hit the dirt" does not knock the host player down | ✅ working |
| Mission 1 backpacks exist for the AI squadmates and they can pick them up | ✅ working |
| AI squadmate death / revive | ✅ working (no changes needed) |
| Character switching (Up/Down arrow) – host | ✅ working |
| Character switching (Up/Down arrow) – client, including handing a body over between PCs | ✅ working |
| Squad orders / highlight leave the other human player out (also after switching) | ✅ working |
| Picking the other human as a single order target | ⚠️ still possible (accepted) |
| Two copies on one PC (loopback LAN test) | ✅ working |
| Steam: login, friends-only lobby, invite overlay (F7 and menu) | ✅ working (one PC) |
| Tunnel test mode (`TunnelTest=1`: two copies, same tunnel code as Steam) | ✅ working |
| Steam name used as the in-game player name (switchable) | ✅ working |
| Steam play between two real PCs | ⚠️ not tested |
| VIDEO screen: WINDOWED row, wider resolution list, borderless fullscreen | ✅ working |
| Saved resolution / window mode re-applied at startup | ✅ working (startup crash fixed, see 4.11) |
| VIDEO screen shows the resolution actually in use | ✅ working |
| OPTIONS > STEAM screen (invite, player name, lobby list) | ✅ working |
| OPTIONS > GRAPHICS screen (AA, filtering, FPS limit → `dgVoodoo.conf`) | ✅ working |
| GameSpy entries removed (main menu GAMESPY ID LOGIN, MULTIPLAYER INTERNET) | ✅ working |
| MULTIPLAYER LAN renamed STEAM CO-OP (button + screen title), lobby line in the notice | ✅ working |
| Missions 2–15 | ⚠️ not tested |
| Real two-PC LAN (`LoopbackTest=0`) | ⚠️ not tested |
| 3–4 human players | ⚠️ not tested |

Gameplay testing so far: two copies of the game on one PC, LAN co-op, mission 1. Display, menus and Steam were tested on one PC at 1920×1080 on Windows 11 with dgVoodoo.

---

## 2. Files

| File | Purpose |
|---|---|
| `src/squadmod.c` | Everything except the network tunnel: spawner, orders, relay, switching, kit fix, window/display handling, VIDEO screen, STEAM/GRAPHICS menus, diagnostics. |
| `src/tunnel.c` | Virtual-LAN tunnel (Steam transport and the two-copy UDP test transport) and the Steam glue (lobby, invite, names). |
| `src/dinput8.def` | Export forwards for the proxy DLL. |
| `release/dinput8.dll` | Build output; goes in the game folder. |
| `release/squadmod.ini` | Settings; goes in the game folder. |
| `release/steam_api.dll` | Valve's Steamworks redistributable (32-bit). Loaded at runtime only when `Steam=1`. |
| `squadmod_1.log`, `squadmod_2.log` | Written to the game folder (one per running copy). |
| `steam_appid.txt` | Created in the game folder with `480` if it is missing. |
| `dgVoodoo.conf.squadmod.bak` | Copy of the original `dgVoodoo.conf`, made by the first GRAPHICS APPLY and never overwritten. |

### Build and install

```sh
build.bat
rem then copy release\dinput8.dll and release\squadmod.ini next to ConflictGlobal.exe
```

To test on one PC: start copy 1, wait about 20 seconds, then start copy 2.

### Why a `dinput8.dll` proxy

- `D3D9.dll` in the game folder is already **dgVoodoo**, so the mod has to use another proxy DLL. The game imports DirectInput 8, so `dinput8.dll` works. The mod loads the real system `dinput8.dll` and forwards to it.
- The exe has an `.rld` section: a cracked SecuROM build where several functions are **virtualized** (for example 0xd94 "is network game" → `FUN_00bbc6d0`, `0x574a14` and `0x574a74`). Editing the exe file is fragile, so every change is made **at runtime** from the proxy: call-site patches, JMP hooks, IAT hooks and function-table edits.

### `squadmod.ini`

```ini
[SquadMod]
Enabled=1        ; fill empty squad slots with AI in multiplayer
FillCount=4      ; total squad size (1-4)
HostOwnsAI=1     ; AI is simulated by the host and mirrored to clients (recommended)
Log=1            ; write squadmod_N.log
LoopbackTest=0   ; 1 = two copies on ONE PC (instance 1 = 127.0.0.1, instance 2 = 127.0.0.2)
FreeMouse=1      ; don't lock the mouse; ALSO turns on all D3D/window hooks (display handling)
Debug=0          ; extra diagnostics (order handler, net messages, inventories, crash/exit logging)
Steam=1          ; Steam virtual LAN (the code default is 0 if the key is missing)
SteamName=1      ; 1 = Steam name as the player name, 0 = profile name (written by OPTIONS > STEAM)
TunnelTest=0     ; with LoopbackTest=1: route the two copies through the tunnel (UDP 127.0.0.1:4700+N)

[Display]        ; written by VIDEO > APPLY, read at startup
Windowed=0       ; 1 = framed window, 0 = borderless window covering the monitor
Width=1920
Height=1080
```

Notes:
- Keys are read with `GetPrivateProfileIntA`. The instance number comes from the mutex `Local\CGTSquadMod_Instance1`: the first process to create it is instance 1.
- `FreeMouse=0` also skips the `Direct3DCreate9` hook. Without it there is no display handling, no saved resolution and no STEAM/GRAPHICS menus, because the menus are driven from the `Present` hook.

---

## 3. Reverse-engineering reference

### 3.1 Global objects

| Address | Meaning |
|---|---|
| `*(0x6ffe14)` | Network manager; `+0xC0` = **session**. |
| `session + slot*0x8C` | Player slot record. `+0x00` = name, `+0x44` = character (1..4 → entity ids 9, 12, 10, 11). |
| `session+0x500C & 2` | This machine is the **host**. |
| `session+0x5008 & 4`, `+0x5010` | Hidden dev "fill slots" mode and its count (set by `FUN_00590500` / `-n` switch). |
| `*(0x8de7c0)` | Entity manager. `+0x2C` = entity table, `+0x30` = max id (`0x62f560` = get entity by id). |
| `0x7017c4` (byte) | This machine's slot (host = 0). |
| `0x71849c` (byte) | Local team. |
| `0x71f770` (word) | Player count (engine table entry 0x358). |
| `0x6f4314` | Slot → entity id table. |
| `0x6f1da0` | **Engine function table** given to mission DLLs (`LevelSetup(engineTable)`, loader `FUN_0041ab20`). |
| `0x6f1290` | Input action table: 8-byte entries, name pointer + id. |
| `*(0x71f590)` | Current mission script callback object (`+0xC` = event dispatcher). |
| `*(0x736b20)` | **UI root** (front-end). `+0x28` = screen manager. See 3.8. |
| `*(0x72fc70)` | Message-box screen object (vtable `0x6cecd8`). Used by the VIDEO "keep these settings?" dialog. |
| `*(0x72fb4c)` | The screen the VIDEO APPLY puts on layer 4 (the confirmation countdown). |
| `0x739600` / `0x739620` | VIDEO resolution list: at most **8** entries. `0x739620 + 48*i` = the entry's `"%ux%u"` string. |
| `session + 0x5018` | Local player name, copied from the save profile by `0x58f2e0`. |

### 3.2 Entity (soldier) fields

| Offset | Meaning |
|---|---|
| `+0x04` | Type (1 = soldier). |
| `+0x08` | Entity id. |
| `+0x15` | Team. |
| `+0x4A` | **Owner machine.** The machine that simulates the entity (host = 0, client = its slot). |
| `+0x3A0` | Controller that drives this soldier (if a player's). |
| `+0x3AC` | Network id. |
| `+0x808` / `+0x814` | Current item / inventory (40 item pointers; item `+0x24` = stat block). |
| `+0xC38` bit `0x4000000` | Prone order flag. |
| `+0xD7C` | Allowed-actions mask (bit 0 orders, **bit 1 = can be switched to**). |
| `+0xF1C` | **Network/physics mode**: 1 = simulated here, 2 = ragdoll, 3 = mirrored from another machine. |
| `+0xF9A` bit 4 | "Player-controlled": skipped by squad-wide orders such as hit the dirt. |

### 3.3 Player controller (`ctl`)

| Offset | Meaning |
|---|---|
| `+0x448` | Controlled soldier. |
| `+0x450` | State. |
| `+0x4B4` / `+0x4B5` | Context option count / selected option. |
| `+0x4B8` | Context target entity. |
| `+0x4C8` | Marker id. |
| `+0x4CC` / `+0x4D0` | Option array (stride 8). |
| `+0x671` bit `0x10` | Advance ("go to") mode, toggled by `0x41c6b0`. |

### 3.4 Per-player update / squad panel

- `FUN_0050a360` = per-player update. `this+0x7C` = ctl, `this+0x88` = **squad panel**.
- Squad panel:
  - vtable `0x6cc998`
  - `+0x81` = member count, `+0x7E` = selected index
  - `+0xDC[4]` = members, `+0xD8` = controller
  - `+0x118` bit 0 = all-squad
- Panel functions:
  - `vtbl[8]` = next (`0x4f6ad0` → `0x5a2010`), `vtbl[9]` = previous
  - `vtbl[31]` = **selectable** check `0x4f6d50(panel, idx)`: refuses dead members and lets the mission script veto via event `0x6e`
  - `vtbl[7]` (message 0x17) → `0x422f60`
- **`0x422f60(ctl, soldier)`** (thiscall) = *switch the player to this soldier* (the campaign character switch). It requires `soldier+0xD7C & 2`. It calls `0x422210(ctl, soldier)`, which writes `ctl+0x448`.

### 3.5 Input action ids (from table 0x6f1290)

| Id | Name | Id | Name |
|---|---|---|---|
| 0x01 | NEXT_GRUNT | 0x03 | PREV_GRUNT |
| 0x1C | ORDER | 0x1F | FOLLOW |
| 0x20 | HOLD | 0x21 | ADVANCE (go to; context: shield / interact) |
| 0x22 | ALL_PRONE | 0x23 | ALL_PRONE_KB |
| 0x24 | FIRE_AT_WILL_TOGGLE | 0x32 | SUPPRESSIVE_FIRE_TOGGLE |
| 0x33 | HEAL_MIA | 0x52–0x55 | GRUNT_1..4 (F1–F4) |
| 0x56–0x59 | GRUNT_FOLLOW_HALT_1..4 | 0x5A | ALL_FOLLOW_HALT |
| 0x5B | ALL_FIRE_AT_WILL | | |

### 3.6 Network

- `0x471ec0` = "is network game" (engine 0xd94).
- `0x471ff0` = engine 0xda4. `0x471fe0` = engine 0xda0.
- Game traffic (all through `WSOCK32.dll`, imported **by ordinal**):
  - UDP **10010**: LAN game discovery by broadcast.
  - TCP port **4658**: the client connects to the host.
  - UDP **4658**: game state.
  - The mod adds UDP **4659** for its relay (4.4).
  - Debug LAN mode reads `networkconfig.txt` (`FUN_00471880`, port 0x1232).
- Host dispatcher = `FUN_0046b470`. Send function = `0x46d3a0`.
- Built-in order forwarding exists only for `0x49fe50` / `0x498cf0` (message 0x1B client → host, reply 0x1A).
- Entity network registration (both virtualized):
  - `0x574a14(netId, mode, callback, b)`
  - `0x574a74(netId, b)`
  - The game uses these combinations:
    - **Mirrored** (`F1C = 3`): `0x574a14(netId, 1, 0x473410, 1)` + `0x574a74(netId, 1)`. `0x473410` is the state-sync callback.
    - **Local** (`F1C = 1`): `0x574a14(netId, 0, NULL, 1)` (see `0x4b3155`).
    - **Ragdoll** (`F1C = 2`): `0x574a14(netId, 1, 0x4a7d90, 1)`.
- Prologue owner callbacks used by the squad spawner:
  - `0x4730f0` (host): owner = local slot.
  - `0x473110` (client): owner = 0 and mirrored.

### 3.7 Mission scripts (`MissionXX.dll`)

- Each exports `LevelSetup(engineTable)` and calls the engine through the table at `0x6f1da0`.

Useful engine table entries:

| Offset | Function | Meaning |
|---|---|---|
| 0x2BC | `0x446c80` | Enable/disable entity. |
| 0x358 | – | Player count (`0x71f770`). |
| 0x45C | `0x447cf0` | **Delete object.** |
| 0x610 | – | Soldier `+0xD7C` bitfield. |
| 0x714 | – | Entity vtable+0x78 (character enable). |
| 0x890 | `0x44b940` | Give item to soldier (`0x493ba0`). |
| 0xD94 | `0x471ec0` | Is network game. |
| 0xDA4 | `0x471ff0` | Network query. |
| 0xDB0 | `0x4724d0` | **"Character is a human-played participant"** (looks the character up through the slot → id table). |

Mission01-specific details:
- Setup `FUN_100064f0`: `DAT_10012778` = network game, `DAT_1001277c` = more than one player.
- `FUN_10001290` disables characters 3, 0 and 2 in single player with more than one player.
- `FUN_10006be0`: in a network game, for characters 3, 2, 1 and 0, if `engine[0xdb0](char) == 0` it calls `engine[0x45c]` on objects `0x4E`, `0x4D`, `0x4B` and `0x4C`. **These are the character backpacks.**
- `FUN_10002120` / `FUN_10003120`: objective checks that also use `engine[0xdb0]`. For example, "every participant is alive and carries item 0x39".

### 3.8 Front-end UI engine (menus)

Found while building the STEAM and GRAPHICS screens. All functions are `thiscall`, which the mod calls as `__fastcall` with a dummy `edx`.

**Screens and layers**

| Item | Meaning |
|---|---|
| `*(0x736b20)` | UI root. `+0x28` = screen manager. |
| `0x599e20(ui, id, layer)` | Go to the screen with this id. Menus use layer **4**. |
| `0x597540(mgr, id)` | Get a screen object by id (screens exist for the whole session). |
| `0x599980(ui, layer)` | Screen currently on a layer. |
| `0x599690(mgr, layer)` | Close the screen on a layer. |
| `mgr + 0x240` | Current layer-4 screen. The test tool `scr.py` reads its id from `+0x14`. |
| Screen `vtbl[7]` | `OnEvent(ev, widget)`, `ret 8`. Returns 1 when handled. |
| Screen `vtbl[13]` | Per-frame `Update()`. The base screen update is `0x59e010`. |

**Screen events** (first argument of `vtbl[7]`)

| Event | Meaning |
|---|---|
| `0x04` | Select (Enter / click). The widget argument can be the **screen itself**, not the button: read the focused row from the group (`group+0x50`). |
| `0x05` | Back (Esc / right mouse). |
| `0x15` | Screen entered. Screens run their own setup here; ONLINE OPTIONS runs GameSpy code. |
| `0x1e` | Screen opened (first build). |
| `0x1d` | Seen on screens around a mode change. |
| `0x21` | Broadcast to screens while the VIDEO settings are applied. |
| `0x03`, `0x17` | Focus moves (seen on every Up / Down). |

**Widgets**

| Offset | Meaning |
|---|---|
| `+0x00` | vtable. Every widget's vtable lies in `0x600000..0x6fffff`, which is how the mod tells a built widget from garbage. |
| `+0x0c` | Parent widget (for a row: its group). |
| `+0x14` | **Id**: a 32-bit hash of the widget's name, e.g. `0x3a21c6fd` = `BN_APPLY`. |
| `+0x20` | Flags. `0x8000` = visible, `0x00100000` = present on PC. The mod uses `0x00108000` to show a row the PC build hid. |
| `+0x24` | Rect `x, y, w, h` (floats) in the parent's coordinates. Rescaled by the game on resolution changes. |
| `+0x3d` bit 8 | Text widget: draw the string hash at `+0x48` (otherwise the `wchar_t*` at `+0x4c`). |
| `+0x40` | Child container: array at `+0x44`, count (byte) at `+0x48`. |
| `+0x48` | Text widget (type `0x1c6e1bbb`, vtable `0x6d7c98`): **string hash**, looked up through `0x686a10` each time it is drawn. |
| `+0x50` | Group: currently focused child. |
| `+0x74 / +0x75 / +0x76` | Toggle (`< value >` row): value / min / max bytes. |

Widget functions:
- `0x5a0160(container, id)`: find a direct child by id.
- `0x5a0500(container, id)`: find a widget anywhere under a container (the mod passes `screen+0x24`).
- `0x5a01a0(container, type, &idx)`: find a child by type.

**Localisation**
- `0x686a10(table, hash)` returns the `wchar_t*` for a string hash.
- Its first bytes are `8B 41 38 85 C0 76`. The mod relocates `mov eax,[ecx+38h]; test eax,eax` to a trampoline that jumps back to `0x686a15`.
- Custom strings use hashes `0x5D5D0000 + n`.
- The game font has only plain ASCII, so anything shown in a menu is filtered to `0x20..0x7e`.

**Layout and navigation in a group**
- A group (for example the OPTIONS list) lays out **and** navigates its visible children in **child-array order**.
- Hiding a row (clearing `0x8000`) removes it from both layout and navigation. Showing it puts it back at its array position.
- Changing a row's `+0x28` (y) moves only the drawing, not the navigation order.
- **Do not reorder the child array.** Swapping entries made the game quit later, when VIDEO applied settings (event `0x21` goes through the group's children).

**Message box / confirmation dialog**
- The message-box screen `*(0x72fc70)` (vtable `0x6cecd8`) keeps widget pointers at `+0x74 .. +0x8c`.
- They hold `0` or the heap fill `0xFAFAFAFA` until the screen has been built.
- `0x50fec0` uses them **without checks**, see 4.11.

**Exception handling**
- The game catches access violations itself and calls `ExitProcess(0xC0000005)`, from `0x6a88ed`.
- So a crash inside the game shows as a silent exit with no Windows Error Reporting event. See 5 for how the mod logs it.

**Screens and ids used by the mod**

| Id | Name | Notes |
|---|---|---|
| `0x215067a4` | SCR_OPTIONS | Event vtable `0x6cd730`. |
| `0x10b6fd66` | SCR_VIDEO | Group `0x4b5da259`. Rows: TOG_RESOLUTION `0x256da361`, TOG_COLOUR_DEPTH `0x96109e70`, TOG_WINDOWED `0xf03fcd4a`, BN_DEFAULT `0xacf936f4`, BN_SCAN_HARDWARE `0x991ebb3f`, BN_ADVANCED_SETTINGS `0xcd8e30b8`, BN_APPLY `0x3a21c6fd`. |
| `0x2ac24a2b` | SCR_ONLINE_OPTIONS (console/GameSpy, unused on PC) → **STEAM** | vtable `0x6ce4f0`. Title widget `0x08736add`. Buttons FRIENDS `0x72e30658`, OPTIONS `0x33c9dd57`, SCOREBOARDS `0x1dfaca0c`. GameSpy icon `0xfeab8365`. |
| `0x80862421` | SCR_NETWORK_OPTIONS (voice options, unused on PC) → **GRAPHICS** | vtable `0x6ce558`. Title widget `0x0873dc9d`. Toggles `0x214631f9`, `0x8e0547aa`, `0x8e45ca5d`; their value texts `0x5ea43dea`, `0x733802e1`, `0x35102b0d`. BTN_NET_OPTIONS_APPLY `0x4304c950`. |

Other ids and hashes:
- Both reused screens' titles hold the string hash `0x7e74ba26`; the mod replaces it with its own.
- OPTIONS buttons:
  - BN_OPTIONS_VIDEO `0x46016b2c`
  - BN_OPTIONS_AUDIO `0xd0d96e32`
  - BN_OPTIONS_REFRESH_RATE `0x40919218` → STEAM
  - BN_OPTIONS_CENTRE_SCREEN `0x835b97d7` → GRAPHICS
  - The last two are hidden on PC.
- **Network toggle class** (the GRAPHICS toggles), vtable `0x6cd620`:
  - `vtbl[7]` = `0x510560`. It handles only events `0x1d` and `0x1f5` and returns 0 otherwise. Left / right value changes are handled by the base input code, not here.
  - `vtbl[13]` = `0x510820`, which calls the base update `0x5a5e70` and then `0x510240`.
  - Every frame, `0x510240` rewrites the value-text hash by toggle id. For APPEAR_OFFLINE it also calls the GameSpy online-status function.

---

## 4. What the mod does (fix by fix)

### 4.1 Spawning AI into empty slots

**Problem.** Vanilla co-op only creates soldiers for occupied player slots.

**Finding.** `FUN_004731b0` is the network squad spawner (4 slots). A slot spawns only if a player is in it (`0x58f3a0`) or the hidden dev "fill" mode is on.

**Fix.**
- `Hook_SpawnSquad` reimplements `0x4731b0` (JMP hook). For each empty slot below `FillCount` it creates a soldier:
  1. First it tries the dev-fill flags.
  2. If that fails, it borrows the host's slot record with a character (record `+0x44`) that no other slot uses.
- AI soldiers get **owner = 0 (host)**. On clients they are registered as mirrored (`F1C = 3` + net registration), so the host simulates them and clients only display them.

### 4.2 Running two copies on one PC (test setup)

- The game mutex `{CGTERROR-713BE74C-...}` and its events are renamed per copy, so the second copy starts.
- **Loopback networking** (`LoopbackTest=1`):
  - `gethostbyname` returns `127.0.0.N`.
  - Sockets are bound to `127.0.0.N` before connect.
  - Broadcasts and "other" addresses are redirected to the other copy.
  - The winsock imports are by ordinal, which the IAT patcher handles.
- **Windowing** (`FreeMouse=1`):
  - The game asks for exclusive fullscreen 800×600, and dgVoodoo minimises it when it loses focus. The mod forces `Windowed=1` in `CreateDevice` / `Reset`. Section 4.11 covers the full display handling that grew out of this.
  - A wrapped window proc (installed through a `RegisterClassA` hook) swallows:
    - `WM_ACTIVATE(WA_INACTIVE)` and `WM_KILLFOCUS`
    - `WM_SIZE(SIZE_MINIMIZED)`, posting a restore instead
    - `SC_MINIMIZE`
  - `ShowWindow(SW_HIDE / SW_MINIMIZE / …)` and `SetWindowPos(SWP_HIDEWINDOW)` on the game window are blocked.
  - DirectInput devices are made non-exclusive (`IDirectInput8::CreateDevice` hook).
  - The cursor is clipped only while the game is in the foreground (`ClipCursor`, `SetCursorPos` and `SetCapture` hooks).
  - `GetTopWindow` is hooked so each copy skips the other copy's `GLOBAL_STORM` window when it looks for its own.
  - `dgVoodoo.conf` settings in use:
    - `[General]`: `FullScreenMode=false`, `ScalingMode=stretched_4_3`, `CaptureMouse=false`
    - `[DirectX]`: `AppControlledScreenMode=false`, `Resolution=unforced`
    - Older backups: `dgVoodoo.conf.bak`, `dgVoodoo.conf.bak_display`.

### 4.3 Squad orders in multiplayer

**Problem.** Squad orders did nothing in multiplayer.

**Finding.** The per-player update `0x50a360` skips squad input when `CALL 0x471ec0` ("is network game") returns true.

**Fix.** Two call sites are patched to `xor eax,eax; nop×3` (`31 C0 90 90 90`):
- `0x50a8e0`: gate before the order handler `FUN_004fe770`.
- `0x50a751`: gate on releasing the order button (0x1C), which leaves order mode.

**Result.** Host orders work. Client follow / stay / hold fire / fire at will already work, because the game forwards those (message 0x1B).

### 4.4 Order relay (client orders the game never forwards)

**Problem.** From the client, "go to", "shield", "interact" and "hit the dirt" did nothing. For hit the dirt the AI dropped and stood up at once, because the host's state overwrote the client's local change.

**Finding.** Only `0x49fe50` / `0x498cf0` have network code. The other order functions inside `0x4fe770` only change local state:

| Order | Function |
|---|---|
| Go to, all | `0x424580(ctl)` |
| Go to, one member | `0x421bb0(ctl, member)` |
| Hit the dirt, all | `0x41b910(ctl)` (reads only `ctl+0x448`) |
| Hit the dirt, one member | `0x473ef0(soldier, member)` |
| Context order (shield / interact), all | `0x4a3dc0(soldier, a)` |
| Context order (shield / interact), one member | `0x49fc50(soldier, member, a)` |

**Fix: a small UDP relay on port 4659** (packet magic `'CGTS'`).
- **Client:** the 7 `E8` call sites inside `0x4fe770` are redirected to wrappers:

  | Call site | Wrapper |
  |---|---|
  | `0x4feb67` | AdvAll |
  | `0x4feb72` | AdvOne |
  | `0x4febf9`, `0x4fedc6` | ProneAll |
  | `0x4fec06` | ProneOne |
  | `0x4fed76` | CtxAll |
  | `0x4fed7e` | CtxOne |

  Each wrapper sends the operation, the orderer's entity id, the member id, the argument and the controller block `ctl[0x440..0x680)`. Each dword is tagged RAW, ENT (an entity pointer, sent as its id) or KEEP (a local pointer, not sent). Then it runs the original locally.
- **Host:** it reads the socket in `RelayPump`, called from the order-handler probe on the **game thread**.
  - It overlays the block onto its own controller (entity ids mapped back to pointers, `ctl+0x448` = orderer), calls the same function, and restores the controller.
- **Finding the host address:** recorded from the game's TCP connect to port 4658. In loopback mode it falls back to the other copy's `127.0.0.N`.

### 4.5 Client's squad-wide "hit the dirt" knocked the host down

**Finding.** `0x41b910` walks the whole team and skips only soldiers with `+0xF9A` bit 4 set. The host player's soldier doesn't have that bit on the host while replaying the client's order.

**Fix.** While replaying `PRONE_ALL`, the host temporarily sets `+0xF9A` bit 4 on every **human** squad member except the one who gave the order, and clears it afterwards.
- A soldier counts as human if it is the host controller's soldier or has owner ≠ 0.
- Originally the slot table was used. That changed with character switching, because the slot table goes stale after a switch.

### 4.6 Mission 1 backpacks missing for the AI

**Investigation.** An inventory dump showed that every squad member, human or AI, *carries* the same kit: knife, frag, flashbang, phosphor and smoke grenades, medikit. So the missing "equipment" was the **backpacks placed in the level** ("Bradley's Backpack").

**Finding.** In a network game, Mission01 `FUN_10006be0` deletes the backpack (`engine[0x45c]`) of every character for whom `engine[0xdb0]` says "not a human participant". So the AI's backpacks vanished.

**Fix (narrow on purpose).**
- The mod replaces entry `0xdb0` in the engine table (`0x6f1da0 + 0xdb0`) with `Hook_CharIsPlayer`.
- It returns 1 for an AI-filled character **only when the instruction bytes right after the call contain `call [reg+0x45c]`**, which is a "delete object" call.
- Objective checks that also use `0xdb0` (such as "all participants picked up their backpack") keep seeing the AI as non-players, so the mission can't stall waiting for the AI.
- A byte scan of all mission DLLs found this pattern only in Mission01 (4 sites).

### 4.7 Character switching (Up / Down arrows)

**Problem.** F1–F4 / NEXT_GRUNT do nothing in multiplayer. The squad panel selection never changes.

**Fix (host).**
- `SquadSwitchPoll` runs every frame on the game thread (from the order-handler probe). It reads `VK_UP` / `VK_DOWN` with `GetAsyncKeyState`, only when this copy has focus.
- It steps through the squad panel members (Down = next, Up = previous, wrapping round). It skips:
  - the current soldier
  - soldiers owned by another machine (`+0x4A ≠ 0`)
  - soldiers without `+0xD7C & 2`
  - members the panel's selectable check (`0x4f6d50`) refuses
- It then calls the game's own `0x422f60(ctl, soldier)` and updates the panel selection.
- The body left behind becomes a normal AI squadmate.

**Fix (client): hand the body over between machines.**
1. The client sends `OP_SWITCH` (its current soldier id + direction) over the relay.
2. The host picks the next free AI squadmate (owner 0, not the host's own soldier, switchable and selectable). It then:
   - **New body** → owner = client slot, mirrored on the host (`F1C = 3`, `0x574a14(netId, 1, 0x473410, 1)`, `0x574a74(netId, 1)`).
   - **Client's old body** → owner = 0, local on the host (`F1C = 1`, `0x574a14(netId, 0, NULL, 1)`), so the host's AI drives it.
   - It replies `OP_SWITCH_OK` (or `OP_SWITCH_NO`).
3. The client does the same swap on its side (new body local, old body mirrored) and calls `0x422f60(ctl, newBody)`.

**Bug found on the way.** The first version changed only the owner byte and `F1C`. Then nobody could move: the bodies stayed registered with the wrong network sync mode, so the host and client kept overwriting each other's movement. Re-registering with the game's own local and mirrored modes fixed it.

### 4.8 Orders and highlight left out the other human player

**Problem.** After a character switch, the order menu highlighted the whole squad, the other human player included.

**Finding.** The game decides which squad members are human players from the slot → id table `0x6f4314` (`int[4]`, read by `0x46b090`). A switch did not update it, so the game kept treating each player's *original* body as the human one.

**Fix.**
- `SetSlotBody(slot, id)` rewrites the table on every switch:
  - host switch → slot 0
  - client switch → the client's slot, on the host and on the client
- The host also sends an `OP_HUMANS` packet about three times a second: each human's body id, with its slot in the top byte. The client applies it to the table, so it learns about the host's switches.
- Clients send `OP_HELLO` once a second so the host knows where to send that list.
- Extra layers added on the way:
  - **Squad-order shield:** human bodies get `+0xF9A` bit 4 during the order handler (call at `0x50a8fa`) and during relayed orders, so squad-wide orders skip them.
  - **Squad panel hooks:** vtable `0x6cc998` entries [30] (`0x503720`) and [31] (`0x4f6d50`) are hooked. The logs show they are not the path the game uses for highlighting.
- **Still open (accepted by the user):** the other human can still be chosen as a single order target. The logs showed that pick never goes through the hooked functions.

### 4.9 Online play over Steam (virtual LAN tunnel, `tunnel.c`)

**Problem.** The game only knows LAN play: UDP broadcast discovery on 10010, TCP 4658 to join, UDP 4658 for game state. Internet play would need port forwarding and typed IP addresses. GameSpy is long gone.

**Idea.** Make remote players look like LAN machines. Each machine (a "peer") gets a virtual loopback address, and everything the game sends to a peer's address, or as a broadcast, is carried over Steam and re-sent on the other side from a local socket bound to the sender's virtual address. The game sees normal LAN packets and TCP connections. No game code is changed for this; it is all winsock IAT hooks.

**Virtual addresses**
- Format: `127.NS.HI.LO`.
  - `NS` is the namespace, 20 for Steam. In test mode it is `20 + instance`, so the two copies on one PC see different namespaces.
  - `HI.LO` is a 16-bit peer key: `0x0100 + ((uint32)steamId * 2654435761 >> 16) % 0xFE00`. A clash with your own key is logged as a warning.
- `Route(ip)`:
  - `255.255.255.255` or `x.x.x.255` → broadcast to all peers.
  - `127.20-29.x.x` → that peer, unless it is your own key.
  - Other `127.*` and `0.0.0.0` → normal local traffic.
  - Any other real IP (for example the host advertising its LAN address inside a packet) → the peer we last got game traffic from, or the only peer if there is one.

**Winsock hooks** (IAT of `WSOCK32.dll`)

| Hook | What it does |
|---|---|
| `bind` | Rebinds every game socket to our own virtual address. |
| `sendto` | Sends routed packets as `M_UDP {src port, dst port, payload}` to the peer (or all peers, for a broadcast); normal local traffic passes through. Exported as `Tun_Sendto` so the relay uses it too. |
| `connect` | For a peer: makes sure a local **listener** exists on that peer's virtual address and port, then connects the game to it. Port 4658 also records the host address for the relay (`g_hostIp`). Unbound sockets are bound to our virtual address first. |
| `gethostbyname` | For our own name / `localhost` / empty: returns our virtual address only. |

**Tunnel messages** (first byte = type)

| Type | Name | Content | Use |
|---|---|---|---|
| 1 | `M_UDP` | `src`, `dst` ports + payload | Unreliable. Re-sent by a per-peer UDP proxy socket bound to the sender's virtual address, to our virtual address on `dst`. |
| 2 | `M_OPEN` | stream id, `dst` port | Reliable. Our listener accepted the game's TCP connection, so the other side opens a TCP connection from the sender's virtual address to its own game. |
| 3 | `M_DATA` | stream id, `fromOpener` + bytes | Reliable. TCP stream data, up to 16000 bytes per message. |
| 4 | `M_CLOSE` | stream id | Reliable. Close the stream. |

Limits: 8 peers, 64 UDP proxies, 16 listeners, 64 TCP streams. TCP sockets get `TCP_NODELAY`.

**Pump thread**
- One background thread runs:
  - `SteamPump` (callbacks and incoming messages)
  - a `select()` over every proxy, listener and stream socket, with a 2 ms timeout for Steam and 5 ms for the test transport
  - the window-title update
  - F7
  - menu invite requests (`Tun_Invite` sets a flag; the thread opens the overlay)
- Everything shared is guarded by one critical section.

**Steam transport** (all through the **flat API**, resolved with `GetProcAddress` at runtime, so there is no link-time Steam dependency)
- Setup:
  - App ID **480** (Spacewar): writes `steam_appid.txt` if missing and sets the environment variables `SteamAppId` and `SteamGameId` to 480.
  - `SteamAPI_InitFlat`, then `SteamAPI_ManualDispatch_Init` (it must come after init).
  - Interfaces used: `SteamUser_v023`, `SteamFriends_v018`, `SteamMatchmaking_v009`, `SteamNetworkingMessages_SteamAPI_v002`, `SteamNetworkingUtils_SteamAPI_v004`.
  - `InitRelayNetworkAccess` is called at startup.
- **Lobby:**
  - `CreateLobby(friends-only, 4)`. On `LobbyCreated_t` (513) the mod sets lobby data `game = cgt-squadmod`.
  - Rich presence `status` = "Conflict: Global Terror co-op (SquadMod)".
  - `LobbyEnter_t` (504): we are in the lobby; refresh the members.
  - `LobbyChatUpdate_t` (506): someone joined or left; refresh the members.
  - `GameLobbyJoinRequested_t` (333): invite accepted or *Join game* clicked. Leave the current lobby, drop all peers, join the friend's lobby.
- **Peers = lobby members.** Joining creates a peer with a virtual address; leaving closes that peer's sockets.
- **Messages:**
  - `SteamNetworkingMessages`, channel 0.
  - Flags: reliable = `8 | 1` (reliable, no Nagle); unreliable = `0`; always `| 32` (auto-restart a broken session).
  - `SessionRequest` (1251) is accepted only from lobby members. `SessionFailed` (1252) is logged.
  - The `SteamNetworkingIdentity` passed to Steam is `{type 16 (SteamID), size 8, steamId}`, packed.
- **Manual dispatch:** `SteamAPICallCompleted_t` (703) results are fetched with `GetAPICallResult` and handled like normal callbacks.
- **Window title:** `Conflict: Global Terror - Steam lobby (N players) - F7 invite friends`, or `connecting`.
- **Failure:** if init fails (Steam not running, DLL missing), the log says why and the tunnel turns off. The game is then plain LAN.

**Disconnects and reconnecting** (first two-PC session, 2026-11-18)
- **What happened:** the friend dropped out of the Steam lobby mid-mission. On the other PC, `SteamRefreshMembers` immediately ran `DropPeerSockets`, which closed the game's TCP stream, so the match ended at once.
- **Bug: "LOBBY (0)":** the dropped side kept `S.lobby` set while Steam no longer counted it as a member. `GetNumLobbyMembers` then returned 0 or a list without us, and nothing ever rejoined.
- **Fix, in `tunnel.c`:**
  - **Self-check:** `SteamTick` runs once a second inside the pump. It refreshes the member list, and if we aren't in it, `LobbyLost()` runs. `LobbyKicked_t` (512) also calls `LobbyLost()`.
  - **`LobbyLost()`:** remembers the lobby as `g_lastLobby`, clears `S.lobby`, and marks every peer gone without dropping it.
  - **Rejoin:** `JoinLobby(g_lastLobby)` every 5 s, up to 6 tries. A `LobbyEnter_t` response of 2 (doesn't exist) skips straight to `CreateLobby`. A failed `LobbyCreated_t` retries after 10 s.
  - **Grace period:** a member who leaves the lobby gets `Peer.goneAt` instead of being dropped. Their virtual address, proxies and streams stay alive for `PEER_GRACE_MS` (90 s). If they reappear in the member list, that's logged as `is back`; if not, `did not come back` and the sockets close. `PeerCount()` ignores gone peers. The virtual address comes from the Steam id, so a returning peer gets the same one.
  - **Status:** `Tun_SteamStatus()` returns 2 while reconnecting. The STEAM screen shows `LOBBY: CONNECTION LOST - REJOINING...`, and the MULTIPLAYER notice shows `STEAM LOBBY LOST - REJOINING...`.
  - **Test key:** with `Debug=1`, **Ctrl+Shift+F8** calls `LeaveLobby` behind the mod's back, which simulates a drop.
- **Verified on one PC:** the drop is detected within 1 s, the rejoin gets response 2 (alone, so the lobby is gone), and a new lobby is ready about 1.5 s after the drop. The STEAM screen then shows `LOBBY (1)`. The grace path and rejoining a lobby a friend is still in need the two-PC test.
- **Joining a mission in progress:** the game doesn't support it. The server's session flow is `SessionLoadLevel` → `ReadyToPlay` → `StartPlay` (strings `CNetServer::OnPkt_*`). There is no path to bring a client into a running level, and adding one would need world-state sync. Not attempted.

**Test transport** (`LoopbackTest=1` + `TunnelTest=1`)
- Copy N sends to UDP `127.0.0.1:4700+other`, with header `{magic 'CGTT', instance}`.
- The peer is fixed (the other copy, key = its instance number).
- It exercises exactly the same proxy/stream code as Steam, so the tunnel could be debugged on one PC without a second Steam account.

### 4.10 Steam name as the player name

**Finding.** `0x58f300` (session, `thiscall`) copies the local name `session+0x5018` into this machine's slot record (`session + slot*0x8C`). It sets `session+0x230` while copying and then sets the dirty flag `session+0x234`, so the name is sent to the other players. The name itself comes from the save profile (`0x58f2e0`).

**Fix.**
- With `Steam=1`, `0x58f300` is replaced by a JMP (expected bytes `80 3D C4 17 70 00 FD` = `cmp byte [0x7017c4], 0xFD`) with `Hook_ApplyLocalName`.
- It does the same work, but first overwrites `session+0x5018` with the Steam persona name when `SteamName=1`.
- The name is filtered to printable ASCII and cut to 15 characters, because the game font has nothing else.
- `SteamName=0`, set from OPTIONS > STEAM, keeps the profile name.

### 4.11 Display: resolution, windowed / borderless, saved settings

**Problems.**
1. The PC release always starts at 800×600 and forgets the resolution chosen in VIDEO.
2. The WINDOWED row exists in the VIDEO screen but the PC build hides it.
3. The resolution list is filtered down to a few old modes.
4. Real exclusive fullscreen through dgVoodoo minimises on alt-tab.

**How it works now.**
- **Direct3D always runs windowed.**
  - The `CreateDevice` and `Reset` hooks (`IDirect3D9` vtable slot 16, `IDirect3DDevice9` slot 16) set `Windowed=1` (pp `+0x20`) and `FullScreen_RefreshRateInHz=0` (pp `+0x30`).
  - The back buffer size the game asked for (pp `+0x00`/`+0x04`) is kept and recorded as `g_curW/H`.
  - `ApplyWindow` then shapes the window:
    - **Windowed off** → `WS_POPUP` covering the monitor (borderless fullscreen; dgVoodoo scales the back buffer).
    - **Windowed on** → `WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX`, client area = resolution, centred.
  - In loopback test mode the windows are always framed, copy 1 at the left and copy 2 at the right.
- **Resolution list.**
  - `0x417cf0(w, h)` (originally a thunk `jmp [0x117ed8c]` into the protected section) decides which modes VIDEO lists. It is replaced with `ModeAllowed`:
    - 800×600, 1024×768, 1280×720, 1280×1024, 1366×768, 1600×900, 1920×1080
    - plus the desktop size
    - nothing larger than the desktop
  - The list holds at most 8 entries.
- **WINDOWED row.** The PC build hides `TOG_WINDOWED` each time the screen is built or refreshed:
  - `0x51f090` is called from event `0x1e` at `0x520e73`.
  - `0x520200` (refresh) is called from `0x520699`, `0x5206d8`, `0x52079e`, `0x520e6c`, `0x520eae` and `0x513591`.

  The mod wraps these calls and sets `0x8000` again, sets min/max to 0/1 and the value from `g_dispWindowed`. Every frame (`Present` hook) `FixVideoScreen` keeps it visible and spaces the rows below RESOLUTION and COLOUR DEPTH evenly, because the hidden row overlapped the next slot.
- **APPLY** (call at `0x520e96` to `0x520cc0`, protected code):
  - The wrapper `W_VideoApply` records the wanted window mode and runs the game's APPLY (which calls D3D `Reset`).
  - If only the window mode changed (no `Reset` happened), it reshapes the window itself.
  - It then writes `[Display] Windowed/Width/Height` to `squadmod.ini`.
- **Restore at startup** (`RestoreDisplay`, once, right after a `Present`):
  1. Wait until the VIDEO screen exists **and** 6 s have passed **and** the message box `*(0x72fc70)` is built (see the crash below). Give up after 60 s more.
  2. Call `0x520200` to build the resolution list, find the saved `"WxH"` in `0x739620 + 48*i`, select it (`TOG_RESOLUTION +0x74`) and call the game's APPLY `0x520cc0`.
  3. APPLY opens the "keep these settings?" countdown on layer 4 (`*(0x72fb4c)`), which would revert on timeout. The mod closes it the way the game's own timeout path does (`0x599690(mgr, 4)`), without reverting: the settings are already known to be good.
- **OPTIONS > VIDEO shows the size in use.**
  - The game selects the RESOLUTION entry from its own settings, which say 800×600 after every restart. Pressing APPLY there without looking would drop back to 800×600.
  - The call at `0x513591` (OPTIONS → VIDEO) is wrapped by `W_VideoEnter`. After the normal refresh it selects the list entry that matches `g_curW × g_curH`.

**Bug: random silent exit at startup (fixed).**
- Symptom: sometimes the game closed during the startup restore. There was no error and no WER event, and the log just stopped.
- Diagnosis: with `Debug=1` the mod logged `diag: exception C0000005 at 0050FEF2 (ecx FAFAFAFA)`, then `ExitProcess` called from `0x6a88ed`.
- Cause: APPLY ends up in `0x50fec0`, which uses the message box's widget pointers `+0x74..+0x8c` without checks. If APPLY runs before the front end has built that screen, they still hold `0xFAFAFAFA`.
- What did not work:
  - Checking `*(0x72fb4c)`: that is a different object.
  - Checking only that `*(0x72fc70)` is non-zero: the object exists before its widgets do.
- Fix: wait until all seven pointers are `>= 0x10000`, not `0xFAFAFAFA`, readable, and point at a game vtable (`0x600000..0x6fffff`). Several restarts in a row now restore cleanly.

### 4.12 In-game STEAM and GRAPHICS screens

**Goal.** Settings screens that look and behave exactly like the game's own (fonts, frames, arrows, focus highlight, keyboard and mouse), instead of a separate overlay.

**Approach: reuse what the PC build already contains but never shows.**
- OPTIONS has two hidden buttons, REFRESH RATE and CENTRE SCREEN. They are shown (`+0x20 |= 0x108000`) and relabelled **STEAM** and **GRAPHICS**.
- Two console-only screens exist in the front end and are never reached on PC:
  - ONLINE OPTIONS (3 buttons) becomes **STEAM**.
  - NETWORK OPTIONS (3 toggles + APPLY) becomes **GRAPHICS**.
- Text: each row's caption is a text widget holding a string hash. The mod sets it to its own hash `0x5D5D00nn` and answers that hash in the hooked lookup `0x686a10` from a `wchar_t` table it fills (`SetTxt`). Changing text at runtime is just `SetTxt`, because the game looks the string up on every draw. Titles are found by scanning the title widget for the old hash `0x7e74ba26`.
- Setup (`MenusSetup`) runs from the `Present` hook and retries once a second until the three screens exist. It checks each screen's id (`+0x14`) before touching it.

**Hooks**

| Where | Replaced by | Purpose |
|---|---|---|
| `0x686a10` (JMP + trampoline) | `Hook_Loc` | Serve the mod's strings. |
| OPTIONS `0x6cd730[7]` | `W_OptionsEvent` | Event 4 on STEAM / GRAPHICS → `0x599e20(ui, id, 4)`. |
| ONLINE OPTIONS `0x6ce4f0[7]` | `W_SteamEvent` | Select INVITE / NAME / LOBBY. Back (5) → OPTIONS. Entered (`0x15`) → refresh texts and **return 0 without calling the original** (the original runs GameSpy code). |
| ONLINE OPTIONS `0x6ce4f0[13]` | `W_SteamUpdate` | Base update `0x59e010` + refresh the texts every 500 ms (lobby list). |
| NETWORK OPTIONS `0x6ce558[7]` | `W_GfxEvent` | Select (4) on APPLY → save. Back → OPTIONS. Entered (`0x15`) → original, then load `dgVoodoo.conf` into the toggles. |
| Net toggle `0x6cd620[7]` | `W_NetTogEvent` | Returns 0 for the three graphics toggles (keeps the voice-chat code away from them); others go to the original. |
| Net toggle `0x6cd620[13]` | `W_NetTogUpdate` | For the graphics toggles only the base `0x5a5e70` runs. This skips `0x510240`, which every frame reset the value text (shown as `*no string*`) and touched GameSpy status. |

**Every frame** (`MenuFrame`, from `Present`):
- Keep the OPTIONS rows and the three STEAM rows visible.
- Hide the GameSpy icon `0xfeab8365`.
- Place the three graphics toggles and APPLY at `y0 + rowHeight*i`, measured from the first toggle the game placed, so it scales with resolution.
- Refresh the value texts and min/max (`GfxText`). The game resets value text hashes on entry, so they are re-set every frame.

**STEAM screen logic**
- INVITE calls `Tun_Invite()` (only when in a lobby). The pump thread then calls `ActivateGameOverlayInviteDialog`.
- NAME toggles `g_useSteamName` and writes `SteamName` to `squadmod.ini`.
- The LOBBY line comes from `Tun_LobbyNames`: names filtered to ASCII and taken under the tunnel lock. With Steam off, the rows say so.

**GRAPHICS screen logic** (`dgVoodoo.conf` in the game folder)
- A small INI scanner finds `key = value` inside `[section]` (case-insensitive, stops at spaces / `;` / CR).
  - This matters because `Antialiasing` exists in both `[Glide]` and `[DirectX]`. Only `[DirectX]` is used.
- **Load on entry:** each toggle is set to the matching value index (default index 0).
- **APPLY:**
  1. `CopyFileA(conf, "dgVoodoo.conf.squadmod.bak", failIfExists=TRUE)` keeps the very first original.
  2. Only the value text of the three keys is replaced; every other byte of the file stays the same.
  3. Each change is logged as `menus: dgVoodoo [section] key = value`.
- dgVoodoo reads its config only at process start, so the screen says "restart".

**Bugs found on the way**
- **Reordering OPTIONS.** Moving STEAM/GRAPHICS to the bottom by swapping child-array entries made the game quit later, when VIDEO applied settings. Forcing their y position instead left a gap, because navigation still followed the array. Final choice: keep the natural order (GAMMA, STEAM, GRAPHICS, VIDEO, AUDIO).
- **`*no string*` on ANTI-ALIASING:** the net toggle's per-frame update `0x510240` rewrote the value hash. Fixed by the `vtbl[13]` hook above.
- **APPLY did nothing:** event 4 on this screen arrives with the **screen** as the widget argument, not the button. The handler now also accepts it when the group's focused child (`APPLY->+0x0c` group, `+0x50`) is APPLY.

**Verified in game (one PC, keyboard):**
- OPTIONS shows the two rows with no gap.
- STEAM:
  - The title, invite overlay, name toggle (logged and saved) and lobby line (`LOBBY (1): <name>`) all work.
  - ESC returns to OPTIONS with STEAM focused.
- GRAPHICS:
  - Values are read from the file and left/right changes them.
  - APPLY wrote `Antialiasing = 4x` and `Filtering = trilinear` (only the `[DirectX]` lines changed), and the backup was created.
  - Setting the values back produced a `dgVoodoo.conf` byte-identical to the original.
- VIDEO opens showing 1920×1080.

### 4.13 MAIN MENU / MULTIPLAYER clean-up (GameSpy entries)

**Screens and widgets** (found by dumping the widget tree)

| Screen | Id | Children |
|---|---|---|
| Main menu | `0xf23ea323` (vtable `0x6ce710`) | BN_TRAINING_GAME `0x5634a532`, BN_SINGLE_PLAYER `0x77cf06b9`, BN_MULTIPLAYER `0x257ec6dc`, BTN_EDIT_PROFILE `0xa433ad3f`, BN_OPTIONS `0x46c6c7d9`, BN_CHEATS `0xc075eeaa` (hidden), **BN_SIGNIN_OUT `0x053949b1`** ("GAMESPY ID LOGIN"), BN_QUIT_TO_WINDOWS `0x98c86206`, BN_QUIT_TO_DEMOS_MENU `0xd0274c70` (hidden). |
| SCR_MULTIPLAYER_MENU | `0x50dc7ff1` (vtable `0x6ce6a8`) | BN_SPLIT_SCREEN `0x01b4f3dc` (hidden on PC), BN_LAN `0x4abc37d8`, **BN_ONLINE `0xb6478360`** ("INTERNET"), title `0x08736a86` (hash `0x08284d6c` M_PLAYER), notice TXT_WELCOME3 `0x37077291` (hash `0x8b3807d0` ESRB_NOTICE). |
| SCR_MULTIPLAYER_GAME | `0x26317ee8` (vtable `0x6cec78`) | Shared by LAN and INTERNET. BN_MPGAME_QUICK_MATCH / OPTIMATCH (hidden), BN_SEARCH_FOR_GAME `0xc53d4e7c`, BN_MPGAME_CREATE_GAME `0xdc68e241`, title TXT_MULTIPLAYER_GAME_TITLE `0x08b4d15a`. |
| Session settings (CREATE MATCH) | `0xe7819b23` | |

The LAN button text **and** the LAN screen's title both use the string hash `0x0ec98d0f` (PLAY_LAN).

**Changes**
- **Rename.** `Hook_Loc` returns the mod's text for two hashes whenever the tunnel is active (`Tun_SteamStatus() >= 0`):
  - `0x0ec98d0f` (PLAY_LAN) → "STEAM CO-OP"
  - `0x8b3807d0` (ESRB_NOTICE) → "STEAM LOBBY (N): names - INVITE FRIENDS WITH F7 OR OPTIONS > STEAM", refreshed every 500 ms in `MenuFrame`

  Hooking the string, rather than individual widgets, renames every place it is used. No widget code is touched, so nothing can reset it.
- **Hide.** `MenuFrame` clears visible flag `0x8000` on BN_ONLINE and BN_SIGNIN_OUT every frame. A hidden row is skipped by both layout and keyboard navigation, so the remaining rows close up with no gap. No child array is reordered (see 4.12).

**Verified:**
- The main menu has 6 rows and wraps correctly; EXIT is reachable.
- MULTIPLAYER shows only STEAM CO-OP, and Down does nothing.
- STEAM CO-OP opens SCR_MULTIPLAYER_GAME, titled STEAM CO-OP.
- CREATE MATCH opens the session settings.
- **Left as is:** the session settings help text still says "Create a LAN game."

### 4.14 Lessons and gotchas

- **Prefer the game's own code paths.** Calling APPLY, the screen switch, the character switch (`0x422f60`) and the network registration functions with the game's own arguments has always been more robust than recreating their effects field by field.
- **The game swallows crashes.** An access violation becomes `ExitProcess(0xC0000005)` with no dialog. Turn on `Debug=1` (VEH + `ExitProcess` hook) before guessing.
- **Objects exist before their contents.** Screens and dialogs are allocated early and filled later, and the heap fills unbuilt pointers with `0xFAFAFAFA`. Validate by vtable range, not by non-zero.
- **Never reorder UI child arrays.** Other code walks them by index.
- **Per-frame game code can undo your changes.** Hidden rows are re-hidden on refresh and value texts are reset on entry or every frame. Either hook the code that undoes them or re-apply from `Present`.
- **Event arguments vary by screen.** Don't assume the widget argument is the pressed button; check the group's focused child.
- **The front end can start in a different state.** After a forced kill the game may show its launcher dialog or *CHOOSE A PROFILE*. Automated tests should check the current screen id (`scr.py`) before sending keys.
- **Mouse in menus is relative.** Synthetic clicks land in the wrong place; drive tests with the keyboard.

---

## 5. Diagnostics (kept, off by default)

- `Debug=1` turns on:
  - The order-handler probe log (squad / controller state changes).
  - The network send probe (hook on `0x46d3a0`) and network state dumps.
  - The inventory watcher (each squad member's 40-slot inventory with item names).
- Always logged:
  - Spawn per slot (id, entity, owner, PLAYER/AI, local or mirrored).
  - Every relayed order and its execution on the host.
  - Backpack keeps (`mission kit: keeping items of AI squadmate character N`).
  - Every character switch (`switch: …`).
- `Debug=1` also logs `HUMDBG` squad panel state (me / human / AI per member).
- **Crash / exit logging** (`Debug=1`). This is what found the startup crash in 4.11.
  - A vectored exception handler logs the first 20 `C0000005` / `C000001D` / `C0000096` / `C00000FD` / `C0000409` exceptions as `diag: exception <code> at <EIP> (ecx … esi …)`. It only logs and always continues the search, so the game's own handling is unchanged.
  - An IAT hook on `KERNEL32!ExitProcess` logs `diag: ExitProcess(code) from <caller>, stack …` before exiting.
  - Together they show crashes the game hides (see 3.8, exception handling).
- `Debug=1` also hooks `IDirect3D9::GetAdapterModeCount` / `EnumAdapterModes` (slots 6 / 7) and logs every display mode the game sees.
- Always logged (display, Steam, menus):
  - `CreateDevice` / `Reset`: `game asks WxH fullscreen|windowed`, the result, and `display: WxH window|borderless fullscreen`.
  - `display settings saved: …` and `display restore: applying WxH` / `confirmation dialog closed` / `settings dialog never built, skipped` / `… not in the mode list`.
  - `steam: …`: login and virtual address, lobby created / entered, members joining and leaving with their virtual addresses, accepted connections, send failures (at most every 2 s), invite dialog (`(menu)` when opened from the STEAM screen).
  - `tunnel: …` and the first `sendto` / every `connect` for each address: where game traffic is routed.
  - `menus: hooks installed`, `menus: STEAM and GRAPHICS screens ready (…)`, `menus: player name = Steam|profile`, `menus: dgVoodoo [section] key = value`.
  - Only on failure: `menus: MULTIPLAYER > INTERNET not found, left as is` / `menus: main menu GAMESPY ID LOGIN not found, left as is`.
- **Still in the build:** `OrderProbe` writes `SWITCHDBG handler=… squad=… vtbl=…` once per session. It was a temporary probe used to find the squad panel and can be removed.

### Test tooling used during development (not shipped)

These Python helpers lived in a scratch folder, not in the repo:
- Read the game's memory: dump a screen's widget tree; `scr.py` prints the current layer-4 screen id from `*(0x736b20) → +0x28 → +0x240 → +0x14`.
- Send keys to the game window found by the `ConflictGlobal.exe` PID (it refuses to send anywhere else), and take screenshots.
- A typical automated menu test from the main menu (focus on TRAINING):
  1. Down ×4, Enter → OPTIONS (`0x215067a4`).
  2. Down ×2, Enter → GRAPHICS (`0x80862421`).
  3. Left / Right on the rows, Down to APPLY, Enter.
  4. Grep the log and `dgVoodoo.conf`.

  Check the screen id after every Enter.

---

## 6. Known limits and next steps

- **Other missions (2–15)** have not been tested. Many call `engine[0xdb0]` in objective logic. None of them delete objects through it, but other "player-only" logic may treat the AI or switched bodies unexpectedly.
- **Slot table after switching:** the mod now keeps the slot → id table (`0x6f4314`) up to date on every switch (4.8). Game code that cached a body earlier (scores, respawn, mission checks through `0xdb0`) has not been checked.
- **Real two-PC LAN:** set `LoopbackTest=0` on both PCs. The relay then binds UDP 4659 on all interfaces and learns the host from the TCP 4658 connect. Firewalls must allow UDP 4659.
- **Steam between two PCs** is the next big test. On one PC, the following have been verified:
  - login, lobby creation and the invite overlay
  - the whole tunnel code path, through `TunnelTest`
  - Things to watch in the log on a real test:
    - `steam: <friend> joined, virtual address …`
    - `steam: accepted connection from …`
    - `connect … -> … (<friend>)` on port 4658
    - no `send … failed`
- **Steam limits:** at most 4 lobby members and 8 peers. Peer keys come from a hash of the Steam id, so two members could get the same virtual address. This is very unlikely, and is logged as `virtual address clash`.
- **Player name** is sent when the game copies it into the slot record (`0x58f300`). Changing it in OPTIONS > STEAM in the middle of a session shows up the next time the game does that.
- **GRAPHICS** needs a restart (dgVoodoo reads its config once). Values not in the mod's lists show as the first entry until changed. If `dgVoodoo.conf` lacks a key, that key is skipped and logged.
- **"Create a LAN game."** The help text on the session settings screen still says LAN. Its string hash has not been looked up yet; once found, it is one more case in `Hook_Loc`.
- **OPTIONS order** is GAMMA, STEAM, GRAPHICS, VIDEO, AUDIO. Putting the new rows last would need a way to change the group's layout/navigation order that is safe for the VIDEO apply broadcast. That way has not been found yet.
- **Resolution list** holds at most 8 entries (game tables at `0x739600` / `0x739620`). Monitors larger than 1920×1080 get their desktop size as the 8th entry.
- **Display handling needs `FreeMouse=1`** (the D3D hooks are installed in that branch).
- **3–4 human players:** relay and switching are written for any client slot but have only been tested with one client.
- **Packet loss:** the relay is plain UDP without retries. On LAN this has been fine. A lost switch request just means pressing the key again.
- **Game's own switch keys:** F1–F4 / NEXT_GRUNT are not wired up in multiplayer. The mod uses Up/Down instead, so it doesn't touch the game's key bindings.
