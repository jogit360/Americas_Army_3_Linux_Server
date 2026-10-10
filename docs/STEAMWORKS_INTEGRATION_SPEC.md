# AA3 Linux Server – Steamworks SDK Integration Specification

**SDK:** Steamworks SDK v1.64 (March 2026), shipped at `/home/odeyin/Documents/AA3/steamworks_sdk_164/sdk/`
**Target:** `aa3-linux-server/` (Linux x86_64, GCC 10+/Clang 12+, C++17, CMake 3.16+)
**Status of current code:** Phase 1 skeleton present; partially wired and contains build-breaking errors against SDK 1.64.

This spec is the implementation guide. Roadmap (Section 2) lists the ordered work items; Sections 3–10 are the binding contract for each item.

---

## 1. Audit of the current codebase (what exists, what is broken)

| Area | File | State | Notes |
|---|---|---|---|
| GameServer init | `src/auth/steam_auth.cpp:62` | **Broken on SDK 1.64** | Calls `EnableHeartbeats(true)`. Renamed to `SetAdvertiseServerActive(bool)` in modern SDKs. |
| Auth callback | `src/auth/steam_auth.cpp:10` | **Not wired** | `ValidateAuthTicketResponseHandler` exists but is never registered with the SDK callback dispatcher (no `STEAM_GAMESERVER_CALLBACK` / `CCallback<>` instance). Callbacks will never fire. |
| Login lifecycle | `src/auth/steam_auth.cpp:83` | Incomplete | `LogOnAnonymous()` is called, but no callback for `SteamServersConnected_t` / `SteamServerConnectFailure_t` / `SteamServersDisconnected_t` / `GSPolicyResponse_t`. We don't know if/when login succeeded. |
| Server config push | `src/auth/steam_auth.cpp:74` | Order issue | `SetProduct/SetGameDescription/SetModDir/SetDedicatedServer` are correct (must be set before LogOn), but `ServerName/MapName/MaxPlayers` from config are never pushed; the engine never calls the setters. |
| Engine wiring | `src/engine/engine_init.cpp:60` | **SteamAuthManager not used** | `Engine::Run()` has only TODO comments. There is no SteamAuthManager instance and no `SteamGameServer_RunCallbacks()` per tick. |
| Build wiring | `CMakeLists.txt:45` | **Broken** | Links `${STEAMWORKS_LIB_DIR}/libsteamgameserver.so` which does **not exist** in SDK 1.64 — only `libsteam_api.so` ships under `redistributable_bin/linux64/`. SDK path also points at `external/steamworks_sdk_linux/` which is empty. |
| AppID | `config/steam_appid.txt` | Placeholder | Set to `10` (Source SDK Base test ID). For shipping, AA3's real Steam AppID (Valve partner-assigned) replaces this; until then `10` allows local sandbox dev. |
| Network | `src/network/socket_manager.cpp` | Stub | UDP bind only. Not relevant to Phase 1 except that game port and Steam game port must agree (see §6). |

---

## 2. Implementation roadmap (ordered)

Each item is small, independently buildable, and verifiable. Total ~10–14 hrs to complete Phase 1 correctly.

