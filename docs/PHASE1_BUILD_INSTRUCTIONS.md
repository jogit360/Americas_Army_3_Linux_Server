# Phase 1: Steamworks SDK Integration - Build Instructions

## Overview
This document provides step-by-step instructions for building the AA3 Linux Server with Steamworks SDK integration.

## Prerequisites

### System Requirements
- Linux x86_64 system (Ubuntu 20.04 LTS or later recommended)
- GCC 10+ or Clang 12+
- CMake 3.16 or later
- git

### Required Packages
```bash
sudo apt-get update
sudo apt-get install -y \
    build-essential \
    cmake \
    libpthread-stubs0-dev \
    pkg-config
```

## Step 1: Download Steamworks SDK

The Steamworks SDK is available to Valve partners at https://partner.steamgames.com/

1. Log in to the Steamworks partner portal
2. Navigate to "Tools" → "Steamworks SDK"
3. Download the latest Steamworks SDK for Linux
4. Extract to `external/steamworks_sdk_linux/`

```bash
cd aa3-linux-server
mkdir -p external
unzip -q SteamworksSDK.zip -d external/
mv SteamworksSDK steamworks_sdk_linux
```

### Steamworks SDK Structure
```
external/steamworks_sdk_linux/
├── public/
│   ├── steam/
│   │   ├── steam_gameserver.h       (Critical for server implementation)
│   │   ├── steam_api.h
│   │   ├── isteamgameserver.h
│   │   └── ... (100+ other headers)
│   └── ... (client API headers)
├── redistributable_bin/
│   ├── linux64/
│   │   ├── libsteamgameserver.so   (Critical library)
│   │   ├── libsteam_api.so
│   │   └── steam_appid.txt
│   └── ... (other architectures)
├── sample/
│   └── SampleGameServer/ (Example implementation)
└── ... (documentation)
```

### Key Files Needed
- `public/steam/steam_gameserver.h` - GameServer API declarations
- `public/steam/isteamgameserver.h` - ISteamGameServer interface
- `public/steam/steamtypes.h` - Type definitions
- `redistributable_bin/linux64/libsteamgameserver.so` - Compiled library

## Step 2: Verify SDK Installation

```bash
# Check SDK structure
ls -la external/steamworks_sdk_linux/public/steam/ | grep gameserver
ls -la external/steamworks_sdk_linux/redistributable_bin/linux64/

# Should show:
# - steam_gameserver.h
# - isteamgameserver.h
# - libsteamgameserver.so
```

## Step 3: Build the Project

```bash
# Create build directory
mkdir -p build
cd build

# Run CMake
cmake ..

# Build
make -j$(nproc)

# Output: aa3_server executable
ls -la aa3_server
```

## Step 4: Configure Steam App ID

**CRITICAL**: The correct Steam App ID must be set before running the server.

Edit `config/steam_appid.txt`:
```
# Production (replace with actual AA3 Steam App ID when available)
# For testing, use AppID 10 (which is designed for testing)
10
```

The `steam_appid.txt` file is also copied to the build directory by CMake.

## Step 5: Run the Server

```bash
# From build directory
./aa3_server -c ../config/aa3_server.ini

# Or with custom config
./aa3_server -c /path/to/custom_config.ini
```

### Expected Output
```
[TIMESTAMP] [INFO] ========================================
[TIMESTAMP] [INFO] AA3 Linux Server - Starting
[TIMESTAMP] [INFO] ========================================
[TIMESTAMP] [INFO] Config file: ../config/aa3_server.ini
[TIMESTAMP] [INFO] Engine initialized successfully
[TIMESTAMP] [INFO] SteamGameServer initialized successfully
[TIMESTAMP] [INFO] Starting main game loop
```

## Step 6: Verify Steam Integration

The server should:
1. Successfully load Steamworks SDK
2. Initialize ISteamGameServer
3. Log on anonymously to Steam
4. Start the game loop
5. Accept client authentication via Steam tickets

Check logs:
```bash
tail -f aa3_server.log | grep "Steam"
```

## Troubleshooting

### "libsteamgameserver.so not found"
- Ensure `STEAMWORKS_LIB_DIR` in CMakeLists.txt points to correct path
- Verify library exists: `ls -la external/steamworks_sdk_linux/redistributable_bin/linux64/libsteamgameserver.so`
- Check build RPATH: `ldd ./aa3_server | grep steam`

### "SteamGameServer_Init failed"
- Verify `steam_appid.txt` exists in the same directory as executable
- Content must be a valid integer (10 for testing)
- Check log for detailed error messages

### "Cannot get ISteamGameServer interface"
- Ensure `SteamGameServer()` is called after `SteamGameServer_Init()`
- Check that Steam is running on system (for local testing)
- Verify Steamworks SDK is correctly installed

### Compilation Errors
- Ensure C++17 support: `g++ --version` should be GCC 7+
- Verify all Steamworks headers are in include path
- Check for typos in steam_gameserver.h includes

## Next Steps

Phase 1 is now complete. You can proceed to:

### Phase 1.5: Test Steam Integration (2-3 hours)
- Write unit tests for SteamAuthManager
- Test BeginAuthSession() with mock Steam tickets
- Test server property updates (name, map, player count)
- Verify heartbeats are being sent

### Phase 2: Network Layer Implementation (8-10 hours)
- Implement UDP socket handling (socket_manager.cpp)
- Implement player connection/disconnection handling
- Implement state message serialization/deserialization
- Implement player state synchronization

### Phase 3: Game Logic Implementation (20-30 hours)
- Implement player spawning and team assignment
- Implement player damage and health
- Implement ammunition and weapon systems
- Implement objective gameplay
- Implement scoring and match completion

## Reference

- [Steamworks SDK Documentation](https://partner.steamgames.com/doc/)
- [ISteamGameServer Interface](https://partner.steamgames.com/doc/api/ISteamGameServer)
- [SteamworksSDK/sample/SampleGameServer/](Example C++ server implementation)
- [Linux Dedicated Server FAQ](https://partner.steamgames.com/doc/sdk/uploading_builds/linux)

