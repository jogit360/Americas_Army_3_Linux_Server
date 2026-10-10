# AA3Baseline.u — Client/Server Protocol Surface

**Source:** `/home/odeyin/Documents/AA3/AA3Baseline_extracted/` (UEViewer/UE Explorer dump of `AA3Baseline.u` and `Engine.u`).
**Companion to:** `aa3-linux-server/docs/AA3_CLIENT_SERVER_PROTOCOL_SPEC.md`. That document is sourced from the native binary disassembly (`libaa3.dll`, `AA3Game.exe`). This document is sourced from the **UnrealScript package** that sits on top of it. Together they describe the same wire surface from two angles: **C/native transport** (binary spec) and **UnrealScript replication** (this spec).

---

## 0. Source quality — read first

The dump is a **header-only fragment dump**, not a clean recompile.

- `AA3Baseline_extracted/AA3Baseline/Class/` = **863 `.uc` files**.
- `AA3Baseline_extracted/Engine/Class/` = **81 `.uc` files**.
- Asset folders (`AnimSet/`, `Material3/`, `MaterialInstanceConstant/`, `SkeletalMesh3/`, `StaticMesh3/`, `Texture2D/`) hold cooked content, not script.

What survived intact:
- **Filenames.** Each `.uc` file is named after a real symbol from the package's name table — these are *true* identifiers from the original AA3 build (class names, property names, function names, enum/const names, delegate stubs).
- **`extends` edges.** The first line `class Foo extends Bar;` preserves real inheritance pointers, though `Bar` is sometimes another scrambled name.

What did **not** survive:
- **Bytecode.** Every function is annotated `// Bytecode: 1 bytes` — i.e., the decompiler emitted a 1-byte placeholder. No control flow, no opcodes, no resolvable RPC bodies.
- **Internal symbol resolution.** Function bodies, parameter names, and most struct member names are randomly remapped to other entries in the name table (e.g. a function in `AA3MbsStartLocalServerListener.uc` has parameters named `iFragGrenadesThrown` and `EditorPreviewDoorPrimitive` — these are unrelated names from elsewhere in the package, not the real parameters). **Treat all in-file content as noise.**

Practical consequence: the file list is a **recovered symbol table for AA3Baseline.u**. The bodies are not. Anywhere this spec quotes a symbol, the source-of-truth is its presence in the symbol table — *not* the surrounding code.

---

## 1. Where AA3Baseline.u sits in the stack

Mapping to the layered architecture in the existing protocol spec:

```
Layer 3 — Backend services (libaa3.dll: aa3_auth_*, aa3_mbs_*, aa3_ats_*, MAS*)
Layer 2 — UnrealScript replication  ← AA3Baseline.u + OnlineSubsystemAA3 + AA3Game.u
Layer 1 — UE3 transport  (IpDrv: NetDriver, NetConnection, channels, bunches)
```

`AA3Baseline.u` is the largest of the AA3-specific Layer 2 packages. It defines:
- AA3 player controller / pawn / PRI / GRI subclasses,
- The auth-flow client-side state machine (`AA3Auth*`, `OnAuth*`, `Notify*`),
- The master-browser client-side state machine (`AA3Mbs*`, `MBS*`),
- TeamSpeak3 voice integration (`TS3*`),
- Server admin commands (`Server*Admin*`, `Vote*`, `Kick*`),
- Stats/persistence client-side hooks (`ES2*`, `Steam*Profile*`, `WriteEndOfGameStats`).

Everything below is grouped by what each cluster tells us about the wire surface.

---

## 2. Authentication subsystem (AA3Auth*)

The AA3 auth server (Layer 3) is dead. These are the **client-side script entry points** the original AA3 client used to talk to it. A reimplemented backend must satisfy the same callback contract — these are what the UI listens on, so a Steam-backed replacement either re-routes through the same delegates or stubs them with success.

### Confirmed symbols (filenames in `AA3Baseline/Class/`)

