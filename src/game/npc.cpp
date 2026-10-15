// Enemy NPC soldiers (AA_NPCEnemyPawn), from the retail Bridge AI capture of
// 2026-10-12 (captures/bridge_ai_2026-10-12/FINDINGS.md) and
// AA_MCMissionManager.SpawnNPCs / AA_NPCEnemySoldierController:
//
//   spawn     at match start, at the pathing level's AA_NPCEnemyPlayerStarts:
//             up to 35, and no more than 46 minus the players ("Max NPCs and
//             Players to Spawn: 46, spawning up to 35 NPCs").
//   wire      only the pawn: no controller or PRI. Its open names it
//             (AA_NPCPawn.sNPCName = sBodySoldierName, e.g. 'Largsargsky'),
//             iBodyTeamIndex 4; then movement, posture graphs and FlashCount
//             as for any soldier; damage exactly as a player's.
//   behaviour (a first cut of the retail thinking classes) hold the start,
//             turn to the nearest soldier in sight within 5000 units, and
//             after a reaction delay fire 3-round bursts with distance-
//             scaled spread; hit, they turn to the shooter.
//   killed    the killer: ClientNotifyNeutralizedNME(name, False) +
//             ClientNotifyScoringEventDetails(7, 32, +10) (retail 13:42:54).
#include <algorithm>
#include <array>
#include <memory>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include "game/world.h"
#include "util/logging.h"

