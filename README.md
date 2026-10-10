# AA3 Linux Server Implementation

//Currently THIS IS STILL A WORK IN PROGRESS -- tested with the real AA3 client
//(Wine) and with scripted bot clients, not yet with a full server of players.
//A player can join, pick a team and role, spawn on Bridge, move, shoot, open
//doors, vault, climb, get wounded/incapacitated, and play full rounds and matches.

A Linux dedicated server for America's Army 3 (v3.3.1), reimplemented from
the retail server's network traffic, the cooked game packages and the
original UnrealScript source that ships inside them.

## Quick Start

### Prerequisites
- Linux x86_64, GCC 10+ or Clang 12+, CMake 3.16+
- Steamworks SDK in `external/steamworks_sdk_linux/` (only for Steam mode;
  `--standalone` runs without it)
- A legit AA3 install: the map data in `config/maps/` and `data/collision/`
  is generated from its cooked packages

### Build & Run
```bash
cd aa3-linux-server
mkdir -p build && cd build
cmake .. && make -j$(nproc)
cd ..                                   # the server reads config/ relative to here
build/aa3_server -c config/aa3_server.ini --standalone
```
Clients connect by IP (console `open <ip>:8777`) or through the in-game LAN
browser. `--steam` (default) also advertises to the Steam master server.

## Project Status

**Playable on Bridge (classic mode).** Last updated 2026-10-10.

| Area | Status |
|------|--------|
| Handshake, Steam auth (or standalone), map handoff (`WELCOME` + `USES`) | done, retail-exact |
| Packet layer: bunches, reliability, retransmission, acks | done |
| Actor replication: WorldInfo, GRI, TeamInfo, PRI, PC, Pawn, inventory, round actors | done |
| Client RPC decoding (movement, posture, fire, reload, use...) | done (99.998% of retail bunches) |
| LAN browser beacon (in-game server list + Join) | done |
| Team join, role selection, deployment, spawn with per-role kits | done |
| Movement, stance, sprint, vault/mantle, ladders, hold breath | done |
| Map collision (static meshes, terrain, BSP) + server-side bullet traces | done |
| Weapons: fire modes, ammo, reloads; breakable windows; doors; ES2 objects | done |
| Damage by hit zone, incapacitation (bleed-out, withdraw), death | done |
| Round end: elimination, extraction, time limit; scoreboard; next round; 6-round match with side swap | done (bot-tested) |
| Stats (server-authoritative, retail-exact encoding, JSON export + web viewer) | done; few counters fed yet |
| Medical treatment (healing a downed teammate) | **blocked**: client offers no treat option yet |
| Dropped weapons, grenades, M320, TAH/VIP/other modes, map rotation | not started |

Full write-up, section by section with the retail evidence:
`docs/AA3_ACTOR_REPLICATION.md` (sections 1-27, 36; 28-35 are only in commit messages so far).

## Directory Structure

```
aa3-linux-server/
├── src/
│   ├── main.cpp                 entry point, CLI flags
│   ├── engine/                  config, engine loop, map manifests, package reader
│   ├── auth/steam_auth.cpp      Steamworks game server (auth + advertise)
│   ├── network/
│   │   ├── socket_manager.cpp   UDP
│   │   ├── protocol_handler.cpp handshake, channels, reliability, per-client replication
│   │   ├── rep_encode.cpp       property replication (NetSerializeItem formats)
│   │   ├── rpc_encode.cpp       server -> client RPCs
│   │   ├── rpc_decode.cpp       client -> server RPCs
│   │   ├── lan_beacon.cpp       LAN browser (ACPU beacon on 9000/9001)
│   │   └── a2s_info.cpp         Steam A2S query replies
│   ├── game/
│   │   ├── world.cpp            game state: teams, rounds, spawns, weapons, damage
│   │   ├── collision.cpp        BVH + line traces over the map's collision
│   │   └── stats_export.cpp     stats JSON
│   └── util/                    bit streams, JSON, CRC, logging
├── include/                     headers; network/rep_layout_gen.h = generated RepIndex tables
├── config/
│   ├── aa3_server.ini           server configuration
│   ├── spawn_inventory.json     the spawn kit, extracted from retail
│   └── maps/                    per-map data: *.aa3.manifest.json (USES), *.round.json,
│                                *.join.json, *.match.json, *.map_actors.json,
│                                *.windows.json, *.rules.json
├── data/collision/              <map>.col, generated (git-ignored)
├── tools/                       aa3_build_manifests (package dumper/manifest builder),
│                                gen_rep_layout.py, stats_web.py ...
├── tests/                       unit tests + fake_client.py / bot_client.py
└── docs/                        protocol and replication notes
```