| Symbol | Kind | Role |
|---|---|---|
| `AA3AuthGetBlockedPlayerList` | class | Result/request struct for "fetch my blocked-player list". |
| `AA3AuthRemoveBlockedPlayerResult` | class | Result of an unblock RPC. |
| `AA3AuthPlayerUpdateAccountResult` | class | Result of an account-update RPC. |
| `AuthCreateSoldierTimer` | property/timer | Drives the create-soldier flow timeout. |
| `AuthLogoutRequests` | property/array | Tracks in-flight logout requests. |
| `iAuthCount` | int property | Number of auth attempts (retry counter). |
| `bAuthLoginOptionAutoLogin` | bool property | "Remember me" persisted login. |
| `bAutoLogin` | bool property | Runtime auto-login flag. |
| `bDebugSteamAuth` | bool property | Steam auth debug flag (confirms Steam path was already in baseline). |
| `LoginPlayer` | class/function | Login entry point. |
| `OnLoginCancelled` | event | UI cancellation hook. |
| `WaitingForAuthProfileInit` | bool property/state | Set while profile init is pending. |
| `NotifyAuthInitFailure` | event/function | Server replied with init-failure. |
| `NotifyAuthNameAvailability` | event/function | Server replied with name-availability check. |
| `OnAuthCreateNewAccount` | event/function | Hook fired during create-account flow. |
| `__onAccountCreationResult__Delegate` | delegate | Callback for create-account result. |
| `__onSoldierCreationResult__Delegate` | delegate | Callback for create-soldier result. |
| `ValidateSoldierProfileUnlockKey` | function | Validates a soldier-unlock-key (DLC/promo gate). |
| `PlayerRequestAccountInformationEmailResult` | class | Result of "email me my account info" recovery flow. |
| `sNewAccountPassword` | string property | Holds new-account password (in-flight). |
| `sPlayerAdminAccountName` | string property | Admin-account name buffer. |
| `AA_Scaleform_Auth_UI` | class | Scaleform/GFx UI movie that drives the auth screens. |
| `AA_InGameUI_NotificationMessageBox_Login` | class | Modal "login..." spinner. |
| `ConnectionStatusDelegate` | delegate | Generic connection-status callback. |

### What this implies for a replacement backend

1. **Account-management RPCs exist as first-class concepts**: create-account, update-account, recover-account-by-email, name-availability, blocked-player list, create-soldier, validate-soldier-key. Any Steam-backed replacement that wants to keep the original UI working must satisfy these callbacks (they fire delegates that drive Scaleform UI state).
2. **Auto-login** was supported (`bAuthLoginOptionAutoLogin` + `bAutoLogin`) — the original wire flow stored credentials persistently on the client. A Steam replacement maps this to "the Steam ticket is already valid, skip the login screen."
3. **Steam was a debug-only path in baseline** (`bDebugSteamAuth`). The original retail flow used the AA3 auth backend, with Steam as opt-in. The Linux server's choice to make Steam the *primary* path is a deliberate divergence from the baseline contract.

---

## 3. Master Browser Server subsystem (AA3Mbs* / MBS*)

The MBS provided server-list, filter-settings, and join-flow services. Also dead.

### Confirmed symbols

| Symbol | Kind | Role |
|---|---|---|
| `AA3MbsStartLocalServerListener` | class | Local-server listener (LAN/dedicated-host announce side). |
| `MBSCloseFilterSettings` | function | UI-side close of the filter dialog. |
| `MBSDidServerListQueryFail` | function/state | Set when a server-list fetch fails. |
| `sMBSFilterCheatsEnabled` | string property | Filter token: hide/show cheats-enabled servers. |
| `iSelectedServerQueryPort` | int property | Currently selected server's query port. |
| `sServerIP` | string property | Currently selected server's IP. |
| `FindServerIndex` | function | Server-list lookup helper. |
| `bEnableGamespyReporting` | bool property | GameSpy reporting flag (vestigial — GameSpy was already deprecated). |
| `OnlineGameSearch` | class | UE3 Online subsystem game-search query object. |
| `GetOnlineSubsystem` | function | Resolves the active OnlineSubsystem instance. |
| `AA_MainUI_Deployment_LAN_GfxMovie` | class | LAN deployment UI. |

### What this implies

1. **Two listing paths existed:** AA3 MBS (retail) and LAN listener (`AA3MbsStartLocalServerListener`). The LAN path is independent of the dead backend — a Linux server can answer LAN broadcasts without any backend reimplementation.
2. **Server-side filter tokens were strings** (`sMBSFilterCheatsEnabled`) — the MBS protocol exposed at least cheat-state, anti-cheat-state, and password-state as filterable predicates.
3. **`OnlineGameSearch`** is the UE3-standard query class. The AA3 MBS backend is fronted by an `OnlineSubsystem` provider — the right replacement is a Steam Master Server query (already noted: `MASStartServerModeQuery` in `FUN_004011b0`, port `0x2249`).
4. **`bEnableGamespyReporting`** is a no-op in modern AA3 but the symbol is still present — confirms the codebase predates the GameSpy shutdown and was never fully cleaned up.

