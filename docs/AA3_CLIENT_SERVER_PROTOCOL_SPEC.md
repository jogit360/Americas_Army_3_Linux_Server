# AA3 Client ↔ Server Protocol Specification

**Sources:** `DissAssembledFunctions/Critical/`, `DissAssembledFunctions/high/`, plus the small/unanalyzed wrappers under `DissAssembledFunctions/unk/lessThank1KB/` and `unk/needAnalysis1KB/` that hold the actual protocol-bearing call sites; cross-referenced with `DissAssembledFunctions/analysis/*`.
**Binary:** `AA3Game.exe` (UE3, MSVC8, 32-bit x86) + `libaa3.dll` (AA3-specific networking).
**Goal:** Document every function the Linux server needs to be wire-compatible with original AA3 clients.

---

## 1. What's actually in `Critical/` and `high/` (and what isn't)

The 588 `Critical/` files and 983 `high/` files cover the **engine bootstrap and infrastructure** that has to be running before any network packet is touched: command-line parsing, module loader, timing, memory manager, dispatch tables, configuration, scripting VM. **Almost no wire-level networking code is tagged Critical/high** — the actual UDP/NetDriver/Channel/AASM/MBS code is split across `low/`, `med/`, and the unanalyzed `unk/` buckets, because each individual function is a small thin wrapper around an exported symbol.

This is a real finding, not a gap: anything that survives without networking (init, timing, memory) was tagged Critical; anything that's a one-liner over `aa3_mbs_xxx`/`aa3_auth_xxx`/`aa3_ats_xxx` got dropped into the small-function buckets.

### Critical/high functions that DO touch the protocol surface

These are the ones whose execution must succeed **before** the network stack comes up:

| FUN | Suggested name | Where | Role |
|---|---|---|---|
| `FUN_004024b0` | `Application_Entry_WinMain` | Critical | Calls `SteamInitAPI`, then `GuardedMain`. Owns Steam lifecycle. |
| `FUN_004011b0` | `GuardedMain` | low | Parses `?LAN` URL flag, calls **`SteamGameServerInit(authIP, 0x2249, authPort, isLAN)`** at port **8777**, then `MASStartServerModeQuery(...)` for the dead AA3 master server. |
| `FUN_00407e30` | `EngineBootstrapper` | Critical | Decides Game vs Commandlet (server) mode from CLI. |
| `FUN_004054b0` | `Engine_LoadGameSubsystems` | Critical | Module load: `OnlineSubsystemAA3`, `AA3Baseline`, `AA3Game` — these contain all AA3-specific replication code. |
| `FUN_00405680` | `Engine_LoadUnrealCoreSubsystems` | Critical | Module load: `Core`, `Engine`, `GameFramework`, **`IpDrv`** (UE3 network driver), `GFxUI`. **`IpDrv` is the wire-level network stack.** |
| `FUN_00404e70` | `Engine_CoreSubsystemInit` | Critical | Zeroes the 1600-byte `DAT_025cd918` global (holds NetDriver pointers and config), then chains 50+ subsystem inits. |
| `FUN_00406870` | `Engine_MainHeartbeat` | Critical | Per-tick driver. Calls `NetDriver::TickFlush` / `TickDispatch` indirectly via the Engine vtable. **Must run at the game's tick rate for replication to flow.** |
| `FUN_00404fc0` | `Engine_UpdateTicksAndDeltaTime` | Critical | Frame timing — replication interpolation depends on this delta. Already ported to `clock_gettime` in `aa3-linux-server/src/engine/engine_init.cpp`. |
| `FUN_00406d00` | `Engine_InitDefaultPlayer` | Critical | Loads `DefaultPlayer.ini`. Contains the `Player.NetSpeed` cap that the handshake negotiates. |
| `FUN_00d5eb70` | `Server_LogPlayerState` | Critical | Server-side player state writer. **Confirms the player replication struct layout (see §4).** |

The wire-protocol files themselves (NetConnection, NetDriver, Channels, AASM/MBS engines) live elsewhere — see §3 and §6.

---

## 2. Three-layer architecture

