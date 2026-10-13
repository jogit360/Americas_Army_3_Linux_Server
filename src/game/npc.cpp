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
constexpr float kSightRange = 3000.0f, kEye = 31.0f;
constexpr float kFovCos = 0.26f;                 // ~75 degrees either side of where it faces
constexpr float kNearAware = 600.0f;             // all-round awareness this close
constexpr double kReactionMin = 1.5, kReactionMax = 3.0;   // seconds from spotting to the first shot
// Retail's first NPC (Bridge AI capture, ch 279) fired 23 rounds in 160 s.
constexpr double kShotGap = 0.15, kBurstPauseMin = 2.0, kBurstPauseMax = 4.0;
constexpr int kBurst = 3;
constexpr double kLookInterval = 0.25;           // how often an NPC re-checks its target
constexpr float kTurnRate = 32768.0f;            // yaw units per second (180 degrees)

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
        // above it; the pawn's location is ~48 above the floor standing).
        const TraceHit floor = collision_.LineTrace({s.loc[0], s.loc[1], s.loc[2] + 10.0f},
                                                    {s.loc[0], s.loc[1], s.loc[2] - 400.0f},
                                                    CollisionWorld::kBlockPawn);
        if (floor.hit) pawn->location[2] = floor.location.z + 48.0f;
        pawn->rotation_yaw = s.yaw;
        pawn->body_soldier_name = pawn->npc_name;
        pawn->body_team_index = kNPCBodyTeam;
        pawn->original_owning_team = -1;
        pawn->inventory_association = "AA_NPCEnemyPawn_" + std::to_string(i);
        pawn->inventory_association_number = 96;
        NpcBrain brain;
        brain.home = {pawn->location[0], pawn->location[1], pawn->location[2] - 48.0f};
        brain.next_patrol_at = clock_ + Rand(15.0, 60.0);
        brain.next_grenade_at = clock_ + Rand(10.0, 30.0);
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
    item->Set(16, -1, byte(3, 3));                             // ReplicatedCollisionType
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
    for (const auto& f : npc_firing_positions_) grow(f[0], f[1], f[2]);
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
    const Vec3 from{npc.location[0], npc.location[1], npc.location[2] - 48.0f};
    auto path = navgrid_->FindPath(from, to);
    if (path.size() < 2 || path.size() > 200) return false;   // no route, or too far to bother
    b.path = std::move(path);
    b.path_i = 1;
    b.speed = speed;
    if (npc.crouched) {
        ApplyPawnAction(&npc, PawnAction::kStand);
        npc.location[2] += 14.0f;
    }
    return true;
}

// One step along the route: location on the floor + 48, velocity and
// facing replicated as for any soldier.
void World::NpcMove(ServerPawn& npc, NpcBrain& b, double dt) {
    if (b.path_i >= b.path.size()) {
        if (!b.path.empty()) {
            b.path.clear();
            npc.velocity[0] = npc.velocity[1] = npc.velocity[2] = 0.0f;
        }
        return;
    }
    const Vec3& p = b.path[b.path_i];
    const float dx = p.x - npc.location[0], dy = p.y - npc.location[1];
    const float d = std::sqrt(dx * dx + dy * dy);
    const float step = b.speed * static_cast<float>(dt);
    if (d <= step) {
        npc.location[0] = p.x; npc.location[1] = p.y; npc.location[2] = p.z + 48.0f;
        ++b.path_i;
        return;
    }
    npc.location[0] += dx / d * step;
    npc.location[1] += dy / d * step;
    npc.location[2] = p.z + 48.0f;
    npc.velocity[0] = dx / d * b.speed;
    npc.velocity[1] = dy / d * b.speed;
    npc.velocity[2] = 0.0f;
    if (!b.target) npc.rotation_yaw = static_cast<int32_t>(std::atan2(dy, dx) * 32768.0 / 3.14159265358979) & 0xFFFF;
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
        if (z < o.location[2] - hh || z > o.location[2] + hh) continue;
        *best_t = t;
        victim = &o;
    }
    return victim;
}

