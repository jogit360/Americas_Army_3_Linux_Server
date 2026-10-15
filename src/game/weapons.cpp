// Soldier weapons (doc sec 34/37): each role's rifle kit, the rail mods the
// player picked for it, and hand grenades from the pick-up to the blast.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
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
    // XM320 rounds (no weapon of their own; fired by FireLauncher): they go
    // off on impact, so no fuse. The M433 is gone when it bursts (LifeSpan
    // 0, Destroy in Explode); the M714 smokes for its LifeSpan of 60 s.
    {"",                          27823, "AA_Projectile_M433_HEDP",       0,
     &ServerPlayerReplicationInfo::m433_rounds,        -1, 0.0f,  0.0f, Blast::kFrag},
    {"",                          27867, "AA_Projectile_M714",            0,
     &ServerPlayerReplicationInfo::m714_rounds,        -1, 0.0f, 60.0f, Blast::kSmoke},
};
constexpr int kM433Spec = 7, kM714Spec = 8;     // their kGrenades entries
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
// The M433's own table (AA_Projectile_M433_HEDP, Damage 100, DamageRadius
// 524.93): deadly to 66, two-thirds at 175, a third at 437.
constexpr DamageStep kM433Table[] = {
    {0.0f,   1.0f,   6475},
    {66.0f,  1.0f,   6479},
    {175.0f, 0.667f, 6481},
    {437.0f, 0.337f, 6477},
    {524.0f, 0.0f,   0},
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

    // The loadout (AA_MCMissionManager.AssignLoadout): standard is the
    // role's own counts, light and heavy its rltLightLoadout /
    // rltHeavyLoadout (RoleLoadoutTypes: bDefined, iPrimaryOneAmmo, ...,
    // iFragGrenades 5, iSmokeGrenades 6, iM106SmokeGrenades 7,
    // iIncendiaryGrenades 8, iStunGrenades 9, mcpmaPrimaryOneModAmmo 10
    // {iM203FragGrenades, iM203SmokeGrenades}). Retail frame 376: the PRI
    // counts at spawn.
    auto role_int = [&](uint32_t field) {
        const DataField* f = role->Get(field);
        return f ? static_cast<int32_t>(f->v.i) : 0;
    };
    const int lo = std::min<int>(pc.pri->selected_loadout, 2);
    auto struct_int = [&](uint32_t field, size_t member, int sub = -1) -> int32_t {
        const DataField* f = role->Get(field);
        if (!f || f->v.kind != DataValue::Kind::kStruct || member >= f->v.members.size()) return 0;
        const DataValue& m = f->v.members[member];
        if (sub < 0) return static_cast<int32_t>(m.i);
        if (m.kind != DataValue::Kind::kStruct || static_cast<size_t>(sub) >= m.members.size()) return 0;
        return static_cast<int32_t>(m.members[static_cast<size_t>(sub)].i);
    };
    const uint32_t lo_field = lo == 0 ? CR::rltLightLoadout : CR::rltHeavyLoadout;
    auto pick = [&](uint32_t standard, size_t member) {
        return lo == 1 ? role_int(standard) : struct_int(lo_field, member);
    };
    pc.pri->frag_grenades = pick(CR::iFragGrenades, 5);
    if (lo == 1 && pc.pri->frag_grenades == 0)            // "if( aamcrpsi.iFragGrenades == 0 )"
        pc.pri->frag_grenades = struct_int(CR::rltMediumLoadout, 5);
    pc.pri->smoke_grenades = pick(CR::iSmokeGrenades, 6);
    pc.pri->m106_grenades = pick(CR::iM106SmokeGrenades, 7);
    pc.pri->incendiary_grenades = pick(CR::iIncendiaryGrenades, 8);
    pc.pri->stun_grenades = pick(CR::iStunGrenades, 9);

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
    // An XM320 on the bottom rail: its 40 mm rounds (AA_MCMissionManager,
    // standard loadout: at least 2 of each; one M433 comes loaded).
    pc.pri->m433_rounds = pc.pri->m714_rounds = 0;
    pc.pawn->launcher = pc.pawn->launcher_mag = nullptr;
    if (weapon) {
        for (auto& it : items) {
            if (it->class_name != "AA_WeaponModWeapon_XM320") continue;
            // AA_MCMissionManager: M433 light at least 1, standard at
            // least 2, heavy at least 3 (a reinforcement gets 10), less
            // the one loaded; M714 at least 1 / 2 / 2 (a reinforcement
            // 6); MAX_M320AMMO 16 in all.
            constexpr int32_t kMaxM320 = 16;
            const bool respawn = pc.reinforcement_spawn;
            int32_t he = lo == 1 ? role_int(CR::iPrimaryOneModM203FragGrenades) : struct_int(lo_field, 10, 0);
            if (lo == 0) he = std::max(he, 1);
            else if (lo == 2) he = std::clamp(respawn ? 10 : he, 3, kMaxM320);
            else he = std::max(he, 2);
            int32_t smoke = lo == 1 ? role_int(CR::iPrimaryOneModM203SmokeGrenades) : struct_int(lo_field, 10, 1);
            if (lo == 0) smoke = std::max(smoke, 1);
            else if (lo == 2) smoke = std::clamp(respawn ? 6 : smoke, 2, kMaxM320);
            else smoke = std::max(smoke, 2);
            pc.pri->m433_rounds = he - 1;
            if (pc.pri->m433_rounds + smoke > kMaxM320) smoke = kMaxM320 - pc.pri->m433_rounds;
            pc.pri->m714_rounds = std::max(smoke, 0);
            SetUpLauncher(pc, items, *weapon, *it);
            break;
        }
    }
    pc.pawn->selected_rof = kit->rof;
    pc.pawn->mag_rounds = kit->mag_rounds;
    pc.pawn->weapon_inv_type = kit->weapon_type;
    pc.pawn->ammo_inv_type = kit->ammo_type;
    // Magazines for the loadout, the loaded one among them (retail Bridge
    // AI capture: heavy DMR, iPrimaryOneAmmo 8 -> eight entries). The M249
    // gets 1 / 2 / 3 boxes when the role says more than 3; none at all
    // falls back to 6 (M16A4), 2 (M249) or 3.
    int32_t mags = lo == 1 ? (af ? static_cast<int32_t>(af->v.i) : 0) : struct_int(lo_field, 1);
    const bool saw = std::string(kit->weapon) == "AA_BulletWeapon_M249";
    if (saw && mags > 3) mags = lo == 0 ? 1 : lo == 2 ? 3 : 2;
    if (mags <= 0) mags = std::string(kit->weapon) == "AA_BulletWeapon_M16A4" ? 6 : saw ? 2 : 3;
    mags = std::min(mags, ServerPlayerReplicationInfo::kMaxMagazines);
    pc.pawn->spare_mags = mags;
    pc.pri->primary_one_ammo = mags;
    // The magazine list, all full (retail frame 371: eight 30-round
    // entries for the DMR, none in the weapon until the initial reload
    // loads entry 0).
    {
        auto& pri = *pc.pri;
        pri.primary_mags = {};
        const int total = mags;
        for (int i = 0; i < total; ++i) pri.primary_mags[static_cast<size_t>(i)].rounds = kit->mag_rounds;
        pri.primary_mag_index = 0;
        pri.primary_weapon_active = true;
        pri.mag_class_pkg = "AA3Baseline";
        pri.mag_class_net = kit->mag_cls;
        pri.mag_actor = nullptr;
        for (const auto& it : items)
            if (it->class_name.rfind("AA_AmmoContainer", 0) == 0) { pri.mag_actor = it.get(); break; }
    }
    // What the rifle and its mods weigh (class Weight defaults; the M95
    // light has none of its own).
    {
        static const std::pair<const char*, float> kWeights[] = {
            {"AA_BulletWeapon_M4A1", 2.52f}, {"AA_BulletWeapon_M16A4", 3.77f}, {"AA_BulletWeapon_M16DMR", 4.0f},
            {"AA_BulletWeapon_M249", 6.68f}, {"AA_WeaponModSights_Iron_US_A3Handle", 0.204f},
            {"AA_WeaponModSights_CCO_US_M68comp4", 0.335f}, {"AA_WeaponModSights_2x_ACOG_RCO", 0.452f},
            {"AA_WeaponModSights_35x_ACOG_RCO", 0.452f}, {"AA_WeaponModSights_ACOG_RCO", 0.452f},
            {"AA_WeaponModSights_4x_US_ACOG_ECOS", 0.452f}, {"AA_WeaponModWeapon_XM320", 1.62f}};
        float w = 0.0f;
        for (const auto& it : items)
            for (const auto& kw : kWeights)
                if (it->class_name == kw.first) w += kw.second;
        pc.pawn->kit_weight = w;
        pc.pawn->mag_weight = saw ? 3.14f : 0.45f;       // 200-round box / 30-round magazine
        pc.pawn->carried_weight = -1.0f;
    }
    const DataField* sight = weapon ? weapon->Get(kSightType) : nullptr;
    LOG_INFO("Kit for %s (%s loadout): %s, %d magazines, %s; grenades frag %d smoke %d M106 %d stun %d incendiary %d%s",
             pc.pri->player_name.c_str(), lo == 0 ? "light" : lo == 2 ? "heavy" : "standard", kit->weapon,
             pc.pawn->spare_mags,
             sight ? sight->v.s.c_str() : "no sight", pc.pri->frag_grenades, pc.pri->smoke_grenades,
             pc.pri->m106_grenades, pc.pri->stun_grenades, pc.pri->incendiary_grenades,
             pc.pawn->launcher ? ("; XM320 with 1 M433 loaded, " + std::to_string(pc.pri->m433_rounds) + " M433 and " +
                                  std::to_string(pc.pri->m714_rounds) + " M714 more").c_str() : "");
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
    // Back to a different rifle than the one put away for the grenade (the
    // client's best weapon can be a picked-up one; 2026-10-14 live): the
    // firing state moves to it, the other stays in its slot.
    if (rifle && rifle != pawn->rifle && pawn->rifle) {
        ServerRoundActor* to = FindInventory(pawn, rifle);
        if (to && to->class_name.rfind("AA_BulletWeapon", 0) == 0) {
            to->Set(kHidden, -1, DataValue::Bool(false));
            pawn->previous_weapon = pawn->rifle;
            if (ServerRoundActor* im = FindInventory(pawn, pawn->inv_manager))
                im->Set(30, -1, DataValue::Actor(pawn->rifle));      // PreviousWeapon
            RifleInHand(*pawn, *to);
            LOG_INFO("%s draws the %s after the grenade", SoldierName(pawn).c_str(), to->class_name.c_str());
        }
    }
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
    if (PlayerRoundStats* rs = LiveStats(&pawn)) {          // round statistics (doc sec 45)
        switch (spec - kGrenades) {
        case 0: ++rs->iM67Thrown; break;
        case 1: ++rs->iM83Thrown; break;
        case 2: ++rs->iM84Thrown; break;
        case 3: rs->bM106Thrown = 1; break;
        case 4: ++rs->iM14Thrown; break;
        default: break;
        }
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
                if (g.impact && !g.dud && !g.exploded) {
                    // A 40 mm round strikes the first wall or soldier in
                    // its path (AA_Projectile.HitWall / Touch -> Explode):
                    // armed, it goes off there; not yet armed, it's a dud
                    // that bounces off (bDeadRound, gone 5 s later).
                    float best = h.hit ? h.time : 1.0f;
                    const Vec3 seg{next.x - g.pos.x, next.y - g.pos.y, next.z - g.pos.z};
                    ServerPawn* struck = TracePawns(g.pos, seg, g.instigator, /*skip_npcs=*/false, &best);
                    if (struck || h.hit) {
                        const Vec3 at{g.pos.x + seg.x * best, g.pos.y + seg.y * best, g.pos.z + seg.z * best};
                        if (clock_ >= g.arm_at) {
                            const Vec3 n = struck ? Vec3{0, 0, 0} : h.normal;
                            g.pos = {at.x + n.x, at.y + n.y, at.z + n.z};
                            LOG_INFO("%s struck %s at (%.0f, %.0f, %.0f), %.0f units out", kGrenades[g.spec].projectile,
                                     struck ? SoldierName(struck).c_str() : "the world", at.x, at.y, at.z,
                                     Length(Vec3{at.x - g.launch.x, at.y - g.launch.y, at.z - g.launch.z}));
                            ExplodeGrenade(g);
                            if (g.spec == kM433Spec) { g.vel = {0, 0, 0}; g.resting = true; break; }
                        } else {
                            g.dud = true;
                            g.gone_at = clock_ + 5.0;          // DestroyProjectile timer
                            g.drop_at = g.gone_at + 10.0;
                            LOG_INFO("%s hit %s before arming (%.2f s): a dud", kGrenades[g.spec].projectile,
                                     struck ? SoldierName(struck).c_str() : "the world", clock_ - g.thrown_at);
                        }
                        if (struck) {                          // off the soldier: drop at his feet
                            g.pos = at;
                            g.vel = {0, 0, std::min(g.vel.z, 0.0f)};
                            continue;
                        }
                    }
                }
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
        if (!g.exploded && !g.impact && clock_ >= g.explode_at) ExplodeGrenade(g);
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
    if (spec.blast == Blast::kIncendiary) {
        // AA_ThrowProjectile_AN_M14_TH3.DoExplosion plays its cue on the
        // server (Role == ROLE_Authority): ClientHearSound of
        // S_Weap_Grenades M14_Explosion (141); the clients don't play it.
        PlaySound("S_Weap_Grenades", 141, g.actor.get(), g.pos, false, true, 0.0f, 8000.0f);
        g.next_burn = clock_;
        return;
    }
    if (spec.blast == Blast::kSmoke) { NpcSmoke(g.pos, 45.0); return; }
    if (spec.blast == Blast::kStun) { NpcFlashbang(g.pos); return; }
    if (spec.blast != Blast::kFrag) return;
    if (g.spec == kM433Spec) {                 // gone with the burst
        g.gone_at = clock_ + 1.0;
        g.drop_at = g.gone_at + 10.0;
        NpcGrenadeNear(g.pos, true, g.instigator);
    }
    const DamageStep* table = g.spec == kM433Spec ? kM433Table : kFragTable;
    const size_t n = g.spec == kM433Spec ? sizeof(kM433Table) / sizeof(kM433Table[0])
                                         : sizeof(kFragTable) / sizeof(kFragTable[0]);
    const Vec3 origin{g.pos.x, g.pos.y, g.pos.z + 10.0f};
    for (auto& other : pawns_) {
        ServerPawn& v = *other;
        if (v.torn_off || (!v.controller && !v.is_npc)) continue;   // players and enemy NPCs
        const Vec3 to{v.location[0] - g.pos.x, v.location[1] - g.pos.y, v.location[2] - g.pos.z};
        const float dist = Length(to);
        if (dist >= table[n - 1].distance) continue;
        const Vec3 target{v.location[0], v.location[1], v.location[2]};
        TraceHit h = collision_.LineTrace(origin, target, CollisionWorld::kBlockTrace);
        if (h.hit) continue;                                         // behind cover
        size_t i = 1;
        while (i < n && dist >= table[i].distance) ++i;
        const DamageStep& a = table[i - 1];
        const DamageStep& b = table[i];
        const float pct = a.percent + (b.percent - a.percent) * (dist - a.distance) / (b.distance - a.distance);
        const Vec3 dir = dist > 1.0f ? Vec3{to.x / dist, to.y / dist, to.z / dist} : Vec3{0, 0, 1};
        if (g.instigator && !g.instigator->is_npc)
            StatHit(*g.instigator, v, -1, g.spec == kM433Spec ? kStatM320 : kStatM67);
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
        if (g.instigator && !g.instigator->is_npc) StatHit(*g.instigator, v, -1, kStatM14);
        ApplyDamage(v, g.instigator, kBurnDamage, kFireDamage, Vec3{v.location[0], v.location[1], v.location[2]}, dir);
    }
}

// ---------------------------------------------------------------- XM320
//
// No retail grenadier capture exists; this follows the scripts
// (AA_WeaponModWeapon[_XM320], AA_Weapon/AA_BulletWeapon fire modes and
// ModReloading), the soldier state graph (Sold_Tech.upk,
// Sold_Tech_Base_BaseMovement_NR_StateGraph, weapon graph 4):
//   LargeWeaponIdle 79 -> Loadm320CharAnimSet 149 -> LgToXM320 131
//     (StdPstlGrip_Grab, 2.33 s) -> XM320Idle 130
//   XM320Idle 130 -> XM320ToLg 132 (StdPstlGrip_Release, 2.10 s) -> 79
//   XM320Idle 130 -> XM320ReloadEmpty 140 (StdTacRld_01, 2.87 s) -> 130
//                 -> XM320ReloadFull 160 (StdTacRld_Change, 3.33 s) -> 130
// (animation lengths from Mod_Grenade.upk), and the class defaults in
// AA3Baseline.u: the M433 leaves at Speed 2640, arms after 0.4 s and
// bursts by its own DamageLookupTable; the M714 arms at once and smokes.
namespace {
constexpr uint32_t kLauncherDesiredAmmo = 29, kLauncherPreloadedMag = 30,   // AA_WeaponModWeapon
                   kLauncherRange = 28;                                      // WeaponZeroingIdx
constexpr uint32_t kRifleLoadedMagazine = 34, kRifleChamberedRound = 57;     // AA_Weapon / AA_BulletWeapon
constexpr uint32_t kMagRoundIndex = 29;                                      // AA_AmmoContainerBase
constexpr uint32_t kM433Container = 998, kM714Container = 1007;              // AA_AmmoContainer_M433 / _M714
constexpr uint32_t kM433AmmoType = 1046;                                     // AA_AmmoType_M433 (both rounds)
constexpr float kLauncherSpeed = 2640.0f;      // AA_Projectile_M433_HEDP / _M714 Speed
constexpr double kM433ArmingTime = 0.4, kM714ArmingTime = 0.01;
constexpr double kGripGrab = 2.333, kGripRelease = 2.1;          // StdPstlGrip_Grab / _Release
constexpr double kReloadEmpty = 2.867, kReloadChange = 3.333;    // StdTacRld_01 / StdTacRld_Change
constexpr float kUnitsPerMeter = 52.4934f;     // the M67's 10 m DamageRadius is 524.93 units
constexpr uint8_t kGraphLgIdle = 79, kGraphLoadXM320 = 149, kGraphXM320Idle = 130, kGraphXM320ToLg = 132,
                  kGraphXM320ReloadEmpty = 140, kGraphXM320ReloadFull = 160;

DataValue AmmoTypeValue(bool loaded) {
    if (!loaded) return DataValue::Actor(nullptr);
    DataValue v; v.kind = DataValue::Kind::kObj; v.s = "AA3Baseline"; v.net = kM433AmmoType;
    return v;
}
}  // namespace

// AA_WeaponModWeapon.RegisterModFireMode on the server: the launcher's
// PreloadedMag (an AA_AmmoContainer_M433 spawned by the rifle, attached to
// the launcher) becomes the rifle's LoadedMagazine[1], and FinishedReloading
// chambers its round (ChamberedRound[1], RoundIndex 1; the class goes to the
// owner again by XM320.ClientSetChamberedRound). DesiredAmmoIndex = 0 (M433).
void World::SetUpLauncher(ServerPlayerController& pc, std::vector<std::unique_ptr<ServerRoundActor>>& items,
                          ServerRoundActor& weapon, ServerRoundActor& launcher) {
    auto mag_it = std::find_if(items.begin(), items.end(), [](const std::unique_ptr<ServerRoundActor>& a) {
        return a->class_name.rfind("AA_AmmoContainer", 0) == 0;
    });
    if (mag_it == items.end() || !pc.pawn) return;
    items.push_back(std::make_unique<ServerRoundActor>(**mag_it));
    ServerRoundActor& round = *items.back();
    round.class_name = "AA_AmmoContainer_M433";
    round.cdo_net_index = kM433Container + 1;
    round.object_index = ObjectIndex("AA3Baseline", round.cdo_net_index);
    round.max_index = 30;
    round.label = "M433:" + pc.pri->player_name;
    // Spawned, not given: no inventory chain, no ClientGivenTo.
    round.fields.erase(std::remove_if(round.fields.begin(), round.fields.end(),
                                      [](const DataField& f) {
                                          return f.idx == kHiddenByInvManager || f.idx == 24 || f.idx == kInventory ||
                                                 f.idx == kClientGivenTo;
                                      }),
                       round.fields.end());
    round.Set(kOwner, -1, DataValue::Actor(&weapon));
    round.Set(kAttachedActor, -1, DataValue::Actor(&launcher));
    round.Set(kMagRoundIndex, -1, DataValue::Int(1));
    launcher.Set(kLauncherDesiredAmmo, -1, DataValue::Int(0));
    launcher.Set(kLauncherPreloadedMag, -1, DataValue::Actor(&round));
    weapon.Set(kRifleLoadedMagazine, 1, DataValue::Actor(&round));
    weapon.Set(kRifleChamberedRound, 1, AmmoTypeValue(true));
    ServerPawn& pawn = *pc.pawn;
    pawn.launcher = &launcher;
    pawn.launcher_mag = &round;
    pawn.fire_mode = 0;
    pawn.fire_mode_target = -1;
    pawn.launcher_loaded = true;
    pawn.launcher_ammo = pawn.launcher_loaded_ammo = 0;
    pawn.launcher_range = 0;
    pawn.launcher_reload_at = -1.0;
    ++pawn.launcher_chamber_serial;
}

bool World::SetWeaponFireMode(ServerPawn* pawn, uint8_t mode) {
    if (!pawn || !pawn->launcher || pawn->incapacitated || pawn->torn_off || mode > 1) return false;
    if (pawn->launcher_reload_at >= 0) return false;                 // IsBusyReloading
    for (const auto& j : deliberate_reloads_) if (j.pawn == pawn) return false;
    for (const auto& j : reloads_) if (j.pawn == pawn) return false;
    if (pawn->held_grenade) return false;
    const uint8_t from = pawn->fire_mode_target >= 0 ? static_cast<uint8_t>(pawn->fire_mode_target) : pawn->fire_mode;
    if (mode == from) return false;
    pawn->fire_mode_target = mode;
    if (mode == 1) {
        // RunStateSequence(XM320Idle); the server's grip notify ends it
        // (AA_Weapon.Notify_TransitionToForwardGripComplete is authority-only
        // in multiplayer: the client waits for ClientTransitionToForwardGripComplete).
        pawn->SetGraph(4, kGraphLoadXM320, kGraphXM320Idle);
        pawn->fire_mode_done_at = clock_ + kGripGrab;
    } else {
        // RunStateSequence(IdleStateName): the client sets its own fire mode
        // back at StateNotify_TransitionFromForwardGrip_Exited.
        pawn->SetGraph(4, kGraphXM320ToLg, kGraphLgIdle);
        pawn->fire_mode = 0;
        pawn->fire_mode_done_at = clock_ + kGripRelease;
    }
    LOG_INFO("%s: %s", SoldierName(pawn).c_str(), mode == 1 ? "raising the XM320" : "back to the rifle");
    return true;
}

void World::FireLauncher(ServerPawn& pawn) {
    if (!pawn.launcher || pawn.fire_mode != 1 || pawn.fire_mode_target >= 0 || !pawn.launcher_loaded) return;
    if (pawn.launcher_reload_at >= 0 || pawn.incapacitated || pawn.torn_off) return;
    const ServerPlayerController* pc = pawn.controller;
    ServerRoundActor* weapon = FindInventory(&pawn, pawn.rifle ? pawn.rifle : pawn.loaded_weapon);
    if (!weapon) weapon = FindInventory(&pawn, pawn.loaded_weapon);
    const bool m433 = pawn.launcher_loaded_ammo == 0;
    const GrenadeSpec& spec = kGrenades[m433 ? kM433Spec : kM714Spec];

    // The muzzle (XM320 MuzzleSocket) points along the view, raised by the
    // zeroing node so the leaf sight's range (50 m per step, 50-300 m)
    // lands on flat ground: sin(2a) = g R / v^2, 45 degrees past reach.
    const float g = std::fabs(world_info_ ? world_info_->world_gravity_z : -520.0f);
    const float range = (50.0f + 50.0f * static_cast<float>(pawn.launcher_range)) * kUnitsPerMeter;
    const float reach = g * range / (kLauncherSpeed * kLauncherSpeed);
    const double lift = reach >= 1.0f ? 3.14159265358979 / 4.0 : 0.5 * std::asin(reach);
    const double k = 3.14159265358979 * 2.0 / 65536.0;
    const int32_t view_pitch = pc ? static_cast<int16_t>(pc->rotation_pitch & 0xFFFF) : 0;
    const int32_t view_yaw = pc ? (pc->rotation_yaw & 0xFFFF) : (pawn.rotation_yaw & 0xFFFF);
    const double pitch = std::min(view_pitch * k + lift, 3.14159265358979 / 2.0 - 0.01);
    const double yaw = view_yaw * k;
    const Vec3 dir{static_cast<float>(std::cos(pitch) * std::cos(yaw)),
                   static_cast<float>(std::cos(pitch) * std::sin(yaw)),
                   static_cast<float>(std::sin(pitch))};
    // From under the barrel: the camera's height less a few units, 30
    // forward, pulled back if that's in a wall.
    const float up = (pawn.prone ? 3.0f : pawn.crouched ? 20.0f : 33.0f) - pawn.body_drop - 6.0f;
    const Vec3 eye{pawn.location[0], pawn.location[1], pawn.location[2] + up};
    const float fy = static_cast<float>(yaw);
    const Vec3 fwd{eye.x + std::cos(fy) * 30.0f, eye.y + std::sin(fy) * 30.0f, eye.z};
    TraceHit wall = collision_.LineTrace(eye, fwd, CollisionWorld::kBlockTrace);
    const Vec3 start = wall.hit ? Vec3{wall.location.x - std::cos(fy) * 4.0f, wall.location.y - std::sin(fy) * 4.0f,
                                       wall.location.z}
                                : fwd;
    // The client flies its own copy from the replicated (whole-unit) Velocity.
    const Vec3 vel{std::round(dir.x * kLauncherSpeed), std::round(dir.y * kLauncherSpeed),
                   std::round(dir.z * kLauncherSpeed)};

    auto proj = std::make_unique<ServerRoundActor>();
    proj->class_name = spec.projectile;
    proj->cdo_package = "AA3Baseline";
    proj->cdo_net_index = spec.projectile_cls + 1;
    proj->object_index = ObjectIndex("AA3Baseline", proj->cdo_net_index);
    proj->max_index = 24;                                   // AA_Projectile (bDeadRound 23)
    proj->transient = true;
    proj->label = std::string(spec.projectile) + ":" + (pawn.pri ? pawn.pri->player_name : "?");
    proj->location[0] = start.x; proj->location[1] = start.y; proj->location[2] = start.z;
    DataValue rot; rot.kind = DataValue::Kind::kRotator;
    rot.v3[0] = static_cast<int32_t>(std::lround(pitch / k)) & 0xFFFF;
    rot.v3[1] = view_yaw; rot.v3[2] = 0;
    proj->Set(kProjRotation, -1, rot);
    proj->Set(kProjVelocity, -1, VectorValue(vel.x, vel.y, vel.z));
    proj->Set(kInstigator, -1, DataValue::Actor(&pawn));
    ThrownGrenade t;
    t.actor = std::move(proj);
    t.instigator = &pawn;
    t.spec = m433 ? kM433Spec : kM714Spec;
    t.thrown_at = clock_;
    t.explode_at = 1.0e18;
    t.impact = true;
    t.arm_at = clock_ + (m433 ? kM433ArmingTime : kM714ArmingTime);
    t.gone_at = clock_ + (m433 ? 30.0 : spec.life);        // a miss into the sky still ends
    t.drop_at = t.gone_at + 10.0;
    t.pos = t.launch = start;
    t.vel = vel;
    thrown_.push_back(std::move(t));

    if (PlayerRoundStats* rs = LiveStats(&pawn)) {
        if (m433) ++rs->iM320ShotsFired;
        else ++rs->iM714Fired;
    }
    // Spent: the chamber empties (the client's ConsumeAmmo does the same);
    // the case stays in the launcher until the reload.
    pawn.launcher_loaded = false;
    if (weapon) weapon->Set(kRifleChamberedRound, 1, AmmoTypeValue(false));
    ++pawn.launcher_chamber_serial;
    LOG_INFO("%s fired an %s: view pitch %d, range setting %d (%.0f m) lifts it %.1f deg; leaves (%.0f, %.0f, %.0f) at "
             "(%.0f, %.0f, %.0f)",
             SoldierName(&pawn).c_str(), m433 ? "M433" : "M714", view_pitch, pawn.launcher_range,
             range / kUnitsPerMeter, lift * 180.0 / 3.14159265358979, start.x, start.y, start.z, vel.x, vel.y, vel.z);
}

// AA_Weapon.Reload(RT_WeaponMod): XM320IdleReset then ModReloadEmpty (or
// ModReloadFull with a round still in -- it goes back to the pouch first).
// GetNextAmmoMag reuses the launcher's own container: a round out of the
// PRI count, RoundIndex 0; FinishedReloading pops it into the chamber.
bool World::BeginLauncherReload(ServerPawn* pawn) {
    if (!pawn || !pawn->launcher || !pawn->pri || pawn->fire_mode != 1 || pawn->fire_mode_target >= 0) return false;
    if (pawn->launcher_reload_at >= 0 || pawn->incapacitated || pawn->torn_off) return false;
    const bool want_m433 = pawn->launcher_ammo == 0;
    const int32_t left = want_m433 ? pawn->pri->m433_rounds : pawn->pri->m714_rounds;
    if (left <= 0) return false;
    if (pawn->launcher_loaded && pawn->launcher_loaded_ammo == pawn->launcher_ammo) return false;  // nothing to do
    const bool full = pawn->launcher_loaded;
    pawn->SetGraph(4, full ? kGraphXM320ReloadFull : kGraphXM320ReloadEmpty, kGraphXM320Idle);
    pawn->launcher_reload_at = clock_;
    pawn->launcher_reload_stage = full ? 0 : 1;
    pawn->launcher_reload_full = full;
    ++pawn->confirm_reload_serial;
    LOG_INFO("%s reloads the XM320 with an %s (%s)", SoldierName(pawn).c_str(), want_m433 ? "M433" : "M714",
             full ? "swapping the loaded round" : "empty");
    return true;
}

void World::LauncherLoadAmmo(ServerPawn* pawn, const std::string& type) {
    if (!pawn || !pawn->launcher || !pawn->pri || pawn->fire_mode != 1) return;
    std::string t;
    for (char c : type) t.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    const int idx = t.find("714") != std::string::npos ? 1 : t.find("433") != std::string::npos ? 0 : -1;
    if (idx < 0 || idx == pawn->launcher_ammo) return;
    if ((idx == 0 ? pawn->pri->m433_rounds : pawn->pri->m714_rounds) <= 0) return;   // AdvancedHasAmmoMag
    pawn->launcher_ammo = idx;
    if (ServerRoundActor* l = FindInventory(pawn, pawn->launcher)) l->Set(kLauncherDesiredAmmo, -1, DataValue::Int(idx));
    BeginLauncherReload(pawn);
}

void World::CycleLauncherRange(ServerPawn* pawn, bool up, bool reset) {
    if (!pawn || !pawn->launcher) return;
    if (!reset && pawn->fire_mode != 1) return;
    // AA_WeaponModWeapon.CycleWeaponZeroingRange: up is the nearer socket.
    const int idx = reset ? 0 : std::clamp(pawn->launcher_range + (up ? -1 : 1), 0, 5);
    if (idx == pawn->launcher_range) return;
    pawn->launcher_range = idx;
    if (ServerRoundActor* l = FindInventory(pawn, pawn->launcher)) l->Set(kLauncherRange, -1, DataValue::Int(idx));
    LOG_INFO("%s sets the XM320 sight to %d m", SoldierName(pawn).c_str(), 50 + 50 * idx);
}

void World::UpdateLauncher() {
    for (auto& p : pawns_) {
        ServerPawn& pawn = *p;
        if (!pawn.is_npc) UpdateRifleSwitch(pawn);
        if (!pawn.launcher) continue;
        if (pawn.torn_off || pawn.incapacitated) {
            pawn.fire_mode_target = -1;
            pawn.launcher_reload_at = -1.0;
            continue;
        }
        if (pawn.fire_mode_target >= 0 && clock_ >= pawn.fire_mode_done_at) {
            if (pawn.fire_mode_target == 1) {
                pawn.fire_mode = 1;
                ++pawn.grip_complete_serial;              // ClientTransitionToForwardGripComplete
                pawn.SetGraph(4, kGraphXM320Idle, kGraphXM320Idle);
            } else {
                pawn.SetGraph(4, kGraphLgIdle, kGraphLgIdle);
            }
            pawn.fire_mode_target = -1;
        }
        if (pawn.launcher_reload_at < 0 || !pawn.pri) continue;
        const double t = clock_ - pawn.launcher_reload_at;
        ServerRoundActor* mag = FindInventory(&pawn, pawn.launcher_mag);
        ServerRoundActor* weapon = FindInventory(&pawn, pawn.rifle ? pawn.rifle : pawn.loaded_weapon);
        if (!weapon) weapon = FindInventory(&pawn, pawn.loaded_weapon);
        // Stage 0 (a round still in): it goes back to the PRI count
        // (AdvancedReplaceAmmoMag: RoundIndex 0 -> iM433Rounds++).
        if (pawn.launcher_reload_stage == 0 && t >= 0.8) {
            if (pawn.launcher_loaded_ammo == 0) ++pawn.pri->m433_rounds;
            else ++pawn.pri->m714_rounds;
            pawn.launcher_loaded = false;
            if (weapon) weapon->Set(kRifleChamberedRound, 1, AmmoTypeValue(false));
            ++pawn.launcher_counts_serial;
            pawn.launcher_reload_stage = 1;
        }
        // Stage 1: the new round out of the pouch (GetNextAmmoMag): one off
        // the count, the container RoundIndex 0 (and its class if the type
        // changed).
        const double grab_at = pawn.launcher_reload_full ? 1.6 : 1.0;
        if (pawn.launcher_reload_stage == 1 && t >= grab_at) {
            const bool m433 = pawn.launcher_ammo == 0;
            int32_t& count = m433 ? pawn.pri->m433_rounds : pawn.pri->m714_rounds;
            if (count <= 0) { pawn.launcher_reload_at = -1.0; pawn.SetGraph(4, kGraphXM320Idle, kGraphXM320Idle); continue; }
            --count;
            if (mag) {
                const uint32_t cls = m433 ? kM433Container : kM714Container;
                if (mag->cdo_net_index != cls + 1) {
                    mag->class_name = m433 ? "AA_AmmoContainer_M433" : "AA_AmmoContainer_M714";
                    mag->cdo_net_index = cls + 1;
                    mag->object_index = ObjectIndex("AA3Baseline", mag->cdo_net_index);
                }
                mag->Set(kMagRoundIndex, -1, DataValue::Int(0));
                mag->Touch(kMagRoundIndex);
            }
            pawn.launcher_loaded_ammo = pawn.launcher_ammo;
            ++pawn.launcher_counts_serial;
            pawn.launcher_reload_stage = 2;
        }
        // StateNotify_ModReload_Exited -> FinishedReloading: the round is
        // chambered (RoundIndex 1, ChamberedRound[1], ClientSetChamberedRound).
        const double length = pawn.launcher_reload_full ? kReloadChange : kReloadEmpty;
        if (pawn.launcher_reload_stage == 2 && t >= length) {
            if (mag) { mag->Set(kMagRoundIndex, -1, DataValue::Int(1)); mag->Touch(kMagRoundIndex); }
            if (weapon) { weapon->Set(kRifleChamberedRound, 1, AmmoTypeValue(true)); weapon->Touch(kRifleChamberedRound, 1); }
            pawn.launcher_loaded = true;
            ++pawn.launcher_chamber_serial;
            pawn.SetGraph(4, kGraphXM320Idle, kGraphXM320Idle);
            pawn.launcher_reload_at = -1.0;
            LOG_INFO("%s: XM320 loaded (%d M433, %d M714 left)", SoldierName(&pawn).c_str(), pawn.pri->m433_rounds,
                     pawn.pri->m714_rounds);
        }
    }
}

// ---------------------------------------------------------------- encumbrance
//
// AA_Pawn.DoEncumbrance is native; fitted to the retail Bridge AI capture
// (user session 2026-10-12: AA_InventoryManager.CurrentWeight against
// AA_Pawn.MovementSpeedModifier at full health, 9 samples from 11.34 to
// 15.84 kg, all within 0.0005): 1.1023 - 0.018797 per kg, never under 0.7
// (24.46 kg, the M249 life). CurrentWeight is the class Weight of what's
// carried:
//   - the rifle and its mods;
//   - every magazine with rounds (30-round 0.45, M249 box 3.14);
//   - the grenades by type (M67 0.397, M83 / M106 0.45, M84 0.374,
//     AN-M14 0.9) and 40 mm rounds (0.23);
//   - the preloaded grenade set, +3.021 from 4.66 s after spawn;
//   - until the initial reload, the loaded magazine twice.
// Retail heavy DMR: 12.744 at spawn, 12.294 after the reload, 15.315
// steady. Injury lowers it further (our InjuredSpeedModifier, scaled to
// full health).
void World::UpdateEncumbrance(ServerPawn& p) {
    if (p.is_npc || p.torn_off || p.incapacitated || !p.pri) return;
    const ServerPlayerReplicationInfo& pri = *p.pri;
    float w = p.kit_weight;
    for (const auto& m : pri.primary_mags)
        if (m.rounds > 0 || m.in_weapon) w += p.mag_weight;
    if (!p.loaded_mag) w += p.mag_weight;                      // before the initial reload
    w += 0.397f * static_cast<float>(pri.frag_grenades) + 0.45f * static_cast<float>(pri.smoke_grenades) +
         0.45f * static_cast<float>(pri.m106_grenades) + 0.374f * static_cast<float>(pri.stun_grenades) +
         0.9f * static_cast<float>(pri.incendiary_grenades);
    w += 0.23f * static_cast<float>(pri.m433_rounds + pri.m714_rounds + (p.launcher_loaded ? 1 : 0));
    if (clock_ - p.spawned_at >= 4.66) w += 3.021f;
    w = std::round(w * 1000.0f) / 1000.0f;
    const float enc = std::clamp(1.1023f - 0.018797f * w, 0.7f, 1.0f);
    p.movement_speed_modifier = std::round(enc * InjuredSpeedModifier(p.health) / 0.8852f * 10000.0f) / 10000.0f;
    if (w != p.carried_weight) {
        p.carried_weight = w;
        LOG_INFO("%s carries %.3f kg: speed x%.4f", SoldierName(&p).c_str(), w, p.movement_speed_modifier);
        if (ServerRoundActor* im = FindInventory(&p, p.inv_manager)) {
            DataValue f; f.kind = DataValue::Kind::kFloat; f.f = w;
            im->Set(29, -1, f);                                   // AA_InventoryManager.CurrentWeight
        }
    }
}

// ---------------------------------------------------------------- picked-up rifles
//
// Retail Bridge AI user session, 13:30:12 (doc sec 46): ServerRepExec
// ('Pickup', the AA_DroppedPickup) on a dropped rifle while carrying the
// M249. The pickup's channel closes; the rifle (Owner / Instigator the
// pawn, InvManager, LoadedMagazine[0], ChamberedRound[0], ClientGivenTo
// (pawn, bDoNotActivate False)) goes on the right shoulder slot (its
// InventoryChain; the rifle's AttachedActor and LastInventoryContainer),
// with its magazine (RoundIndex as dropped, AttachedActor the rifle, hidden
// by the inventory manager) and the PC's HUD notices: ammo type, weapon
// type, ReceiveLocalizedMessage(LocalMessage, OptionalObject = the class).
// The rifle stays slung until the player picks it from the weapon menu.
namespace {
struct PickupRifle { const char* pickup; const char* weapon; uint32_t cls, max_index; uint8_t inv_type;
                     const char* mag; uint32_t mag_cls; uint8_t ammo_type; float weight, mag_weight; int32_t rounds; };
const PickupRifle kPickupRifles[] = {
    {"AA_DroppedPickup_ObranKNP", "AA_BulletWeapon_ObranKNP", 5259, 71, 11,
     "AA_AmmoContainer_NME_30x556mmBall", 1021, 17, 4.25f, 0.45f, 30},
    {"AA_DroppedPickup_ObranMSP", "AA_BulletWeapon_ObranMSP", 5297, 71, 9,
     "AA_AmmoContainer_NME_30x556mmBall", 1021, 17, 4.5f, 0.45f, 30},
    {"AA_DroppedPickup_ObranLMK", "AA_BulletWeapon_ObranLMK", 5293, 72, 10,
     "AA_AmmoContainer_200x556mmBall_Drum", 946, 18, 4.75f, 3.14f, 200},
};
constexpr uint32_t kSlotChain = 29;          // AA_InventoryContainerBase.InventoryChain
constexpr uint32_t kRifleRepIdx56 = 56, kRifleROF = 58, kRifleLastContainer = 33, kRifleAmmoType = 35;
}  // namespace

bool World::PickUpWeapon(ServerPlayerController& pc, const ServerActor* obj) {
    ServerPawn* pawn = pc.pawn;
    if (!pawn || pawn->torn_off || pawn->incapacitated || !obj) return false;
    ServerRoundActor* item = nullptr;
    for (auto& w : world_items_) if (w.get() == obj && !w->gone) item = w.get();
    if (!item) return false;
    const PickupRifle* r = nullptr;
    for (const auto& k : kPickupRifles) if (item->class_name == k.pickup) r = &k;
    if (!r) return false;
    if (std::hypot(item->location[0] - pawn->location[0], item->location[1] - pawn->location[1]) > 250.0f) return false;
    auto& inv = pawn_inventory_[pawn];
    ServerRoundActor* own_rifle = nullptr;
    ServerRoundActor* own_mag = nullptr;
    ServerRoundActor* shoulder = nullptr;
    int carried = 0;
    for (auto& a : inv) {
        if (a->gone) continue;
        if (a->class_name.rfind("AA_BulletWeapon", 0) == 0) { ++carried; if (!own_rifle) own_rifle = a.get(); }
        if (a->class_name.rfind("AA_AmmoContainer", 0) == 0 && !own_mag && a->class_name.find("M433") == std::string::npos &&
            a->class_name.find("M714") == std::string::npos) own_mag = a.get();
        if (a->class_name == "AA_WeaponSlot_RightShoulder") shoulder = a.get();
    }
    if (!own_rifle || !own_mag || !shoulder) return false;
    if (carried >= 2) {
        LOG_INFO("%s already carries two rifles", SoldierName(pawn).c_str());
        return false;
    }
    // The magazine as dropped (LoadedMagazines[0] {class, RoundCount, RoundIndex}).
    int32_t round_index = 1;
    if (const DataField* lm = item->Get(29, 0); lm && lm->v.kind == DataValue::Kind::kStruct && lm->v.members.size() >= 3)
        round_index = static_cast<int32_t>(lm->v.members[2].i);

    auto gun = std::make_unique<ServerRoundActor>(*own_rifle);
    gun->class_name = r->weapon;
    gun->cdo_net_index = r->cls + 1;
    gun->object_index = ObjectIndex("AA3Baseline", gun->cdo_net_index);
    gun->max_index = r->max_index;
    gun->label = std::string(r->weapon) + ":" + (pc.pri ? pc.pri->player_name : "?");
    gun->fields.erase(std::remove_if(gun->fields.begin(), gun->fields.end(), [](const DataField& f) {
        return f.idx == kSightType || f.idx == kModInventory || f.idx == kRifleRepIdx56 || f.idx == kRifleROF ||
               f.idx == 34 || f.idx == 57 || f.idx == 62 || f.idx == kInventory;
        // ClientWeaponSet (30) stays: retail's pickup carries it, and it's
        // what puts the rifle in the client's inventory properly.
    }), gun->fields.end());
    auto mag = std::make_unique<ServerRoundActor>(*own_mag);
    mag->class_name = r->mag;
    mag->cdo_net_index = r->mag_cls + 1;
    mag->object_index = ObjectIndex("AA3Baseline", mag->cdo_net_index);
    mag->max_index = 30;
    mag->label = std::string(r->mag) + ":" + (pc.pri ? pc.pri->player_name : "?");
    mag->fields.erase(std::remove_if(mag->fields.begin(), mag->fields.end(), [](const DataField& f) {
        return f.idx == kInventory || f.idx == kHidden;
    }), mag->fields.end());
    DataValue ammo; ammo.kind = DataValue::Kind::kObj; ammo.s = "AA3Baseline"; ammo.net = 1044;   // AA_AmmoType_556mmBall
    gun->Set(kHidden, -1, DataValue::Bool(true));
    gun->Set(kAttachedActor, -1, DataValue::Actor(shoulder));
    gun->Set(kRifleLastContainer, -1, DataValue::Actor(shoulder));
    gun->Set(34, 0, DataValue::Actor(mag.get()));             // LoadedMagazine[0]
    gun->Set(kRifleAmmoType, -1, ammo);                        // CurrentAmmoType
    gun->Set(57, 0, ammo);                                     // ChamberedRound[0]
    DataValue given; given.kind = DataValue::Kind::kStruct;
    given.members = {DataValue::Bool(true), DataValue::Actor(pawn), DataValue::Bool(false)};   // bDoNotActivate False
    gun->Set(kClientGivenTo, -1, given);
    mag->Set(kHiddenByInvManager, -1, DataValue::Bool(true));
    mag->Set(kAttachedActor, -1, DataValue::Actor(gun.get()));
    mag->Set(29, -1, DataValue::Int(round_index));             // RoundIndex
    DataValue mgiven; mgiven.kind = DataValue::Kind::kStruct;
    mgiven.members = {DataValue::Bool(true), DataValue::Actor(pawn), DataValue::Bool(true)};
    mag->Set(kClientGivenTo, -1, mgiven);
    shoulder->Set(kSlotChain, -1, DataValue::Actor(gun.get()));
    pawn->kit_weight += r->weight + r->mag_weight;
    pawn->pickup_ammo_type = r->ammo_type;
    pawn->pickup_weapon_type = r->inv_type;
    pawn->pickup_class = r->cls;
    ++pawn->pickup_serial;
    item->gone = true;
    LOG_INFO("%s picked up an %s (%d of %d rounds)", SoldierName(pawn).c_str(), r->weapon,
             r->rounds - round_index + 1, r->rounds);
    inv.push_back(std::move(mag));
    inv.push_back(std::move(gun));
    return true;
}

// Retail 13:30:18 (doc sec 46), ServerSetCurrentWeapon(DMR) with the M249
// in hand: graph 4 {151} at once; +0.2 s the M249 to its chest slot, the
// DMR shown, InventoryManager.ClientSetPreviousWeapon(M249) and
// PreviousWeapon; +0.24 s graph 4 {151 -> 79}, ActiveInventory = DMR;
// +0.57 s the DMR in hand (AttachedActor the pawn).
// The server's firing state follows the rifle in hand: its own magazine
// (shown now it's in use), rounds per magazine, rate of fire, and
// AA_Weapon.ClientLoadAmmoMag(0, magazine) to the owner (via
// loaded_mag_serial) -- without it the client never fired a picked-up
// rifle (2026-10-14 live: 1 ServerStartFire in 15 s of trying).
void World::RifleInHand(ServerPawn& p, ServerRoundActor& to) {
    p.loaded_weapon = &to;
    p.rifle = &to;
    if (const DataField* lm = to.Get(34, 0); lm && lm->v.kind == DataValue::Kind::kActor) {
        p.loaded_mag = lm->v.actor;
        if (ServerRoundActor* m = FindInventory(&p, p.loaded_mag)) {
            m->Set(kHidden, -1, DataValue::Bool(false));
            m->Set(kHiddenByInvManager, -1, DataValue::Bool(false));
        }
    }
    ++p.loaded_mag_serial;
    const bool box = to.class_name.find("LMK") != std::string::npos || to.class_name.find("M249") != std::string::npos;
    p.mag_rounds = box ? 200 : 30;
    p.mag_weight = box ? 3.14f : 0.45f;
    DataValue rof; rof.kind = DataValue::Kind::kByte; rof.bits = 2; rof.i = p.selected_rof;
    to.Set(kRifleROF, -1, rof);
}

bool World::SwitchRifle(ServerPawn* pawn, const ServerActor* to) {
    if (!pawn || !to || pawn->torn_off || pawn->incapacitated) {
        LOG_INFO("rifle switch: no pawn/weapon (%p %p)", static_cast<const void*>(pawn), static_cast<const void*>(to));
        return false;
    }
    ServerRoundActor* target = FindInventory(pawn, to);
    if (!target) LOG_INFO("%s: rifle switch target not in the inventory", SoldierName(pawn).c_str());
    if (target && target->class_name.rfind("AA_BulletWeapon", 0) == 0 && !pawn->first_reload_done)
        LOG_INFO("%s: no rifle switch before the initial reload is done", SoldierName(pawn).c_str());
    if (!pawn->first_reload_done) return false;
    if (!target || target->class_name.rfind("AA_BulletWeapon", 0) != 0) return false;
    const ServerActor* from = pawn->active_inventory;
    ServerRoundActor* cur = FindInventory(pawn, from);
    if (!cur || cur == target || cur->class_name.rfind("AA_BulletWeapon", 0) != 0) {
        LOG_INFO("%s: no rifle switch (in hand: %s)", SoldierName(pawn).c_str(), cur ? cur->class_name.c_str() : "none");
        return false;
    }
    if (pawn->fire_mode != 0 || pawn->switch_at >= 0.0) {
        LOG_INFO("%s: rifle switch busy (fire mode %d, switch at %.1f)", SoldierName(pawn).c_str(), pawn->fire_mode,
                 pawn->switch_at);
        return false;
    }
    pawn->trigger_held = false;
    pawn->SetGraph(4, 151, 151);
    pawn->switch_from = from;
    pawn->switch_to = to;
    pawn->switch_at = clock_;
    pawn->switch_stage = 0;
    LOG_INFO("%s switches to the %s", SoldierName(pawn).c_str(), target->class_name.c_str());
    return true;
}

void World::UpdateRifleSwitch(ServerPawn& p) {
    if (p.switch_at < 0.0) return;
    ServerRoundActor* from = FindInventory(&p, p.switch_from);
    ServerRoundActor* to = FindInventory(&p, p.switch_to);
    if (!from || !to || p.torn_off) { p.switch_at = -1.0; return; }
    const double t = clock_ - p.switch_at;
    if (p.switch_stage == 0 && t >= 0.2) {
        // The old rifle back to its own slot (the kit's primary: the chest).
        const DataField* slot = from->Get(kRifleLastContainer);
        const ServerActor* chest = nullptr;
        for (auto& a : pawn_inventory_[&p]) if (a->class_name == "AA_WeaponSlot_Chest") chest = a.get();
        const bool primary = from->class_name.find("Obran") == std::string::npos;
        if (primary && chest) from->Set(kAttachedActor, -1, DataValue::Actor(chest));
        else if (slot) from->Set(kAttachedActor, -1, slot->v);
        to->Set(kHidden, -1, DataValue::Bool(false));
        p.previous_weapon = from;
        ++p.previous_weapon_serial;
        if (ServerRoundActor* im = FindInventory(&p, p.inv_manager)) im->Set(30, -1, DataValue::Actor(from));
        p.switch_stage = 1;
    }
    if (p.switch_stage == 1 && t >= 0.24) {
        p.SetGraph(4, 151, 79);
        p.active_inventory = to;
        RifleInHand(p, *to);
        p.switch_stage = 2;
    }
    if (p.switch_stage == 2 && t >= 0.57) {
        to->Set(kAttachedActor, -1, DataValue::Actor(&p));
        p.SetGraph(4, 79, 79);
        p.switch_at = -1.0;
    }
}

// ---------------------------------------------------------------- round statistics
//
// AA_MP_PlayerRoundStatistics as retail fills it (Bridge AI user session,
// round 2 published at t=1292.02):
//   - shots fired / hit / headshots / kills per weapon (kills = enemies
//     neutralized with it: DMR 14 + M249 11 + M4A1 4 = iNeutralizedEnemy 29);
//   - hits by body part (sum 52 = the hits);
//   - iSecuredEnemy / iConfirmedEnemy, iIncapacitatedByEnemy,
//     iWoundsTreatedByTeammate, ES2 objects used / reported / in the round.
// Retail publishes it with the match block at round end and has the client
// commit both with ClientTransmitLastRoundStats at the next deployment.
PlayerRoundStats* World::LiveStats(const ServerPawn* p) {
    if (!p || p->is_npc) return nullptr;
    for (auto& c : controllers_) if (c->pawn == p) return &c->live_round;
    return p->controller ? &p->controller->live_round : nullptr;
}

uint8_t World::RifleStat(const ServerPawn& p) {
    // The rifle in hand (a picked-up Obran counts as its own weapon).
    const ServerRoundActor* g = FindInventory(&p, p.loaded_weapon);
    if (!g) return kStatNone;
    static const std::pair<const char*, uint8_t> kMap[] = {
        {"AA_BulletWeapon_M4A1", kStatM4A1}, {"AA_BulletWeapon_M16A4", kStatM16A4},
        {"AA_BulletWeapon_M16DMR", kStatDMR}, {"AA_BulletWeapon_M249", kStatM249},
        {"AA_BulletWeapon_ObranKNP", kStatKNP}, {"AA_BulletWeapon_ObranMSP", kStatMSP},
        {"AA_BulletWeapon_ObranLMK", kStatLMK}};
    for (const auto& m : kMap) if (g->class_name == m.first) return m.second;
    return kStatNone;
}

namespace {
// Stat fields by name (the generated table gives each one's offset and
// width: kills are bytes, shots ints).
void Bump(PlayerRoundStats& s, const char* name) {
    if (!name) return;
    const StatField* f = PlayerRoundStatsFields();
    for (size_t i = 0; i < PlayerRoundStats::kNumFields; ++i) {
        if (std::strcmp(f[i].name, name) != 0) continue;
        char* at = reinterpret_cast<char*>(&s) + f[i].offset;
        if (f[i].kind == StatKind::kInt) ++*reinterpret_cast<int32_t*>(at);
        else if (f[i].kind == StatKind::kByte) { uint8_t& b = *reinterpret_cast<uint8_t*>(at); if (b < 255) ++b; }
        return;
    }
}
struct WeaponStatFields { const char *fired, *hit, *head, *kills; };
const WeaponStatFields* StatFieldsOf(uint8_t w) {
    static const WeaponStatFields kF[] = {
        {nullptr, nullptr, nullptr, nullptr},
        {"iM4A1ShotsFired", "iM4A1ShotsHit", "iM4A1Headshots", "iM4A1Kills"},
        {"iM16A4ShotsFired", "iM16A4ShotsHit", "iM16A4Headshots", "iM16A4Kills"},
        {"iM16DMRShotsFired", "iM16DMRShotsHit", "iM16DMRHeadshots", "iM16DMRKills"},
        {"iM249ShotsFired", "iM249ShotsHit", "iM249Headshots", "iM249Kills"},
        {"iM320ShotsFired", "iM320ShotsHit", "iM320Headshots", "iM320Kills"},
        {"iM67Thrown", "iM67Hit", nullptr, "iM67Kills"},
        {"iM84Thrown", "iM84Hit", nullptr, "iM84Kills"},
        {"iM14Thrown", "iM14Hit", nullptr, "iM14Kills"},
        {"iObranKNPShotsFired", "iObranKNPShotsHit", "iObranKNPHeadshots", "iObranKNPKills"},
        {"iObranMSPShotsFired", "iObranMSPShotsHit", "iObranMSPHeadshots", "iObranMSPKills"},
        {"iObranLMKShotsFired", "iObranLMKShotsHit", "iObranLMKHeadshots", "iObranLMKKills"},
    };
    return w < sizeof(kF) / sizeof(kF[0]) && kF[w].fired ? &kF[w] : nullptr;
}
}  // namespace

void World::StatShot(ServerPawn& p) {
    PlayerRoundStats* s = LiveStats(&p);
    const WeaponStatFields* f = StatFieldsOf(RifleStat(p));
    if (s && f) Bump(*s, f->fired);
}

// zone: 0 head, 1 upper torso, 2 lower torso, 3 upper leg, 4 lower leg.
void World::StatHit(ServerPawn& shooter, ServerPawn& victim, int zone, uint8_t weapon) {
    victim.last_hit_weapon = weapon;
    PlayerRoundStats* s = LiveStats(&shooter);
    const WeaponStatFields* f = StatFieldsOf(weapon);
    if (!s || !victim.is_npc) return;
    if (f) {
        Bump(*s, f->hit);
        if (zone == 0) Bump(*s, f->head);
    }
    switch (zone) {
    case 0: ++s->iHead; break;
    case 1: ++s->iUpperTorso; break;
    case 2: ++s->iLowerTorso; break;
    case 3: ++s->iRightUpperLeg; break;
    case 4: ++s->iRightLowerLeg; break;
    default: break;
    }
}

// An enemy neutralized by this player: the round stat, the weapon's kill,
// and the scoreboard's PRI counts and Personal Courage (+10; retail
// t=121.35: iNeutralizedEnemy / iRoundEnemyNeutralizedCount at once, the
// XP 1 s on).
void World::StatNeutralized(ServerPlayerController& pc, ServerPawn& victim) {
    ++pc.live_round.iNeutralizedEnemy;
    if (const WeaponStatFields* f = StatFieldsOf(victim.last_hit_weapon)) Bump(pc.live_round, f->kills);
    if (ServerPlayerReplicationInfo* pri = pc.pri) {
        ++pri->neutralized;
        ++pri->round_neutralized;
        ++pri->confirmed_neutralized;
        pri->xp_courage += 10;
        pri->xp_round_honor += 10;
        pri->match_total_score += 10;
    }
}

// Round end: the round block (bDataValid, bDirty, stamped) and the match
// block (rounds won / lost, the best scores so far) go to each player's
// SteamStatManager.
void World::PublishRoundStats(int32_t winner) {
    const float stamp = static_cast<float>(std::time(nullptr));
    for (auto& c : controllers_) {
        ServerPlayerController& pc = *c;
        if (!pc.stat_manager || !pc.pri || !pc.pri->OnPlayableTeam()) continue;
        PlayerRoundStats r = pc.live_round;
        r.bDataValid = true;
        r.bDirty = true;
        r.fpStampLastUpdate = stamp;
        int es2 = 0;
        for (const auto& a : match_actors_) if (a->class_name == "AA_ES2InstaPerk") ++es2;
        r.iES2ObjectsInRound = es2;
        pc.stat_manager->round = r;
        PlayerMatchStats& m = pc.stat_manager->match;
        m.bDataValid = true;
        m.bDirty = true;
        m.fpStampLastUpdate = stamp;
        const int32_t team = TeamOf(pc);
        if (winner >= 0 && team == winner) { ++m.iRoundsWon; ++pc.pri->rounds_won; }
        else if (winner >= 0) { ++m.iRoundsLost; ++pc.pri->rounds_lost; }
        m.iHighestMatchNeutralizationScore = std::max<int32_t>(m.iHighestMatchNeutralizationScore, r.iNeutralizedEnemy);
        m.iHighestMatchMedicScore = std::max(m.iHighestMatchMedicScore,
            pc.pri->medic_active + pc.pri->medic_passive + pc.pri->medic_recap);
        m.iHighestMatchObjectiveScore = std::max(m.iHighestMatchObjectiveScore, pc.pri->objectives_completed);
        m.iHighestMatchTotalScore = std::max(m.iHighestMatchTotalScore, pc.pri->match_total_score);
        LOG_INFO("Round stats for %s: %d neutralized, %d secured, %d confirmed, %d hits", pc.pri->player_name.c_str(),
                 r.iNeutralizedEnemy, r.iSecuredEnemy, r.iConfirmedEnemy,
                 r.iHead + r.iUpperTorso + r.iLowerTorso + r.iRightUpperLeg + r.iRightLowerLeg);
    }
}

// A new round: the round's counts start over (the match's carry on).
void World::ResetRoundStats() {
    for (auto& c : controllers_) {
        c->live_round = PlayerRoundStats{};
        if (ServerPlayerReplicationInfo* pri = c->pri) {
            pri->round_neutralized = pri->round_confirmed = 0;
            pri->xp_courage = pri->xp_duty = pri->xp_respect = pri->xp_round_honor = 0;
        }
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
    ++pc.live_round.iES2ObjectsUsed;
    perk->Set(kPerkAmount, -1, DataValue::Int(amount - 1));
    if (amount - 1 < 1) perk->gone = true;        // "how perky is it really" -> Destroy()
    LOG_INFO("%s took from ES2 perk type %lld (%lld left)", pri.player_name.c_str(),
             static_cast<long long>(type), static_cast<long long>(amount - 1));
    return true;
}

}  // namespace game
}  // namespace aa3