| # | Task | Files touched | Verifies |
|---|------|---------------|----------|
| **R1** | Repoint build at the in-tree SDK and link `libsteam_api.so` (not `libsteamgameserver.so`) | `CMakeLists.txt` | Compiles & links |
| **R2** | Place `steam_appid.txt` next to the binary at runtime | `CMakeLists.txt` | `SteamGameServer_Init` doesn't fail with "no app id" |
| **R3** | Replace deprecated `EnableHeartbeats` with `SetAdvertiseServerActive` and adjust call ordering | `src/auth/steam_auth.cpp` | Phase 1 compile passes |
| **R4** | Register SDK callbacks via `CCallback<>` for: `SteamServersConnected_t`, `SteamServerConnectFailure_t`, `SteamServersDisconnected_t`, `GSPolicyResponse_t`, `GSClientApprove_t`, `GSClientDeny_t`, `GSClientKick_t`, `ValidateAuthTicketResponse_t` | `include/auth/steam_auth.h`, `src/auth/steam_auth.cpp` | Login result is observable; auth result is observable |
| **R5** | Defer `SetAdvertiseServerActive(true)` until `SteamServersConnected_t` (or until `GSPolicyResponse_t` if running in `eServerModeAuthenticationAndSecure`) | `src/auth/steam_auth.cpp` | Server appears in browser only when ready |
| **R6** | Push `ServerName`, `MapName`, `MaxPlayers`, `GameTags`, `GameDescription` from `EngineConfig` to the Steam server (after init, before `SetAdvertiseServerActive`) | `src/auth/steam_auth.cpp`, `src/engine/engine_init.cpp` | Browser entry shows correct values |
| **R7** | Instantiate `SteamAuthManager` from the engine and call `Update()` (== `SteamGameServer_RunCallbacks`) every tick. Shut down cleanly on signal. | `include/engine/engine.h`, `src/engine/engine_init.cpp`, `src/main.cpp` | Callbacks fire; clean shutdown |
| **R8** | Add an `IPlayerAuthSink` (interface) that receives auth results from `SteamAuthManager` and route into the future `PlayerManager`. For Phase 1 this is a logging stub. | `include/auth/steam_auth.h`, `src/auth/steam_auth.cpp` | Decouples auth from Phase 2 work |
| **R9** | Implement `SteamGameServer_InitEx` error-path: on `k_ESteamAPIInitResult_FailedGeneric` / `VersionMismatch` etc., log the populated `SteamErrMsg` and exit cleanly | `src/auth/steam_auth.cpp` | Diagnostics for misbuilds / wrong SDK |
| **R10** | Smoke test: build, launch with sandbox AppID 480, point a Steam client server browser at the LAN tab, confirm appearance | `docs/PHASE1_BUILD_INSTRUCTIONS.md` | Phase 1 done |

After R1–R10, Phase 1 success criteria in `docs/IMPLEMENTATION_ROADMAP.md` are met.

### Phase 2 hooks (future, not implemented now)
- AA3 client → server "auth blob" path: client calls `ISteamUser::GetAuthSessionTicket`, sends ticket bytes inline in the AA3 join packet. Server passes it to `BeginAuthSession` (R8 sink).
- Original AA3 GameServer port was `8777` (RE: `REVERSE_ENGINEERING_NOTES.md`). Phase 2 may switch the default from 7777 → 8777 to match shipping clients.
- AA3 used MAS (Master Server) for browser; replaced entirely by Steam master server through `SetAdvertiseServerActive(true)`.

---

## 3. Build system contract (R1, R2)

**`CMakeLists.txt` changes:**

```cmake
# Steamworks SDK paths — point at the in-tree SDK
set(STEAMWORKS_SDK_DIR "${CMAKE_SOURCE_DIR}/../steamworks_sdk_164/sdk"
    CACHE PATH "Path to Steamworks SDK root containing public/ and redistributable_bin/")
set(STEAMWORKS_LIB_DIR     "${STEAMWORKS_SDK_DIR}/redistributable_bin/linux64")
set(STEAMWORKS_INCLUDE_DIR "${STEAMWORKS_SDK_DIR}/public")

# Verify presence at configure time, fail fast otherwise
if(NOT EXISTS "${STEAMWORKS_LIB_DIR}/libsteam_api.so")
    message(FATAL_ERROR
        "libsteam_api.so not found at ${STEAMWORKS_LIB_DIR}. "
        "Set -DSTEAMWORKS_SDK_DIR=<sdk root>.")
endif()

target_link_libraries(aa3_server PRIVATE
    ${STEAMWORKS_LIB_DIR}/libsteam_api.so   # GameServer APIs live here in SDK 1.64
    pthread
    dl
)
```

`steam_appid.txt` placement (R2): leave the existing `configure_file(... COPYONLY)` rule. Document that the file must sit next to the binary at runtime; `SteamGameServer_Init` fails immediately without it.

