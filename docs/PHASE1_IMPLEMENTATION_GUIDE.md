# Phase 1: Steamworks SDK Integration - Implementation Guide

## Architecture Overview

The Phase 1 implementation replaces the original AA3 authentication system with Steamworks SDK. This simplifies server authentication while leveraging Steam's proven infrastructure.

```
┌─────────────────────────────────────────────┐
│         AA3 Linux Server                    │
├─────────────────────────────────────────────┤
│                                             │
│  Engine (main.cpp)                          │
│  ├─ Config Loading (engine_config.cpp)     │
│  ├─ Timing System (engine_init.cpp)        │
│  ├─ Main Loop (engine_init.cpp)            │
│  │                                          │
│  ├─ SteamAuthManager (steam_auth.cpp)      │
│  │  ├─ SteamGameServer_Init()              │
│  │  ├─ ISteamGameServer API calls          │
│  │  ├─ BeginAuthSession()                  │
│  │  └─ Callback handling                   │
│  │                                          │
│  ├─ SocketManager (socket_manager.cpp)    │
│  │  ├─ UDP socket creation                │
│  │  ├─ Non-blocking I/O                   │
│  │  └─ Client connections                 │
│  │                                          │
│  └─ GameLoop (future)                     │
│     ├─ Player management                  │
│     └─ State synchronization               │
│                                             │
└──────────────────┬──────────────────────────┘
                   │
                   ▼
         ┌─────────────────────┐
         │  Steamworks SDK     │
         │ (libsteamgameserver │
         │        .so)         │
         ├─────────────────────┤
         │ - Steam backend     │
         │ - Authentication    │
         │ - Server registry   │
         │ - Heartbeats        │
         │ - Callbacks         │
         └─────────────────────┘
```

## Critical Code Sections

### 1. Engine Initialization (`src/engine/engine_init.cpp`)

The engine serves as the central coordinator:

```cpp
// Timeline:
// 1. Load config from INI file
// 2. Initialize SteamAuthManager
// 3. Start main loop
// 4. On loop tick: Update Steam, Poll sockets, Update game logic
```

Key responsibilities:
- Config file loading and validation
- Timing system (Delta time calculation)
- Main game loop orchestration
- Graceful shutdown on signals

### 2. Steamworks SDK Integration (`src/auth/steam_auth.cpp`)

This is the core Phase 1 implementation:

```cpp
bool SteamAuthManager::Initialize(const std::string& app_id, 
                                  uint16_t game_port, 
                                  uint16_t query_port)
{
    // Step 1: Initialize Steamworks SDK
    if (!SteamGameServer_Init(0, game_port, query_port, 
                              eServerModeAuthentication, "1.0.0")) {
        return false;
    }

    // Step 2: Get ISteamGameServer interface
    game_server_ = SteamGameServer();

    // Step 3: Configure server properties
    game_server_->SetProduct("AA3");
    game_server_->SetGameDescription("America's Army 3");
    game_server_->SetDedicatedServer(true);
    game_server_->EnableHeartbeats(true);

    // Step 4: Log on anonymously
    game_server_->LogOnAnonymous();

    return true;
}
```

### 3. Player Authentication Flow

When a client connects with a Steam authentication ticket:

```cpp
bool SteamAuthManager::BeginAuthSession(uint64_t steam_id, 
                                        const uint8_t* auth_ticket, 
                                        uint32_t ticket_len)
{
    CSteamID steamid(steam_id);
    EBeginAuthSessionResult result = game_server_->BeginAuthSession(
        (const void*)auth_ticket,
        ticket_len,
        steamid
    );

    // Steam will fire ValidateAuthTicketResponse_t callback when validated
    // Callback result: k_EAuthSessionResponseOK = player authenticated
    return result == k_EBeginAuthSessionResultOK;
}
```

Callback handling:

```cpp
void OnValidateAuthTicket(ValidateAuthTicketResponse_t* pCallback)
{
    if (pCallback->m_eAuthSessionResponse == k_EAuthSessionResponseOK) {
        // Player authenticated - create game player object
        uint64_t steam_id = pCallback->m_SteamID.ConvertToUint64();
        CreatePlayer(steam_id);
    } else {
        // Authentication failed - reject connection
        uint64_t steam_id = pCallback->m_SteamID.ConvertToUint64();
        DisconnectPlayer(steam_id);
    }
}
```

### 4. Main Game Loop Integration

```cpp
void Engine::Run()
{
    while (is_running_) {
        // 1. Calculate delta time
        delta_time_ = CalculateDeltaTime();
        
        // 2. Update Steamworks (process callbacks)
        steam_auth_manager_->Update();
        
        // 3. Poll network sockets
        socket_manager_->Poll();
        
        // 4. Update game state
        UpdateGameLogic(delta_time_);
        
        // 5. Send state to clients
        BroadcastPlayerStates();
        
        // 6. Sleep to maintain ~60 Hz tick rate
        SleepUntilNextTick();
    }
}
```

