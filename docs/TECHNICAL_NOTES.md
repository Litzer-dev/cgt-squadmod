# SquadMod – AI squadmates for Conflict: Global Terror co-op

This mod gives online/LAN co-op in **Conflict: Global Terror** (PC, 2005) the same four-man squad as the campaign. Any squad slot that no human uses is filled by an AI squadmate. The AI follows orders from every player, gets its mission kit, can be revived, and any player can switch into it, like in single player.

These notes cover what was found by reverse-engineering `ConflictGlobal.exe` and the mission DLLs, what the mod changes, and why.

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
| Missions 2–15 | ⚠️ not tested |
| Real two-PC LAN (`LoopbackTest=0`) | ⚠️ not tested |
| 3–4 human players | ⚠️ not tested |

All testing so far: two copies of the game on one PC, LAN co-op, mission 1.

---

## 2. Files

| File | Purpose |
|---|---|
| `src/squadmod.c` | The whole mod (single C file). |
| `src/dinput8.def` | Export forwards for the proxy DLL. |
| `release/dinput8.dll` | Build output; goes in the game folder. |
| `release/squadmod.ini` | Settings; goes in the game folder. |
| `squadmod_1.log`, `squadmod_2.log` | Written to the game folder (one per running copy). |

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
LoopbackTest=1   ; two copies on ONE PC (instance 1 = 127.0.0.1, instance 2 = 127.0.0.2); 0 on real PCs
FreeMouse=1      ; don't lock the mouse to the window (DirectInput non-exclusive)
Debug=0          ; extra diagnostics (order handler, net messages, inventories)
```

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
- Game traffic:
  - TCP port **4658** (client connects to the host).
  - UDP traffic for game state.
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
  - The game asks for exclusive fullscreen 800×600, and dgVoodoo minimises it when it loses focus. The mod forces `Windowed=1` in `CreateDevice` / `Reset`.
  - It swallows `WM_ACTIVATE` / `WM_ACTIVATEAPP` deactivation.
  - It sets DirectInput to non-exclusive.
  - It clips the cursor only when the game is in the foreground.
  - `dgVoodoo.conf` settings: `FullScreenMode=false`, `AppControlledScreenMode=false`, `CaptureMouse=false`, 1024×768 (backup `dgVoodoo.conf.bak`).

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
- **Still in the build:** `OrderProbe` writes `SWITCHDBG handler=… squad=… vtbl=…` once per session. It was a temporary probe used to find the squad panel and can be removed.

---

## 6. Known limits and next steps

- **Other missions (2–15)** have not been tested. Many call `engine[0xdb0]` in objective logic. None of them delete objects through it, but other "player-only" logic may treat the AI or switched bodies unexpectedly.
- **Slot table after switching:** the slot → id table (`0x6f4314`) still points at each human's *original* character after a switch. The mod no longer depends on it at runtime, but game code that does (scores, respawn, mission checks using `0xdb0`) may see stale data.
- **Real two-PC LAN:** set `LoopbackTest=0` on both PCs. The relay then binds UDP 4659 on all interfaces and learns the host from the TCP 4658 connect. Firewalls must allow UDP 4659.
- **3–4 human players:** relay and switching are written for any client slot but have only been tested with one client.
- **Packet loss:** the relay is plain UDP without retries. On LAN this has been fine. A lost switch request just means pressing the key again.
- **Game's own switch keys:** F1–F4 / NEXT_GRUNT are not wired up in multiplayer. The mod uses Up/Down instead, so it doesn't touch the game's key bindings.