---

## 4. Voice — TeamSpeak3 integration (TS3*)

AA3 has dual VoIP paths: native UE3 voice channels and a TS3 client embed. The TS3 path is the AA3-specific addition. Symbols imply a *direct* TS3 client SDK integration, not just a TS3-server-side hint.

### Confirmed symbols

| Symbol | Source |
|---|---|
| `bTS3Enabled` | runtime toggle |
| `bServerVOIPAllDebugTalk` | server-broadcast-everyone debug |
| `TS3CLIENT_NAME_IDENTIFIER` | TS3 client SDK constant (mirrored into UnrealScript). |
| `TS3CLIENT_UNIQUE_IDENTIFIER` | TS3 unique-id field. |
| `TS3CLIENT_FLAG_AVATAR` | TS3 flag. |
| `TS3CLIENT_OUTPUT_MUTED` | TS3 mute flag. |
| `TS3CHANNEL_FLAG_SEMI_PERMANENT` | TS3 channel flag. |
| `LocalTalker` / `UnregisterLocalTalker` | UE3 voice subsystem. |
| `pTS3ServerLibFunctions` | `aapcController` debug var — confirms a TS3 server-lib is also linked in the **server** binary. |

### Implication

The server binary speaks two voice protocols:
- **UE3 voice channel** (per-actor `UVoiceChannel` over the same UDP 8777 socket).
- **TS3 server library** (`pTS3ServerLibFunctions`) — a co-resident TS3 server is initialised in-process. The AA3 server hosts a TS3 channel automatically and clients connect their TS3 client to it.

For the Linux port: native UE3 voice is mandatory for wire compat. TS3 is *optional* — disabling it leaves clients without team voice but doesn't break the rest of the protocol.

---

## 5. Server admin / votekick / spectator commands

These are exec/RPC commands sent client→server. Naming convention `Server*` = `exec function Server*()` with `Server` reliable RPC tag.

### Confirmed symbols

| Symbol | Direction | Role |
|---|---|---|
| `ServerAdminListPlayerAdmins` | C→S | List admins. |
| `ServerChangeTeam` | C→S | Switch team. |
| `ServerLowerSights` | C→S | Lower iron sights (gameplay RPC). |
| `ServerSuperSpectator` | C→S | Enter super-spectator mode (admin). |
| `ServerNPCDebugListNPCs` | C→S | Debug NPC list (admin/debug). |
| `ServerDevTBW` | C→S | Dev-only command (TBW = "to be wired"?). |
| `kickPlayerName` | property | Holds the target name for a kick. |
| `KickReasonRasism` | enum value | Kick reason enumerant — confirms vote-kick categories are wire-encoded. |
| `VoteKick_Player` | class/struct | Vote-kick payload. |
| `iReadyVotes` | int | Vote count. |
| `MapVote` | class | Map-vote object. |
| `sVoteDetails` | string | Vote-detail buffer. |
| `AA_InGameUI_ServerCallAVote` | class | Vote UI. |
| `bReadSuperAdminOnly` | bool | Gate: super-admin-only data. |
| `bPBEnabled` | bool | PunkBuster flag (vestigial — PB shipped with AA3). |

### Implication

- The vote-kick wire format includes a typed reason enum (one of the values is `KickReasonRasism` [sic]).
- A separate **super-admin** tier exists above admin (`bReadSuperAdminOnly`, `ServerSuperSpectator`) — the auth/permission layer is two-level, not a single bool.
- **PunkBuster** integration was present at the script level (`bPBEnabled`). A modern Linux replacement should treat this as inert and route anti-cheat through Steam's VAC/EAC.

---

## 6. Replication and RPC patterns (Client* / Server* / *RepExec)

UE3 replication uses naming conventions that are preserved here.

### Confirmed symbols