bool World::NpcCanSee(const ServerPawn& npc, const ServerPawn& target) const {
    const Vec3 eye{npc.location[0], npc.location[1], npc.location[2] + kEye};
    const Vec3 aim{target.location[0], target.location[1], target.location[2] + (target.crouched ? 10.0f : 25.0f)};
    const float dx = aim.x - eye.x, dy = aim.y - eye.y, dz = aim.z - eye.z;
    if (dx * dx + dy * dy + dz * dz > kSightRange * kSightRange) return false;
    const TraceHit hit = collision_.LineTrace(eye, aim, CollisionWorld::kBlockTrace);
    return !hit.hit;
}

void World::NpcShoot(ServerPawn& npc, const ServerPawn& target) {
    const Vec3 start{npc.location[0], npc.location[1], npc.location[2] + kEye};
    Vec3 aim{target.location[0] - start.x, target.location[1] - start.y,
             target.location[2] + (target.crouched ? 5.0f : 15.0f) - start.z};
    const float dist = std::sqrt(aim.x * aim.x + aim.y * aim.y + aim.z * aim.z);
    if (dist < 1.0f) return;
    // Spread: ~2.5 degrees, wider far off and while the target moves.
    // A moving target is much harder to hit: walking ~1.8x the spread,
    // sprinting 3x (retail's NPCs missed a sprinting carrier for a minute).
    const float speed = std::hypot(target.velocity[0], target.velocity[1]);
    const float moving = target.sprinting || speed > 250.0f ? 3.0f : speed > 50.0f ? 1.8f : 1.0f;
    const float spread = 0.10f * moving * (1.0f + dist / 3000.0f);
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
    if (victim) {
        const Vec3 at{start.x + seg.x * best_t, start.y + seg.y * best_t, start.z + seg.z * best_t};
        DamagePawn(*victim, &npc, at, dir);
    } else if (hit.hit && hit.owner) {
        HitMapActor(hit.owner - 1);
    }
}

