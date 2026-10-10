# AA3 Linux Server Implementation

//Currently THIS IS STILL A WORK IN PROGRESS and MINIMAL TESTING HAS BEEN DONE
//BUT it does seem to load a player on the MAP and does allow the player to move around it and shoot

//To Be Continued


A Linux dedicated server implementation for America's Army 3.


## Quick Start

### Prerequisites
- Linux x86_64 (Ubuntu 20.04 LTS or later)
- GCC 10+ or Clang 12+
- CMake 3.16+
- Steamworks SDK (from partner.steamgames.com)

### Build & Run
```bash
# Download Steamworks SDK and extract to external/steamworks_sdk_linux/
cd aa3-linux-server
mkdir build && cd build
cmake ..
make -j$(nproc)

# Run server
./aa3_server -c ../config/aa3_server.ini
```

See `docs/PHASE1_BUILD_INSTRUCTIONS.md` for detailed build instructions.

## Project Status

- **Current Phase**: Phase 1 (Steamworks SDK Integration)
- **Estimated Total Duration**: 72-95 hours
- **Progress**: In development

## Directory Structure

```
aa3-linux-server/
├── src/
│   ├── main.cpp                      Entry point
│   ├── engine/
│   │   ├── engine_init.cpp          Engine lifecycle
│   │   └── engine_config.cpp        Config file parsing
│   ├── auth/
│   │   └── steam_auth.cpp           Steamworks SDK integration
│   ├── network/
│   │   ├── socket_manager.cpp       UDP socket layer
│   │   └── network_layer.cpp        Network protocol
│   ├── game/
│   │   ├── game_loop.cpp            Main game loop
│   │   └── player_manager.cpp       Player management
│   └── util/
│       └── logging.cpp              Logging system
│
├── include/
│   ├── engine/engine.h
│   ├── auth/steam_auth.h
│   ├── network/network.h
│   ├── game/game.h
│   └── util/logging.h
│
├── config/
│   ├── aa3_server.ini               Server configuration
│   └── steam_appid.txt              Steam app identifier
│
├── external/
│   └── steamworks_sdk_linux/        (Download from partner.steamgames.com)
│       ├── public/steam/
│       └── redistributable_bin/linux64/
│
├── docs/
│   ├── IMPLEMENTATION_ROADMAP.md    Overall project plan
│   ├── PHASE1_BUILD_INSTRUCTIONS.md Build guide
│   ├── PHASE1_IMPLEMENTATION_GUIDE.md Implementation details
│   └── ...
│
├── CMakeLists.txt                  Build configuration
└── README.md                        This file
```

## Architecture Overview

The server uses a modular architecture with clear separation of concerns:

```
┌─────────────────────────────────────────┐
│         Main Loop (60 Hz)               │
├─────────────────────────────────────────┤
│  ├─ Update Steamworks (auth callbacks) │
│  ├─ Poll network sockets                │
│  ├─ Update game logic                   │
│  └─ Broadcast player states             │
└─────────────────────────────────────────┘
         ▲              ▲         ▲
         │              │         │
    ┌────┴──┐  ┌───────┴──┐  ┌──┴───┐
    │Steam  │  │Network   │  │Game  │
    │Auth   │  │Layer     │  │Logic │
    └───────┘  └──────────┘  └──────┘
```

## Key Components

### SteamAuthManager (src/auth/steam_auth.cpp)
Handles Steam authentication via Steamworks SDK:
- Initializes `ISteamGameServer` interface
- Manages player authentication sessions
- Processes Steam callbacks
- Handles server heartbeats

### SocketManager (src/network/socket_manager.cpp)
Implements UDP network communication:
- UDP socket creation and binding
- Non-blocking I/O
- Client packet handling
- State broadcasting

### Engine (src/engine/)
Central game engine:
- Config file loading (INI format)
- Timing system (delta time calculation)
- Main game loop orchestration
- Subsystem coordination

## Configuration

Server configuration is stored in `config/aa3_server.ini`:

```ini
ServerName = "AA3 Server"
MapName = "Compound"
Port = 7777
QueryPort = 27015
MaxPlayers = 32
SteamAppID = 10
```

See `config/aa3_server.ini` for all available options.

## Building from Source

### Full Build Instructions
See `docs/PHASE1_BUILD_INSTRUCTIONS.md` for:
- Detailed prerequisites
- SDK download & installation
- Compilation steps
- Troubleshooting guide

### Quick Build
```bash
mkdir build && cd build
cmake ..
make
./aa3_server
```

## Running the Server

```bash
# Default config
./aa3_server

# Custom config
./aa3_server -c /path/to/config.ini

# Debug logging
LOG_LEVEL=DEBUG ./aa3_server
```