| Symbol | Pattern |
|---|---|
| `ClientEndHoldBreath` | server→owner "stop holding breath" RPC. |
| `ClientRepDebugLine` | server→client debug-line replication. |
| `ClientRequest` / `ClientRequestSteamSoldierProfile` | client request RPCs. |
| `ClientSendObjectiveDroppedNotification` | server→client objective notify. |
| `DelayedClientSynchPostureState` | server→owner posture re-sync. |
| `RMBT_ServerRepExec` | "RepExec" suffix = replicated-then-execute idiom; `RMBT` likely a weapon system tag. |
| `NetId` | property — the per-actor network ID (bunched in actor-channel headers). |
| `iClientNetSpeed` | client's negotiated NetSpeed (NMT_Netspeed). |
| `AddPRI` | function — adds a `PlayerReplicationInfo` to the GRI list. |
| `PlayStatusChangeMessage` | replicated chat-style status message. |
| `bPlayerCapacitatedInRound` | replicated bool (medic system). |
| `bRecapacitatedPlayerThisMatch` | replicated bool. |
| `bGameEnded` | replicated GRI bool. |
| `bLastRoundTied`, `bTeamWonLastRound`, `iRounds` | round/match state replicated on GRI. |
| `iMaxPlayersPerTeam` | replicated config. |
| `fpRoundTimeInSeconds`, `fpMatchScore`, `fpHonorXP` | replicated float properties (note `fp` Hungarian = "float property"). |
| `TeamScores` | replicated team-score array. |
| `UpdateTeamInfo` | function. |
| `aaftiActiveFireTeam` / `iPlayerFireteamIndex` / `bIsFireTeamLeader` | fire-team replication. |

### Implication

- **GRI fields** the wire format must replicate (subset; the full set lives in `AA_GameReplicationInfo`):
  `bGameEnded`, `bLastRoundTied`, `bTeamWonLastRound`, `iRounds`, `iMaxPlayersPerTeam`, `fpRoundTimeInSeconds`, `fpMatchScore`, `TeamScores`, plus standard UE3 GRI fields.
- **PRI fields** beyond the standard set: `NetId`, `iPlayerFireteamIndex`, `bIsFireTeamLeader`, `bPlayerCapacitatedInRound`, `bRecapacitatedPlayerThisMatch`, `fpHonorXP`. The binary-derived spec already nailed the byte layout (§4.1 of the companion doc) — these are the script-side names for those fields.
- The **`*RepExec` suffix** (e.g. `RMBT_ServerRepExec`) indicates pre-UDK convention: the server replicates a flag, the client's replication notifier executes the action. Each one is a wire-observable state change.
- **`ClientRequestSteamSoldierProfile`** is significant: there is a *script-level* RPC for requesting a Steam profile load — the Steam-profile path is reachable from gameplay, not just from the auth screen.

---

## 7. Stats / persistence (ES2 = "External Stats v2"?)

| Symbol | Role |
|---|---|
| `aaes2Manager` | Top-level manager class. |
| `ES2Mgr` | Manager instance. |
| `ES2Activated` | Toggle. |
| `ES2MaxTotalObj` | Cap on tracked objectives. |
| `ES2ObjLocation`, `sES2SubObjPosition` | Per-objective position payload. |
| `ES2StrategicSpawnPerk` | Spawn-perk record. |
| `bES2EarnedFindersKeepersThisRound` | Per-round earned flag. |
| `eES2IOIPerk` | "IOI" perk enum. |
| `ReportES2Object` | Outbound RPC: report an objective event upstream. |
| `iTeam1FalseAlarmsFound` / `iES2FalseAlarmObjectsFoundInRound` | Round counters. |
| `WriteEndOfGameStats` | End-of-match upload entry. |
| `ReadOnlineStatsForFriends` | OnlineSubsystem stats read. |
| `__OnGetLeaderBoardEntries__Delegate` | Leaderboard fetch callback. |
| `SteamScoreboardSetMissionData` | Steam-side scoreboard write. |
| `StatisticsAddMissionCompleted` / `StatisticsAddTrainingStart` | Stat-counter increments. |

### Implication

- **ES2 is a bidirectional event-reporting channel.** `ReportES2Object` is the outbound RPC. The original target was the AA3 Anti-Tracking/Stats Server (Layer 3, dead). A replacement is a no-op write or a forward to Steam stats.
- **Leaderboards and stats already had a Steam path in baseline** (`SteamScoreboardSetMissionData`, `__OnGetLeaderBoardEntries__Delegate`). Aligns with the binary-derived plan to collapse Layer 3 onto Steam.

---

## 8. Career / GoArmy hooks

| Symbol | Role |
|---|---|
| `AA_Main_GoArmy_Careers` | Top-level "GoArmy.com careers" UI scene. |
| `AA_InGameUI_AIT_AdvRifleman_Movie` | Per-MOS career UI movie (AIT = Advanced Individual Training). |
| `Flash_SetUpRoleLoadoutCB` | Scaleform CB for role-loadout UI. |

This is **client-only outbound** (opens a browser to GoArmy.com). No server contract — but it confirms the in-game-to-recruitment hook was wired through Scaleform, not a native shell-out.

---

## 9. URL/connect-string fields

