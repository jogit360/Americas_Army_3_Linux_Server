# AA3 Linux Server Implementation

//Currently THIS IS STILL A WORK IN PROGRESS -- tested with the real AA3 client
//(Wine) and with scripted bot clients, not yet with a full server of players.
//A player can join, pick a team and role, spawn on Bridge, move, shoot every
//role's rifle with its sights, throw and cook grenades, pick up ES2 perks, open
//doors, vault, climb, get wounded, bleed, be treated or revived by a teammate's
//IFAK, and play full rounds and matches.

A Linux dedicated server for America's Army 3 (v3.3.1), reimplemented from
the retail server's network traffic, the cooked game packages and the
original UnrealScript source that ships inside them.

## Quick Start

### Prerequisites
- Linux x86_64, GCC 10+ or Clang 12+, CMake 3.16+
- Steamworks SDK in `external/steamworks_sdk_linux/` (only for Steam mode;
  `--standalone` runs without it)
- libzstd-dev, liblzo2-dev, OpenSSL dev headers

### Build & Run
```bash
cd aa3-linux-server
mkdir -p build && cd build
cmake .. && make -j$(nproc)
cd ..                                   # the server reads config/ relative to here
build/aa3_server -l                     # list the available maps
build/aa3_server -c config/aa3_server.ini --standalone -m Bridge_night_clear
```
Clients connect by IP (console `open <ip>:8777`) or through the in-game LAN
browser. `--steam` (default) also advertises to the Steam master server.

## Project Status

**Playable across all 18 retail maps, plus the Bridge AI co-op mission,** with zero dependencies on client packages. Last updated 2026-10-12.

