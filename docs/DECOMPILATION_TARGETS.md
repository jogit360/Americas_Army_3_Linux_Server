# Files to Decompile from `./AA3/` for the Full Client/Server Protocol

**Already done:** `AA3/Binaries/AA3Game.exe` (Ghidra → `DissAssembledFunctions/`).
**Tools needed:**
- **UEViewer** (already in `./UEViewer/`) — extracts assets from `.upk` / `.u`.
- **UE Explorer** or **WOTgreal Decompiler** (Windows) — decompiles UnrealScript bytecode in `.u` packages back to readable script. (UEViewer alone exports/lists; it does not produce `.uc` source.)
- **Ghidra** or **IDA Pro** — for the remaining DLLs.
- **strings(1)** + **dumpbin** / **objdump** — quick recon.

---

## Tier 1 — REQUIRED (decompile fully to close the protocol)

### 1A. UnrealScript packages (`.u`) — these define the wire layout

These files contain the actual `replication { … }` blocks, control-message handlers, and reliable RPC declarations that determine what bytes go on the wire. They override and extend the UE3 base. Decompile to `.uc`.

| File | Size | Why it's needed |
|---|---|---|
| `AA3/AA3Game/CookedPC/Engine.u` | 3.6 MB | UE3 base classes: `Actor`, `PlayerController`, `Pawn`, `PlayerReplicationInfo`, `GameReplicationInfo`, `GameInfo`, `NetConnection`, `ControlChannel`, `ActorChannel`. Their `replication` blocks define the parent contract that every AA3 class inherits. **Without this, you can't read AA3Baseline.u correctly.** |
| `AA3/AA3Game/CookedPC/IpDrv.u` | 42 KB | UE3 `TcpipNetDriver`, `TcpLink`, `UdpLink`, `ClientBeacon`. Tiny but declares the script-side surface of the wire layer (config keys, listen-port logic, beacon ping/pong). |
| `AA3/AA3Game/CookedPC/GameFramework.u` | 8.7 KB | Thin overlay above Engine; small but fast to read. |
| `AA3/AA3Game/CookedPC/AA3Baseline.u` | **78 MB** | **The single most important file.** Defines every AA3 replicated class: `AA_GameInfo`, `AA_GameReplicationInfo`, `AA_PlayerReplicationInfo`, `AA_ServerAuthorization`, `AAA_PlayerController`, `AAA_Pawn`, weapons, mission classes, soldier-profile structs. Each class's `replication { if (bNetDirty) … }` block is the wire spec. |
| `AA3/AA3Game/CookedPC/AA3Game.u` | 742 KB | High-level game-mode orchestration: round flow, mission types (`co`, `ao`, `ad`, `tah`, `vip`, `csar`, `dm`, `dp`, `classic`), spectator. Contains the `class default` properties that drive the GRI. |
| `AA3/AA3Game/CookedPC/OnlineSubsystemAA3.u` | 77 KB | **Critical for auth.** AA3-specific online subsystem — defines how the client packs the Steam auth ticket / AA3 auth blob into the connect URL or `NMT_Login`, MBS query format, soldier-profile fetch. This is the file that closes the §9 gap from `AA3_CLIENT_SERVER_PROTOCOL_SPEC.md` ("`NMT_Login` payload format in AA3"). |
| `AA3/AA3Game/CookedPC/Core.u` | 98 KB | Base UObject, FName, FArchive, struct primitives. Mostly background reference — read once to understand serialization. |

Decompile order (small → big): `Core.u` → `IpDrv.u` → `GameFramework.u` → `Engine.u` → `OnlineSubsystemAA3.u` → `AA3Game.u` → `AA3Baseline.u`.

### 1B. The legacy backend DLL

| File | Size | Why it's needed |
|---|---|---|
| `AA3/Binaries/libaa3.dll` | 1.85 MB | Implements every `aa3_auth_*`, `aa3_ats_*`, `aa3_mbs_*`, `aasm_*`, `MAS*` exported symbol that the binary calls. Already partially decompiled in `libaa3.dll.0.c` and `libaa3.dll.c` in the project root (137,285 lines), and summarized in `DissAssembledFunctions/analysis/LIBAA3_PROTOCOL_AND_SERVER_FUNCTIONS.txt`. **Finish the decompile only if you intend to spoof the legacy backend.** If you commit to Steam-only auth + listing (the current direction in `aa3-linux-server/`), this is **reference-only**: skim for the few wire formats that the script side still expects (e.g., what bytes the client packs into `?AuthServerIP=…`). |