The dump exposes property names that map to UE3 connect-URL keys:

| Symbol | URL key (inferred) |
|---|---|
| `sServerIP` | host part |
| `iSelectedServerQueryPort` | implicit `:port` |
| `iClientNetSpeed` | `?NetSpeed=` |
| `bAutoLogin` / `bAuthLoginOptionAutoLogin` | not URL — settings.ini |
| (LAN flag) | `?LAN` (confirmed in companion spec from `FUN_004011b0`) |

---

## 10. Engine-package extras (`AA3Baseline_extracted/Engine/Class/`)

The companion `Engine.u` dump contains 81 fragment files. Protocol-relevant entries:

| Symbol | Role |
|---|---|
| `EOnlineGameState` | enum — UE3 standard online-game-state values (NoSession / Pending / InProgress / Ending). The server must set/replicate this. |
| `OnlineStatsWrite` | Base class for stats-write payloads. AA3 subclasses live in AA3Baseline. |
| `OnStartOnlineGameComplete` | Delegate fired when `StartOnlineGame()` finishes — gates the actual "we are now accepting connections" transition. |
| `OnListElementsSorted` | UI sort callback. |
| `RemoveFromTeam` | UE3 standard team API. |
| `Projectile` | Base actor class. |
| `Weapon` | Base actor class. |

`EOnlineGameState` and `OnStartOnlineGameComplete` are the canonical UE3 online-session lifecycle hooks. The Linux server's session manager must drive these in the right order or AA3 clients sitting on the lobby/match-start path will hang.

---

## 11. What's *missing* from the dump (and why)

Because this is a header-only fragment dump, the following are **definitively not recoverable** from `AA3Baseline_extracted/`:

- **`replication { }` blocks.** UE3 stores per-class replication conditions in compiled bytecode. None survived. The condition under which each property is replicated has to be reconstructed from the binary or guessed from UE3 conventions.
- **Server/Client RPC body code.** Every `ServerXxx`/`ClientXxx` function is a 1-byte stub. We know the *name* of the RPC and (sometimes) the parent class, but not the parameter types or the server-side behaviour.
- **Full inheritance chain.** Many `extends` lines point to scrambled names (`<invalid_name_1691904>`) — the dump's name table had collisions that got resolved randomly.
- **Configuration defaults.** `defaultproperties { }` blocks were not extracted.

For these, the binary spec (`AA3_CLIENT_SERVER_PROTOCOL_SPEC.md`) and `DissAssembledFunctions/` traces are authoritative. This document supplies the **script-level vocabulary** to label what's already been recovered there.

---

## 12. Cross-reference — script symbol → binary location

A non-exhaustive list of where script-level symbols here are likely to surface in the C decompilation:

| Script symbol | Likely binary site |
|---|---|
| `LoginPlayer`, `OnLoginCancelled` | `aa3_auth_*` exports in `libaa3.dll`. |
| `AA3MbsStartLocalServerListener` | `aa3_mbs_*` exports + LAN broadcast handler. |
| `ReportES2Object` | `aa3_ats_*` exports in `libaa3.dll`. |
| `ServerChangeTeam` etc. | `AA_GameInfo`-equivalent VTable in `AA3Game.exe`. |
| `WriteEndOfGameStats` | Pulls down to `aa3_ats_*` *and* `ISteamUserStats`. |
| `ClientRequestSteamSoldierProfile` | `SteamClientLoadSoldierProfileIndexFile` in `FUN_004011b0` neighbourhood. |
| `pTS3ServerLibFunctions` | The TS3 server-lib symbol cluster in `libaa3.dll` (TS3 server SDK is co-loaded). |

---

## 13. Recommended next decomp targets, prioritised by protocol surface

1. **`AA3MbsStartLocalServerListener` + `OnlineGameSearch` flow** — the LAN path is the only Layer-3 surface that can be exercised end-to-end without any backend reimplementation. Validates Layer-1 and Layer-2 in isolation.
2. **`LoginPlayer` → `__onAccountCreationResult__Delegate` / `__onSoldierCreationResult__Delegate`** — pin down the auth-flow callback contract so the Steam-backed replacement can fire the same delegates.
3. **`*ServerRepExec` cluster** — recover the replicated-then-executed action set; these are the gameplay-critical RPCs.
4. **`ES2Mgr` / `ReportES2Object`** — define a stub interface so end-of-match flow doesn't hang on a dead upload.
5. **`pTS3ServerLibFunctions` resolution** — decide whether to ship in-process TS3 server (mandatory if we want full feature parity) or stub it out.
