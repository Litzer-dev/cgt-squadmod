SquadMod for Conflict: Global Terror
====================================

Co-op with a full 4-man squad: every empty squad slot gets an AI squadmate
that follows everyone's orders. Plus online play over Steam (no port
forwarding, no IP addresses), proper windowed / fullscreen display, and
new STEAM and GRAPHICS screens in the game's options.


INSTALL
-------
1. Copy EVERYTHING in this folder (except this README) into your game
   folder, next to ConflictGlobal.exe. Say "Replace" if Windows asks.
2. Start Steam, then start the game.

Both players need the mod installed. Nothing in the game itself is changed;
to uninstall, delete dinput8.dll.

The game must be the same version as the one the mod was made for:
ConflictGlobal.exe, 14,475,401 bytes. If yours is different, the mod
writes "unexpected bytes" / "wrong game version" lines in squadmod_1.log
(in the game folder) - tell me and send that file.


PLAYING TOGETHER (over Steam)
-----------------------------
1. Both: start Steam first, then the game.
2. The host invites the friend:
     OPTIONS > STEAM > INVITE FRIENDS   (or press F7 in game)
   If the Steam overlay doesn't show up: in the Steam friends list,
   right-click the friend > Invite to game.
3. The friend accepts the invite (or right-clicks the host in the Steam
   friends list > Join game). The game must already be running.
   OPTIONS > STEAM shows everyone in the lobby, e.g. "LOBBY (2): ..."
4. Host:   MULTIPLAYER > STEAM CO-OP > CREATE MATCH
   Friend: MULTIPLAYER > STEAM CO-OP > SEARCH FOR MATCH, then join.

Steam will show you as playing "Spacewar". That is normal: the game isn't
sold on Steam, so the mod uses Valve's free test app to get Steam
networking.


IN GAME
-------
- Squad orders work for every player (follow, hold, fire at will,
  go to, hit the dirt, shield, interact).
- Up / Down arrow: switch to the previous / next AI squadmate.
- AI squadmates can be downed and revived.


DISPLAY AND GRAPHICS
--------------------
- OPTIONS > VIDEO: pick your RESOLUTION and WINDOWED (NO = borderless
  fullscreen, YES = normal window), then APPLY. The game remembers it and
  switches to it by itself a few seconds after the menu appears next time
  (the game itself always starts at 800x600).
- OPTIONS > GRAPHICS: anti-aliasing, texture filtering, frame rate limit.
  Press APPLY, then restart the game.
  If the picture looks wrong or the game is slow, set ANTI-ALIASING to OFF
  or 2X there.
- OPTIONS > STEAM > PLAYER NAME: use your Steam name or your profile name.


IF SOMETHING GOES WRONG
-----------------------
- The game writes squadmod_1.log in the game folder. Send it to me.
- Not connecting? Check Steam is running before the game starts. Both of
  you should be in the same lobby (OPTIONS > STEAM).
- If Steam drops you from the lobby, the mod rejoins it by itself
  (OPTIONS > STEAM shows "CONNECTION LOST - REJOINING..."). If the lobby
  is gone, it makes a new one; then invite your friend again.
- You can't join a match that is already running. After a disconnect, the
  host creates a new match and the other player joins it again.
- Without Steam you can still play on a real LAN: open squadmod.ini and
  set Steam=0. The host's firewall must allow UDP 10010, TCP and UDP
  4658, and UDP 4659.


FILES
-----
dinput8.dll      the mod
squadmod.ini     mod settings
steam_api.dll    Valve's Steamworks library (for Steam play)
D3D9.dll         dgVoodoo 2.82 (graphics wrapper that makes the old game run well)
dgVoodoo.conf    dgVoodoo settings, set up for the mod (windowed mode)
dgVoodooCpl.exe  dgVoodoo control panel (optional)