### Expected Output
```
[2026-05-03 10:15:30] [INFO] ========================================
[2026-05-03 10:15:30] [INFO] AA3 Linux Server - Starting
[2026-05-03 10:15:30] [INFO] ========================================
[2026-05-03 10:15:30] [INFO] Config file: ../config/aa3_server.ini
[2026-05-03 10:15:31] [INFO] Engine initialized successfully
[2026-05-03 10:15:31] [INFO] SteamGameServer initialized successfully
[2026-05-03 10:15:31] [INFO] Starting main game loop
```

## Development

### Project Phases

| Phase | Component | Status | Effort |
|-------|-----------|--------|--------|
| 1 | Steamworks SDK | 🔄 | 15-20 hrs |
| 2 | Network & Players | ⏳ | 12-15 hrs |
| 3 | Game Logic | ⏳ | 20-25 hrs |
| 4 | Maps & Config | ⏳ | 6-8 hrs |
| 5 | Client Integration | ⏳ | 8-10 hrs |
| 6 | Performance | ⏳ | 4-6 hrs |
| 7 | Admin Tools | ⏳ | 3-5 hrs |
| 8 | Deployment | ⏳ | 4-6 hrs |

See `docs/IMPLEMENTATION_ROADMAP.md` for complete project plan.

### Code Style

- **Language**: C++17
- **Naming**: CamelCase for classes, snake_case for functions
- **Comments**: Minimal; code should be self-documenting
- **Formatting**: 4-space indentation, opening braces on same line

### Logging

Use the logging macros defined in `include/util/logging.h`:

```cpp
LOG_DEBUG("Debug message");
LOG_INFO("Information: %d", value);
LOG_WARN("Warning: %s", message);
LOG_ERROR("Error: %s", error);
```

Set log level:
```cpp
LOG_SET_LEVEL(LOG_LEVEL_DEBUG);
```

### Testing

Unit tests for each component are needed:
```bash
# Run tests (to be implemented)
./run_tests.sh
```

## Architecture Decisions

### Authentication: Steamworks SDK ✅
- Replaces original AA3 authentication servers
- Leverages Steam's proven infrastructure
- No external server dependencies

### Network Model: Server-Authoritative ✅
- Server is source of truth for all game state
- Prevents client-side cheating
- Fair gameplay for all players

### Timing: 60 Hz Game Loop ✅
- 16.67 ms per tick
- Smooth gameplay and network sync
- Scalable to 32+ players

### Anti-Cheat: None (Optional VAC) ✅
- PunkBuster system removed (obsolete)
- Server-side validation for key events
- Optional Valve Anti-Cheat integration via Steam

## Known Limitations

- **Phase 1**: Network client handling not yet implemented
- **Phase 1**: No player state tracking yet
- **Phase 1**: No actual game logic yet
- SteamAppID set to 10 (test ID, will need production AA3 ID)

## Next Steps

1. **Immediate**: Complete Phase 1 testing and validation
2. **Phase 2**: Implement network layer and player management
3. **Phase 3**: Implement game logic (objectives, scoring, etc.)
4. **Phase 4-8**: Complete remaining phases per roadmap

See `docs/PHASE1_IMPLEMENTATION_GUIDE.md` for detailed Phase 1 info.

## Troubleshooting

### Build Issues
- Verify CMake 3.16+: `cmake --version`
- Verify C++17 support: `g++ --version` (need GCC 7+)
- Check Steamworks SDK installation: `ls external/steamworks_sdk_linux/public/steam/steam_gameserver.h`

### Runtime Issues
- Missing `steam_appid.txt`: Copy from config/ to binary directory
- SteamGameServer_Init failed: Check Steamworks SDK installation
- Socket bind error: Ensure port 7777 is available

See `docs/PHASE1_BUILD_INSTRUCTIONS.md` for more troubleshooting.

## References

### Documentation
- `docs/IMPLEMENTATION_ROADMAP.md` - Project timeline and phases
- `docs/PHASE1_IMPLEMENTATION_GUIDE.md` - Detailed Phase 1 implementation
- `docs/PHASE1_BUILD_INSTRUCTIONS.md` - Build and setup guide

### External Resources
- [Steamworks SDK](https://partner.steamgames.com/doc/sdk/)
- [ISteamGameServer API](https://partner.steamgames.com/doc/api/ISteamGameServer)
- [America's Army 3](https://www.americasarmy.com/)

### Original Analysis
- Analysis files: `/home/odeyin/Documents/AA3/DissAssembledFunctions/analysis/`
- Decompiled functions: `/home/odeyin/Documents/AA3/DissAssembledFunctions/Critical/`

## License

This project is reverse-engineered from America's Army 3. Use for educational purposes only.

## Authors

- Reverse engineering and initial analysis: Team effort
- Linux server implementation: Ongoing collaborative project
- Phase 1 implementation: 2026-05-03

---

**Status**: Active Development  
**Current Phase**: 1 (Steamworks SDK Integration)  
**Last Updated**: 2026-05-03