---

## 4. SteamAuthManager – public surface (R3, R4, R6, R8)

**Header (`include/auth/steam_auth.h`)** – additions in **bold**:

```cpp
namespace aa3 { namespace auth {

struct SteamServerProperties {
    std::string product;          // e.g. "AA3"          (set before LogOn)
    std::string game_description; // e.g. "America's Army 3"
    std::string mod_dir;          // e.g. "AA3"
    std::string version;          // "x.x.x.x"
    std::string server_name;
    std::string map_name;
    std::string game_tags;        // optional, comma-separated filter tags
    int32_t     max_players = 32;
    int32_t     bot_count   = 0;
    bool        password_protected = false;
    bool        secure      = false; // true → eServerModeAuthenticationAndSecure (VAC)
};

// Phase-1 friendly sink. Phase 2's PlayerManager will implement this.
class IPlayerAuthSink {
public:
    virtual ~IPlayerAuthSink() = default;
    virtual void OnAuthResult(uint64_t steam_id,
                              uint64_t owner_steam_id,
                              bool approved,
                              int    deny_reason /* EDenyReason or 0 */) = 0;
};

class SteamAuthManager {
public:
    SteamAuthManager();
    ~SteamAuthManager();

    bool Initialize(uint32_t bind_ip /*0=INADDR_ANY*/,
                    uint16_t game_port,
                    uint16_t query_port,
                    const SteamServerProperties& props);

    // Called every engine tick.
    void RunFrame();

    // Server lifecycle
    void Shutdown();
    bool IsLoggedOn() const { return logged_on_; }
    bool IsAdvertising() const { return advertising_; }

    // Player auth API (used by network layer in Phase 2)
    bool BeginAuthSession(uint64_t steam_id,
                          const uint8_t* ticket, uint32_t ticket_len);
    void EndAuthSession  (uint64_t steam_id);

    void SetAuthSink(IPlayerAuthSink* sink) { sink_ = sink; }

private:
    // Steam callbacks (see Section 5 for macro forms)
    void OnSteamServersConnected   (SteamServersConnected_t*);
    void OnSteamServerConnectFailure(SteamServerConnectFailure_t*);
    void OnSteamServersDisconnected(SteamServersDisconnected_t*);
    void OnPolicyResponse          (GSPolicyResponse_t*);
    void OnValidateAuthTicket      (ValidateAuthTicketResponse_t*);
    void OnClientApprove           (GSClientApprove_t*);
    void OnClientDeny              (GSClientDeny_t*);
    void OnClientKick              (GSClientKick_t*);

    SteamServerProperties props_;
    bool                  initialized_ = false;
    bool                  logged_on_   = false;
    bool                  advertising_ = false;
    IPlayerAuthSink*      sink_        = nullptr;

    // CCallback<> members declared in .cpp (see §5)
};

}} // namespace
```

---

## 5. Callback registration pattern (R4) – binding details

The Steamworks dispatcher uses pack-of-template `CCallback<T, CallbackT>` instances **owned by the object** that handles the callback. They register on construction, deregister on destruction. For game-server callbacks, use the `STEAM_GAMESERVER_CALLBACK` form.

In `src/auth/steam_auth.cpp` (private members of `SteamAuthManager`, declared in the .cpp via `STEAM_GAMESERVER_CALLBACK`):

```cpp
class SteamAuthManager_Impl {  // or fold into SteamAuthManager directly
    STEAM_GAMESERVER_CALLBACK( SteamAuthManager, OnSteamServersConnected,    SteamServersConnected_t );
    STEAM_GAMESERVER_CALLBACK( SteamAuthManager, OnSteamServerConnectFailure,SteamServerConnectFailure_t );
    STEAM_GAMESERVER_CALLBACK( SteamAuthManager, OnSteamServersDisconnected, SteamServersDisconnected_t );
    STEAM_GAMESERVER_CALLBACK( SteamAuthManager, OnPolicyResponse,           GSPolicyResponse_t );
    STEAM_GAMESERVER_CALLBACK( SteamAuthManager, OnValidateAuthTicket,       ValidateAuthTicketResponse_t );
    STEAM_GAMESERVER_CALLBACK( SteamAuthManager, OnClientApprove,            GSClientApprove_t );
    STEAM_GAMESERVER_CALLBACK( SteamAuthManager, OnClientDeny,               GSClientDeny_t );
    STEAM_GAMESERVER_CALLBACK( SteamAuthManager, OnClientKick,               GSClientKick_t );
};
```