Companion tools in the parent repo: `aa3-tools/map_manifest/` (pcap
decoders, round/map actor and mission-rule extractors) and
`aa3-tools/collision/` (collision export).

## Configuration

`config/aa3_server.ini` (selected keys):

```ini
ServerName = "AA3 Server"
MapName = "Bridge_night_clear"   # base map; needs config/maps/<MapName>.aa3.manifest.json
GameModeLevel = ""               # gametype sublevel; empty = <Base>_inf_classic_large_dev
Port = 8777
QueryPort = 27015
Standalone = 0                   # 1 = no Steam (also --standalone)
LanBeacon = 1                    # in-game LAN browser
RoleSelectionTime = 10           # seconds (retail 10)
DeploymentTime = 10              # seconds (retail 10)
MissionTimeLimit = 420           # round length, seconds (retail Bridge classic 420)
RespawnDelay = 0                 # seconds; 0 = no respawn until the next round (retail classic)
IncapBleeding = true             # gunshot-incapacitated soldiers bleed out
SpawnPoint0 = "x,y,z,yaw"        # per team; defaults to retail's Black spawn
StatsFile = ""                   # write player stats JSON here
```

## Generating map data

Per-map files come from a legit install's cooked packages (decompressed
copies in `../clean_packages/`) and retail captures:

```bash
# collision
python3 ../aa3-tools/collision/export_collision.py Bridge_night_clear data/collision/Bridge_night_clear.col
# end-of-round rules (extraction volumes, viewpoints)
python3 ../aa3-tools/map_manifest/extract_mission_rules.py --level Bridge_inf_classic_large_dev \
    --map Bridge_night_clear --out config/maps/Bridge_inf_classic_large_dev.rules.json
```
Round/join/match actor data: `aa3-tools/map_manifest/extract_round_actors.py`
(needs a retail capture of that map). Only Bridge has the full set today.

## Testing

```bash
# scripted players against a running server (port from the ini)
python3 tests/bot_client.py --port 8777 --team 1 --role 1 --no-fire           # a target
python3 tests/bot_client.py --port 8777 --team 0 --role 1 --rate 2            # shoots the nearest soldier
python3 tests/bot_client.py --port 8777 --team 0 --no-fire --goto=-6612,2900,40   # walks into Bridge's extraction zone
# decode a capture against a class's replication layout
python3 ../aa3-tools/map_manifest/decode_rep.py --pcap cap.pcapng --frame 1 --to-frame 99999 \
    --ch 4 --class AA_GameReplicationInfo --cdo --server-port 8777
```
Unit tests (`build/rep_roundtrip_test`, `rpc_decode_pcap_test`,
`lan_beacon_test`, `collision_test`) take fixture arguments; see their
sections in `docs/AA3_ACTOR_REPLICATION.md`.

The real client runs under Wine against this server; see the notes in the
parent repo's session memory / docs.

## Reverse-engineering sources

- Retail captures (`../GamePlayPCAP_Legit_Server.pcapng` and later live
  captures): ground truth for every wire format and timing.
- `../script_source/<Package>/<Class>.uc`: the original UnrealScript source,
  extracted from the `TextBuffer` exports still inside the cooked `.u`
  packages. Game rules come from here; natives are only in the exe.
- `AA3Game.exe` (Ghidra project in `../AA3_Decomp`, live gdb on the retail
  server/client) for native code.

## Known Limitations

- One map's data set (Bridge classic). Other maps need their manifests,
  collision, actor and rules files generated.
- No server-side movement physics: client locations are trusted.
- Healing doesn't work yet (the client never sends the treatment request).
- Side swap (round 4) only flips which team attacks; roles and spawn points
  don't swap. After round 6 the match restarts on the same map.
- Dropped weapon pickups, grenades and other game modes are not implemented.
- No anti-cheat.

## Next Steps

1. Real-client test of round end, scoreboard and spectator camera.
2. Medical treatment: read the client's `CanTreat_Pawn` path in
   `script_source/AA3Baseline/AA_PlayerController.uc` / `AA_UIManager_Medic.uc`.
3. Dropped weapons (`AA_DroppedPickup_*`), team-1 spawn points, full side swap.
4. Data for more maps, then map rotation.

## License

This project is reverse-engineered from America's Army 3. Use for educational purposes only.

---

**Status**: Active development -- playable rounds on Bridge
**Last Updated**: 2026-10-10