## Steamworks SDK Functions Used

### Initialization
- `SteamGameServer_Init()` - Initialize the SDK
- `SteamGameServer()` - Get ISteamGameServer interface
- `SteamGameServer_Shutdown()` - Clean shutdown

### Server Properties
- `ISteamGameServer::SetProduct()` - Game identifier
- `ISteamGameServer::SetGameDescription()` - Server description
- `ISteamGameServer::SetMapName()` - Current map
- `ISteamGameServer::SetServerName()` - Server display name
- `ISteamGameServer::SetMaxPlayerCount()` - Player limit
- `ISteamGameServer::SetDedicatedServer()` - Dedicated mode flag

### Authentication
- `ISteamGameServer::BeginAuthSession()` - Start authentication
- `ISteamGameServer::EndAuthSession()` - End authentication
- `ISteamGameServer::LogOnAnonymous()` - Log server on

### Heartbeats & Updates
- `ISteamGameServer::EnableHeartbeats()` - Enable periodic updates
- `ISteamGameServer::LogOff()` - Disconnect from Steam
- `SteamGameServer_RunCallbacks()` - Process Steam callbacks

## Configuration System

The INI-based config system provides:

```ini
# Server Identity
ServerName = "AA3 Server"
MapName = "Compound"

# Network
Port = 7777
QueryPort = 27015
MaxPlayers = 32

# Steam
SteamAppID = 10
```

Config is loaded from file and accessible via:
```cpp
std::string name = engine.GetConfigValue("ServerName", "Default");
int max_players = engine.GetConfigInt("MaxPlayers", 32);
```

## Error Handling Strategy

### Critical Errors (Fail and Exit)
- SteamGameServer_Init() failure
- ISteamGameServer interface not available
- Config file parse errors
- Socket bind failure

### Recoverable Errors (Log and Continue)
- Player authentication timeout
- Network send failures
- Invalid player commands

### Network Errors (Retry/Disconnect)
- Socket recv() errors
- Client disconnection
- Malformed packet data

## Testing Strategy for Phase 1

### Unit Tests Needed
1. **Config Loading**
   - Test INI parsing
   - Test default values
   - Test invalid config handling

2. **SteamAuthManager**
   - Test Initialize() with mocked SDK
   - Test SetServerProperties()
   - Test BeginAuthSession()
   - Test callback handling

3. **SocketManager**
   - Test socket creation
   - Test bind to port
   - Test non-blocking operations

### Integration Tests Needed
1. **Full Startup Sequence**
   - Load config → Init Steam → Start loop → Shutdown

2. **Player Authentication Flow**
   - Generate mock Steam ticket → BeginAuthSession → Verify callback

3. **Server Registry**
   - Verify server appears in Steam community servers list

## Performance Considerations

### Timing System
- Main loop: 60 Hz (16.67 ms per tick)
- Delta time: Calculated via `std::chrono::high_resolution_clock`
- Precision: Microsecond-level timing

### Memory Management
- Config: Stored in std::map (negligible)
- Players: Dynamic vector with pre-allocation
- Sockets: One UDP socket per server
- Steamworks: Managed by SDK

### Network Bandwidth
- Outgoing: Server-authoritative state (116-byte player messages × max players)
- Incoming: Client input commands (minimal)
- Heartbeats: ~1 KB every 30 seconds to Steam

## Known Limitations & Future Work

### Phase 1 Limitations
1. ⚠️ No actual network client handling yet (socket polling reads only)
2. ⚠️ No player state tracking yet
3. ⚠️ No game logic yet (damage, objectives, scoring)
4. ⚠️ No VAC (Valve Anti-Cheat) integration yet
5. ⚠️ SteamAppID set to 10 (testing ID, needs production AA3 ID)

### Phase 2 Requirements
- Implement full UDP socket handling
- Player connection management
- State message serialization
- Client input processing

### Phase 3 Requirements
- Game loop physics
- Weapon/ammunition system
- Objective system
- Team management
- Scoring

## Debugging

Enable debug logging:
```cpp
LOG_SET_LEVEL(LOG_LEVEL_DEBUG);  // Show all debug messages
```

Check for Steamworks issues:
```bash
# Verify steam_appid.txt is in same directory as executable
cat steam_appid.txt
# Should output: 10 (or production AppID)

# Check library linking
ldd ./aa3_server | grep steam
# Should show: libsteamgameserver.so => ...
```

## References

- Steamworks SDK: `/external/steamworks_sdk_linux/public/steam/steam_gameserver.h`
- Sample Implementation: `/external/steamworks_sdk_linux/sample/SampleGameServer/`
- Build System: `/CMakeLists.txt`
- Main Loop: `/src/engine/engine_init.cpp`