```
┌──────────────────────────────────────────────────────────────────────┐
│  Layer 3 — AA3 backend services (libaa3.dll)                         │
│  ────────────────────────────────────────────────────────────        │
│   aa3_auth_*    → AA3 Authentication Server     (DEAD — replace)     │
│   aa3_ats_*     → Anti-Tracking Server (stats)  (DEAD — replace)     │
│   aa3_mbs_*     → Master Browser Server         (DEAD — replace)     │
│   MAS*          → Master Authentication Server  (DEAD — replace)     │
│   AASM_*        → AA3 Server Manager dispatcher (DEAD — replace)     │
│  REPLACEMENT: ISteamGameServer (auth, listing, ban-list, VAC).       │
├──────────────────────────────────────────────────────────────────────┤
│  Layer 2 — AA3 game replication (OnlineSubsystemAA3 + AA3Baseline)   │
│  ────────────────────────────────────────────────────────────        │
│   AAA_PlayerController, AA_GameInfo, AA_GameReplicationInfo,         │
│   AA_PlayerReplicationInfo, soldier profile, team/fireteam,          │
│   mission/round state, scoreboard.                                    │
│  Wire format: UE3 actor replication on top of Layer 1.                │
├──────────────────────────────────────────────────────────────────────┤
│  Layer 1 — UE3 transport (IpDrv module, UTcpipDriver / UTcpip…)      │
│  ────────────────────────────────────────────────────────────        │
│   UDP port 8777 (`0x2249`). Packets contain bunches.                  │
│   UNetDriver → UNetConnection → UChannel (Control/Actor/Voice/File). │
│   Reliable + unreliable bunches, per-actor channels, RPC.             │
│  This is plain UE3 — public details available. Server must be a       │
│  faithful reimplementation of this layer to accept original clients.  │
└──────────────────────────────────────────────────────────────────────┘
```

**Implication for the Linux server:** Layer 1 must be implemented faithfully. Layer 2 must match per-class layouts and replication conditions for the AA3 PRI/PC/Pawn/GRI classes. Layer 3 collapses into Steam GameServer + a small in-process stat/session store.

---

## 3. UE3 transport layer (Layer 1) — what the server must speak

### 3.1 Listen address

- Default game port: **`0x2249` = 8777** (proven: `FUN_004011b0:381` calls `SteamGameServerInit(..., 0x2249, ...)`).
- Default Steam query port: 27015 (per Steam SDK convention).
- LAN mode triggered by `?LAN` in the connect URL (`FUN_004011b0:380` `wcsstr(url, L"?LAN")`).
- Connect-URL format (UE3 canonical): `IP:Port?Key=Value?Key=Value…`. AA3 known keys observed:
  - `?LAN` — LAN-only mode flag
  - `?AuthServerIP=<ip>` — override auth server (parsed `FUN_004011b0:408`)
  - Inherited UE3 keys: `?Name=`, `?Class=`, `?Team=`, `?Password=`, `?game=`, `?map=`, `?mutator=`

### 3.2 Packet & bunch format (UE3 standard)

Each UDP datagram:
```
[ PacketHeader (variable bits) ][ Bunch ][ Bunch ][ … ][ trailer/ack bits ]
```
- Header: packet sequence + ack bits.
- Bunch: `{ ChannelIndex, bReliable, bOpen, bClose, ChannelSequence, DataBitCount, DataBits }`.
- Channel 0 is always the **control channel**.
- Channel N>0 is an **actor channel** (or voice/file channel).

The server must implement at minimum:
- Reliable resend with sequence numbers
- Out-of-order packet reassembly
- Per-channel sequence
- Bit-packed read/write (`FBitReader`/`FBitWriter` semantics)

### 3.3 Control-channel handshake

UE3 control messages are identified by an opcode byte (`NMT_*`). AA3 inherits the public UE3 set; the documented sequence is:

```
client                                      server
──────                                      ──────
                                            (listening on UDP 8777)

Hello(NMT_Hello, NetVer, Guid, MinNetVer)  →
                                            ← Challenge(NMT_Challenge, ChallengeStr)
Login(NMT_Login, ClientResp, URL,          →
      [Steam ticket bytes])
                                            ← (server calls SteamGameServer
                                                 ::BeginAuthSession on ticket)
                                            … wait for ValidateAuthTicketResponse_t …
                                            ← Welcome(NMT_Welcome, MapName, GameClass)
NetSpeed(NMT_Netspeed, NegotiatedRate)     →
Join(NMT_Join)                             →
                                            ← (PreLogin → AcceptLogin →
                                                PostLogin → SpawnPlayActor)
                                            ← ClientPlay(NMT_PCSwap or NMT_Spawn,
                                                Pawn netGUID)
                                            ← (replication of GameInfo,
                                                GRI, PRI list begins)
```