namespace aa3 {
namespace game {

namespace {
constexpr uint32_t kCdoNPCEnemyPawn = 21100;     // AA3Baseline Default__AA_NPCEnemyPawn
constexpr int32_t kMaxPlayersAndNPCs = 46;
constexpr int32_t kNPCBodyTeam = 4;              // retail iBodyTeamIndex
// An NPC's eye / muzzle above its location, by posture (as players', fitted
// from retail's shot rays: standing 33, crouched 20, prone 3 above the
// body; a prone NPC's location is body_drop above that).
float NpcEye(const ServerPawn& p) { return (p.prone ? 3.0f : p.crouched ? 20.0f : 33.0f) - p.body_drop; }
// Retail NPCs top out at 200 (Bridge AI capture: 358 velocity samples at 200,
// none faster), reached over ~0.5 s.
constexpr float kNpcSpeed = 200.0f, kNpcAccel = 400.0f;
// An NPC's location above the floor by posture. Retail (Bridge AI capture,
// two NPCs, every posture change): standing 47, crouched always 10 lower.
// Lower and the client finds its copy's cylinder in the floor and refuses
// the update (seen 2026-10-13: crouched NPCs stepping forward and snapping
// back over and over). Prone keeps the standing location: retail never
// lowers a prone NPC (bot capture 2026-10-12, 3 of 3), the client's prone
// pose does, and lowering it too sank the body into the floor (2026-10-13
// live). Its body (hits, eyes) lies at kProneZ: CollisionHeight_Prone 15
// plus the 3 standing has over its 44.
constexpr float kStandZ = 47.0f, kCrouchZ = 37.0f, kProneZ = 18.0f;
float PostureHeight(int posture) { return posture == 1 ? kCrouchZ : kStandZ; }

// The gear retail opens with each NPC (frame 11181): rifle, magazine, dump
// pouch and the chest / right-shoulder weapon slots. AA3Baseline NetIndex of
// each default object, the class MaxIndex.
struct GearClass { const char* name; uint32_t cdo; uint32_t max; };
constexpr GearClass kRifles[] = {{"AA_BulletWeapon_ObranKNP", 5260, 71}, {"AA_BulletWeapon_ObranMSP", 5298, 71},
                                 {"AA_BulletWeapon_ObranLMK", 5294, 72}};
constexpr GearClass kMag30 = {"AA_AmmoContainer_NME_30x556mmBall", 1022, 30};
constexpr GearClass kDrum = {"AA_AmmoContainer_200x556mmBall_Drum", 947, 30};
constexpr GearClass kDumpPouch = {"AA_InventoryContainer_Pouch_DumpPouch", 13435, 31};
constexpr GearClass kShoulder = {"AA_WeaponSlot_RightShoulder", 36171, 30};
constexpr GearClass kChest = {"AA_WeaponSlot_Chest", 36167, 30};
constexpr uint32_t kAmmoType556 = 1044;          // class AA_AmmoType_556mmBall
// RepIndex (Actor / AA_Inventory / AA_Weapon / AA_AmmoContainerBase).
constexpr uint32_t kInvAssoc = 3, kInstigator = 5, kHidden = 13, kAttachInfo = 27, kAttachedActor = 28,
                   kRoundIndex = 29, kLastContainer = 33, kLoadedMag = 34, kAmmoType = 35, kChambered = 57;

double Rand(double lo, double hi) { return lo + (hi - lo) * (std::rand() / static_cast<double>(RAND_MAX)); }

// Retail names are a player-style handle plus a Slavic-sounding ending
// ('Largsargsky', 'Tearsovec', 'Mayhemomus', 'Sgtfrancovic').
std::string NPCName(size_t i) {
    static const char* kStems[] = {"Largsarg", "Falconlord", "Rubberboot", "Tears", "Bluefalcon", "Vetchildr",
                                   "Scarciofuw", "Docbonkus", "Boxers", "Cpthunterjoeside", "Sgtfranc",
                                   "Usalocorob", "Mayhem", "Unamed", "Darthallos", "Lvbsfcjohn",
                                   "Ironwolf", "Steelrain", "Nightowl", "Redfox", "Coldsnap", "Duststorm"};
    static const char* kEnds[] = {"sky", "esky", "ovec", "omus", "ovic", "ofeska", "ssky", "sovic"};
    const size_t ns = sizeof(kStems) / sizeof(*kStems), ne = sizeof(kEnds) / sizeof(*kEnds);
    return std::string(kStems[(i * 7 + static_cast<size_t>(std::rand())) % ns]) + kEnds[static_cast<size_t>(std::rand()) % ne];
}
}  // namespace

std::vector<const ServerPawn*> World::NpcPawns() const {
    std::vector<const ServerPawn*> out;
    for (const auto& p : pawns_) if (p->is_npc && !p->destroyed) out.push_back(p.get());
    return out;
}

void World::SpawnNPCs() {
    if (!npcs_enabled_ || npc_starts_.empty() || npc_max_ <= 0) return;
    int32_t players = 0;
    for (const auto& c : controllers_) if (TeamOf(*c) == 0 || TeamOf(*c) == 1) ++players;
    const int32_t count = std::min<int32_t>({npc_max_, kMaxPlayersAndNPCs - players,
                                             static_cast<int32_t>(npc_starts_.size())});
    npc_brains_.clear();
    for (int32_t i = 0; i < count; ++i) {
        const NpcStart& s = npc_starts_[static_cast<size_t>(i)];
        auto pawn = std::make_unique<ServerPawn>();
        pawn->is_npc = true;
        pawn->npc_name = NPCName(static_cast<size_t>(i));
        pawn->object_index = BaselineIndex(kCdoNPCEnemyPawn);
        pawn->transient = true;
        pawn->health = 100.0f;
        pawn->label = "NPC:" + pawn->npc_name;
        for (int k = 0; k < 3; ++k) pawn->location[k] = s.loc[k];
        // On the floor, as retail spawns them (the marker sits up to ~50
        // above it; the pawn's location is 47 above the floor standing).
        const TraceHit floor = collision_.LineTrace({s.loc[0], s.loc[1], s.loc[2] + 10.0f},
                                                    {s.loc[0], s.loc[1], s.loc[2] - 400.0f},
                                                    CollisionWorld::kBlockPawn);
        if (floor.hit) pawn->location[2] = floor.location.z + kStandZ;
        pawn->rotation_yaw = s.yaw;
        pawn->body_soldier_name = pawn->npc_name;
        pawn->body_team_index = kNPCBodyTeam;
        pawn->original_owning_team = -1;
        pawn->inventory_association = "AA_NPCEnemyPawn_" + std::to_string(i);
        pawn->inventory_association_number = 96;
        NpcBrain brain;
        brain.start = i;
        // bSpawnRandomPatrolLocation: at one of its firing positions.
        if (s.random_spawn && !s.fp.empty()) {
            const int f = s.fp[static_cast<size_t>(std::rand()) % s.fp.size()];
            if (f >= 0 && static_cast<size_t>(f) < npc_firing_positions_.size()) {
                const FiringPos& fp = npc_firing_positions_[static_cast<size_t>(f)];
                const TraceHit ff = collision_.LineTrace({fp.loc[0], fp.loc[1], fp.loc[2] + 10.0f},
                                                         {fp.loc[0], fp.loc[1], fp.loc[2] - 400.0f},
                                                         CollisionWorld::kBlockPawn);
                pawn->location[0] = fp.loc[0];
                pawn->location[1] = fp.loc[1];
                pawn->location[2] = (ff.hit ? ff.location.z : fp.loc[2] - 40.0f) + kStandZ;
                pawn->rotation_yaw = fp.yaw;
                brain.cur_fp = f;
            }
        }
        brain.floor_z = pawn->location[2] - kStandZ;
        brain.home = {pawn->location[0], pawn->location[1], brain.floor_z};
        // Eyesight (AA_NPCBasePlayerStart: at 19200, 20% see 60% of it, 40%
        // 80%), never past the map's AI fog.
        float sight = static_cast<float>(s.sight);
        if (s.sight >= 19200) {
            const double r = Rand(0, 1);
            sight *= r < 0.2 ? 0.6f : r < 0.6 ? 0.8f : 1.0f;
        }
        if (npc_fog_ > 0.0f) sight = std::min(sight, npc_fog_);
        brain.sight = sight;
        brain.bravery = s.frightened ? 60 : 80;
        brain.lmk = s.weapon == "AA_BulletWeapon_ObranLMK";
        brain.marksman = s.weapon == "AA_BulletWeapon_ObranMSP";
        brain.mag_size = brain.rounds = brain.lmk ? 100 : 30;
        brain.scan_base = pawn->rotation_yaw;
        brain.shift_at = clock_ + Rand(15.0, 30.0);
        npc_brains_[pawn.get()] = brain;
        GiveNpcGear(*pawn, s.weapon);
        npc_weapon_[pawn.get()] = s.weapon;
        pawns_.push_back(std::move(pawn));
    }
    LOG_INFO("Spawned %d enemy NPCs (%d players, cap %d)", count, players, npc_max_);
}

void World::GiveNpcGear(ServerPawn& npc, const std::string& weapon) {
    const GearClass* rifle = &kRifles[0];
    for (const auto& r : kRifles) if (weapon == r.name) rifle = &r;
    const GearClass& mag_class = rifle == &kRifles[2] ? kDrum : kMag30;
    auto make = [&](const GearClass& g) {
        auto a = std::make_unique<ServerRoundActor>();
        a->class_name = g.name;
        a->max_index = g.max;
        a->cdo_package = "AA3Baseline";
        a->cdo_net_index = g.cdo;
        a->object_index = BaselineIndex(g.cdo);
        a->transient = true;
        a->label = std::string(g.name) + ":" + npc.npc_name;
        for (int k = 0; k < 3; ++k) a->location[k] = npc.location[k];
        DataValue assoc; assoc.kind = DataValue::Kind::kName;
        assoc.s = npc.inventory_association; assoc.i = npc.inventory_association_number;
        a->Set(kInstigator, -1, DataValue::Actor(&npc));
        a->Set(kInvAssoc, -1, assoc);
        return a;
    };
    DataValue ammo; ammo.kind = DataValue::Kind::kObj; ammo.s = "AA3Baseline"; ammo.net = kAmmoType556;
    auto gun = make(*rifle);
    auto mag = make(mag_class);
    auto pouch = make(kDumpPouch);
    auto shoulder = make(kShoulder);
    auto chest = make(kChest);
    gun->Set(kHidden, -1, DataValue::Bool(false));
    gun->Set(kAttachedActor, -1, DataValue::Actor(&npc));
    gun->Set(kAmmoType, -1, ammo);
    gun->Set(kChambered, 0, ammo);
    gun->Set(kLastContainer, -1, DataValue::Actor(chest.get()));
    gun->Set(kLoadedMag, 0, DataValue::Actor(mag.get()));
    mag->Set(kHidden, -1, DataValue::Bool(false));
    mag->Set(kAttachedActor, -1, DataValue::Actor(gun.get()));
    mag->Set(kRoundIndex, -1, DataValue::Int(1));
    DataValue attach; attach.kind = DataValue::Kind::kStruct;
    DataValue socket; socket.kind = DataValue::Kind::kName; socket.s = "DumpPouchSocket";
    attach.members = {DataValue::Int(-1), socket};
    pouch->Set(kAttachInfo, -1, attach);
    pouch->Set(kAttachedActor, -1, DataValue::Actor(&npc));
    shoulder->Set(kAttachedActor, -1, DataValue::Actor(&npc));
    chest->Set(kAttachedActor, -1, DataValue::Actor(&npc));
    npc.active_inventory = gun.get();
    npc.dump_pouch = pouch.get();
    npc.next_mag_pouch = pouch.get();
    auto& inv = pawn_inventory_[&npc];
    inv.clear();
    for (auto* a : {&gun, &mag, &pouch, &shoulder, &chest}) inv.push_back(std::move(*a));
}

// A killed NPC's rifle on the ground (retail Bridge AI capture, frame 12295):
// AA_DroppedPickup_<rifle>, falling, then at rest on the floor (Physics
// None, 0.1 s later); InventoryClass, the NPC side's TeamInfo, the loaded
// magazine; gone after AA_NPCBaseSoldierController's 120 s.
void World::DropNpcWeapon(const ServerPawn& npc) {
    struct Drop { const char* weapon; uint32_t cdo, cls, ammo_cls, rounds; };
    static const Drop kDrops[] = {{"AA_BulletWeapon_ObranKNP", 7130, 5259, 1021, 30},
                                  {"AA_BulletWeapon_ObranMSP", 7134, 5297, 1021, 30},
                                  {"AA_BulletWeapon_ObranLMK", 7132, 5293, 946, 200}};
    const auto w = npc_weapon_.find(&npc);
    const Drop* d = &kDrops[0];
    if (w != npc_weapon_.end())
        for (const auto& k : kDrops) if (w->second == k.weapon) d = &k;
    auto item = std::make_unique<ServerRoundActor>();
    item->class_name = std::string("AA_DroppedPickup_") + (d->weapon + std::strlen("AA_BulletWeapon_"));
    item->max_index = 30;
    item->cdo_package = "AA3Baseline";
    item->cdo_net_index = d->cdo;
    item->object_index = BaselineIndex(d->cdo);
    item->transient = true;
    item->label = item->class_name;
    for (int k = 0; k < 3; ++k) item->location[k] = npc.location[k];
    item->location[2] += 20.0f;
    auto byte = [](int64_t v, uint32_t bits) { DataValue b; b.kind = DataValue::Kind::kByte; b.i = v; b.bits = bits; return b; };
    auto obj = [](uint32_t net) { DataValue o; o.kind = DataValue::Kind::kObj; o.s = "AA3Baseline"; o.net = net; return o; };
    DataValue rot; rot.kind = DataValue::Kind::kRotator; rot.v3[1] = npc.rotation_yaw;
    item->Set(21, -1, rot);                                    // Rotation
    DataValue vel; vel.kind = DataValue::Kind::kVector; vel.v3[2] = 45;
    item->Set(4, -1, vel);                                     // Velocity
    item->Set(19, -1, byte(2, 4));                             // Physics: Falling
    item->Set(16, -1, byte(3, 4));                             // ReplicatedCollisionType (4 bits, retail)
    item->Set(8, -1, DataValue::Bool(true));                   // bBlockActors
    item->Set(24, -1, obj(d->cls));                            // InventoryClass
    DataValue mesh; mesh.kind = DataValue::Kind::kRaw; mesh.s = "111111111111";
    item->Set(26, -1, mesh);                                   // Mesh (retail's bits)
    if (const ServerTeamInfo* t = FindTeam(4)) item->Set(27, -1, DataValue::Actor(t));   // InventoryTeam
    DataValue mag; mag.kind = DataValue::Kind::kStruct;        // LoadedMagazines[0]
    mag.members = {obj(d->ammo_cls), DataValue::Int(d->rounds), DataValue::Int(1 + std::rand() % static_cast<int>(d->rounds))};
    item->Set(29, 0, mag);
    world_items_.push_back(std::move(item));
    item_timers_.push_back({world_items_.back().get(), clock_ + 0.12, clock_ + 120.0});
}

void World::UpdateWorldItems() {
    for (auto it = item_timers_.begin(); it != item_timers_.end();) {
        ServerRoundActor& a = *it->item;
        if (it->settle_at >= 0.0 && clock_ >= it->settle_at) {
            it->settle_at = -1.0;
            const TraceHit floor = collision_.LineTrace({a.location[0], a.location[1], a.location[2]},
                                                        {a.location[0], a.location[1], a.location[2] - 400.0f},
                                                        CollisionWorld::kBlockPawn);
            if (floor.hit) {
                a.location[2] = floor.location.z + 4.0f;
                DataValue loc; loc.kind = DataValue::Kind::kVector;
                for (int k = 0; k < 3; ++k) loc.v3[k] = static_cast<int32_t>(std::lround(a.location[k]));
                a.Set(22, -1, loc);                                // Location
            }
            DataValue still; still.kind = DataValue::Kind::kVector;
            a.Set(4, -1, still);
            DataValue none; none.kind = DataValue::Kind::kByte; none.i = 0; none.bits = 4;
            a.Set(19, -1, none);                                   // Physics: None (at rest)
        }
        if (clock_ >= it->expire_at) {
            a.gone = true;
            it = item_timers_.erase(it);
            continue;
        }
        ++it;
    }
}

void World::StartNavBuild() {
    if (npc_starts_.empty() || nav_thread_.joinable()) return;
    // The NPCs' area: their starts and firing positions, plus a margin.
    Vec3 lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
    auto grow = [&](float x, float y, float z) {
        lo = {std::min(lo.x, x), std::min(lo.y, y), std::min(lo.z, z)};
        hi = {std::max(hi.x, x), std::max(hi.y, y), std::max(hi.z, z)};
    };
    for (const auto& s : npc_starts_) grow(s.loc[0], s.loc[1], s.loc[2]);
    for (const auto& f : npc_firing_positions_) grow(f.loc[0], f.loc[1], f.loc[2]);
    lo = {lo.x - 1500.0f, lo.y - 1500.0f, lo.z - 1500.0f};
    hi = {hi.x + 1500.0f, hi.y + 1500.0f, hi.z + 1500.0f};
    std::vector<uint32_t> doors;
    for (const auto& a : map_actors_) if (a->class_name == "AA_Door") doors.push_back(a->cdo_net_index);
    auto copy = std::make_shared<CollisionWorld>(collision_);
    nav_thread_ = std::thread([this, copy, lo, hi, doors]() {
        auto grid = std::make_unique<NavGrid>();
        grid->Build(*copy, lo, hi, 48.0f, doors);
        LOG_INFO("NPC navigation grid ready: %zu cells", grid->CellCount());
        navgrid_ = std::move(grid);
        nav_ready_.store(true, std::memory_order_release);
    });
}

bool World::NpcRoute(ServerPawn& npc, NpcBrain& b, const Vec3& to, float speed) {
    if (!nav_ready_.load(std::memory_order_acquire)) return false;
    const Vec3 from{npc.location[0], npc.location[1], b.floor_z};
    auto path = navgrid_->FindPath(from, to);
    if (path.size() < 2 || path.size() > 200) return false;   // no route, or too far to bother
    // String-pull the grid's 8-way steps into straight legs (a clear line at
    // knee and chest height, no big height change), so the walk -- and the
    // velocity the clients follow -- doesn't zigzag.
    std::vector<Vec3> smooth{path.front()};
    size_t i = 0;
    while (i + 1 < path.size()) {
        size_t j = std::min(path.size() - 1, i + 12);
        for (; j > i + 1; --j) {
            const Vec3& a = path[i];
            const Vec3& c = path[j];
            if (std::fabs(c.z - a.z) > 24.0f) continue;
            bool clear = true;
            // centre line and both sides of the body (radius 22), so a leg
            // never takes the cylinder through a corner the client would
            // stop it at
            const float lx = c.x - a.x, ly = c.y - a.y, ll = std::max(1.0f, std::hypot(lx, ly));
            const float ox = -ly / ll * 22.0f, oy = lx / ll * 22.0f;
            for (float h : {30.0f, 70.0f})
                for (float side : {0.0f, -1.0f, 1.0f})
                    if (clear && collision_.LineTrace({a.x + ox * side, a.y + oy * side, a.z + h},
                                                      {c.x + ox * side, c.y + oy * side, c.z + h},
                                                      CollisionWorld::kBlockPawn).hit)
                        clear = false;
            // and no drop along the way
            for (int k = 1; clear && k < 4; ++k) {
                const float t = k / 4.0f;
                const Vec3 m{a.x + (c.x - a.x) * t, a.y + (c.y - a.y) * t, a.z + (c.z - a.z) * t};
                if (!collision_.LineTrace({m.x, m.y, m.z + 30.0f}, {m.x, m.y, m.z - 40.0f}, CollisionWorld::kBlockPawn).hit)
                    clear = false;
            }
            if (clear) break;
        }
        smooth.push_back(path[j]);
        i = j;
    }
    path = std::move(smooth);
    b.path = std::move(path);
    b.path_i = 1;
    b.speed = speed;
    b.cur_speed = 0.0f;
    npc.SetGraph(2, 7, 7);                         // walking (retail graph 2 {7})
    NpcPosture(npc, b, 0);
    return true;
}

// One step along the route: location on the floor + 47, velocity and
// facing replicated as for any soldier.
void World::NpcMove(ServerPawn& npc, NpcBrain& b, double dt) {
    if (b.path_i >= b.path.size()) {
        if (!b.path.empty()) {
            b.path.clear();
            npc.velocity[0] = npc.velocity[1] = npc.velocity[2] = 0.0f;
        }
        return;
    }
    // The real floor under a point: the walk grid's heights are coarse (a
    // route point stood 14 above the floor at a Bridge start), and a client
    // refuses to put its copy of the pawn partly inside the floor -- it then
    // runs the pawn on its velocity alone and snaps it back to the last
    // spot it accepted (seen 2026-10-13: every walking NPC).
    // The client rests the cylinder on the highest floor under any part of
    // it (a stair edge under one side lifts the whole body), so sample the
    // rim too. Start no higher than a step (35): from 60 up the trace landed
    // on rails and crates and popped the NPC 59 into the air.
    auto floor_at = [&](float x, float y, float near_z) {
        constexpr float kStep = 35.0f, kRim = 20.0f;
        float best = -1e9f;
        for (int k = 0; k < 9; ++k) {
            const float a = k * 0.785398f;
            const float sx = k == 8 ? x : x + std::cos(a) * kRim, sy = k == 8 ? y : y + std::sin(a) * kRim;
            const TraceHit h = collision_.LineTrace({sx, sy, near_z + kStep}, {sx, sy, near_z - 120.0f}, CollisionWorld::kBlockPawn);
            // a hit right at the start is the trace beginning inside a wall
            if (h.hit && h.location.z < near_z + kStep - 1.0f) best = std::max(best, h.location.z);
        }
        return best > -1e8f ? best : near_z;
    };
    const Vec3 p = b.path[b.path_i];   // a copy: arriving clears the path
    const float dx = p.x - npc.location[0], dy = p.y - npc.location[1];
    const float d = std::sqrt(dx * dx + dy * dy);
    b.cur_speed = std::min(b.speed, b.cur_speed + kNpcAccel * static_cast<float>(dt));
    const float step = b.cur_speed * static_cast<float>(dt);
    if (d <= step) {
        npc.location[0] = p.x; npc.location[1] = p.y;
        // Arrived: stop now. Callers stop calling once the route is done,
        // and a walking velocity left behind made every client walk its
        // copy on, to be pulled back by each 1 s keyframe (2026-10-13 live:
        // NPCs at firing positions stepping off and snapping back).
        if (++b.path_i >= b.path.size()) {
            b.path.clear();
            b.path_i = 0;
            npc.velocity[0] = npc.velocity[1] = npc.velocity[2] = 0.0f;
        }
    } else {
        npc.location[0] += dx / d * step;
        npc.location[1] += dy / d * step;
        npc.velocity[0] = dx / d * b.cur_speed;
        npc.velocity[1] = dy / d * b.cur_speed;
        npc.velocity[2] = 0.0f;
        npc.rotation_yaw = static_cast<int32_t>(std::atan2(dy, dx) * 32768.0 / 3.14159265358979) & 0xFFFF;
    }
    b.floor_z = floor_at(npc.location[0], npc.location[1], std::max(p.z, b.floor_z));
    npc.location[2] = b.floor_z + PostureHeight(npc.prone ? 2 : npc.crouched ? 1 : 0);
    b.turning = false;
}

// Bullets: the first soldier a segment passes through (cylinders as in
// TraceShot), skipping `ignore` and, for an NPC's shot, other NPCs.
ServerPawn* World::TracePawns(const Vec3& start, const Vec3& seg, const ServerPawn* ignore, bool skip_npcs,
                              float* best_t) {
    ServerPawn* victim = nullptr;
    for (auto& other : pawns_) {
        ServerPawn& o = *other;
        if (&o == ignore || o.torn_off || o.destroyed || (!o.controller && !o.is_npc)) continue;
        if (skip_npcs && o.is_npc) continue;
        const float r = 22.0f, hh = o.prone ? 14.0f : (o.crouched ? 34.0f : 48.0f);
        const float cx = o.location[0] - start.x, cy = o.location[1] - start.y;
        const float a = seg.x * seg.x + seg.y * seg.y;
        if (a < 1e-6f) continue;
        const float b = -2.0f * (cx * seg.x + cy * seg.y);
        const float c = cx * cx + cy * cy - r * r;
        const float disc = b * b - 4 * a * c;
        if (disc < 0) continue;
        const float t = (-b - std::sqrt(disc)) / (2 * a);
        if (t < 0 || t >= *best_t) continue;
        const float z = start.z + seg.z * t;
        const float oz = o.location[2] - o.body_drop;
        if (z < oz - hh || z > oz + hh) continue;
        *best_t = t;
        victim = &o;
    }
    return victim;
}

bool World::NpcCanSee(const ServerPawn& npc, const ServerPawn& target) const {
    const Vec3 eye{npc.location[0], npc.location[1], npc.location[2] + NpcEye(npc)};
    const Vec3 aim{target.location[0], target.location[1], target.location[2] + (target.crouched ? 10.0f : 25.0f)};
    return !collision_.LineTrace(eye, aim, CollisionWorld::kBlockTrace).hit && !NpcSmokeBlocks(eye, aim);
}

// ----------------------------------------------------------------------------
// The brain: AA_NPCBaseSoldierController's states and AA_NPCBaseSoldier-
// BaseThinking, as retail runs them on Bridge AI (doc sec 40.8). Every start
// there has bManFiringPositionsOnly, which keeps an NPC to ScanForTargets,
// ChangeFiringPosition (among its own positions), TargetingNewEnemy,
// HidingInCover, ReloadingWeapon, WeaponJam, the grenade throws and
// Flashbanged: no patrols, no running to cover, no chasing.

namespace {
constexpr double kThinkDelay = 0.25;             // AA_NPCBaseController.fpThinkingDelay
constexpr float kCrackRadius = 192.0f;           // a round this close suppresses
constexpr float kHearGunfire = 18000.0f;         // iMaxHearDistance (450) * 40 for a player's rifle
constexpr float kGrenadeReact = 650.0f;          // iMinGrenadeDistanceToReact
constexpr float kShrapnelRange = 600.0f;         // AA_ThrowProjectile_M67.ShrapnelRange (approx.)
constexpr float kTargetTurnRate = 40000.0f, kScanTurnRate = 45000.0f;
int32_t YawTo(float dx, float dy) {
    return static_cast<int32_t>(std::lround(std::atan2(dy, dx) * 32768.0 / 3.14159265358979)) & 0xFFFF;
}
float YawDiff(int32_t from, int32_t to) {
    return std::fmod(static_cast<float>(to) - static_cast<float>(from) + 98304.0f, 65536.0f) - 32768.0f;
}
bool Visible(const ServerPawn& p) { return !p.is_npc && !p.torn_off && !p.destroyed && !p.incapacitated && p.controller; }
}  // namespace

void World::NpcPosture(ServerPawn& npc, NpcBrain& b, int posture) {
    const int now = npc.prone ? 2 : npc.crouched ? 1 : 0;
    if (now == posture) return;
    // A prone body is about 80 long; across a step, a slope or into a wall
    // its head or feet go inside the world, so it crouches there instead.
    if (posture == 2) {
        constexpr float kHalfBody = 42.0f, kUneven = 18.0f;
        const float yaw = static_cast<float>(npc.rotation_yaw) * 3.14159265f / 32768.0f;
        const float fx = std::cos(yaw), fy = std::sin(yaw);
        for (const float end : {kHalfBody, -kHalfBody}) {
            const Vec3 mid{npc.location[0], npc.location[1], b.floor_z + 16.0f};
            const Vec3 tip{mid.x + fx * end, mid.y + fy * end, mid.z};
            const TraceHit wall = collision_.LineTrace(mid, tip, CollisionWorld::kBlockPawn);
            const TraceHit h = collision_.LineTrace({tip.x, tip.y, b.floor_z + 35.0f},
                                                    {tip.x, tip.y, b.floor_z - 40.0f}, CollisionWorld::kBlockPawn);
            if (wall.hit || !h.hit || std::fabs(h.location.z - b.floor_z) > kUneven) {
                posture = 1;
                break;
            }
        }
        if (posture == now) return;
    }
    ApplyPawnAction(&npc, posture == 2 ? PawnAction::kProne : posture == 1 ? PawnAction::kCrouch : PawnAction::kStand);
    npc.location[2] = b.floor_z + PostureHeight(posture);
    npc.body_drop = posture == 2 ? kStandZ - kProneZ : 0.0f;
    // The client's copy needs the posture too, as retail states it: crouched
    // is bIsCrouched plus bProxyInCrouchState; prone is bProxyInProneState
    // alone, never bIsProne (bot capture 2026-10-12). A crouched body left
    // standing sat below the floor and the client's physics dropped it
    // through (2026-10-13: NPCs vanishing and snapping back).
    npc.b_is_crouched = posture == 1;
    npc.b_is_prone = false;
    npc.proxy_in_stand = posture == 0;
    npc.proxy_in_crouch = posture == 1;
    npc.proxy_in_prone = posture == 2;
}

bool World::NpcSmokeBlocks(const Vec3& a, const Vec3& b) const {
    constexpr float kSmokeRadius = 300.0f;
    for (const auto& c : smoke_clouds_) {
        if (clock_ >= c.until) continue;
        const Vec3 d{b.x - a.x, b.y - a.y, b.z - a.z};
        const float len2 = d.x * d.x + d.y * d.y + d.z * d.z;
        float t = len2 > 0 ? ((c.at.x - a.x) * d.x + (c.at.y - a.y) * d.y + (c.at.z - a.z) * d.z) / len2 : 0.0f;
        t = std::max(0.0f, std::min(1.0f, t));
        const float px = a.x + d.x * t - c.at.x, py = a.y + d.y * t - c.at.y, pz = a.z + d.z * t - c.at.z;
        if (px * px + py * py + pz * pz < kSmokeRadius * kSmokeRadius) return true;
    }
    return false;
}

// AA_NPCBaseSoldierController.CanSeePlayer (+ the engine's peripheral
// vision when `fov`: one of four cones each look, NotifySeePlayer).
bool World::NpcCanSeePlayer(const ServerPawn& npc, const NpcBrain& b, const ServerPawn& p, bool fov) const {
    if (!Visible(p)) return false;
    const float dx = p.location[0] - npc.location[0], dy = p.location[1] - npc.location[1],
                dz = p.location[2] - npc.location[2];
    const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (dist > b.sight) return false;
    if (fov && dist > 1.0f) {
        static const float kCones[8] = {0.78f, 0.698f, 0.698f, 0.5235f, 0.5235f, 0.349f, 0.349f, 0.349f};
        const double fy = npc.rotation_yaw * 3.14159265358979 / 32768.0;
        const float dl = std::sqrt(dx * dx + dy * dy);
        if (dl > 1.0f && (dx * std::cos(fy) + dy * std::sin(fy)) / dl < kCones[std::rand() % 8]) return false;
    }
    const float speed = std::hypot(p.velocity[0], p.velocity[1]);
    if (dist > 7800.0f && p.crouched && speed < 100.0f &&
        Rand(0, 1) < std::clamp(1.0f - (dist - 7800.0f) / 15600.0f, 0.02f, 0.93f))
        return false;
    if (p.prone && npc.prone && dist >= 1040.0f && !(b.start >= 0 && npc_starts_[static_cast<size_t>(b.start)].only))
        return false;
    if (dist > 2600.0f && p.prone && speed < 100.0f &&
        Rand(0, 1) >= std::clamp(1.0f - (dist - 2600.0f) / 5200.0f, 0.05f, 0.95f))
        return false;
    const Vec3 eye{npc.location[0], npc.location[1], npc.location[2] + NpcEye(npc)};
    const float head = NpcEye(p);
    const Vec3 at{p.location[0], p.location[1], p.location[2] + head};
    if (NpcSmokeBlocks(eye, at)) return false;
    if (!collision_.LineTrace(eye, at, CollisionWorld::kBlockTrace).hit) return true;
    const Vec3 body{p.location[0], p.location[1], p.location[2] + (p.prone ? 8.0f : 16.0f)};
    return !collision_.LineTrace(eye, body, CollisionWorld::kBlockTrace).hit;
}

// AA_NPCBaseSoldierBaseThinking.EvaluateNewPawn, as retail has it (its
// distance term grows with distance: 1 - (3200 - d) / 3200, then +2.5).
float World::NpcThreat(const ServerPawn& npc, const NpcBrain&, const ServerPawn& p) const {
    float t = 0.25f;
    const float dx = npc.location[0] - p.location[0], dy = npc.location[1] - p.location[1];
    const float d2 = std::sqrt(dx * dx + dy * dy);
    const double py = p.rotation_yaw * 3.14159265358979 / 32768.0;
    if (d2 > 1.0f && (dx * std::cos(py) + dy * std::sin(py)) / d2 > 0.0f) t += 0.5f;   // it faces us
    if (p.held_grenade) t += 1.0f;
    else t += 0.5f;                              // (a reload in progress would be -0.5)
    const float dist = std::sqrt(dx * dx + dy * dy + std::pow(npc.location[2] - p.location[2], 2.0f));
    const float d = 3200.0f - dist;
    t += (d < 0 ? 0.10f : 1.0f - d / 3200.0f) + 2.5f;
    if (p.incapacitated) t -= 2.6f;
    return t;
}

void World::NpcEnterScan(ServerPawn& npc, NpcBrain& b) {
    b.state = NpcState::kScan;
    b.step = 0;
    b.wait_until = clock_ + 0.25;
    b.target = nullptr;
    const bool only = b.start >= 0 && npc_starts_[static_cast<size_t>(b.start)].only;
    b.shift_at = clock_ + (only ? Rand(15.0, 30.0) : Rand(5.0, 10.0));
    b.scan_i = 0;
    b.next_scan_at = clock_ + 0.5;
    b.scan_base = b.turning ? b.want_yaw : npc.rotation_yaw;
    b.entered_targeting_at = clock_;
}

// ChangeFiringPosition: another of its own positions (bManFiringPositions-
// Only), else the nearest one near home that sees the enemy.
bool World::NpcEnterChangeFP(ServerPawn& npc, NpcBrain& b) {
    if (!nav_ready_.load(std::memory_order_acquire) || npc_firing_positions_.empty()) return false;
    const NpcStart* st = b.start >= 0 ? &npc_starts_[static_cast<size_t>(b.start)] : nullptr;
    int dest = -1;
    if (st && st->only) {
        if (st->fp.size() <= 1) return false;
        for (int tries = 0; tries < 8 && dest < 0; ++tries) {
            const int f = st->fp[static_cast<size_t>(std::rand()) % st->fp.size()];
            if (f != b.cur_fp) dest = f;
        }
    } else {
        float best = 1e9f;
        for (size_t i = 0; i < npc_firing_positions_.size(); ++i) {
            const FiringPos& f = npc_firing_positions_[i];
            const float d = std::hypot(f.loc[0] - b.home.x, f.loc[1] - b.home.y);
            if (static_cast<int>(i) == b.cur_fp || d > 1500.0f) continue;
            float score = d;
            if (b.target) {
                const Vec3 eye{f.loc[0], f.loc[1], f.loc[2] + 40.0f};
                const Vec3 at{b.target->location[0], b.target->location[1], b.target->location[2]};
                if (collision_.LineTrace(eye, at, CollisionWorld::kBlockTrace).hit) score += 5000.0f;
            }
            score += static_cast<float>(Rand(0, 600));
            if (score < best) { best = score; dest = static_cast<int>(i); }
        }
    }
    if (dest < 0) return false;
    b.state = NpcState::kChangeFP;
    b.step = 0;
    b.dest_fp = dest;
    b.wait_until = clock_ + 0.25;
    b.buzzes_here = 0;
    b.last_enemy_dist = b.target ? std::hypot(b.target->location[0] - npc.location[0],
                                              b.target->location[1] - npc.location[1]) : 1e9f;
    LOG_INFO("NPC %s moves to firing position %d", npc.npc_name.c_str(), dest);
    return true;
}

void World::NpcTargetEnemy(ServerPawn& npc, NpcBrain& b, ServerPawn* enemy, bool contact) {
    if (!enemy || npc.incapacitated) return;
    switch (b.state) {                           // states that refuse the change
    case NpcState::kHiding: case NpcState::kReloading: case NpcState::kJam: case NpcState::kThrowing:
    case NpcState::kFlashbanged: case NpcState::kFleeGrenade:
        return;
    case NpcState::kTargeting:
        if (b.target == enemy) return;
        break;
    default: break;
    }
    if (b.path_i < b.path.size()) { b.path.clear(); b.path_i = 0; npc.velocity[0] = npc.velocity[1] = 0.0f; }
    b.target = enemy;
    b.last_known = {enemy->location[0], enemy->location[1], enemy->location[2]};
    b.state = NpcState::kTargeting;
    b.step = 0;
    b.spotted_at = clock_;
    b.entered_targeting_at = clock_;
    b.monitor_at = clock_ + 8.0;
    b.lost_sight_at = 0.0;
    // From sighting to the first round: the turn and settle (retail's
    // FinishRotation + post-spin delay), 1.5-3 s in the retail captures.
    b.wait_until = clock_ + Rand(1.5, 3.0);
    LOG_INFO("NPC %s targets %s (%.0f away)", npc.npc_name.c_str(), SoldierName(enemy).c_str(),
             std::hypot(enemy->location[0] - npc.location[0], enemy->location[1] - npc.location[1]));
    // ascSeesEnemyPlayer: RequestingSuppressiveFire, NMESpotted, ReportLocation,
    // RequestingSupportAtMyLocation.
    if (contact) NpcVoice(npc, b, {976, 965, 973, 975});
    if (contact) NpcEnemyContact(npc, *enemy);
}

// AA_NPCCombatManager.NotifyEnemyContact: every healthy NPC within 12000
// that hasn't had an order for 30 s attacks if it sees the enemy, else heads
// there (refused by firing-position-only NPCs).
void World::NpcEnemyContact(ServerPawn& npc, ServerPawn& enemy) {
    for (auto& [pp, ob] : npc_brains_) {
        ServerPawn* o = const_cast<ServerPawn*>(pp);
        if (o == &npc || o->torn_off || o->incapacitated || o->health < 80.0f) continue;
        if (clock_ - ob.order_at <= 30.0) continue;
        if (std::hypot(o->location[0] - npc.location[0], o->location[1] - npc.location[1]) >= 12000.0f) continue;
        if (ob.state != NpcState::kScan && ob.state != NpcState::kChangeFP) continue;
        ob.order_at = clock_;
        if (NpcCanSeePlayer(*o, ob, enemy, true)) {
            NpcTargetEnemy(*o, ob, &enemy, false);
        } else if (!(ob.start >= 0 && npc_starts_[static_cast<size_t>(ob.start)].only)) {
            ob.target = &enemy;
            NpcEnterChangeFP(*o, ob);
            ob.target = nullptr;
        }
    }
}

// AA_NPCBaseSoldierBaseThinking.WantsToThrowGrenade.
bool World::NpcWantsGrenade(const ServerPawn& npc, NpcBrain& b, bool smoke, bool just_lost_sight) {
    if (b.lmk) return false;
    if (smoke) {
        if (b.smokes <= 0 || clock_ - b.last_smoke <= 30.0 || Rand(0, 1) < 0.8) return false;
        if (b.target && NpcCanSee(npc, *b.target) &&
            std::hypot(b.target->location[0] - npc.location[0], b.target->location[1] - npc.location[1]) < 1800.0f &&
            b.rounds > 0)
            return false;
        b.last_smoke = clock_;
        return true;
    }
    if (b.frags <= 0 || clock_ - b.last_frag <= 30.0 || Rand(0, 1) < (just_lost_sight ? 0.25 : 0.5)) return false;
    // Ours: one NPC frag per 120 s map-wide -- retail threw 6 in 22 minutes
    // of the Bridge AI capture, which its per-NPC rules alone overshoot.
    if (clock_ < next_npc_grenade_at_) return false;
    // AnyFriendliesBetweenEnemyAndSelf: another NPC near the line.
    for (const auto& [o, ob] : npc_brains_) {
        if (o == &npc || o->torn_off) continue;
        const float ax = b.last_known.x - npc.location[0], ay = b.last_known.y - npc.location[1];
        const float l2 = ax * ax + ay * ay;
        if (l2 < 1.0f) continue;
        const float t = ((o->location[0] - npc.location[0]) * ax + (o->location[1] - npc.location[1]) * ay) / l2;
        if (t <= 0.0f || t >= 1.0f) continue;
        if (std::hypot(npc.location[0] + ax * t - o->location[0], npc.location[1] + ay * t - o->location[1]) < 150.0f)
            return false;
    }
    b.last_frag = clock_;
    next_npc_grenade_at_ = clock_ + 120.0;
    return true;
}

void World::NpcBeginThrow(ServerPawn& npc, NpcBrain& b, bool smoke, const Vec3& at) {
    b.state = NpcState::kThrowing;
    b.step = 0;
    b.throw_smoke = smoke;
    if (!smoke) NpcVoice(npc, b, {919});         // ascThrowingGrenade: NME_Auto_FragOut
    b.throw_at = at;
    b.wait_until = clock_ + 1.0;                 // the grenade comes to hand
    if (npc.prone) NpcPosture(npc, b, 1);
}

// The NPC's round: retail aims at the target's centre plus an error of up
// to 15 across / 25 up-down (10 / 15 for the MSP marksman rifle; +5 when
// IsAimingAffectedBySuppression), then the rifle's own dispersion.
void World::NpcShoot(ServerPawn& npc, NpcBrain& b, const Vec3& at_point) {
    const Vec3 start{npc.location[0], npc.location[1], npc.location[2] + NpcEye(npc)};
    float h = b.marksman ? 10.0f : 15.0f, v = b.marksman ? 15.0f : 25.0f;
    if (clock_ - b.supp_start > 6.0) { h += 5.0f; v += 5.0f; }
    Vec3 aim{at_point.x + static_cast<float>(Rand(-h, h)) - start.x, at_point.y + static_cast<float>(Rand(-h, h)) - start.y,
             at_point.z + static_cast<float>(Rand(-v, v)) - start.z};
    const float dist = std::sqrt(aim.x * aim.x + aim.y * aim.y + aim.z * aim.z);
    if (dist < 1.0f) return;
    // Dispersion (the rifle's own spread isn't modelled; ours stands in),
    // tuned so the mission bot lives about as long per spawn as on retail
    // (~110 s; doc sec 40.8): ~5 degrees, wider far off and against a moving
    // target (walking ~1.8x, sprinting 3x).
    float moving = 1.0f;
    if (b.target) {
        const float speed = std::hypot(b.target->velocity[0], b.target->velocity[1]);
        moving = b.target->sprinting || speed > 250.0f ? 3.0f : speed > 50.0f ? 1.8f : 1.0f;
    }
    const float spread = 0.175f * moving * (1.0f + dist / 3000.0f);
    Vec3 dir{aim.x / dist + static_cast<float>(Rand(-spread, spread)),
             aim.y / dist + static_cast<float>(Rand(-spread, spread)),
             aim.z / dist + static_cast<float>(Rand(-spread, spread) * 0.6)};
    const float n = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    dir = {dir.x / n, dir.y / n, dir.z / n};
    constexpr float kRange = 32768.0f;
    const Vec3 end{start.x + dir.x * kRange, start.y + dir.y * kRange, start.z + dir.z * kRange};
    const TraceHit hit = collision_.LineTrace(start, end, CollisionWorld::kBlockTrace);
    float best_t = hit.hit ? hit.time : 1.0f;
    const Vec3 seg{end.x - start.x, end.y - start.y, end.z - start.z};
    ServerPawn* victim = TracePawns(start, seg, &npc, /*skip_npcs=*/true, &best_t);
    npc.flash_count = static_cast<uint8_t>(npc.flash_count + 1);
    if (npc.flash_count == 0) npc.flash_count = 1;
    --b.rounds;
    {
        const Vec3 at = victim ? Vec3{start.x + seg.x * best_t, start.y + seg.y * best_t, start.z + seg.z * best_t}
                               : (hit.hit ? hit.location : end);
        BulletSounds(npc, start, at, victim, b.rounds <= 0, b.lmk ? 2 : b.marksman ? 1 : 0);
    }
    // NotifyShootingWeapon: firing steadies it (LMK +8, rifles +2).
    b.bravery = std::min(100, b.bravery + (b.lmk ? 8 : 2));
    if (victim) {
        const Vec3 at{start.x + seg.x * best_t, start.y + seg.y * best_t, start.z + seg.z * best_t};
        DamagePawn(*victim, &npc, at, dir);
        if (victim->torn_off || victim->incapacitated) b.bravery = std::min(100, b.bravery + 30);  // NotifyKilledEnemyPlayer
    } else if (hit.hit && hit.owner) {
        HitMapActor(hit.owner - 1);
    }
}

// AA_NPCBaseThinking.Think: ConsiderSafety, AdjustMentalState,
// ConsiderNewAction (whose RunForCover a firing-position-only NPC refuses).
void World::NpcThink(ServerPawn& npc, NpcBrain& b) {
    bool danger = false;
    for (const auto& c : controllers_)
        if (c->pawn && !c->pawn->torn_off && (TeamOf(*c) == 0 || TeamOf(*c) == 1) && b.injured_by[TeamOf(*c)]) danger = true;
    if (b.danger_end_at >= 0.0 && clock_ >= b.danger_end_at) { b.danger_near = false; b.danger_end_at = -1.0; }
    if (danger) b.danger_near = true;
    if (npc.health >= 75.0f) {
        if (!b.danger_near && b.bravery <= 75) {
            if (Rand(0, 1) <= 0.6) b.bravery += b.state == NpcState::kHiding || NpcFP(b) ? 2 : 1;
        } else if (b.bravery > 40 && b.danger_near && Rand(0, 1) <= 0.7) {
            b.bravery -= 1;
        }
    }
    const bool only = b.start >= 0 && npc_starts_[static_cast<size_t>(b.start)].only;
    if (!only && b.bravery <= 20 && b.state != NpcState::kChangeFP && b.state != NpcState::kThrowing &&
        b.state != NpcState::kReloading)
        NpcEnterChangeFP(npc, b);                // RunForCover
}

void World::UpdateNPCs(double dt) {
    smoke_clouds_.erase(std::remove_if(smoke_clouds_.begin(), smoke_clouds_.end(),
                                       [&](const SmokeCloud& c) { return clock_ >= c.until; }),
                        smoke_clouds_.end());
    for (auto& up : pawns_) {
        ServerPawn& npc = *up;
        if (npc.is_npc && (npc.torn_off || npc.zip_tie) && !npc.npc_body_gone && npc.npc_body_until >= 0.0 &&
            clock_ >= npc.npc_body_until)
            npc.npc_body_gone = true;
        if (!npc.is_npc || npc.torn_off || npc.destroyed || npc.incapacitated) continue;
        auto it = npc_brains_.find(&npc);
        if (it == npc_brains_.end()) continue;
        NpcTick(npc, it->second, dt);
    }
}

void World::NpcTick(ServerPawn& npc, NpcBrain& b, double dt) {
    if (clock_ >= b.think_at) {
        b.think_at = clock_ + kThinkDelay;
        NpcThink(npc, b);
    }
    // Turning toward want_yaw.
    if (b.turning) {
        const float rate = b.state == NpcState::kScan ? kScanTurnRate : kTargetTurnRate;
        const float diff = YawDiff(npc.rotation_yaw, b.want_yaw);
        const float step = rate * static_cast<float>(dt);
        if (std::fabs(diff) <= step) { npc.rotation_yaw = b.want_yaw & 0xFFFF; b.turning = false; }
        else npc.rotation_yaw = static_cast<int32_t>(std::lround(npc.rotation_yaw + (diff > 0 ? step : -step))) & 0xFFFF;
    }
    // Turn to `yaw` (small corrections, under ~2 degrees, are made at once).
    auto face = [&](int32_t yaw) {
        b.want_yaw = yaw & 0xFFFF;
        if (std::fabs(YawDiff(npc.rotation_yaw, b.want_yaw)) < 360.0f) { npc.rotation_yaw = b.want_yaw; b.turning = false; }
        else b.turning = true;
    };
    auto face_point = [&](const Vec3& p) { face(YawTo(p.x - npc.location[0], p.y - npc.location[1])); };
    // Looking for players (SeePlayer): the most threatening one in view.
    auto look = [&](bool fov) -> ServerPawn* {
        ServerPawn* best = nullptr;
        float best_t = -1e9f;
        for (auto& op : pawns_) {
            ServerPawn& p = *op;
            if (!Visible(p) || !NpcCanSeePlayer(npc, b, p, fov)) continue;
            const float t = NpcThreat(npc, b, p);
            if (t > best_t) { best_t = t; best = &p; }
        }
        return best;
    };
    const bool waiting = clock_ < b.wait_until;
    switch (b.state) {
    case NpcState::kScan: {
        if (clock_ >= b.next_watch_at) {
            b.next_watch_at = clock_ + kThinkDelay;
            if (ServerPawn* p = look(true)) { NpcTargetEnemy(npc, b, p); return; }
        }
        if (waiting) return;
        if (b.step == 0) {                       // begin: crouch, then sweep
            NpcPosture(npc, b, 1);
            b.step = 1;
        }
        if (clock_ >= b.shift_at && NpcEnterChangeFP(npc, b)) return;
        if (clock_ >= b.shift_at) b.shift_at = clock_ + Rand(15.0, 30.0);
        if (clock_ >= b.next_scan_at && !b.turning) {
            // Sweep: eight steps of 2048 to one side and back, the position's
            // facing as the middle.
            static const int kSweep[] = {0, 2048, 4096, 6144, 8192, 6144, 4096, 2048, 0, -2048, -4096, -6144,
                                         -8192, -6144, -4096, -2048};
            face(b.scan_base + kSweep[b.scan_i % 16]);
            ++b.scan_i;
            b.next_scan_at = clock_ + Rand(0.6, 1.4);
        }
        return;
    }
    case NpcState::kChangeFP: {
        if (b.step == 0) {
            if (waiting) return;
            NpcPosture(npc, b, 1);
            b.wait_until = clock_ + 0.35;
            b.step = 1;
            return;
        }
        if (b.step == 1) {
            if (waiting) return;
            if (b.target && NpcWantsGrenade(npc, b, true, false)) {
                NpcBeginThrow(npc, b, true, b.last_known);
                b.after_hiding = NpcState::kChangeFP;
                return;
            }
            NpcPosture(npc, b, 0);
            const FiringPos& f = npc_firing_positions_[static_cast<size_t>(b.dest_fp)];
            if (!NpcRoute(npc, b, {f.loc[0], f.loc[1], f.loc[2]}, kNpcSpeed)) {
                NpcPosture(npc, b, 1);           // no way there: look around, try again later
                NpcEnterScan(npc, b);
                b.wait_until = clock_ + 2.5;
                return;
            }
            b.step = 2;
            return;
        }
        if (b.step == 2) {
            // WatchForEnemyWhileMovingTimer / SeePlayer on the way.
            if (clock_ >= b.next_watch_at) {
                b.next_watch_at = clock_ + 0.5;
                if (ServerPawn* p = look(true)) {
                    const float d = std::hypot(p->location[0] - npc.location[0], p->location[1] - npc.location[1]);
                    if (p != b.target || d < b.last_enemy_dist) { NpcTargetEnemy(npc, b, p); return; }
                    b.last_enemy_dist = d;
                }
            }
            NpcMove(npc, b, dt);
            if (b.path_i < b.path.size()) return;
            b.cur_fp = b.dest_fp;
            const FiringPos& f = npc_firing_positions_[static_cast<size_t>(b.cur_fp)];
            face(f.yaw);
            b.wait_until = clock_ + 1.0;
            b.step = 3;
            return;
        }
        if (waiting) return;
        NpcEnterScan(npc, b);
        b.scan_base = b.want_yaw;
        return;
    }
    case NpcState::kHiding:
    case NpcState::kJam:
        if (waiting) return;
        if (b.after_hiding == NpcState::kTargeting && b.target && Visible(*b.target)) {
            b.state = NpcState::kTargeting;
            b.step = 1;
        } else {
            NpcEnterScan(npc, b);
        }
        return;
    case NpcState::kReloading:
        if (b.step == 0) {
            if (waiting) return;
            NpcPosture(npc, b, npc.crouched ? 1 : 2);   // prone unless already crouched
            b.wait_until = clock_ + 0.5 + (b.lmk ? 4.0 : 3.0);
            b.step = 1;
            return;
        }
        if (waiting) return;
        b.rounds = b.mag_size;
        if (b.target && Visible(*b.target)) { b.state = NpcState::kTargeting; b.step = 1; }
        else NpcEnterScan(npc, b);
        return;
    case NpcState::kThrowing:
        if (waiting) return;
        if (b.step == 0) {
            face_point(b.throw_at);
            b.step = 1;
            return;
        }
        if (b.step == 1) {
            if (b.turning) return;
            NpcThrowFrag(npc, b.throw_at, b.throw_smoke);
            if (b.throw_smoke) --b.smokes;
            else { --b.frags; b.bravery = std::min(100, b.bravery + 12); }   // NotifyThrowingFrag
            b.wait_until = clock_ + 1.2;          // back to the rifle
            b.step = 2;
            return;
        }
        if (b.after_hiding == NpcState::kChangeFP) {
            b.after_hiding = NpcState::kScan;
            NpcPosture(npc, b, 0);
            const FiringPos& f = npc_firing_positions_[static_cast<size_t>(b.dest_fp)];
            if (NpcRoute(npc, b, {f.loc[0], f.loc[1], f.loc[2]}, kNpcSpeed)) { b.state = NpcState::kChangeFP; b.step = 2; return; }
        }
        NpcEnterScan(npc, b);
        return;
    case NpcState::kFlashbanged:
        if (clock_ < b.flash_until) return;
        // NotifyFlashBangEnded.
        b.bravery -= b.bravery > 20 ? 10 : (b.bravery > 1 ? 1 : 0);
        NpcEnterScan(npc, b);
        return;
    case NpcState::kFleeGrenade:
        if (b.step == 0) {
            NpcMove(npc, b, dt);
            if (b.path_i < b.path.size()) return;
            NpcPosture(npc, b, 2);
            b.wait_until = clock_ + 3.5;
            b.step = 1;
            return;
        }
        if (waiting) return;
        NpcPosture(npc, b, 1);
        if (b.target && Visible(*b.target) && NpcCanSeePlayer(npc, b, *b.target, false)) {
            b.state = NpcState::kTargeting;
            b.step = 1;
        } else {
            NpcEnterScan(npc, b);
        }
        return;
    case NpcState::kTargeting:
        break;
    }

    // ---- TargetingNewEnemy ------------------------------------------------
    ServerPawn* enemy = b.target;
    if (enemy && (enemy->torn_off || enemy->destroyed)) enemy = b.target = nullptr;
    const bool in_sight = enemy && Visible(*enemy) && NpcCanSeePlayer(npc, b, *enemy, false);
    if (in_sight) {
        b.last_known = {enemy->location[0], enemy->location[1], enemy->location[2]};
        face_point(b.last_known);
    }
    // MonitorForExitTimer (every 8 s).
    if (clock_ >= b.monitor_at) {
        b.monitor_at = clock_ + 8.0;
        if (!in_sight && Rand(0, 1) > 0.5 && NpcEnterChangeFP(npc, b)) return;
        const bool consider = in_sight ? (b.buzzes_here > 4 || Rand(0, 1) < 0.2) : Rand(0, 1) < 0.05;
        if (consider && NpcEnterChangeFP(npc, b)) return;
    }
    if (b.burst_left > 0) {                      // mid-burst
        if (clock_ < b.next_shot_at) return;
        if (b.rounds <= 0) { b.burst_left = 0; }
        else {
            NpcShoot(npc, b, b.last_known);
            if (Rand(0, 1) < 0.003) {            // a stoppage (AA_BulletWeapon malfunction)
                b.burst_left = 0;
                b.state = NpcState::kJam;
                b.after_hiding = NpcState::kTargeting;
                b.wait_until = clock_ + Rand(3.0, 6.0);
                return;
            }
            if (--b.burst_left > 0) { b.next_shot_at = clock_ + (b.lmk ? 0.09 : 0.1); return; }
        }
        // After the burst (retail: 0.25-0.5 s; the LMK 1-1.75 s).
        b.wait_until = clock_ + (b.lmk ? Rand(1.0, 1.75) : Rand(0.25, 0.5));
        b.step = 2;
        return;
    }
    if (waiting || b.turning) return;
    if (b.step == 2) {
        // Lost sight: once, a frag at where it was (if beyond shrapnel range
        // and this position allows grenades), then NotifyLostSightOfEnemy.
        if (!in_sight) {
            if (b.lost_sight_at == 0.0) {
                b.lost_sight_at = clock_;
                const float d = std::hypot(b.last_known.x - npc.location[0], b.last_known.y - npc.location[1]);
                const FiringPos* fp = NpcFP(b);
                if (d > kShrapnelRange && NpcWantsGrenade(npc, b, false, true) && (!fp || (fp->flags & 8))) {
                    NpcBeginThrow(npc, b, false, b.last_known);
                    return;
                }
            } else if (clock_ - b.lost_sight_at > 2.5) {
                // Brave ones head for the last position -- refused when it
                // only mans firing positions -- the rest scan.
                if (Rand(0, 1) >= b.bravery / 100.0) { NpcEnterScan(npc, b); return; }
            }
        } else {
            b.lost_sight_at = 0.0;
        }
        if (enemy && !enemy->incapacitated) {
            if (Rand(0, 1) > 0.5) {
                if (!npc.crouched && !npc.prone) {
                    NpcPosture(npc, b, 1);
                    if (!in_sight) b.wait_until = clock_ + Rand(3.0, 13.0);   // DetermineDelayBetweenBursts
                }
            } else {
                b.wait_until = clock_ + Rand(0.25, 1.25);
            }
            b.step = 1;
            return;
        }
        NpcEnterScan(npc, b);                    // enemy down: SecureEnemy is refused; scan
        return;
    }
    // ShootAgain.
    const double in_state = clock_ - b.entered_targeting_at;
    if (in_state > 3.0 && !npc.crouched && !npc.prone) {
        const FiringPos* fp = NpcFP(b);
        if (fp && !(fp->flags & 6)) {            // can only shoot standing here: duck, then up again
            NpcPosture(npc, b, 1);
            b.wait_until = clock_ + Rand(2.0, 3.75);
            b.step = 3;
            return;
        }
        if (!fp) NpcPosture(npc, b, 1);
    }
    if (b.step == 3) NpcPosture(npc, b, 0);
    if (in_state > 10.0) {
        b.entered_targeting_at = clock_ - 3.0;
        const bool consider = in_sight ? (b.buzzes_here > 4 || Rand(0, 1) < 0.2) : Rand(0, 1) < 0.05;
        if (consider) {
            NpcPosture(npc, b, 1);
            if (NpcEnterChangeFP(npc, b)) return;
        }
    }
    // AimAtTarget: gone from sight -> maybe a frag; one more burst at where
    // it was; then back to scanning.
    bool final_burst = false;
    if (!in_sight) {
        if (enemy && enemy->incapacitated) { NpcEnterScan(npc, b); return; }
        if (b.lost_sight_at == 0.0) b.lost_sight_at = clock_;
        if (NpcWantsGrenade(npc, b, false, false)) { NpcBeginThrow(npc, b, false, b.last_known); return; }
        final_burst = true;
        b.target = nullptr;
    }
    if (b.rounds <= 0) {                         // NotifyNeedToReload
        NpcVoice(npc, b, {970, 945});            // ascLeavingCover: RegroupAtThisLocation, CoverMe
        b.state = NpcState::kReloading;
        b.step = 0;
        b.wait_until = clock_ + 0.25;
        return;
    }
    // IsSafeToFireAtEnemy: another NPC in the way?
    if (enemy && !final_burst) {
        const Vec3 eye{npc.location[0], npc.location[1], npc.location[2] + NpcEye(npc)};
        const Vec3 seg{enemy->location[0] - eye.x, enemy->location[1] - eye.y, enemy->location[2] + 10.0f - eye.z};
        float t = 1.0f;
        if (ServerPawn* blocker = TracePawns(eye, seg, &npc, false, &t); blocker && blocker->is_npc) {
            if (Rand(0, 1) > 0.5) { b.wait_until = clock_ + 0.25; return; }
            if (NpcEnterChangeFP(npc, b)) return;
            b.wait_until = clock_ + 0.25;
            return;
        }
    }
    face_point(b.last_known);
    if (b.turning) return;
    if (b.lmk) b.burst_left = std::max(2, static_cast<int>((0.15 + Rand(0, 1)) / 0.09));
    else b.burst_left = b.marksman ? 1 : 1 + std::rand() % 3;
    b.burst_left = std::min(b.burst_left, b.rounds);
    b.next_shot_at = clock_;
    if (final_burst) b.step = 2;
}

// A player's shot: NPCs it passes close to are suppressed
// (SuppressedByFire); every NPC within earshot hears it (HeardNoise: turn
// toward it and scan, unless already fighting).
void World::NpcBulletNear(ServerPawn& shooter, const Vec3& start, const Vec3& end, const ServerPawn* hit) {
    if (shooter.is_npc) return;
    const Vec3 d{end.x - start.x, end.y - start.y, end.z - start.z};
    const float len2 = d.x * d.x + d.y * d.y + d.z * d.z;
    for (auto& [pp, b] : npc_brains_) {
        ServerPawn& npc = *const_cast<ServerPawn*>(pp);
        if (npc.torn_off || npc.incapacitated) continue;
        const float hx = npc.location[0] - start.x, hy = npc.location[1] - start.y, hz = npc.location[2] - start.z;
        if (hx * hx + hy * hy + hz * hz < kHearGunfire * kHearGunfire && b.state != NpcState::kTargeting &&
            b.state != NpcState::kThrowing && b.state != NpcState::kReloading && b.state != NpcState::kHiding &&
            b.state != NpcState::kFlashbanged && b.state != NpcState::kFleeGrenade) {
            const int32_t yaw = YawTo(-hx, -hy);
            if (b.state == NpcState::kScan) {
                b.scan_base = yaw;
                b.want_yaw = yaw; b.turning = true;
            } else if (b.state == NpcState::kChangeFP && b.step < 2) {
                NpcEnterScan(npc, b);
                b.scan_base = yaw; b.want_yaw = yaw; b.turning = true;
            }
        }
        if (&npc == hit || len2 < 1.0f) continue;
        float t = (hx * d.x + hy * d.y + hz * d.z) / len2;
        if (t <= 0.0f || t > 1.0f) continue;
        const float px = start.x + d.x * t - npc.location[0], py = start.y + d.y * t - npc.location[1],
                    pz = start.z + d.z * t - npc.location[2];
        if (px * px + py * py + pz * pz < kCrackRadius * kCrackRadius) NpcSuppressed(npc, b, &shooter);
    }
}

void World::NpcSuppressed(ServerPawn& npc, NpcBrain& b, ServerPawn* shooter) {
    if (clock_ - b.last_supp_reaction < 15.0) return;
    const bool recent = clock_ - b.last_supp_reaction < 25.0;
    b.last_supp_reaction = clock_;
    // AA_NPCBaseThinking.SuppressedByFire.
    b.bravery = 35;
    b.danger_near = true;
    b.danger_end_at = clock_ + Rand(1.0, 3.0);
    b.supp_start = clock_;
    ++b.buzzes_here;
    if (b.state == NpcState::kThrowing || b.state == NpcState::kReloading || b.state == NpcState::kFlashbanged ||
        b.state == NpcState::kFleeGrenade || b.state == NpcState::kHiding)
        return;
    if (b.state == NpcState::kChangeFP && b.step == 2) return;      // running: carries on
    if (b.state == NpcState::kTargeting) {
        if (!recent && !npc.prone) NpcPosture(npc, b, 2);
        else if (Rand(0, 1) >= 0.5) NpcPosture(npc, b, npc.crouched || npc.prone ? 2 : 1);
        b.after_hiding = NpcState::kTargeting;
    } else {
        NpcPosture(npc, b, 1);
        b.after_hiding = NpcState::kScan;
    }
    b.burst_left = 0;
    b.state = NpcState::kHiding;
    b.wait_until = clock_ + Rand(2.5, 4.5);
    NpcVoice(npc, b, {950, 945, 975});           // ascBuzzedByWeapon: FallBack, CoverMe, RequestingSupport
    LOG_INFO("NPC %s suppressed by %s", npc.npc_name.c_str(), SoldierName(shooter).c_str());
}

// A player's grenade bouncing or landing near: bravery 20; an M67 sends a
// fighting NPC running (TargetingNewEnemy.RunForCover goes straight to
// RunningToCover): away to past 650, prone 3.5 s, then crouched.
void World::NpcGrenadeNear(const Vec3& at, bool frag, const ServerPawn* thrower) {
    if (thrower && thrower->is_npc) return;
    for (auto& [pp, b] : npc_brains_) {
        ServerPawn& npc = *const_cast<ServerPawn*>(pp);
        if (npc.torn_off || npc.incapacitated) continue;
        const float dx = npc.location[0] - at.x, dy = npc.location[1] - at.y;
        const float d = std::hypot(dx, dy);
        if (d >= kGrenadeReact) continue;
        b.bravery = 20;
        b.danger_near = true;
        b.danger_end_at = clock_ + Rand(1.0, 3.0);
        if (frag) NpcVoice(npc, b, {952, 990, 919, 950});   // ascGrenadeLandedNear
        const bool only = b.start >= 0 && npc_starts_[static_cast<size_t>(b.start)].only;
        if (!frag || (only && b.state != NpcState::kTargeting) || b.state == NpcState::kThrowing ||
            b.state == NpcState::kFleeGrenade || !nav_ready_.load(std::memory_order_acquire))
            continue;
        // FindSafeDistanceFromGrenade: a reachable point ~750 away from it.
        const float ux = d > 1.0f ? dx / d : 1.0f, uy = d > 1.0f ? dy / d : 0.0f;
        bool routed = false;
        for (int k = 0; k < 6 && !routed; ++k) {
            const double a = (k % 2 ? 1 : -1) * (k / 2) * 0.6;
            const float rx = static_cast<float>(ux * std::cos(a) - uy * std::sin(a));
            const float ry = static_cast<float>(ux * std::sin(a) + uy * std::cos(a));
            const Vec3 to{at.x + rx * 750.0f, at.y + ry * 750.0f, b.floor_z};
            NpcPosture(npc, b, 0);
            routed = NpcRoute(npc, b, to, kNpcSpeed);
        }
        if (!routed) continue;
        b.grenade_at = at;
        b.burst_left = 0;
        b.state = NpcState::kFleeGrenade;
        b.step = 0;
    }
}

void World::NpcSmoke(const Vec3& at, double seconds) { smoke_clouds_.push_back({at, clock_ + seconds}); }

// An M84 going off in sight: Flashbanged for 7.5 s (EndFlashBanged).
void World::NpcFlashbang(const Vec3& at) {
    for (auto& [pp, b] : npc_brains_) {
        ServerPawn& npc = *const_cast<ServerPawn*>(pp);
        if (npc.torn_off || npc.incapacitated) continue;
        const Vec3 eye{npc.location[0], npc.location[1], npc.location[2] + NpcEye(npc)};
        const float d = std::sqrt(std::pow(eye.x - at.x, 2.0f) + std::pow(eye.y - at.y, 2.0f) + std::pow(eye.z - at.z, 2.0f));
        if (d > 1000.0f || collision_.LineTrace({at.x, at.y, at.z + 10.0f}, eye, CollisionWorld::kBlockTrace).hit) continue;
        if (b.path_i < b.path.size()) { b.path.clear(); b.path_i = 0; npc.velocity[0] = npc.velocity[1] = 0.0f; }
        b.burst_left = 0;
        b.state = NpcState::kFlashbanged;
        b.flash_until = clock_ + 7.5;
    }
}

// ---------------------------------------------------------------- sounds
//
// Retail Bridge AI user session (2026-10-12), every enemy round: its
// report (S_Weap_Rifles <rifle>_SingleFire; the magazine's last round
// M4A1_SingleFire_MagEmpty) from the shooter, PriorityAdjustment 2,
// distance-delayed; a crack (ClientSuppressedByFire) for a player it
// passed within BulletCrackDistanceMax (120); on a soldier, the impact
// (S_Weap_Bullet Impact_LowCal_Armor / _Flesh, and HEADSHOT.Headshot_Cue
// for the head) from him; otherwise the fly-by (Cav_LowCal_x_01) where it
// ended up. Cue NetIndex values read from the packages.
void World::BulletSounds(ServerPawn& shooter, const Vec3& start, const Vec3& end, const ServerPawn* victim,
                         bool last_round, int weapon) {
    const Vec3 muzzle{shooter.location[0], shooter.location[1], shooter.location[2]};
    if (last_round) PlaySound("S_Weap_Rifles", 322, &shooter, muzzle, false, true, 2.0f, 20000.0f);
    else if (weapon == 2) PlaySound("S_Weap_MG", 228, &shooter, muzzle, false, true, 2.0f, 20000.0f);
    else PlaySound("S_Weap_Rifles", weapon == 1 ? 325 : 324, &shooter, muzzle, false, true, 2.0f, 20000.0f);
    const Vec3 seg{end.x - start.x, end.y - start.y, end.z - start.z};
    const float len2 = seg.x * seg.x + seg.y * seg.y + seg.z * seg.z;
    if (len2 < 1.0f) return;
    for (const auto& c : controllers_) {
        const ServerPawn* p = c->pawn;
        if (!p || p == victim || p == &shooter || p->torn_off) continue;
        const Vec3 mid{p->location[0], p->location[1], p->location[2] + 10.0f};
        const float t = ((mid.x - start.x) * seg.x + (mid.y - start.y) * seg.y + (mid.z - start.z) * seg.z) / len2;
        if (t <= 0.0f || t >= 1.0f) continue;
        const Vec3 near{start.x + seg.x * t, start.y + seg.y * t, start.z + seg.z * t};
        const float d = std::sqrt((near.x - mid.x) * (near.x - mid.x) + (near.y - mid.y) * (near.y - mid.y) +
                                  (near.z - mid.z) * (near.z - mid.z));
        if (d > 120.0f) continue;
        // BulletCavitationCue (Crack_Cav) or BulletCrackCue (Crack): retail
        // 97 to 23 with no pattern in range or miss distance; 80/20 here.
        SoundEvent e;
        e.seq = ++sound_seq_;
        e.at = clock_;
        e.pkg = "S_Weap_Bullet";
        e.net = Rand(0, 1) < 0.8 ? 202 : 203;
        e.source = &shooter;
        e.loc = near;
        e.listener = p;
        sound_events_.push_back(e);
    }
    if (victim) {
        const float hh = victim->prone ? 14.0f : (victim->crouched ? 34.0f : 48.0f);
        const float rel = end.z - (victim->location[2] - victim->body_drop);
        const bool head = rel > hh - 14.0f;
        const bool torso = !head && rel > -hh * 0.2f;
        PlaySound("S_Weap_Bullet", torso ? 205 : 211, victim, end, true, true, 0.0f, 6000.0f);
        if (head) PlaySound("S_Map_Airfield", 44, victim, end, true, true, 0.0f, 6000.0f);
    } else {
        PlaySound("S_Weap_Bullet", 201, &shooter, end, false, false, 0.0f, 5000.0f);
    }
}

void World::NpcVoice(ServerPawn& npc, NpcBrain& b, std::initializer_list<uint32_t> cues) {
    if (npc.torn_off || npc.health <= 0.0f || cues.size() == 0) return;
    if (clock_ - b.last_voice < 5.0) return;
    b.last_voice = clock_;
    const uint32_t cue = *(cues.begin() + static_cast<size_t>(std::rand()) % cues.size());
    PlaySound("S_Char_VO_Soldier", cue, &npc, Vec3{npc.location[0], npc.location[1], npc.location[2]}, true, false,
              0.0f, 5000.0f);
}

void World::NpcVoice(ServerPawn& npc, std::initializer_list<uint32_t> cues) {
    auto it = npc_brains_.find(&npc);
    if (it != npc_brains_.end()) NpcVoice(npc, it->second, cues);
}

}  // namespace game
}  // namespace aa3