These macros expand into `CCallback<SteamAuthManager, T, true /*GameServer*/>` members initialized in the constructor's mem-init list:

```cpp
SteamAuthManager::SteamAuthManager()
    : m_OnSteamServersConnected   (this, &SteamAuthManager::OnSteamServersConnected),
      m_OnSteamServerConnectFailure(this, &SteamAuthManager::OnSteamServerConnectFailure),
      m_OnSteamServersDisconnected(this, &SteamAuthManager::OnSteamServersDisconnected),
      m_OnPolicyResponse          (this, &SteamAuthManager::OnPolicyResponse),
      m_OnValidateAuthTicket      (this, &SteamAuthManager::OnValidateAuthTicket),
      m_OnClientApprove           (this, &SteamAuthManager::OnClientApprove),
      m_OnClientDeny              (this, &SteamAuthManager::OnClientDeny),
      m_OnClientKick              (this, &SteamAuthManager::OnClientKick) {}
```

`SteamGameServer_RunCallbacks()` (called from `RunFrame()`) drives them.

---

## 6. Initialization sequence (R3, R5, R6) – authoritative order

```
Engine::Initialize(cfg)
  ├─ LoadConfig(ini)
  ├─ build SteamServerProperties from config
  └─ steam_.Initialize(0, cfg.port, cfg.query_port, props)
         │
         ├─ SteamErrMsg err = {};
         ├─ EServerMode mode = props.secure
         │       ? eServerModeAuthenticationAndSecure
         │       : eServerModeAuthentication;          // never NoAuth in production
         ├─ SteamGameServer_InitEx(0, game_port, query_port, mode, props.version, &err)
         │       └─ on != k_ESteamAPIInitResult_OK → LOG_ERROR(err); return false
         │
         ├─ ISteamGameServer* gs = SteamGameServer();
         │       └─ if !gs → return false
         │
         ├─ // immutable-after-LogOn properties
         │   gs->SetProduct        (props.product.c_str());
         │   gs->SetGameDescription(props.game_description.c_str());
         │   gs->SetModDir         (props.mod_dir.c_str());
         │   gs->SetDedicatedServer(true);
         │
         ├─ // mutable properties (safe to set before or after LogOn)
         │   gs->SetServerName     (props.server_name.c_str());
         │   gs->SetMapName        (props.map_name.c_str());
         │   gs->SetMaxPlayerCount (props.max_players);
         │   gs->SetBotPlayerCount (props.bot_count);
         │   gs->SetPasswordProtected(props.password_protected);
         │   if (!props.game_tags.empty())
         │       gs->SetGameTags(props.game_tags.c_str());
         │
         ├─ gs->LogOnAnonymous();    // ← async; OnSteamServersConnected fires later
         └─ initialized_ = true; return true;

// later, on callback:
OnSteamServersConnected:
  logged_on_ = true;
  if (!props_.secure) { gs->SetAdvertiseServerActive(true); advertising_ = true; }
  // else: wait for OnPolicyResponse(m_bSecure==true) before advertising

OnPolicyResponse:
  if (cb->m_bSecure && !advertising_) {
      gs->SetAdvertiseServerActive(true);
      advertising_ = true;
  }
```

**Engine tick (R7):**
```
Engine::Run() loop body:
   steam_.RunFrame();          // SteamGameServer_RunCallbacks()
   network_.Poll();            // Phase 2
   game_.Tick(delta_time_);    // Phase 3
   // 60Hz pacing as today
```