| Area | Status |
|------|--------|
| Handshake, Steam auth (or standalone), map handoff (`WELCOME` + `USES`) | done, retail-exact |
| Packet layer: bunches, reliability, retransmission, acks | done |
| Actor replication: WorldInfo, GRI, TeamInfo, PRI, PC, Pawn, inventory, round actors | done |
| Client RPC decoding (movement, posture, fire, reload, use...) | done (99.998% of retail bunches) |
| LAN browser beacon (in-game server list + Join) | done |
| Team join, role selection, deployment, spawn with per-role kits | done (13 roles per team across all maps) |
| Movement, stance, sprint, vault/mantle, ladders (up and down), hold breath | done (ladder-from-the-top fix bot-tested, doc sec 40.1) |
| Map representation: Modernized Unified Map Bundles (`AA3MAP01`), shipped in the repo as `data/maps/<map>.aa3map.zst` | done (18 maps + Bridge AI, 53 MB, lossless; `aa3_mapbake --verify` 19/19 byte-identical to a fresh bake from the install) |
| Map collision: lossless instanced mesh collision (`AA3COL03`) + BVH bullet traces | done (the packages' float vertices; quantized `AA3COL04` optional) |
| Mission & Gameplay data: native extraction of map actors, round roles, rules | done (no capture JSON dependencies) |
| Weapons: every role's rifle (M4A1, M16A4, M16 DMR, M249) with its fire modes, magazine size, ammo, reloads | done |
| Rail mods: iron, CCO, ACOG variants, M95 light, XM320 mount (`ServerSetRailMod`) | done |
| Hand grenades: M67 frag, M83 smoke, M106, M84 stun, AN-M14 incendiary; take out, pin, cook, throw, fuse, blast / smoke / burn | done (real-client tested) |
| ES2 perks: grenade and magazine pickups | done |
| Friend/enemy look: the other team appears as the enemy force from each side | done |
| Breakable windows, doors, ES2 objects | done |
| Damage by hit zone; wounds that bleed out to a HealthTarget (43% at once / 57% bled at 1.2 hp/s); incapacitation (bleed-out, withdraw); death | done (retail-matched) |
| Round end: elimination, extraction, time limit; scoreboard; next round; 6-round match | done (bot-tested) |
| Multi-map CLI: `--map <name>` / `-m <name>`, `--list` / `-l` | done (18 retail maps; `-m Bridge_inf_co_large_ai` for Bridge AI) |
| Co-op "Secure and Extract": the briefcase (pickup, carry, drop on death, pickup from the ground, extraction win, reset) | done (bot-tested; Bridge AI, Airfield, Alley; doc sec 40.2) |
| Co-op reinforcements: F5 respawn queue, 10 s waves at the mission's respawn points | done (bot-tested; doc sec 40.3) |
| Enemy NPC soldiers (AI missions): 35 spawn with retail gear, spot players by line of sight, shoot back, take damage like players, neutralization notice | first cut (bot-tested; they hold position; doc sec 40.4) |
| IFAK treatment: passive treatment (stop bleeding, treat the wound), Saline revive of a downed teammate | done (bot-tested); real-client treat prompt fix awaiting test |
| Other active treatments (tourniquet, dressings...) on a conscious soldier, multiple treaters, medic scoring/voice | not started |
| Dropped weapons, M320 rounds, TAH/VIP/other modes | not started |

Full write-up, section by section with the retail evidence:
- [`docs/MAP_MODERNIZATION_SPEC.md`](file:///home/odeyin/Documents/AA3/standAlone-aa3-linuxServer/docs/MAP_MODERNIZATION_SPEC.md) (Unified Map Bundles, Quantized Collision, Mission Ingestion)
- [`docs/AA3_ACTOR_REPLICATION.md`](file:///home/odeyin/Documents/AA3/standAlone-aa3-linuxServer/docs/AA3_ACTOR_REPLICATION.md) (Protocol and replication notes)


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
│   │   ├── world.cpp            game state: teams, rounds, spawns, damage
│   │   ├── weapons.cpp          role kits, rail mods, grenades, ES2 perks
│   │   ├── medic.cpp            wounds, bleeding, IFAK treatment and revive
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
├── data/maps/                   the 18 retail maps: <map>.aa3map.zst (shipped, lossless)
├── data/collision/              <map>.col, legacy flat collision (git-ignored, dev tests)
├── tools/                       aa3_build_manifests (package dumper/manifest builder),
│                                gen_rep_layout.py, stats_web.py ...
├── tests/                       unit tests + scripted clients (fake_client, bot_client,
│                                grenade_client, medic_client, medic_scenario)
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
EnemyNPCs = 1                    # AI missions' enemy soldiers (0 = none)
MovementChecks = 1               # refuse impossible client moves (0 for scripted tests that teleport)
RespawnDelay = 0                 # seconds; 0 = no respawn until the next round (retail classic)
IncapBleeding = true             # gunshot-incapacitated soldiers bleed out
SpawnPoint0 = "x,y,z,yaw"        # per team; defaults to retail's Black spawn
StatsFile = ""                   # write player stats JSON here
```

## Map data

The maps are part of the server: `data/maps/<map>.aa3map.zst` (tracked in
git). The server loads them directly; an uncompressed `<map>.aa3map` beside one
(a local re-bake) takes precedence. Rebuilding or checking them needs an AA3
install:

```bash
# re-create the shipped bundles from the cooked packages
build/aa3_mapbake --batch-bake --ship --cooked-maps <AA3>/AA3Game/CookedPC/Maps
# prove the shipped bundles are 1:1 with the install (exit 1 on any difference)
build/aa3_mapbake --verify --cooked-maps <AA3>/AA3Game/CookedPC/Maps
# inspect one
build/aa3_mapbake --info data/maps/Bridge_night_clear.aa3map.zst
```
See `docs/MAP_MODERNIZATION_SPEC.md` sec 10.

### Legacy per-map files

Before bundles, per-map files came from a legit install's cooked packages
(decompressed copies in `../clean_packages/`) and retail captures:

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
python3 tests/bot_client.py --port 8777 --team 0 --no-fire --wander           # a teammate that walks around
python3 tests/bot_client.py --retail Bot1 --port 8777 --team 0 --wander       # same, against a *retail* AA3 server
# scripted end-to-end checks (each prints PASS/FAIL; start the server with short
# RoleSelectionTime/DeploymentTime first)
python3 tests/fake_client.py --port 8777 --round      # join, roles, spawn, a full round
python3 tests/grenade_client.py --port 8777           # rail mods, grenades, cook-off, ES2 perks
python3 tests/medic_client.py --port 8777             # 2 players: leg wound, treatment, head shot, Saline revive
python3 tests/medic_scenario.py --port 8777           # 3 players: an enemy wounds, a medic treats and revives
python3 tests/ladder_client.py --port 8777            # ladder from the top (FinishLadderMoveToTarget) vs a vault
# on Bridge AI (MapName = "Bridge_inf_co_large_ai"):
python3 tests/briefcase_scenario.py --port 8777       # pickup, drop on death, F5 respawn, extraction win (EnemyNPCs = 0)
python3 tests/npc_scenario.py --port 8777             # enemy NPCs open, shoot the player, can be neutralized
python3 tests/retail_medic.py --patient NAME --port 8777   # a medic bot for a *retail* server: treats NAME in reach
python3 tests/mission_bot.py --port 8777 [--retail NAME]  # plays Bridge AI start to finish: briefcase, NPCs, F5
build/aa3_navgrid data/maps/<bundle>.aa3map.zst x1 y1 z1 x2 y2 z2   # a walkable route (--serve for many)
# decode a capture against a class's replication layout
python3 ../aa3-tools/map_manifest/decode_rep.py --pcap cap.pcapng --frame 1 --to-frame 99999 \
    --ch 4 --class AA_GameReplicationInfo --cdo --server-port 8777
```
`ctest --test-dir build` runs the 5 registered suites (collision parity,
package extractor, collision quantization, mission extraction, map bundle
validation). Without an AA3 install, the three that read its packages report
as skipped. Other unit tests (`build/rep_roundtrip_test`,
`rpc_decode_pcap_test`, `lan_beacon_test`, `collision_test`) take fixture
arguments; see their sections in `docs/AA3_ACTOR_REPLICATION.md`.

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

- All 18 maps ship and boot, but only Bridge classic has been played against
  retail captures; the other maps' mission data comes from their packages
  alone. Training levels have no teams or objectives.
- No full server-side movement physics: moves are checked for speed and
  for passing through walls (MovementChecks), not simulated.
- Treatment: only passive treatment and the Saline revive are modelled.
  Other IFAK treatments, several medics on one patient, medic voice lines
  and treatment scoring are missing. Reach is retail's 120 units.
- Enemy NPCs run retail's controller and thinking classes as their scripts
  read: scanning, firing positions, suppression, grenades and smoke,
  reloads, flashbangs, incapacitation and securing (doc sec 40.8). Their
  aim spread and a map-wide frag limit are ours, tuned against bot runs on
  retail. Not modelled:
  - picking up a dropped NPC rifle;
  - NPC medics reviving the downed;
  - patrol paths for NPCs without firing positions.
- Collision has the level's blocking volumes now, but a few spots still
  differ from retail (the mission bot logs them as BLOCKED).
- Hit zones: head, torso and legs only (no arms).
- Side swap (round 4) only flips which team attacks; roles and spawn points
  don't swap. After round 6 the match restarts on the same map.
- Dropped weapon pickups, M320 40 mm rounds and other game modes are not implemented.
- No anti-cheat.

## Next Steps

1. Real-client test of the treat prompt fix (doc sec 39.7), then the rest of
   the IFAK: symptom-based active treatments on conscious soldiers, the medic
   call ("m") and withdraw (backspace) RPCs from the retail capture,
   treatment scoring (`ClientNotifyScoringEventDetails`), medic voice and the
   "Stop..." commo reply, arm hit zones.
2. Real-client test of round end, scoreboard and spectator camera.
3. Dropped weapons (`AA_DroppedPickup_*`), M320 rounds, team-1 spawn points, full side swap.
4. Data for more maps, then map rotation.

## License

This project is reverse-engineered from America's Army 3. Use for educational purposes only.

---

**Status**: Active development -- playable rounds on Bridge
**Last Updated**: 2026-10-12
