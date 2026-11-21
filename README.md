# Species Unknown – 5 Player Mod

Raises the co-op player limit from 4 to 5 and adds a 5th seat to the shuttle, **in private lobbies only**.

**Private lobbies only.** Only a lobby you host as **Private** (joined with its code) gets 5 slots, the
5th shuttle seat and the 5th Crew Members row. Public lobbies and the public server browser behave exactly like
the unmodded game: 4 slots, 4 seats, full at 4/4. So modded and unmodded players can still play public games
together.

> [!WARNING]
> **A game update will break this version of the mod.** It patches the game's code byte-for-byte and only
> works with the exact game version below. After an update the mod skips anything that no longer matches
> (marked `FAILED` in `FivePlayerMod.log`), so the game itself keeps running, but the 5-player features stop
> working until the mod is updated. Everyone in the group must also be on the same game version.

## Requirements

| | |
|---|---|
| Game | **Species: Unknown v0.26.8** (Steam app 2747330, **build ID 26617965**) |
| Engine | Unreal Engine 5.7 (CL 49658320) |
| Platform | Windows 64-bit, Steam version |
| Players | Every player installs the mod |

To check your build ID: Steam → Species: Unknown → Properties → Updates (or `buildid` in
`steamapps\appmanifest_2747330.acf`).

## Install

Download `version.dll` (and, if you host, `FivePlayerMod.ini`) from the
[latest release](../../releases/latest), or build it yourself (see *Building*). Put them in the folder that
contains the real game executable:

```
<Steam>\steamapps\common\Species Unknown\SpeciesUnknown\Binaries\Win64\
    SpeciesUnknown-Win64-Shipping.exe
    version.dll        <-- here
```

(It goes next to `SpeciesUnknown-Win64-Shipping.exe`, not next to the small `SpeciesUnknown.exe`
launcher in the game root. The launcher just starts the Shipping exe.)

Also copy `FivePlayerMod.ini` there if you host (it sets where the 5th shuttle seat goes; only the
host's copy matters).

**Everyone in a 5-player private lobby needs the mod**, not just the host: the 5th seat's screen, the
5th Crew Members row, player 5's voice volume and the 5-player monster/objective values are handled on each
player's own machine. For public lobbies the mod changes nothing, so players with and without it can mix there.

To uninstall, delete `version.dll`.

## How it works

The game is Blueprint-only (UE 5.7), so the limit lives in Blueprint data, not C++. `version.dll` is
a proxy that forwards all real `version.dll` calls to `C:\Windows\System32\version.dll`. It also starts a
background thread that finds the engine's object and name tables in memory and patches the loaded
Blueprints:

| Where | Change |
|---|---|
| `BP_MyGameInstance.playerLobbyCapacity` | 5 while you host a **private** lobby (`isPlayerLobbyPrivate`), otherwise the original 4. This drives the EOS lobby size, session size, slot loops and the auto-kick check |
| `BP_MyGameInstance` "Make Player Lobby Transaction" | Lobby creation also opens member slot 4 (the original only opened slots 1–3; unused in 4-slot lobbies) |
| `BP_MyGameInstance.CheckAllowConnectionLobby` | The pre-join rejoin check reads all 5 slots of the lobby being joined |
| `BP_PlayerShip.SpawnChairs` | Private lobbies only: spawns a 5th seat. The host moves it to the offset in `FivePlayerMod.ini` (default: centred behind the back row), and its position syncs to everyone. Otherwise the original 4-seat version is used |
| `BP_PlayerShip.SetUpChairLocal` / `SpawnChairs` | Seat-screen updates ("No destination selected", mission pages) cover every spawned seat, including seat 5; the list is rebuilt once all seats exist |
| `BP_Seat` BeginPlay (other players) | Seat 5 is replicated without its ship link (`ParentShip` isn't replicated); the mod links it on each player's machine so its screen works there too |
| `WBP_PauseMenu_CrewMembers` | A 5th row ("Slot 4", hot-pink colour bar) with its own mute/volume/kick controls under Slot 3. Built at runtime by calling the engine's own widget functions. Shown only when you host a private lobby or a 5th player is present; otherwise collapsed |
| Monsters, spawners and mission objectives (13 Blueprints, 34 switches) | Values picked by player count only had cases for 0–4 players, so 5 players got 0 (monster speed/dodge/combat ×0, spawn counts 0, keypad length 0, …). 5 players now use the 4-player values. Sentry-gun balancing (data table with 1–4 player columns) clamps the player count to 4 |
| `BP_PlayerShip` launch checks (fallback only) | If the 5th seat could not be added, the ship launches when `min(players, 4)` are seated |
| `BP_LobbyGameMode` "Check More Than 4Players" | Diagnostic threshold 4 → 5 |
| `BP_Character_Lobby._GetPlayerStartFromId` | Player 5 spawns at the 4th lobby spawn point instead of nowhere |
| `BP_MyPlayerController.OnPlayerEnterChat` | Per-player voice volume/mute arrays grow for player 5 (otherwise player 5 has volume 0) |
| `WBP_ScorePage.SetUpMaxShare` | 4 other players use the same share rule as 3 |

Before writing anything, each bytecode patch checks the exact original bytes it expects. If a game update
changes a Blueprint, that one patch is skipped and logged instead of being applied blindly.

## Settings (`FivePlayerMod.ini`)

```ini
[Seat5]
X=-220   ; forward (+) / back (-), relative to the centre of the front row (Unreal units = cm)
Y=0      ; right (+) / left (-)
Z=0      ; up (+) / down (-)
```
Front seats are at Y -110/+110, back seats at X -90, Y -240/+240. Restart the game after changing it.

## Reloads

The game sometimes unloads and reloads Blueprint classes (e.g. when the lobby loads), occasionally at the
exact same memory addresses. The mod re-checks all of its patches every 100 ms and quietly re-applies any a
reload undid (logged as `... was reloaded; patches re-applied`).

## Log

`FivePlayerMod.log` is written next to `version.dll`. A good run shows:

```
FNamePool blocks at ...
GUObjectArray.ObjObjects at ...
UStruct::Script offset = 0x..
playerLobbyCapacity at offset 0x..., isPlayerLobbyPrivate at 0x...
...
Private lobby: 5-player mode ON
  playerLobbyCapacity 4 -> 5
  5th shuttle seat (private lobby): ok
BP_PlayerShip_C patched
...
```

Hosting a public lobby logs `Not a private lobby: normal 4-player mode` and `original 4 shuttle seats`.

If something is wrong, the log says which step failed.

## Known limitations

- 5 players only works in **private** lobbies. Hosting public gives the normal 4-player game.
- Guests can't tell a private lobby from a public one, so on a guest's screen the 5th Crew Members row only
  appears once a 5th player has joined (the host always sees it in a private lobby).
- The 5th Crew Members row may not be reachable with gamepad/keyboard navigation; use the mouse.
- 5 players get the game's 4-player difficulty values (the game has no 5-player values), so a 5-player run is
  slightly easier per player than the game's own scaling would make it.
- If the fallback is active (5th seat unavailable), an unseated 5th player isn't counted as "seated" by the
  end-of-mission escape sequence.

## Building

Run `build.bat` (needs Visual Studio 2022/2026 or Build Tools with the C++ x64 toolchain). Output: `build\version.dll`.
Source: `src\fiveplayer.cpp` (patcher), `src\dllmain.cpp` + `src\version_proxy.asm` + `src\version.def` (proxy).

## License

Released into the public domain under [the Unlicense](LICENSE): use, change and share it however you like, no credit needed.