**Shutdown:**
```
SteamAuthManager::Shutdown():
  if (gs && advertising_) gs->SetAdvertiseServerActive(false);
  if (gs && logged_on_)   gs->LogOff();
  SteamGameServer_Shutdown();
  initialized_ = logged_on_ = advertising_ = false;
```

---

## 7. Player authentication flow (R8, Phase-2-ready)

```
client (Phase 2)                          aa3_server (now)
─────────────                              ──────────────
ISteamUser::GetAuthSessionTicket           SocketManager receives JOIN packet
embed ticket in AA3 join packet            extract steam_id + ticket bytes
─────────────────► (UDP) ─────────────►   SteamAuthManager::BeginAuthSession
                                             gs->BeginAuthSession(ticket, len, steamID)
                                             switch (EBeginAuthSessionResult):
                                               OK             → wait for callback
                                               *              → IPlayerAuthSink::OnAuthResult(deny)

… ValidateAuthTicketResponse_t fires async …

OnValidateAuthTicket:
   if m_eAuthSessionResponse == k_EAuthSessionResponseOK:
       sink_->OnAuthResult(steam_id, owner_steam_id, true, 0)
   else:
       sink_->OnAuthResult(steam_id, 0, false, m_eAuthSessionResponse)

On disconnect / kick / leave:
   network layer → SteamAuthManager::EndAuthSession(steam_id)
                       → gs->EndAuthSession(CSteamID(steam_id))
```

`GSClientApprove_t` / `GSClientDeny_t` are the **legacy** path (deprecated `SendUserConnectAndAuthenticate`). We do not use it. Keep the callback handlers but log-and-discard so we notice if something else triggers them.

---

## 7a. Standalone (no-Steam) mode

The server supports running without Steam at all — useful for development, LAN-only play, or environments where the local Steam client is missing or older than the SDK's required interface version.

**Activation (any of):**
- INI: `Standalone = 1`
- CLI: `--standalone` (or `--no-steam`)
- CLI override back to Steam: `--steam`

**Behavior in standalone mode:**
- `SteamAuthManager::Initialize` is **not** called.
- `SteamGameServer_RunCallbacks` is not invoked per tick.
- No master-server listing — players connect by direct IP.
- `BeginAuthSession` / `EndAuthSession` are still callable on the manager but return false / no-op (manager reports `IsInitialized() == false`); Phase 2's network layer must skip auth in standalone.
- Shutdown skips Steam teardown.

**Engine gating:** `Engine` holds a `steam_active_` flag set true only when Steam mode actually finishes initializing successfully. `RunFrame()` and `Shutdown()` paths gate on this flag, so a Steam init failure does not leave a dangling shutdown.

**CLI precedence:** `--standalone` > `--steam` > INI `Standalone`. The chosen mode is logged at INFO on startup so operators can confirm.

## 8. Configuration mapping (R6)

`config/aa3_server.ini` → `SteamServerProperties`:

| INI key | Field | Default | Notes |
|---|---|---|---|
| `ServerName` | `server_name` | "AA3 Server" | Browser title |
| `MapName` | `map_name` | "Compound" | Mutable; updated each round in Phase 3 |
| `GameDescription` | `game_description` | "America's Army 3" | |
| `Port` | game port | 7777 | Original AA3 used 8777 (RE notes) — see §10 |
| `QueryPort` | query port | 27015 | Steam standard |
| `MaxPlayers` | `max_players` | 32 | |
| `SteamAppID` | `steam_appid.txt` content | 10 | **480** for Spacewar sandbox testing; AA3's real AppID is set when partnered |
| (new) `SecureMode` | `secure` | `false` | Toggle VAC-secure mode |
| (new) `GameTags` | `game_tags` | "" | Filter tags |
| (new) `PasswordProtected` | `password_protected` | `false` | |
| (new) `BotCount` | `bot_count` | 0 | |
| (new) `Version` | `version` | "1.0.0.0" | Used for browser version filter |
| (new) `Standalone` | (engine, not Steam) | `0` | `1` skips Steam init entirely (see §7a) |