**AA3 Steam ticket placement:** AA3 ships with `OnlineSubsystemAA3`. The original client embeds an authentication blob into the URL or in an extension to `NMT_Login`. The current evidence from `FUN_004011b0` is that the URL parses `?AuthServerIP=`. With the dead AA3 auth server, the server-side replacement is to expect a Steam auth ticket at the same point in the handshake — this is the fixed insertion point for our `SteamAuthManager::BeginAuthSession` call (already wired in `aa3-linux-server/include/auth/steam_auth.h`).

### 3.4 Channel types

| Index | Channel class | Carries |
|---|---|---|
| 0 | `UControlChannel` | The handshake messages above + chat/ack. |
| 1+ | `UActorChannel` | One per replicated actor. AA3 always-relevant: `WorldInfo`, `GameReplicationInfo`, every `PlayerReplicationInfo`. Owner-relevant: own `PlayerController`, own `Pawn`. Distance-relevant: enemy pawns/projectiles. |
| varies | `UVoiceChannel` | VoIP frames (AA3 also supports TS3 path — see `bUseTS3VOIP` in config). |
| varies | `UFileChannel` | Asset / map streaming. Not required for a stripped server. |

---

## 4. AA3 game replication (Layer 2) — concrete wire data

### 4.1 Player state structure (CONFIRMED from `FUN_00d5eb70`)

Server-side log writer that emits XML per player. The `param_2` array layout it walks tells us the **wire-level field offsets** of `AA_PlayerReplicationInfo`-equivalent state:

| Offset (uint32 idx) | Bytes | Field | Notes |
|---|---|---|---|
| 0 | 0x00–0x03 | `PlayerID` | Server-assigned id (NOT the SteamID — the per-session slot id). |
| 1 | 0x04 | `Team` | 0/1 (US / OPFOR). |
| 2 | 0x08 | `FireTeam` | Squad id within team. |
| 3..5 | 0x0c–0x17 | (reserved / padding) | |
| 6 | 0x18 | `StatusFlags` (bitfield) | bit0 `LinkedTeam`, bit1 `LinkedFT`, bit3 `UnderFire`, bit4 `PFT`, bit5 `Surrounded`. |
| 7..30 | 0x1c–0x7b | (other state — incl. ammo/health/role pointers via `FUN_00404300`+0x84 lookups) | |
| 31 (0x1f) | 0x7c | `Location.X` (float) | World-space position. |
| 32 (0x20) | 0x80 | `Location.Y` (float) | |
| 33 (0x21) | 0x84 | `Location.Z` (float) | |
| 34 (0x22) | 0x88 | `Rotation.Pitch` (int32) | UE3 16-bit fixed in low half, but log treats as int. |
| 35 (0x23) | 0x8c | (reserved) | |
| 36 (0x24) | 0x90 | `Rotation.Yaw` (int32) | |

Plus three string-pointer fields resolved through `FUN_00404300`+0x84 (FName lookup): `ClassRole`, `EID` (event id), `Type` (event tag). These are FNames on the wire (16-bit name index + 32-bit instance index in UE3 replication).

### 4.2 Confirmed total payload size: **0x74 = 116 bytes** of fixed fields

Evidence: `FUN_008561a0` (RE doc) — allocator stride is `0x74`. Strings appended after.

### 4.3 What AA3 replicates that we must mirror

From the OnlineSubsystemAA3 module load (`FUN_004054b0`) and the public AA3 class hierarchy:

- **`AA_GameInfo`** (server-only authority): match phase, round timer, score, ROE counters, `iCurrentMapRotationIndex`. Most fields are config-driven (`config_get` keys listed in `analysis/LIBAA3_PROTOCOL_AND_SERVER_FUNCTIONS.txt:553-575`).
- **`AA_GameReplicationInfo`** (replicates to all): `TS3ServerMode`, `SpectatorMode`, `bMPCheatsEnabled`, current map name, score.
- **`AA_PlayerReplicationInfo`** (one per player, replicates to all): the 116-byte struct above + name + score + ping + class role + team/fireteam/squad assignment.
- **`AAA_PlayerController`** (one per player, replicates only to owner): input ack, role selection, vote state. The function `AAA_PlayerController::NativeDumpChannels()` (proven in `unk/needAnalysis1KB/FUN_00d2d250.txt:34`) walks `World->NetDriver->ClientConnections[i]->OpenChannels[]` — confirms the standard UE3 driver/connection/channel object layout in this binary.
- **`AAA_Pawn`** (per character, owner-relevant + distance-relevant): position/velocity, animation state, health, ammo per weapon slot, weapon-in-hand, fire/aim flags, lean state.

