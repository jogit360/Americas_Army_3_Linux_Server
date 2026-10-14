// Soldier weapons (doc sec 34/37): each role's rifle kit, the rail mods the
// player picked for it, and hand grenades from the pick-up to the blast.
#include <algorithm>
#include <cmath>
#include "game/world.h"
#include "network/rep_layout_gen.h"
#include "util/logging.h"

namespace aa3 {
namespace game {

namespace {

// Field RepIndex values shared by AA_Inventory/AA_Weapon subclasses
// (aa3_build_manifests --rep-layout AA3Baseline.u AA_ThrowWeapon_M67).
constexpr uint32_t kInstigator = 5, kNetOwner = 6, kHidden = 13, kOwner = 15,
                   kHiddenByInvManager = 23, kInventory = 25, kClientGivenTo = 26, kAttachedActor = 28,
                   kLastInventoryContainer = 33, kSightType = 36, kModInventory = 37;
constexpr uint32_t kPulledPin = 57, kCooking = 58;                // AA_ThrowWeapon
constexpr uint32_t kModParentRail = 25, kModNextModPtr = 27;      // AA_WeaponModBase
constexpr uint32_t kProjVelocity = 4, kProjCollideActors = 10,    // AA_ThrowProjectile
                   kProjRotation = 21, kProjExplodeTime = 24, kProjMaxIndex = 25;
constexpr uint32_t kPreloadedGrenades = 27;                       // AA_InventoryManager

// Per-role rifle kits (doc sec 34). Every kit is retail's FTL M4A1 kit
// (config/spawn_inventory.json) with the role's weapon swapped in: the
// role's wpnPrimaryOne class and iPrimaryWeaponOneAmmo magazines, the
// weapon's own magazine class (its WeaponMagClasses default) and the HUD
// inventory types. The sight is the player's main-rail pick; `sight_cls`
// is only the fallback when the PRI has none. AA3Baseline class export
// indices; the default object a channel opens is class + 1. These four are
// every wpnPrimaryOne of the 286 role slots across the 18 retail maps.
struct WeaponKit {
    uint32_t weapon_cls; const char* weapon; uint32_t max_index;
    uint32_t mag_cls; const char* mag;
    uint32_t sight_cls;
    uint8_t weapon_type, ammo_type;
    uint8_t rof;            // eRateOfFire it comes off Safe to (class CurrentROF default)
    int32_t mag_rounds;     // the magazine's RoundCount
};
// The M249 is auto-only and open-bolt (CurrentROF default ROF_Auto,
// bOpenBoltFire) with a 200-round box; the rifles start on semi like
// retail's M4A1 (doc sec 32).
constexpr WeaponKit kKits[] = {
    {5250, "AA_BulletWeapon_M4A1",   71, 979, "AA_AmmoContainer_30x556mmBall",      35972, 14, 17, 1, 30},
    {5152, "AA_BulletWeapon_M16A4",  71, 979, "AA_AmmoContainer_30x556mmBall",      35972, 16, 17, 1, 30},
    {5156, "AA_BulletWeapon_M16DMR", 71, 979, "AA_AmmoContainer_30x556mmBall",      35933, 15, 17, 1, 30},
    {5244, "AA_BulletWeapon_M249",   72, 956, "AA_AmmoContainer_200x556mmBall_Box", 35972, 13, 18, 3, 200},
};

// Rail mods a role can offer (aawmbPrimaryOne*Rail lists of every retail
// map: 35979/35972/35943/35952/35933 main, 35843 right, 36153 bottom), with
// the 4x ECOS for completeness. MaxIndex from config/class_maxindex_table.tsv.
struct ModClass { uint32_t cls; const char* name; uint32_t max_index; bool sight; };
constexpr ModClass kMods[] = {
    {35979, "AA_WeaponModSights_Iron_US_A3Handle", 30, true},
    {35972, "AA_WeaponModSights_CCO_US_M68comp4",  30, true},
    {35943, "AA_WeaponModSights_2x_ACOG_RCO",      30, true},
    {35952, "AA_WeaponModSights_35x_ACOG_RCO",     30, true},
    {35933, "AA_WeaponModSights_ACOG_RCO",         30, true},
    {35959, "AA_WeaponModSights_4x_US_ACOG_ECOS",  30, true},
    {35843, "AA_WeaponModLights_Incan_M95",        29, false},
    {36153, "AA_WeaponModWeapon_XM320",            32, false},
};
const ModClass* FindMod(uint32_t cls) {
    for (const auto& m : kMods) if (m.cls == cls) return &m;
    return nullptr;
}

// PRI mriPrimaryWeaponOneMods order (Main, Top, Left, Right, Bottom,
// RearSight) -> AA_Weapon.eAttachmentRailType (AR_RailMain, AR_RailTop,
// AR_RailBottom, AR_RailLeft, AR_RailRight, AR_Stock, AR_RearSight; 3 bits).
constexpr uint8_t kRailEnum[6] = {0, 1, 3, 4, 2, 6};

// Hand grenades. Slot = AA_InventoryManager.aatwPreloadedGrenades index
// (CreatePreloadedGrenades order); InventoryType = Engine EInventoryType.
// Fuse = AA_ThrowProjectile.fExplodeTime default (M67/AN-M14 inherit the
// base 5, M83/M106 the M18's 1); life = LifeSpan, which is exactly when
// retail closes each projectile's channel (t=455.70 / 459.17 / 490.40 /
// 510.24). Class defaults read from AA3Baseline.u.
enum class Blast { kFrag, kSmoke, kStun, kIncendiary };
struct GrenadeSpec {
    const char* weapon;
    uint32_t projectile_cls; const char* projectile;
    uint8_t inv_type;
    int32_t ServerPlayerReplicationInfo::*count;
    int slot;
    float fuse, life;
    Blast blast;
};
const GrenadeSpec kGrenades[] = {
    {"AA_ThrowWeapon_M67",        32786, "AA_ThrowProjectile_M67",        5,
     &ServerPlayerReplicationInfo::frag_grenades,       0, 5.0f,  6.0f, Blast::kFrag},
    {"AA_ThrowWeapon_M83",        32738, "AA_ThrowProjectile_M83",        3,
     &ServerPlayerReplicationInfo::smoke_grenades,      1, 1.0f, 20.0f, Blast::kSmoke},
    {"AA_ThrowWeapon_M84",        32842, "AA_ThrowProjectile_M84",        6,
     &ServerPlayerReplicationInfo::stun_grenades,       2, 2.0f, 16.0f, Blast::kStun},
    {"AA_ThrowWeapon_M106",       32764, "AA_ThrowProjectile_M106",       4,
     &ServerPlayerReplicationInfo::m106_grenades,       3, 1.0f, 17.0f, Blast::kSmoke},
    {"AA_ThrowWeapon_AN_M14_TH3", 32708, "AA_ThrowProjectile_AN_M14_TH3", 1,
     &ServerPlayerReplicationInfo::incendiary_grenades, 4, 5.0f, 60.0f, Blast::kIncendiary},
    // The enemy NPCs' frag (AA_NPCEnemySoldierController: U23A1; retail
    // Bridge AI capture, AA_ThrowProjectile_U23A1). Thrown by NpcThrowFrag only.
    {"AA_ThrowWeapon_U23A1",      32883, "AA_ThrowProjectile_U23A1",      5,
     &ServerPlayerReplicationInfo::frag_grenades,       0, 4.0f,  6.0f, Blast::kFrag},
    // Their smoke (GetSmokeGrenadeClass: U72), timed as the M83.
    {"AA_ThrowWeapon_U72",        32887, "AA_ThrowProjectile_U72",        3,
     &ServerPlayerReplicationInfo::smoke_grenades,      1, 1.0f, 20.0f, Blast::kSmoke},
};
const GrenadeSpec* GrenadeSpecOf(const std::string& cls) {
    for (const auto& g : kGrenades) if (cls == g.weapon) return &g;
    return nullptr;
}

constexpr float kThrowSpeed = 1020.5f;        // AA_ThrowProjectile_*.Speed
constexpr float kRestitution = 0.4f;          // default PhysMaterial (guess; not in the packages read)
constexpr float kMinRollVelocity = 10.0f;     // AA_Projectile.MinRollVelocity
// M67 DamageLookupTable (Damage 100): {distance, percent, damage class}.
struct DamageStep { float distance, percent; uint32_t damage_class; };
constexpr DamageStep kFragTable[] = {
    {0.0f,     1.0f,   6475},   // AA_DamageType_Explosion_Grenade_Death
    {262.5f,   1.0f,   6479},   // ..._Red
    {349.974f, 0.667f, 6481},   // ..._Yellow
    {437.48f,  0.337f, 6477},   // ..._Green
    {524.934f, 0.0f,   0},
};
constexpr float kFragDamage = 100.0f;
constexpr float kBurnDamage = 10.0f, kBurnRadius = 175.0f, kBurnDuration = 20.0f;
constexpr uint32_t kFireDamage = 6491;        // AA_DamageType_Fire

float Length(const Vec3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

DataValue VectorValue(float x, float y, float z) {
    DataValue v; v.kind = DataValue::Kind::kVector;
    v.v3[0] = static_cast<int32_t>(std::lround(x));
    v.v3[1] = static_cast<int32_t>(std::lround(y));
    v.v3[2] = static_cast<int32_t>(std::lround(z));
    return v;
}

}  // namespace

// ---------------------------------------------------------------- kits

void World::ApplyRoleKit(ServerPlayerController& pc, std::vector<std::unique_ptr<ServerRoundActor>>& items) {
    namespace CR = network::rep::ClassRoleRI;
    if (!pc.pri || !pc.pri->team || !pc.pawn) return;
    // bHasReviveKit (retail sends True at spawn, doc sec 39.7).
    pc.pri->has_revive_kit = false;
    for (const auto& it : items)
        if (it->class_name == "AA_IFAK") pc.pri->has_revive_kit = true;
    const int32_t idx = pc.pri->assigned_class_role_index;
    const auto& slots = role_slots_[static_cast<size_t>(pc.pri->team->team_index) & 1];
    if (idx < 0 || static_cast<size_t>(idx) >= slots.size() || !slots[static_cast<size_t>(idx)].role) return;
    const ServerRoundActor* role = slots[static_cast<size_t>(idx)].role;

    // The role's grenades (retail frame 376: the PRI counts at spawn).
    auto role_int = [&](uint32_t field) {
        const DataField* f = role->Get(field);
        return f ? static_cast<int32_t>(f->v.i) : 0;
    };
    pc.pri->frag_grenades = role_int(CR::iFragGrenades);
    pc.pri->smoke_grenades = role_int(CR::iSmokeGrenades);
    pc.pri->m106_grenades = role_int(CR::iM106SmokeGrenades);
    pc.pri->stun_grenades = role_int(CR::iStunGrenades);
    pc.pri->incendiary_grenades = role_int(CR::iIncendiaryGrenades);

    const DataField* wf = role->Get(CR::wpnPrimaryOne);
    const DataField* af = role->Get(CR::iPrimaryWeaponOneAmmo);
    if (!wf || wf->v.kind != DataValue::Kind::kObj) return;
    const WeaponKit* kit = nullptr;
    for (const auto& k : kKits) if (k.weapon_cls == wf->v.net) kit = &k;
    if (!kit) {
        LOG_WARN("Role %s: no kit for weapon class %u; keeping the M4A1",
                 pc.pri->current_class_role_id.c_str(), wf->v.net);
        return;
    }
    auto retarget = [&](ServerRoundActor& a, const char* cls, uint32_t class_index, uint32_t max_index) {
        a.class_name = cls;
        a.cdo_net_index = class_index + 1;
        a.object_index = ObjectIndex("AA3Baseline", a.cdo_net_index);
        if (max_index) a.max_index = max_index;
    };
    ServerRoundActor* weapon = nullptr;
    for (auto& it : items) {
        ServerRoundActor& a = *it;
        if (a.class_name.rfind("AA_BulletWeapon", 0) == 0) {
            retarget(a, kit->weapon, kit->weapon_cls, kit->max_index);
            weapon = &a;
        } else if (a.class_name.rfind("AA_AmmoContainer", 0) == 0) {
            retarget(a, kit->mag, kit->mag_cls, 0);
        }
    }
    if (weapon) BuildRailMods(pc, items, *weapon, kit->sight_cls);
    pc.pawn->selected_rof = kit->rof;
    pc.pawn->mag_rounds = kit->mag_rounds;
    pc.pawn->weapon_inv_type = kit->weapon_type;
    pc.pawn->ammo_inv_type = kit->ammo_type;
    if (af) pc.pawn->spare_mags = static_cast<int32_t>(af->v.i);
    // The magazine list: the loaded one plus the spares, all full (retail
    // frame 371: eight 30-round entries for the DMR, none in the weapon
    // until the initial reload loads entry 0).
    {
        auto& pri = *pc.pri;
        pri.primary_mags = {};
        const int total = std::clamp(pc.pawn->spare_mags + 1, 0, ServerPlayerReplicationInfo::kMaxMagazines);
        for (int i = 0; i < total; ++i) pri.primary_mags[static_cast<size_t>(i)].rounds = kit->mag_rounds;
        pri.primary_mag_index = 0;
        pri.primary_weapon_active = true;
        pri.mag_class_pkg = "AA3Baseline";
        pri.mag_class_net = kit->mag_cls;
        pri.mag_actor = nullptr;
        for (const auto& it : items)
            if (it->class_name.rfind("AA_AmmoContainer", 0) == 0) { pri.mag_actor = it.get(); break; }
    }
    const DataField* sight = weapon ? weapon->Get(kSightType) : nullptr;
    LOG_INFO("Kit for %s: %s, %d magazines, %s; grenades frag %d smoke %d M106 %d stun %d incendiary %d",
             pc.pri->player_name.c_str(), kit->weapon, pc.pawn->spare_mags,
             sight ? sight->v.s.c_str() : "no sight", pc.pri->frag_grenades, pc.pri->smoke_grenades,
             pc.pri->m106_grenades, pc.pri->stun_grenades, pc.pri->incendiary_grenades);
}

// The rifle's mods, as AA_Weapon.AttachWeaponModToRail builds them on the
// server: one AA_WeaponModBase actor per filled rail of the PRI's
// mriPrimaryWeaponOneMods (ParentWeapon = the rifle, ParentRail, Base the
// rifle), linked through NextModPtr from AA_Weapon.ModInventory; the main
// rail's class is the rifle's sWeaponSightType. Retail's only sample is the
// CCO alone (frame 366): its field set is the template every mod copies.
void World::BuildRailMods(ServerPlayerController& pc, std::vector<std::unique_ptr<ServerRoundActor>>& items,
                          ServerRoundActor& weapon, uint32_t default_sight_cls) {
    auto tpl_it = std::find_if(items.begin(), items.end(), [](const std::unique_ptr<ServerRoundActor>& a) {
        return a->class_name.rfind("AA_WeaponModSights", 0) == 0;
    });
    if (tpl_it == items.end()) return;
    ServerRoundActor* tpl = tpl_it->get();

    std::vector<std::pair<uint8_t, const ModClass*>> wanted;    // rail enum, class
    bool any = false;
    for (size_t r = 0; r < pc.pri->primary_mods.size(); ++r) {
        const auto& m = pc.pri->primary_mods[r];
        if (m.first.empty()) continue;
        any = true;
        const ModClass* mc = m.first == "AA3Baseline" ? FindMod(m.second) : nullptr;
        if (!mc) {
            LOG_WARN("Rail mod %s/%u not modelled; left off the rifle", m.first.c_str(), m.second);
            continue;
        }
        wanted.push_back({kRailEnum[r], mc});
    }
    if (!any)                                // no role pick at all: the kit's sight
        if (const ModClass* mc = FindMod(default_sight_cls)) wanted.push_back({0, mc});
    // Main rail first: the sight heads the list, as in retail's sample.
    std::stable_sort(wanted.begin(), wanted.end(),
                     [](const auto& a, const auto& b) { return (a.first == 0) > (b.first == 0); });

    auto erase_field = [](ServerRoundActor& a, uint32_t idx) {
        a.fields.erase(std::remove_if(a.fields.begin(), a.fields.end(),
                                      [idx](const DataField& f) { return f.idx == idx; }),
                       a.fields.end());
    };
    if (wanted.empty()) {                    // bare rifle
        items.erase(tpl_it);
        erase_field(weapon, kModInventory);
        erase_field(weapon, kSightType);
        return;
    }
    std::vector<ServerRoundActor*> mods;
    for (size_t i = 0; i < wanted.size(); ++i) {
        ServerRoundActor* m = tpl;
        if (i > 0) {
            items.push_back(std::make_unique<ServerRoundActor>(*tpl));
            m = items.back().get();
        }
        const ModClass& mc = *wanted[i].second;
        m->class_name = mc.name;
        m->cdo_net_index = mc.cls + 1;
        m->object_index = ObjectIndex("AA3Baseline", m->cdo_net_index);
        m->max_index = mc.max_index;
        if (wanted[i].first != 0) {          // AR_RailMain is the default
            DataValue rail; rail.kind = DataValue::Kind::kByte; rail.bits = 3; rail.i = wanted[i].first;
            m->Set(kModParentRail, -1, rail);
        }
        mods.push_back(m);
    }
    for (size_t i = 0; i + 1 < mods.size(); ++i)
        mods[i]->Set(kModNextModPtr, -1, DataValue::Actor(mods[i + 1]));
    weapon.Set(kModInventory, -1, DataValue::Actor(mods[0]));
    if (wanted[0].first == 0 && wanted[0].second->sight)
        weapon.Set(kSightType, -1, DataValue::Str(wanted[0].second->name));
    else
        erase_field(weapon, kSightType);
}

// AA_PlayerController.ServerSetRailMod(sWeapon, sRail, iModIndex) as the
// role menu sends it (retail: ("PrimaryOne","Main",1) -> CCO, then
// ("PrimaryOne","Right",-1) -> no light). AA_TeamInfo.IsWeaponModValid:
// the index must name an entry of the role's list for that rail.
bool World::SetRailMod(ServerPlayerController& pc, const std::string& weapon,
                       const std::string& rail, int32_t index) {
    namespace CR = network::rep::ClassRoleRI;
    if (!pc.pri || !pc.pri->team) return false;
    auto lower = [](std::string s) {
        for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    if (lower(weapon) != "primaryone") return false;      // the only weapon a role carries
    static const struct { const char* name; size_t slot; uint32_t field; } kRails[] = {
        {"main", 0, CR::aawmbPrimaryOneMainRail},   {"top", 1, CR::aawmbPrimaryOneTopRail},
        {"left", 2, CR::aawmbPrimaryOneLeftRail},   {"right", 3, CR::aawmbPrimaryOneRightRail},
        {"bottom", 4, CR::aawmbPrimaryOneBottomRail},
    };
    const std::string r = lower(rail);
    const auto* rd = std::find_if(std::begin(kRails), std::end(kRails),
                                  [&](const auto& x) { return r == x.name; });
    if (rd == std::end(kRails)) return false;
    const int32_t role_index = pc.pri->assigned_class_role_index;
    const auto& slots = role_slots_[static_cast<size_t>(pc.pri->team->team_index) & 1];
    if (role_index < 0 || static_cast<size_t>(role_index) >= slots.size() ||
        !slots[static_cast<size_t>(role_index)].role)
        return false;
    auto& slot = pc.pri->primary_mods[rd->slot];
    if (index < 0) {
        slot = {std::string(), 0u};
        return true;
    }
    const DataField* m = slots[static_cast<size_t>(role_index)].role->Get(rd->field, index);
    if (!m || m->v.kind != DataValue::Kind::kObj) return false;
    slot = {m->v.s, m->v.net};
    return true;
}

// ---------------------------------------------------------------- grenades
//
// Retail GamePlayPCAP_Legit_Server (third life, Bridge classic), doc sec 37:
//   ServerSetCurrentWeapon(M67)  t=444.92: weapon graph {151->151}; PC
//       ClientSendHUDNotifyChangeInventoryCount(5, -1) +
//       ClientAdvancedInventoryUpdateGrenadeCounts(5, False)
//     +0.23 grenade bHidden/bHiddenByInvManager False, ClientNotifyChangedWeapon
//           (M67, rifle), graph {151->80}, ActiveInventory = M67
//     +0.27 rifle AttachedActor = its slot; +0.65 M67 AttachedActor = pawn
//     +0.73 PRI iFragGrenades 2 -> 1
//   ServerStartFire     t=446.01: bPulledPin, graph {89->90} (underhand {154->133})
//   CookIt              t=447.82: bCooking, ClientStartCookingDelayedRestoreTimer()
//   ServerStopFire      t=449.25: graph {91->91} (underhand {93->92})
//     +0.49 throw: graph {91->77}; projectile opens (Location, Rotation,
//           Velocity, Instigator, fExplodeTime 5 - 1.92 cooked = 3.1); the
//           thrown grenade loses Owner/bNetOwner/AttachedActor; a new one
//           of its type takes its aatwPreloadedGrenades slot;
//           ClientSwitchToBestWeapon(True)
//   +3.06 blast (bCollideActors False); +6.0 (LifeSpan) channel closed.
//   ServerSetCurrentWeapon(rifle): graph {150->79}, ActiveInventory = rifle,
//     ClientAdvancedInventoryUpdateGrenadeCounts(5, True), ClientNotifyChangedWeapon.

uint8_t World::GrenadeInventoryType(const ServerPawn* pawn, const ServerActor* a) const {
    auto it = pawn_inventory_.find(pawn);
    if (it == pawn_inventory_.end() || !a) return 0;
    for (const auto& item : it->second)
        if (item.get() == a)
            if (const GrenadeSpec* g = GrenadeSpecOf(item->class_name)) return g->inv_type;
    return 0;
}

bool World::TakeGrenade(ServerPawn* pawn, const ServerActor* grenade) {
    if (!pawn || !pawn->pri || pawn->torn_off || pawn->incapacitated) return false;
    ServerRoundActor* g = FindInventory(pawn, grenade);
    const GrenadeSpec* spec = g ? GrenadeSpecOf(g->class_name) : nullptr;
    if (!spec) return false;
    if (pawn->held_grenade == grenade) return true;
    if (pawn->held_grenade) PutAwayGrenade(pawn, nullptr);
    int32_t& count = pawn->pri->*(spec->count);
    if (count <= 0) {
        LOG_INFO("%s has no %s left", pawn->pri->player_name.c_str(), spec->weapon);
        return false;
    }
    --count;                                         // AA_InventoryManager.AdjustGrenadeCount
    pawn->rifle = pawn->active_inventory && pawn->active_inventory != grenade
                      ? pawn->active_inventory : pawn->loaded_weapon;
    pawn->held_grenade = grenade;
    pawn->grenade_equip_at = clock_;
    pawn->grenade_stage = 0;
    pawn->pin_pulled = pawn->cooking = pawn->underhand = false;
    pawn->throw_at = -1.0;
    pawn->rifle_draw_at = -1.0;
    pawn->trigger_held = false;
    pawn->burst_left = 0;
    pawn->SetGraph(4, 151, 151);                     // rifle away
    LOG_INFO("%s takes out %s (%d left)", pawn->pri->player_name.c_str(), spec->weapon, count);
    return true;
}

const ServerActor* World::PutAwayGrenade(ServerPawn* pawn, const ServerActor* weapon) {
    if (!pawn) return nullptr;
    const ServerActor* held = pawn->held_grenade;
    ServerRoundActor* g = FindInventory(pawn, held);
    // Not thrown: back in the pouch and back in the count
    // (AA_InventoryManager.CleanupInactiveInventory -> AddGrenadeCount).
    if (g && pawn->pri)
        if (const GrenadeSpec* spec = GrenadeSpecOf(g->class_name)) {
            ++(pawn->pri->*(spec->count));
            g->Set(kHiddenByInvManager, -1, DataValue::Bool(true));
            g->Set(kPulledPin, -1, DataValue::Bool(false));
            g->Set(kCooking, -1, DataValue::Bool(false));
            if (const DataField* c = g->Get(kLastInventoryContainer)) g->Set(kAttachedActor, -1, c->v);
        }
    pawn->held_grenade = nullptr;
    pawn->pin_pulled = pawn->cooking = false;
    pawn->throw_at = -1.0;
    const ServerActor* rifle = weapon ? weapon : pawn->rifle;
    if (rifle && FindInventory(pawn, rifle)) {
        pawn->active_inventory = rifle;
        pawn->SetGraph(4, 150, 79);                  // draw
        pawn->rifle_draw_at = clock_ + 0.2;
    }
    return held;
}

void World::GrenadeAction(ServerPawn* pawn, const ServerActor* grenade, GrenadeInput in) {
    if (!pawn || pawn->torn_off || pawn->incapacitated || !grenade || pawn->held_grenade != grenade) return;
    ServerRoundActor* g = FindInventory(pawn, grenade);
    const GrenadeSpec* spec = g ? GrenadeSpecOf(g->class_name) : nullptr;
    if (!spec) return;
    switch (in) {
    case GrenadeInput::kSwitchThrowType:
        if (!pawn->pin_pulled) pawn->underhand = !pawn->underhand;
        break;
    case GrenadeInput::kPullPin:
        if (pawn->pin_pulled) break;
        pawn->pin_pulled = true;
        g->Set(kPulledPin, -1, DataValue::Bool(true));
        if (pawn->underhand) pawn->SetGraph(4, 154, 133);
        else pawn->SetGraph(4, 89, 90);
        break;
    case GrenadeInput::kCook:
        // AA_ThrowWeapon.CookIt is global: any time the grenade in hand
        // isn't already cooking. The client sends it twice (retail).
        if (pawn->cooking) break;
        pawn->cooking = true;
        pawn->cook_start = clock_;
        pawn->cooked_grenade = g;
        pawn->cook_clear_at = clock_ + spec->fuse;
        pawn->cook_restore_at = clock_ + spec->fuse + 0.25 + 0.9;
        g->Set(kCooking, -1, DataValue::Bool(true));
        break;
    case GrenadeInput::kRelease:
        if (!pawn->pin_pulled || pawn->throw_at >= 0) break;
        if (pawn->underhand) pawn->SetGraph(4, 93, 92);
        else pawn->SetGraph(4, 91, 91);
        pawn->throw_at = clock_ + 0.49;
        break;
    }
}

// The new grenade that takes a thrown one's place (AA_InventoryManager.
// RecreateGrenade): a fresh copy of the spawn inventory's grenade of that
// class, in the dump pouch, in the same aatwPreloadedGrenades slot.
void World::ReplaceGrenade(ServerPawn& pawn, ServerRoundActor& thrown) {
    const GrenadeSpec* spec = GrenadeSpecOf(thrown.class_name);
    ServerPlayerController* pc = pawn.controller;
    if (!spec || !pc) return;
    auto t = std::find_if(inventory_template_.begin(), inventory_template_.end(),
                          [&](const ActorTemplate& a) { return a.proto.class_name == thrown.class_name; });
    if (t == inventory_template_.end()) return;
    auto made = Instantiate({*t}, {{2, pc}, {10, pc->pri}, {18, pc->proxy}, {159, &pawn},
                                   {160, pawn.inv_manager}, {149, pawn.dump_pouch}});
    if (made.empty()) return;
    std::unique_ptr<ServerRoundActor> fresh = std::move(made[0]);
    fresh->fields.erase(std::remove_if(fresh->fields.begin(), fresh->fields.end(),
                                       [](const DataField& f) { return f.idx == kInventory; }),
                        fresh->fields.end());
    // Inventory.ClientGivenTo(NewOwner, bDoNotActivate): the spawn template's
    // copy names no owner (retail's pawn wasn't open yet); mid-life, retail
    // names the pawn (t=449.74), and that's what puts the grenade in the
    // client's own inventory -- with None the client never used it (live
    // client 2026-10-11: the second smoke couldn't be thrown).
    DataValue given; given.kind = DataValue::Kind::kStruct;
    given.members = {DataValue::Bool(true),               // NewOwner: Send
                     DataValue::Actor(&pawn),             //   the pawn's channel
                     DataValue::Bool(true)};              // bDoNotActivate
    fresh->Set(kClientGivenTo, -1, given);
    if (ServerRoundActor* im = FindInventory(&pawn, pawn.inv_manager))
        im->Set(kPreloadedGrenades, spec->slot, DataValue::Actor(fresh.get()));
    pawn_inventory_[&pawn].push_back(std::move(fresh));
}

void World::ThrowGrenade(ServerPawn& pawn, ServerRoundActor& grenade, bool in_hand) {
    const GrenadeSpec* spec = GrenadeSpecOf(grenade.class_name);
    if (!spec) return;
    const ServerPlayerController* pc = pawn.controller;
    const double k = 3.14159265358979 * 2.0 / 65536.0;
    const int32_t pitch_u = pc ? (pc->rotation_pitch & 0xFFFF) : 0;
    const int32_t yaw_u = pc ? (pc->rotation_yaw & 0xFFFF) : pawn.rotation_yaw & 0xFFFF;
    const double pitch = static_cast<int16_t>(pitch_u) * k, yaw = yaw_u * k;
    const Vec3 dir{static_cast<float>(std::cos(pitch) * std::cos(yaw)),
                   static_cast<float>(std::cos(pitch) * std::sin(yaw)),
                   static_cast<float>(std::sin(pitch))};
    // From the hand: 31 units above the pawn (the bullet trace's eye) and a
    // little forward, pulled back if that's inside a wall.
    const Vec3 eye{pawn.location[0], pawn.location[1], pawn.location[2] + 31.0f};
    Vec3 start = eye;
    Vec3 vel{0, 0, 0};
    if (!in_hand) {
        const Vec3 fwd{eye.x + dir.x * 25.0f, eye.y + dir.y * 25.0f, eye.z + dir.z * 25.0f};
        TraceHit h = collision_.LineTrace(eye, fwd, CollisionWorld::kBlockTrace);
        start = h.hit ? Vec3{h.location.x - dir.x * 4.0f, h.location.y - dir.y * 4.0f, h.location.z - dir.z * 4.0f}
                      : fwd;
        vel = {dir.x * kThrowSpeed + pawn.velocity[0], dir.y * kThrowSpeed + pawn.velocity[1],
               dir.z * kThrowSpeed + pawn.velocity[2]};
    }
    float fuse = spec->fuse;
    if (pawn.cooking) fuse -= static_cast<float>(clock_ - pawn.cook_start);
    if (in_hand || fuse <= 0.0f) fuse = 0.01f;       // AA_ThrowWeapon.SpawnThrowProjectile

    auto proj = std::make_unique<ServerRoundActor>();
    proj->class_name = spec->projectile;
    proj->cdo_package = "AA3Baseline";
    proj->cdo_net_index = spec->projectile_cls + 1;
    proj->object_index = ObjectIndex("AA3Baseline", proj->cdo_net_index);
    proj->max_index = kProjMaxIndex;
    proj->transient = true;
    proj->label = std::string(spec->projectile) + ":" + (pawn.pri ? pawn.pri->player_name : "?");
    proj->location[0] = start.x; proj->location[1] = start.y; proj->location[2] = start.z;
    DataValue rot; rot.kind = DataValue::Kind::kRotator;
    rot.v3[0] = pitch_u; rot.v3[1] = yaw_u; rot.v3[2] = 0;
    proj->Set(kProjRotation, -1, rot);
    if (!in_hand) proj->Set(kProjVelocity, -1, VectorValue(vel.x, vel.y, vel.z));
    proj->Set(kInstigator, -1, DataValue::Actor(&pawn));
    if (std::fabs(fuse - spec->fuse) > 0.001f) {     // only a cooked fuse differs from the default
        DataValue f; f.kind = DataValue::Kind::kFloat; f.f = std::round(fuse * 100.0f) / 100.0f;
        proj->Set(kProjExplodeTime, -1, f);
    }
    ThrownGrenade t;
    t.actor = std::move(proj);
    t.instigator = &pawn;
    t.spec = static_cast<int>(spec - kGrenades);
    t.thrown_at = clock_;
    t.explode_at = clock_ + fuse;
    t.gone_at = clock_ + spec->life;
    t.drop_at = t.gone_at + 10.0;
    t.pos = start;
    t.vel = vel;
    t.resting = in_hand;
    thrown_.push_back(std::move(t));

    // The grenade that left the hand: no owner, off the pawn; a new one
    // takes its place among the preloaded grenades.
    grenade.Set(kOwner, -1, DataValue::Actor(nullptr));
    grenade.Set(kNetOwner, -1, DataValue::Bool(false));
    grenade.Set(kAttachedActor, -1, DataValue::Actor(nullptr));
    if (pawn.underhand) pawn.SetGraph(4, 92, 77);
    else pawn.SetGraph(4, 91, 77);
    ReplaceGrenade(pawn, grenade);
    pawn.held_grenade = nullptr;
    pawn.pin_pulled = pawn.cooking = false;
    pawn.throw_at = -1.0;
    ++pawn.switch_best_serial;
    LOG_INFO("%s threw %s (fuse %.2fs)%s", pawn.pri ? pawn.pri->player_name.c_str() : "?",
             spec->projectile, fuse, in_hand ? " -- cooked off in hand" : "");
}

// An NPC lobs a frag (or smoke) at `target`: the launch angle that carries
// the throw speed's arc to the target's distance under the world's gravity
// (45 degrees when it can't reach), from 31 units above the pawn.
void World::NpcThrowFrag(ServerPawn& npc, const Vec3& target, bool smoke) {
    const GrenadeSpec* spec = GrenadeSpecOf(smoke ? "AA_ThrowWeapon_U72" : "AA_ThrowWeapon_U23A1");
    if (!spec) return;
    const Vec3 start{npc.location[0], npc.location[1], npc.location[2] + 31.0f};
    const float dx = target.x - start.x, dy = target.y - start.y;
    const float d = std::sqrt(dx * dx + dy * dy);
    if (d < 1.0f) return;
    const float g = std::fabs(world_info_ ? world_info_->world_gravity_z : -520.0f);
    const float s = std::min(1.0f, g * d / (kThrowSpeed * kThrowSpeed));
    const float ang = 0.5f * std::asin(s);
    const Vec3 vel{dx / d * kThrowSpeed * std::cos(ang), dy / d * kThrowSpeed * std::cos(ang), kThrowSpeed * std::sin(ang)};
    auto proj = std::make_unique<ServerRoundActor>();
    proj->class_name = spec->projectile;
    proj->cdo_package = "AA3Baseline";
    proj->cdo_net_index = spec->projectile_cls + 1;
    proj->object_index = ObjectIndex("AA3Baseline", proj->cdo_net_index);
    proj->max_index = kProjMaxIndex;
    proj->transient = true;
    proj->label = std::string(spec->projectile) + ":" + npc.npc_name;
    proj->location[0] = start.x; proj->location[1] = start.y; proj->location[2] = start.z;
    DataValue rot; rot.kind = DataValue::Kind::kRotator;
    rot.v3[0] = static_cast<int32_t>(ang * 32768.0f / 3.14159265f) & 0xFFFF;
    rot.v3[1] = static_cast<int32_t>(std::atan2(dy, dx) * 32768.0f / 3.14159265f) & 0xFFFF;
    proj->Set(kProjRotation, -1, rot);
    proj->Set(kProjVelocity, -1, VectorValue(vel.x, vel.y, vel.z));
    proj->Set(kInstigator, -1, DataValue::Actor(&npc));
    ThrownGrenade t;
    t.actor = std::move(proj);
    t.instigator = &npc;
    t.spec = static_cast<int>(spec - kGrenades);
    t.thrown_at = clock_;
    t.explode_at = clock_ + spec->fuse;
    t.gone_at = clock_ + spec->life;
    t.drop_at = t.gone_at + 10.0;
    t.pos = start;
    t.vel = vel;
    thrown_.push_back(std::move(t));
    LOG_INFO("%s threw a %s at (%.0f, %.0f, %.0f)", npc.npc_name.c_str(), smoke ? "smoke" : "frag", target.x, target.y,
             target.z);
}

void World::UpdateGrenades(double dt) {
    // Grenade in hand: the equip timeline, a cook running out, the throw.
    for (auto& p : pawns_) {
        ServerPawn& pawn = *p;
        if (pawn.cook_clear_at >= 0 && clock_ >= pawn.cook_clear_at) {
            pawn.cook_clear_at = -1.0;
            if (ServerRoundActor* c = FindInventory(&pawn, pawn.cooked_grenade))
                c->Set(kCooking, -1, DataValue::Bool(false));
        }
        if (pawn.cook_restore_at >= 0 && clock_ >= pawn.cook_restore_at) {
            pawn.cook_restore_at = -1.0;
            pawn.cooked_grenade = nullptr;
            if (!pawn.torn_off && !pawn.incapacitated && pawn.rifle && FindInventory(&pawn, pawn.rifle)) {
                if (pawn.held_grenade) PutAwayGrenade(&pawn, pawn.rifle);
                ++pawn.weapon_restore_serial;
            }
        }
        if (pawn.rifle_draw_at >= 0 && clock_ >= pawn.rifle_draw_at) {
            pawn.rifle_draw_at = -1.0;
            if (ServerRoundActor* r = FindInventory(&pawn, pawn.rifle))
                r->Set(kAttachedActor, -1, DataValue::Actor(&pawn));
        }
        if (!pawn.held_grenade) continue;
        ServerRoundActor* g = FindInventory(&pawn, pawn.held_grenade);
        if (!g || pawn.torn_off) {
            pawn.held_grenade = nullptr;
            continue;
        }
        const double t = clock_ - pawn.grenade_equip_at;
        if (pawn.grenade_stage == 0 && t >= 0.23) {
            g->Set(kHidden, -1, DataValue::Bool(false));
            g->Set(kHiddenByInvManager, -1, DataValue::Bool(false));
            pawn.active_inventory = g;
            if (!pawn.pin_pulled) pawn.SetGraph(4, 151, 80);
            if (ServerRoundActor* r = FindInventory(&pawn, pawn.rifle))
                if (const DataField* c = r->Get(kLastInventoryContainer)) r->Set(kAttachedActor, -1, c->v);
            pawn.grenade_stage = 1;
        }
        if (pawn.grenade_stage == 1 && t >= 0.65) {
            g->Set(kAttachedActor, -1, DataValue::Actor(&pawn));
            pawn.grenade_stage = 2;
        }
        const GrenadeSpec* spec = GrenadeSpecOf(g->class_name);
        if (spec && pawn.cooking && clock_ - pawn.cook_start >= spec->fuse) {
            pawn.holder_died_grenade = g;                // held too long
            ++pawn.holder_died_serial;
            ThrowGrenade(pawn, *g, /*in_hand=*/true);
        } else if (pawn.throw_at >= 0 && clock_ >= pawn.throw_at) {
            ThrowGrenade(pawn, *g, /*in_hand=*/false);
        }
    }

    // In flight: gravity and bounces (AA_ThrowProjectile.Bounce: velocity
    // times the surface's restitution, then reflected), until it rests.
    const float gravity = world_info_ ? world_info_->world_gravity_z : -520.0f;
    for (auto& g : thrown_) {
        if (g.gone) continue;
        if (!g.resting && !g.exploded) {
            double left = dt;
            while (left > 1e-6 && !g.resting) {
                const float step = static_cast<float>(std::min(left, 1.0 / 60.0));
                left -= step;
                g.vel.z += gravity * step;
                const Vec3 next{g.pos.x + g.vel.x * step, g.pos.y + g.vel.y * step, g.pos.z + g.vel.z * step};
                TraceHit h = collision_.LineTrace(g.pos, next, CollisionWorld::kBlockTrace);
                if (!h.hit) { g.pos = next; continue; }
                if (!g.npcs_told) {                    // NotifyGrenadeBounced to the NPCs near
                    g.npcs_told = true;
                    NpcGrenadeNear(h.location, kGrenades[g.spec].blast == Blast::kFrag, g.instigator);
                }
                g.pos = {h.location.x + h.normal.x, h.location.y + h.normal.y, h.location.z + h.normal.z};
                g.vel = {g.vel.x * kRestitution, g.vel.y * kRestitution, g.vel.z * kRestitution};
                const float vn = g.vel.x * h.normal.x + g.vel.y * h.normal.y + g.vel.z * h.normal.z;
                g.vel = {g.vel.x - 2 * vn * h.normal.x, g.vel.y - 2 * vn * h.normal.y, g.vel.z - 2 * vn * h.normal.z};
                if (Length(g.vel) < kMinRollVelocity) { g.vel = {0, 0, 0}; g.resting = true; }
            }
            if (g.pos.z < -1.0e6f) g.resting = true;     // fell out of the world
        }
        if (!g.exploded && clock_ >= g.explode_at) ExplodeGrenade(g);
        if (g.exploded && kGrenades[g.spec].blast == Blast::kIncendiary &&
            clock_ < g.explode_at + kBurnDuration && clock_ >= g.next_burn) {
            BurnAround(g);
            g.next_burn = clock_ + 1.0;
        }
        if (clock_ >= g.gone_at) g.gone = true;
    }
    thrown_.erase(std::remove_if(thrown_.begin(), thrown_.end(),
                                 [&](const ThrownGrenade& g) { return g.gone && clock_ >= g.drop_at; }),
                  thrown_.end());
}

// The blast (AA_Projectile.HurtRadius): an M67 hurts every soldier in
// sight within DamageRadius, by its DamageLookupTable. Smoke and the M84's
// flash are the clients' own (they run the projectile's DoExplosion on the
// replicated fuse); the AN-M14 burns (BurnAround).
void World::ExplodeGrenade(ThrownGrenade& g) {
    g.exploded = true;
    g.actor->Set(kProjCollideActors, -1, DataValue::Bool(false));   // retail t=452.80
    g.actor->location[0] = g.pos.x; g.actor->location[1] = g.pos.y; g.actor->location[2] = g.pos.z;
    const GrenadeSpec& spec = kGrenades[g.spec];
    LOG_INFO("%s went off at (%.0f, %.0f, %.0f)", spec.projectile, g.pos.x, g.pos.y, g.pos.z);
    if (spec.blast == Blast::kIncendiary) { g.next_burn = clock_; return; }
    if (spec.blast == Blast::kSmoke) { NpcSmoke(g.pos, 45.0); return; }
    if (spec.blast == Blast::kStun) { NpcFlashbang(g.pos); return; }
    if (spec.blast != Blast::kFrag) return;
    const Vec3 origin{g.pos.x, g.pos.y, g.pos.z + 10.0f};
    for (auto& other : pawns_) {
        ServerPawn& v = *other;
        if (v.torn_off || (!v.controller && !v.is_npc)) continue;   // players and enemy NPCs
        const Vec3 to{v.location[0] - g.pos.x, v.location[1] - g.pos.y, v.location[2] - g.pos.z};
        const float dist = Length(to);
        const size_t n = sizeof(kFragTable) / sizeof(kFragTable[0]);
        if (dist >= kFragTable[n - 1].distance) continue;
        const Vec3 target{v.location[0], v.location[1], v.location[2]};
        TraceHit h = collision_.LineTrace(origin, target, CollisionWorld::kBlockTrace);
        if (h.hit) continue;                                         // behind cover
        size_t i = 1;
        while (i < n && dist >= kFragTable[i].distance) ++i;
        const DamageStep& a = kFragTable[i - 1];
        const DamageStep& b = kFragTable[i];
        const float pct = a.percent + (b.percent - a.percent) * (dist - a.distance) / (b.distance - a.distance);
        const Vec3 dir = dist > 1.0f ? Vec3{to.x / dist, to.y / dist, to.z / dist} : Vec3{0, 0, 1};
        ApplyDamage(v, g.instigator, kFragDamage * pct, a.damage_class, target, dir);
    }
}

// AN-M14 TH3: Damage 10 by AA_DamageType_Fire within 175 units
// (its DamageLookupTable) for BurnDuration 20 s; once a second here --
// retail's tick rate for the burn isn't in the capture.
void World::BurnAround(ThrownGrenade& g) {
    for (auto& other : pawns_) {
        ServerPawn& v = *other;
        if (v.torn_off || (!v.controller && !v.is_npc)) continue;
        const Vec3 to{v.location[0] - g.pos.x, v.location[1] - g.pos.y, v.location[2] - g.pos.z};
        const float dist = Length(to);
        if (dist >= kBurnRadius) continue;
        const Vec3 dir = dist > 1.0f ? Vec3{to.x / dist, to.y / dist, to.z / dist} : Vec3{0, 0, 1};
        ApplyDamage(v, g.instigator, kBurnDamage, kFireDamage, Vec3{v.location[0], v.location[1], v.location[2]}, dir);
    }
}

// ---------------------------------------------------------------- ES2 perks
//
// AA_ES2InstaPerk.PerformES2Use (no retail sample of a pick-up yet; the
// live client sends ServerES2Active + ServerUse + ServerES2Use(perk) per
// press). eES2InstaPerk: 0 M4 magazine, 1 M249 box, 2 M67, 3 M84, 4 M83,
// 5 M106, 6 AN-M14, 7 M433, 8 M714, 9 IFAK, 10 enemy display.
// AA_PlayerReplicationInfo caps: MAX_GRENADES 12 (counting one in hand),
// MAX_MAGAZINES 9, MAX_M249AMMO 4. 40 mm rounds and IFAKs aren't modelled.
bool World::UseES2Object(ServerPlayerController& pc, const ServerActor* obj) {
    constexpr uint32_t kPerkAmount = 37, kPerkType = 38;
    ServerRoundActor* perk = nullptr;
    for (auto& a : match_actors_)
        if (a.get() == obj && a->class_name == "AA_ES2InstaPerk" && !a->gone) perk = a.get();
    ServerPawn* pawn = pc.pawn;
    if (!perk || !pawn || !pc.pri || pawn->torn_off || pawn->incapacitated) return false;
    const DataField* amount_f = perk->Get(kPerkAmount);
    const DataField* type_f = perk->Get(kPerkType);
    const int64_t amount = amount_f ? amount_f->v.i : 0;
    const int64_t type = type_f ? type_f->v.i : 0;
    if (amount <= 0) return false;

    ServerPlayerReplicationInfo& pri = *pc.pri;
    bool took = false;
    static int32_t ServerPlayerReplicationInfo::* const kPerkGrenade[] = {
        &ServerPlayerReplicationInfo::frag_grenades,  &ServerPlayerReplicationInfo::stun_grenades,
        &ServerPlayerReplicationInfo::smoke_grenades, &ServerPlayerReplicationInfo::m106_grenades,
        &ServerPlayerReplicationInfo::incendiary_grenades};
    if (type >= 2 && type <= 6) {
        // AA_PlayerReplicationInfo.CanAddGrenade: 12 at most, one in hand counted.
        const int32_t carried = pri.frag_grenades + pri.smoke_grenades + pri.m106_grenades +
                                pri.stun_grenades + pri.incendiary_grenades + (pawn->held_grenade ? 1 : 0);
        if (carried < 12) {
            ++(pri.*kPerkGrenade[type - 2]);
            took = true;
        }
    } else if (type == 0 || type == 1) {
        // A magazine for the weapon it fits (ValidatePlayerQualificationForPerk):
        // 30-round magazines for the M4/M16 family, boxes for the M249.
        const bool box = type == 1;
        if ((pawn->ammo_inv_type == 18) == box && pawn->spare_mags < (box ? 4 : 9)) {
            for (auto& m : pri.primary_mags)
                if (!m.in_weapon && m.rounds == 0) { m.rounds = pawn->mag_rounds; break; }
            ++pawn->spare_mags;
            ++pawn->mag_count_serial;
            took = true;
        }
    }
    if (!took) {
        LOG_INFO("%s can't take from ES2 perk type %lld", pri.player_name.c_str(), static_cast<long long>(type));
        return false;
    }
    perk->Set(kPerkAmount, -1, DataValue::Int(amount - 1));
    if (amount - 1 < 1) perk->gone = true;        // "how perky is it really" -> Destroy()
    LOG_INFO("%s took from ES2 perk type %lld (%lld left)", pri.player_name.c_str(),
             static_cast<long long>(type), static_cast<long long>(amount - 1));
    return true;
}

}  // namespace game
}  // namespace aa3