`steam_appid.txt` and `SteamAppID` in INI must match. Add a one-line check at startup that warns if they diverge.

---

## 9. File-by-file change list (binding)

```
CMakeLists.txt
   ├─ STEAMWORKS_SDK_DIR → ../steamworks_sdk_164/sdk
   ├─ link libsteam_api.so (drop libsteamgameserver.so)
   └─ add configure-time existence check on libsteam_api.so

include/auth/steam_auth.h
   ├─ replace existing SteamAuthManager API with §4 surface
   ├─ add SteamServerProperties struct
   └─ add IPlayerAuthSink interface

src/auth/steam_auth.cpp
   ├─ delete ValidateAuthTicketResponseHandler scaffolding
   ├─ implement SteamAuthManager per §5 + §6
   ├─ STEAM_GAMESERVER_CALLBACK members for the 8 callbacks listed in §5
   ├─ replace EnableHeartbeats(true) with deferred SetAdvertiseServerActive(true)
   ├─ implement RunFrame() → SteamGameServer_RunCallbacks()
   └─ implement Shutdown() in the order in §6

include/engine/engine.h
   └─ add member: aa3::auth::SteamAuthManager steam_;

src/engine/engine_init.cpp
   ├─ Initialize(): build SteamServerProperties from config, call steam_.Initialize(...)
   ├─ Run(): call steam_.RunFrame() at top of each tick
   └─ Shutdown(): call steam_.Shutdown() before returning

src/main.cpp
   └─ no API change; existing g_engine path already covers signal-driven shutdown

include/util/logging.h
   └─ no change

config/aa3_server.ini
   └─ add SecureMode, GameTags, PasswordProtected, BotCount, Version keys (commented defaults)

config/steam_appid.txt
   └─ keep 10 for now; document switch to 480 (Spacewar) for SDK smoke test, then real AA3 AppID
```

---

## 10. Open questions / things that block production (NOT phase-1 blockers)

1. **AA3 Steam AppID.** RE notes don't capture it. Sandbox AppID `480` (Spacewar) is fine to validate the SDK plumbing end-to-end against a real Steam client; the real ID requires a Valve partnership entry.
2. **Original GameServer port `8777` (`0x2249`) vs. `7777`.** Phase 1 doesn't care; Phase 2 (when actual AA3 clients try to connect) must default to whatever the shipping client expects.
3. **VAC.** `secure=false` until the AppID is real — VAC-secure mode requires AppID-bound server keys.
4. **Old `MAS` traffic on the AA3 client.** Original client also pinged AA3-proprietary MAS endpoints. With dead AA3 backends, the client may log warnings but should still accept the Steam-discovered server. Confirm in Phase 5 client-integration testing.
5. **Soldier profiles.** AA3 used `SteamClientLoadSoldierProfileIndexFile` / `SteamClientSaveSoldierProfileFile` (RE notes). Those are *client-side* persistence — out of scope for the dedicated server.

---

## 11. Validation checklist (Phase 1 done = all green)

- [ ] `cmake .. && make -j` succeeds against `steamworks_sdk_164/sdk`.
- [ ] Binary launches with `steam_appid.txt` present.
- [ ] Log shows `SteamGameServer_InitEx` succeeded (no `SteamErrMsg`).
- [ ] Log shows `OnSteamServersConnected` within ~5s of start.
- [ ] Log shows `SetAdvertiseServerActive(true)` called.
- [ ] On `SIGINT`: `SetAdvertiseServerActive(false)` → `LogOff` → `SteamGameServer_Shutdown` → process exits 0.
- [ ] With AppID 480 + a Steam client on the LAN, the server appears in the LAN tab of the Steam server browser with the configured name, map, max players.
- [ ] `BeginAuthSession`/`EndAuthSession` callable through the public API without crashing (test via a temporary stub call from `Engine::Initialize`).

When all 8 boxes are checked, R1–R10 are done and Phase 1 is closed.