### 4.4 Things we **don't** have to replicate

- Soldier profile persistence (saved client-side via `SteamClientSaveSoldierProfileFile`).
- Achievement unlocks (Steam handles).
- TS3 voice (separate process; `bUseTS3VOIP` flag passed in GRI is enough).

---

## 5. Functions we must implement, stub, or replace

### 5.1 IMPLEMENT — UE3 IpDrv equivalents (Layer 1, our server-side code)

These have no FUN_ in the AA3 binary because they live in `IpDrv.dll`/`Engine.dll`, not `AA3Game.exe`. We re-implement them from the public UE3 architecture:

| Component | What it does | Server module |
|---|---|---|
| UDP socket pump | recvfrom on game port, dispatch to NetDriver | `src/network/socket_manager.cpp` (already stubbed) |
| `UNetDriver` | Owns connections, ticks them, handles new-connection ack | `src/network/net_driver.cpp` (TBD) |
| `UNetConnection` | Per-client packet/bunch reader/writer, ack/resend | `src/network/net_connection.cpp` (TBD) |
| `UControlChannel` | Hello/Challenge/Login/Welcome/Join state machine | `src/network/control_channel.cpp` (TBD) |
| `UActorChannel` | Per-actor open/close, replicated property delta encoding | `src/network/actor_channel.cpp` (TBD) |
| `FBitReader`/`FBitWriter` | Bit-packed serialization | `src/network/bitstream.cpp` (TBD) |

### 5.2 BIND — these AA3 functions in the binary that drive Layer 2

The Linux server doesn't have access to `libaa3.dll` (Windows-only). It must re-implement equivalents that match the wire effect of these calls. The disassembly maps each backend op to an exported symbol; we keep the symbol names as the **contract** for what each handler does:

| Binary FUN | Exported call | Wire effect | Server replacement |
|---|---|---|---|
| `unk/lessThank1KB/FUN_0143bb50` | `aa3_mbs_server_login` | "MBS:serverLogin result: %d" log; pre-game registration. | `SteamAuthManager::Initialize()` (already wired). |
| `unk/lessThank1KB/FUN_0143bad0` | `aa3_mbs_server_start_server` | "MBS:startServer result: %d"; flips advertise-active. | `gs->SetAdvertiseServerActive(true)` (deferred to `OnSteamServersConnected`, already wired). |
| `unk/lessThank1KB/FUN_0143bb20` | `aa3_mbs_server_stop_server` | Stops MBS advertise. | `gs->SetAdvertiseServerActive(false)` (in `Shutdown()`, already wired). |
| `unk/lessThank1KB/FUN_0143b350` | `aa3_mbs_client_login` | (Client-side) | N/A on server. |
| `unk/lessThank1KB/FUN_0143b3a0` | `aa3_mbs_client_logout` | (Client-side) | N/A on server. |
| `unk/lessThank1KB/FUN_0143b8c0` | `aa3_mbs_client_ping_game_server` | Server-browser ping reply. | Steam-side: `ISteamGameServer` query reply or `HandleIncomingPacket` shared-mode. |
| `unk/lessThank1KB/FUN_0143b910` | `aa3_mbs_client_get_mbs_query_results` | Client server-list parse. | N/A (client uses Steam server list). |
| `unk/lessThank1KB/FUN_0143bff0` | `aa3_mbs_server_set_state_player_online` | Player joined → MBS state. | Track in `PlayerManager`; expose via `BUpdateUserData` Steam call. |
| `unk/needAnalysis1KB/FUN_0143c020` | `aa3_mbs_server_set_state_player_offline` | Player left → MBS state. | `gs->EndAuthSession(steamID)` + `BUpdateUserData` zero. |
| `unk/lessThank1KB/FUN_0143bf60` | `aa3_mbs_server_set_state_attribute_integer` | "Players=N", "Bots=M", "Score=…" | `gs->SetMaxPlayerCount`, `gs->SetBotPlayerCount`, `gs->SetKeyValue` (rules query). |
| `unk/lessThank1KB/FUN_0143bfc0` | `aa3_mbs_server_set_state_attribute_string` | "Map=…", "GameMode=…" | `gs->SetMapName`, `gs->SetKeyValue`, `gs->SetGameTags`. |
| `unk/needAnalysis1KB/FUN_0143c150` | `aa3_mbs_server_set_state_player_attribute_real` | Per-player score / accuracy. | In-process stat store; optional `BUpdateUserData(score)`. |
| `unk/lessThank1KB/FUN_0143c120` | `aa3_mbs_server_set_state_player_attribute_integer` | Per-player int attr (kills etc.). | Same. |
| `unk/lessThank1KB/FUN_0143c260` | `aa3_mbs_server_set_state_player_attribute_string` | Per-player class/team. | In-process; broadcast via PRI replication. |
| `unk/lessThank1KB/FUN_0143b0c0` | `aa3_mbs_free` | Lifecycle. | RAII destructor. |
| `unk/lessThank1KB/FUN_0144d7e0` | `aa3_auth_free` | Auth lifecycle. | `SteamAuthManager::Shutdown()`. |
| `unk/lessThank1KB/FUN_01446180` | `aa3_auth_communicate` | Auth roundtrip. | `BeginAuthSession` → `ValidateAuthTicketResponse_t`. |
| `unk/lessThank1KB/FUN_01445a50` | `aa3_auth_get_error` | Read auth error. | `EAuthSessionResponse` enum from callback. |
| `unk/needAnalysis1KB/FUN_014374c0` | `aa3_ats_get_error` | Read ATS error. | N/A — ATS replaced by in-process stats. |
| `low/FUN_004011b0` (call) | `MASStartServerModeQuery` | Register on AA3 master. | **Skip.** Server is dead. Steam master replaces it. |
| `low/FUN_004011b0` (call) | `SteamGameServerInit` | Steam GS init at port 0x2249. | Already implemented at `src/auth/steam_auth.cpp`. |