void World::UpdateNPCs(double dt) {
    for (auto& up : pawns_) {
        ServerPawn& npc = *up;
        if (!npc.is_npc || npc.torn_off || npc.destroyed || npc.incapacitated) continue;
        NpcBrain& b = npc_brains_[&npc];
        // Pick (or keep) a target: the nearest living player soldier in sight.
        if (clock_ >= b.next_look_at) {
            b.next_look_at = clock_ + kLookInterval;
            ServerPawn* best = nullptr;
            float best_d = kSightRange * kSightRange;
            for (auto& op : pawns_) {
                ServerPawn& p = *op;
                if (p.is_npc || p.torn_off || p.destroyed || p.incapacitated || !p.controller) continue;
                const float dx = p.location[0] - npc.location[0], dy = p.location[1] - npc.location[1],
                            dz = p.location[2] - npc.location[2];
                const float d = dx * dx + dy * dy + dz * dz;
                if (d >= best_d) continue;
                // In view: ahead of it, unless close by.
                const float dl = std::sqrt(dx * dx + dy * dy);
                const double fy = npc.rotation_yaw * 3.14159265358979 / 32768.0;
                if (dl > kNearAware && dl > 1.0f &&
                    (dx * std::cos(fy) + dy * std::sin(fy)) / dl < kFovCos) continue;
                if (!NpcCanSee(npc, p)) continue;
                best = &p;
                best_d = d;
            }
            if (best != b.target) {
                b.target = best;
                if (best) {
                    b.spotted_at = clock_;
                    b.next_shot_at = clock_ + Rand(kReactionMin, kReactionMax);
                    b.burst_left = kBurst;
                }
            }
        }
        // Movement: following a route; a newly spotted soldier sends it to the
        // nearest firing position (within 700) that sees him; idle, it patrols
        // to a firing position near home now and then.
        const bool moving = b.path_i < b.path.size();
        if (b.target) {
            b.last_seen_at = clock_;
            b.last_seen = {b.target->location[0], b.target->location[1], b.target->location[2]};
            if (clock_ - b.spotted_at < kLookInterval * 1.5 && !moving && nav_ready_.load(std::memory_order_acquire)) {
                const Vec3 here{npc.location[0], npc.location[1], npc.location[2] - 48.0f};
                const std::array<float, 3>* best = nullptr;
                float best_d = 700.0f;
                for (const auto& f : npc_firing_positions_) {
                    const float fx = f[0] - here.x, fy = f[1] - here.y;
                    const float fd = std::sqrt(fx * fx + fy * fy);
                    if (fd < 60.0f || fd >= best_d || std::fabs(f[2] - here.z) > 200.0f) continue;
                    const Vec3 eye{f[0], f[1], f[2] + 40.0f};
                    const Vec3 aim{b.target->location[0], b.target->location[1], b.target->location[2]};
                    if (collision_.LineTrace(eye, aim, CollisionWorld::kBlockTrace).hit) continue;
                    best = &f; best_d = fd;
                }
                if (best && NpcRoute(npc, b, {(*best)[0], (*best)[1], (*best)[2]}, 300.0f))
                    b.next_shot_at = std::max(b.next_shot_at, clock_ + best_d / 300.0 + 0.5);
            }
        } else if (!moving && clock_ >= b.next_patrol_at && nav_ready_.load(std::memory_order_acquire)) {
            b.next_patrol_at = clock_ + Rand(25.0, 50.0);
            std::vector<const std::array<float, 3>*> near;
            for (const auto& f : npc_firing_positions_)
                if (std::hypot(f[0] - b.home.x, f[1] - b.home.y) < 1200.0f) near.push_back(&f);
            if (!near.empty()) {
                const auto* f = near[static_cast<size_t>(std::rand()) % near.size()];
                NpcRoute(npc, b, {(*f)[0], (*f)[1], (*f)[2]}, 170.0f);
            }
        }
        // A soldier gone into cover: sometimes a frag where he was (4-15 s
        // ago, 400-1500 away). Rare, as retail's (6 U23A1 frags in 22 min of
        // the Bridge AI capture): one map-wide every 30 s, each NPC at most
        // every 60-120 s, and only a 30% chance when the moment comes.
        if (!b.target && clock_ >= b.next_grenade_at && clock_ >= next_npc_grenade_at_ &&
            clock_ - b.last_seen_at > 4.0 && clock_ - b.last_seen_at < 15.0) {
            const float gd = std::hypot(b.last_seen.x - npc.location[0], b.last_seen.y - npc.location[1]);
            if (gd > 400.0f && gd < 1500.0f) {
                b.next_grenade_at = clock_ + Rand(60.0, 120.0);
                if (std::rand() % 10 < 3) {
                    NpcThrowFrag(npc, b.last_seen);
                    next_npc_grenade_at_ = clock_ + 30.0;
                }
            }
        }
        NpcMove(npc, b, dt);
        // Fighting from where it stands: crouched (the pawn drops 14, the
        // difference between standing and crouched collision height).
        if (b.target && b.path_i >= b.path.size() && !npc.crouched) {
            ApplyPawnAction(&npc, PawnAction::kCrouch);
            npc.location[2] -= 14.0f;
        }
        // Hit by someone it can't see: turn toward them (AA_NPC took damage).
        ServerPawn* face = b.target;
        if (!face && npc.npc_last_hit_by && clock_ - npc.npc_last_hit_at < 3.0) face = npc.npc_last_hit_by;
        if (!face) continue;
        if (face->destroyed || face->torn_off) {
            if (b.target == face) b.target = nullptr;
            npc.npc_last_hit_by = nullptr;
            continue;
        }
        // Turn toward it.
        const float want = static_cast<float>(std::atan2(face->location[1] - npc.location[1],
                                                         face->location[0] - npc.location[0]) * 32768.0 / 3.14159265358979);
        float diff = std::fmod(want - static_cast<float>(npc.rotation_yaw) + 98304.0f, 65536.0f) - 32768.0f;
        const float step = kTurnRate * static_cast<float>(dt);
        diff = std::max(-step, std::min(step, diff));
        npc.rotation_yaw = static_cast<int32_t>(std::lround(npc.rotation_yaw + diff)) & 0xFFFF;
        // Fire: bursts once facing the target and the reaction time is up.
        if (!b.target || b.path_i < b.path.size() || std::fabs(diff) >= step * 0.99f || clock_ < b.next_shot_at) continue;
        NpcShoot(npc, *b.target);
        if (--b.burst_left > 0) {
            b.next_shot_at = clock_ + kShotGap;
        } else {
            b.burst_left = kBurst;
            b.next_shot_at = clock_ + Rand(kBurstPauseMin, kBurstPauseMax);
        }
    }
}

}  // namespace game
}  // namespace aa3