---

## Tier 2 — HELPFUL (skim, don't fully decompile)

| File | Why |
|---|---|
| `AA3/Binaries/SteamSDKWrapper.dll` (254 KB) | AA3's thin wrapper over `steam_api.dll`. Decompile to confirm AppID and exact call sequence (e.g., does it use legacy `SendUserConnectAndAuthenticate` or modern `BeginAuthSession`?). Small — under an hour in Ghidra. |
| `AA3/Binaries/SteamWrapper.log` | **Plain text** — already on disk. Read directly; gives you a runtime trace of the wrapper's last session. |
| `AA3/Binaries/steamSDKWrapper_strings` (filename suggests pre-extracted strings) | Free intel: read with `cat` / `strings`. |
| `AA3/Binaries/aa3.key.db` | Binary key database for the legacy CPS signature. **Skip** — if Steam-auth replaces CPS, this file becomes irrelevant. Only investigate if you're spoofing the dead AA3 auth server. |

---

## Tier 3 — READ DIRECTLY (no decompiler — these are text)

These are not binaries; they document the wire contract by example. Read them once, harvest the keys.

| File | What you learn |
|---|---|
| `AA3/startserver` | **Already extracted in this session.** Confirms launch URL syntax: `<map>?LAN?MissionType=co?MissionSize=large?MissionTourID=inf?MissionVariant=ai?MaxPlayers=20?MaxSpectators=4` plus CLI: `/STEAMSERVERIP=<ip> /SteamAuth=<32hex> /SteamAppId=13140`. |
| `AA3/startAlley` | Same pattern, different mission. Confirms `?LAN` is sufficient (no `/Steam*` flags needed in LAN mode). |
| `AA3/Binaries/myrunserver.bat` | Original Windows server launcher. Documents every URL key the binary accepts (`?GLOBAL`, `?Listen`, `?AuthServerIP=`, `?AuthServerUserName=`, `?AuthServerPassword=`, `MULTIHOME=`). |
| `AA3/Binaries/masconfig.ini` | List of legacy MAS server IPs (`184.106.174.198`, `184.106.195.86` — Rackspace, both dead). Confirms that without Steam, server discovery is dead. |
| `AA3/Binaries/aa3loader.ini` | Loader settings; check for any embedded URL or version field. |
| `AA3/Binaries/News.ini` | News-ticker URL — telemetry only. |
| `AA3/Binaries/config/config.vdf` | Steam config text — sanity check expected AppID. |
| `AA3/AA3Game/Config/AA3Engine.ini` | UE3 engine config including `[IpDrv.TcpNetDriver]` section — port, MaxClientRate, ConnectionTimeout, NetServerMaxTickRate, allowed cheat protections. **Read in full.** |
| `AA3/AA3Game/Config/AA3Game.ini` | `[AA3Baseline.AA_GameInfo]`, `[AA3Baseline.AA_GameReplicationInfo]`, `[AA3Baseline.AA_ServerAuthorization]` sections. **Read in full.** |
| `AA3/AA3Game/Config/AA3Compat.ini` | Network compatibility / version override keys. |
| `AA3/AA3Game/Config/DefaultGame.ini` | Defaults used to seed `AA3Game.ini`. |
| `AA3/AA3Game/Config/DefaultEngine.ini` | Defaults used to seed `AA3Engine.ini`. |

These reveal config keys that Layer 2 references — the same keys that the spec table at `DissAssembledFunctions/analysis/LIBAA3_PROTOCOL_AND_SERVER_FUNCTIONS.txt:553-575` lists as `config_get` targets.

---

## Tier 4 — SKIP (irrelevant to protocol)

Listed so you don't waste effort:

- **`*.upk`** in `AA3/AA3Game/CookedPC/` — content (textures, meshes, audio, levels). Inspect with UEViewer only if you need to render or load maps. Zero protocol value.
- **`AA3Editor.u`, `Editor.u`, `UnrealEd.u`, `GFxUIEditor.u`, `GFxUI.u`, `UnrealScriptTest.u`** — editor/UI code. Server doesn't run them.
- **`AA3/Binaries/wxmsw28u_*.dll`** — wxWidgets GUI for the loader/launcher. Not used in `AA3Game.exe server` mode.
- **FaceFX, FMod, OpenAL, Speex, Vorbis, Ogg, NxCharacter, NxCooking, PhysX*, ILPointCloudLib, libleverage, FxGraphLayout, binkw32, cudart, nvtt, FonixTts*, libresample** — third-party media/physics middleware. None affect wire packets.
- **`ts3client_*.dll`, `ts3server_*.dll`** — TeamSpeak 3 voice. The server config flag `bUseTS3VOIP` controls inclusion; spec already says set it false on Linux.
- **`steam_api.dll`, `steamclient.dll`, `Steam.dll`, `SteamAPIUpdater.dll`** — Steam runtime. Don't decompile; we use the public Steamworks SDK 1.64 already in-tree.
- **`libeay32.dll`, `SSLeay32.dll`** — OpenSSL 0.9.x; only used by the dead auth server transport.
- **`msvcr*.dll`, `msvcp*.dll`, `tier0_s.dll`, `vstdlib_s.dll`, `dbghelp.dll`, `zlib1.dll`** — system / standard library.
- **`pb/`, `pbsetup.exe`, `pbsvc.exe`** — PunkBuster anti-cheat. Replaced by VAC (off in current `aa3-linux-server` config).
- **`AA3Loader.exe`** — pre-game launcher. Doesn't speak the in-game protocol.
- **`Engine/`, `DirectX/`, `steamapps/`** subtrees — assets and runtime, no protocol artifacts.

---

## Recommended workflow

```
Day 1:  Tier 3 (all *.ini + startserver/startAlley/myrunserver.bat)        ~2 hr
Day 1:  Tier 1A small files: Core.u, IpDrv.u, GameFramework.u              ~2 hr
Day 2:  Tier 1A medium: Engine.u, OnlineSubsystemAA3.u, AA3Game.u          ~6-8 hr
Day 3+: Tier 1A large:  AA3Baseline.u                                       1-3 days
        (decompile in chunks; focus on classes whose names contain
         GameInfo, GameReplicationInfo, PlayerReplicationInfo,
         PlayerController, Pawn, ServerAuthorization, OnlineGameSettings)
Day N:  Tier 2 only if there's a remaining unknown: SteamSDKWrapper.dll
```

Stop conditions per file:
- **`Engine.u`** — stop once you've documented every `replication { … }` block on `Actor`, `Pawn`, `PlayerController`, `PlayerReplicationInfo`, `GameReplicationInfo`, plus `NetConnection`/`ControlChannel`/`ActorChannel` and the `NMT_*` enum.
- **`OnlineSubsystemAA3.u`** — stop once you find the function that builds the connect URL (search "AuthServer" or "BuildURL") and the function that handles `OnlineSubsystemAA3.OnlineSubsystemAA3.MBSServerIPAddress` (already proven referenced in `unk/needAnalysis1KB/FUN_0143ade0`).
- **`AA3Baseline.u`** — full pass needed; this is the bulk of the wire spec.

---

## Two findings worth flagging now (no action requested)

1. **AppID conflict.** `AA3/startserver` shows the running command uses `/SteamAppId=13140` — the **game** AppID. The current `aa3-linux-server/config/steam_appid.txt` holds **`13180`**, which you specified earlier as the SteamCMD AppID. These are intentionally separate Valve-side: `13180` is the dedicated-server *download* AppID, `13140` is the *game* AppID that `SteamGameServer_Init` advertises so clients see this server when browsing AA3. **For runtime correctness, `steam_appid.txt` next to the binary should be `13140`.** The SteamCMD AppID `13180` belongs only in deployment scripts (`steamcmd +app_update 13180`). Want me to flip `steam_appid.txt` to `13140`?

2. **Confirmed launch URL keys** (from `startserver` / `startAlley` / `myrunserver.bat`) for §9 of the protocol spec — server URL accepts `?LAN`, `?GLOBAL`, `?Listen`, `?MissionType=`, `?MissionSize=`, `?MissionTourID=`, `?MissionVariant=`, `?MaxPlayers=`, `?MaxSpectators=`, `?AuthServerIP=`, `?AuthServerUserName=`, `?AuthServerPassword=`, plus CLI flags `/STEAMSERVERIP=`, `/SteamAuth=<32-hex token>`, `/SteamAppId=`, `MULTIHOME=`. The `/SteamAuth=` 32-hex value is the missing piece for closing §9 gap #1 — it's almost certainly an HMAC-style server token tied to `/STEAMSERVERIP`. The exact construction lives in `OnlineSubsystemAA3.u` or `SteamSDKWrapper.dll`.