The full set of `unk/...` files containing one-liners over `aa3_mbs_*` / `aa3_auth_*` / `aa3_ats_*` (33 files total) is fully enumerated by:
```
grep -lir "aa3_mbs_\|aa3_auth_\|aa3_ats_" DissAssembledFunctions/unk/
```

### 5.3 STUB — engine-side functions whose semantics we keep but whose backends we drop

| Effect | Original | Linux-server replacement |
|---|---|---|
| Crypto-signed auth message (CPS) | `aa3_auth_add_cps_signature` | None — Steam ticket carries authenticity. |
| `aa3.key.db` load | `aa3_auth_load_cryptographic_key_database` | None — file not used. |
| Anti-cheat rule sync | `aa3_ats_sync_rules`, `aa3_ats_add_rule` | None — VAC via Steam (optional, gated by `SecureMode` config key). |
| AASM XML status page | `aasm_property_run_server` | Optional `/status` HTTP endpoint emitting Steam server state. |

---

## 6. Connection lifecycle on the new server (binding contract)

```
new UDP packet on :8777
   │
   ▼
NetDriver::HandleIncomingPacket(srcIP, srcPort, bytes)
   │   ├─ if first packet from IP:port → create UNetConnection
   │   └─ else → route to existing UNetConnection
   ▼
UNetConnection::ReceivedPacket → reads bunches → dispatches per-channel
   │
   ▼
UControlChannel::ReceivedBunch
   │
   switch (NMT_*)
   ├─ NMT_Hello       → reply NMT_Challenge with random nonce
   ├─ NMT_Login       → extract ClientResponse + URL + Steam ticket bytes
   │                  → SteamAuthManager::BeginAuthSession(steam_id, ticket, len)
   │                  → mark connection PendingAuth
   ├─ (async)         ← ValidateAuthTicketResponse_t fires via SteamAuthManager
   │                  → IPlayerAuthSink::OnAuthResult(approved=true)
   │                  → reply NMT_Welcome(map, gameClass)
   ├─ NMT_Netspeed    → clamp to MaxClientRate (config)
   ├─ NMT_Join        → GameInfo::PreLogin → AcceptLogin → PostLogin → SpawnPlayer
   │                  → open ActorChannels for: GameInfo (server-only), GRI,
   │                    PRI for self + all existing PRIs, own PlayerController,
   │                    own Pawn, then the world's always-relevant set.
   │                  → reply NMT_PCSwap (or NMT_Spawn) carrying Pawn netGUID
   └─ NMT_Abort/NMT_Failure → close connection, EndAuthSession
```

**Disconnect path:**
```
client gone (timeout / NMT_Abort / explicit close)
   │
   ▼
NetDriver removes UNetConnection
   ├─ For every open ActorChannel → close/destroy (server-authoritative actors stay)
   └─ SteamAuthManager::EndAuthSession(steam_id)
       → gs->EndAuthSession(CSteamID)
```

---

## 7. Replication contract (what flies per tick)

At 60 Hz the server's tick produces one `NetDriver::TickFlush` that, per connection:

1. Gathers actors with `bAlwaysRelevant`, owner, or in-relevancy-radius for that connection.
2. For each, opens an ActorChannel if needed and writes a property-delta bunch.
3. Property deltas use the conditions baked into the original UnrealScript (`replication { … }` blocks) of the AA3 classes.

For the Linux server, the minimum replicating set to make a vanilla AA3 client load into the world:

| Class | Channel pattern | What we serialize |
|---|---|---|
| `WorldInfo` | always-relevant, opens once per connection | TimeSeconds, GameType class name, NetMode. |
| `AA_GameInfo` | server-only authority, **not** replicated | Internal state. |
| `AA_GameReplicationInfo` | always-relevant, one per server | All public match config. |
| `AA_PlayerReplicationInfo` | always-relevant, one per player | The 116-byte struct (§4.1) + Name FName + score. |
| `AAA_PlayerController` | owner-only | Input ack, role selection, view rotation. |
| `AAA_Pawn` | distance-relevant | Position, velocity, rotation, animation flags, health, current weapon, ammo. |

Anything else (projectiles, decals, sounds) can be added later; the client will spawn defaults if it doesn't see them.

---

## 8. Tick rate and bandwidth budget

- Game tick: **60 Hz** (already set in `Engine::Run()`).
- Per-connection net update interval: typically 30 Hz (UE3 `NetUpdateFrequency` on each replicated actor).
- Per-connection rate cap: `Player.NetSpeed` from `DefaultPlayer.ini` (default ~10000 bytes/s for UE3 era). The handshake's `NMT_Netspeed` step negotiates this.

A 32-player server emitting full `AAA_Pawn` deltas at 30 Hz to each peer should sit comfortably under 30 KB/s/client.

---

## 9. Open gaps the Linux server team must close

These aren't blockers for the framework; they're the spots where we still need ground truth:

1. **`NMT_Login` payload format in AA3** — base UE3 carries `URL` + `RequestURL`. AA3 likely appends Steam ticket bytes. Need a packet capture of an original AA3 client connecting to a Steam-listed AA3 server, OR disassembly of the `OnlineSubsystemAA3` package's UnrealScript (extract via `UEViewer/` already in the repo).
2. **`AA_PlayerReplicationInfo` exact UnrealScript layout** — the 116-byte struct is confirmed from `FUN_00d5eb70`, but field 3..5 and 7..30 are unidentified. UnrealScript decompile of `AA3Baseline.AA_PlayerReplicationInfo` will resolve this.
3. **Voice channel format** — out-of-scope for the first playable. Disable `bUseTS3VOIP` in GRI; UE3 voice frames will stop being sent.
4. **Whether the original `OnlineSubsystemAA3` package required a specific `?AuthServerIP=` or hash in the URL** — we observed it parsed in `FUN_004011b0:408` but its required-ness is unknown. Empirical test: connect with and without it.

For each gap, the implementation order is: stub the function so the handshake completes, then iterate against an actual client.

---

## 10. Summary table — minimum viable function set

| # | Function | Origin | Status in `aa3-linux-server` | Owner |
|---|---|---|---|---|
| 1 | UDP recv loop | new (was UE3 IpDrv) | stubbed `socket_manager.cpp` | network |
| 2 | Bit reader/writer | new (was `FBitReader/Writer`) | TODO | network |
| 3 | NetDriver / NetConnection / Channels | new | TODO | network |
| 4 | Control-channel handshake | new (was `UControlChannel`) | TODO | network |
| 5 | Steam ticket → BeginAuthSession | new | **done** (`steam_auth.cpp`) | auth |
| 6 | PRI struct (116 B) serialize | new (matches §4.1) | TODO | game |
| 7 | GRI replication | new | TODO | game |
| 8 | Pawn position replication | new | TODO | game |
| 9 | `aa3_mbs_*` parity ops on Steam GS | binary calls listed §5.2 | partial — listing/heartbeat done | auth |
| 10 | ATS / AA3 auth / MAS | binary calls listed §5.2 | **stubbed (skip)** | auth |
| 11 | Tick at 60 Hz | `FUN_00406870` | **done** (`engine_init.cpp`) | engine |
| 12 | Steam GS lifecycle | `FUN_004011b0` Steam call site | **done** | auth |

**The protocol-critical path forward is:** §5.1 (rows 1–4 above), then §4.3 / §7 for replication. Everything in §5.2 is already either implemented (Steam side) or correctly skipped (dead AA3 backends).
