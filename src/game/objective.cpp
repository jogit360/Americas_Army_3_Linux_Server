// Carried objective: the briefcase of a co ("Secure and Extract") mission,
// from the retail Bridge AI capture of 2026-10-12
// (captures/bridge_ai_2026-10-12/FINDINGS.md):
//
//   join      AA_Objective_Briefcase_US at its AA_PickupFactory_Objective
//             (AttachInfo SpineBSocket); the factory (placed, Inv = the
//             briefcase); the AA_MCCarryableObjectiveMarker (MyLocation,
//             PreplanSymbol, aLinkedObjectiveItems[0] = MyFactory = factory).
//   pickup    ServerRepExec('Pickup', factory) + ServerUse: the briefcase's
//             channel closes and a fresh one opens attached to the carrier --
//             to the carrier: Owner, Instigator, bNetOwner, team, InvManager,
//             AttachInfo, ClientGivenTo(pawn), AttachedActor; to the others:
//             Instigator, namePawnInventoryAssociation, team, AttachInfo,
//             AttachedActor. Factory: bCollideActors/bBlockActors off, Inv
//             None; marker bHideSAIIcon. The carrier: SetNotificationMessage
//             + ReceiveLocalizedMessage(LocalMessage, AA_Objective_Briefcase_US).
//   death     the briefcase lets go (Owner/Instigator/AttachedActor None,
//             bNetOwner off) and an AA_DroppedObjective opens at the body
//             (Physics Falling, then None when it lands); the carrier gets
//             ClientSendObjectiveDroppedNotification + DiscardInventory(22).
//   win       the carrier inside its team's extraction zone.
//   reset     everything closes; a fresh briefcase opens on the factory.
#include <algorithm>
#include <cmath>
#include "game/world.h"
#include "util/logging.h"

namespace aa3 {
namespace game {

namespace {
// AA3Baseline NetIndex (aa3_build_manifests --dump-exports).
constexpr uint32_t kCdoDroppedObjective = 7016;   // Default__AA_DroppedObjective
constexpr uint32_t kClassBriefcaseUS = 26201;     // class AA_Objective_Briefcase_US
constexpr uint32_t kMaxDroppedObjective = 34;
// RepIndex: Actor / Inventory / AA_Inventory (AA_Objective_Briefcase_US).
constexpr uint32_t kOriginalTeam = 0, kInvAssociation = 3, kInstigator = 5, kNetOwner = 6, kOwner = 15,
                   kInvManager = 24, kClientGivenTo = 26, kAttachInfo = 27, kAttachedActor = 28;
// AA_PickupFactory_Objective.
constexpr uint32_t kFactoryBlockActors = 8, kFactoryCollideActors = 10, kFactoryInv = 26;
// AA_MCCarryableObjectiveMarker.
constexpr uint32_t kMarkerHideSAIIcon = 27;
// AA_DroppedObjective.
constexpr uint32_t kDropVelocity = 4, kDropBlockActors = 8, kDropCollisionType = 16, kDropPhysics = 19,
                   kDropRotation = 21, kDropLocation = 22, kDropInventoryClass = 24, kDropMesh = 26,
                   kDropMarker = 28, kDropFactory = 31;
constexpr float kPickupReach = 200.0f;            // pawn to the factory / dropped case
constexpr uint8_t kPhysNone = 0, kPhysFalling = 2;

DataValue Byte(int64_t v, uint32_t bits) {
    DataValue d; d.kind = DataValue::Kind::kByte; d.i = v; d.bits = bits; return d;
}
DataValue Obj(const std::string& pkg, uint32_t net) {
    DataValue d; d.kind = DataValue::Kind::kObj; d.s = pkg; d.net = net; return d;
}
DataValue Vector(const float* v) {
    DataValue d; d.kind = DataValue::Kind::kVector;
    for (int k = 0; k < 3; ++k) d.v3[k] = static_cast<int32_t>(std::lround(v[k]));
    return d;
}
float Dist(const float* a, const float* b) {
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}
}  // namespace

uint32_t World::ObjectiveItemClassIndex() const {
    return carry_.active ? BaselineIndex(kClassBriefcaseUS) : 0;
}

// After the join actors exist: the briefcase moves out of them (it is
// replaced on every pickup), its at-rest copy is kept for resets.
void World::SetupCarriedObjective() {
    objective_actors_.clear();
    if (!carry_.active) return;
    for (auto it = join_actors_.begin(); it != join_actors_.end();) {
        ServerRoundActor* a = it->get();
        if (a->class_name == "AA_PickupFactory_Objective") carry_.factory = a;
        else if (a->class_name == "AA_MCCarryableObjectiveMarker") carry_.marker = a;
        if (a->class_name.rfind("AA_Objective_", 0) == 0) {
            carry_.proto = *a;
            objective_actors_.push_back(std::move(*it));
            carry_.item = objective_actors_.back().get();
            it = join_actors_.erase(it);
            continue;
        }
        ++it;
    }
    if (!carry_.item || !carry_.factory) {
        LOG_WARN("Carried objective: no briefcase or factory in the join actors; disabled");
        carry_.active = false;
        return;
    }
    LOG_INFO("Carried objective: %s at (%.0f, %.0f, %.0f)", carry_.proto.class_name.c_str(),
             carry_.home[0], carry_.home[1], carry_.home[2]);
}

// The briefcase back on its factory (round start).
void World::ResetCarriedObjective() {
    if (!carry_.active) return;
    for (auto& a : objective_actors_) a->gone = true;
    objective_actors_.push_back(std::make_unique<ServerRoundActor>(carry_.proto));
    carry_.item = objective_actors_.back().get();
    carry_.dropped = nullptr;
    carry_.carrier = nullptr;
    carry_.settle_at = -1.0;
    carry_.factory->Set(kFactoryCollideActors, -1, DataValue::Bool(true));
    carry_.factory->Set(kFactoryBlockActors, -1, DataValue::Bool(true));
    carry_.factory->Set(kFactoryInv, -1, DataValue::Actor(carry_.item));
    if (carry_.marker) carry_.marker->Set(kMarkerHideSAIIcon, -1, DataValue::Bool(false));
}

bool World::PickUpObjective(ServerPlayerController& pc, uint32_t static_obj, const ServerActor* actor) {
    ServerPawn* pawn = pc.pawn;
    const char* why = !carry_.active ? "no carried objective" : carry_.carrier ? "already carried"
                    : !pawn ? "no soldier" : pawn->torn_off ? "dead" : pawn->incapacitated ? "down" : nullptr;
    const bool from_factory = static_obj != 0 && static_obj == ObjectIndex(carry_.factory_pkg, carry_.factory_net);
    const bool from_ground = actor && carry_.dropped && actor == carry_.dropped;
    if (!why && !from_factory && !from_ground) why = "not the objective";
    if (why) {
        LOG_INFO("%s can't pick up the objective: %s (obj %u, factory %u)", pc.pri ? pc.pri->player_name.c_str() : "?",
                 why, static_obj, ObjectIndex(carry_.factory_pkg, carry_.factory_net));
        return false;
    }
    const float* at = from_ground ? carry_.dropped->location : carry_.home;
    if (Dist(pawn->location, at) > kPickupReach) {
        LOG_INFO("%s too far from the objective to pick it up (%.0f)",
                 pc.pri ? pc.pri->player_name.c_str() : "?", Dist(pawn->location, at));
        return false;
    }
    const int32_t team = TeamOf(pc);

    // A fresh briefcase actor, attached to the carrier.
    for (auto& a : objective_actors_) a->gone = true;
    auto item = std::make_unique<ServerRoundActor>(carry_.proto);
    for (int k = 0; k < 3; ++k) item->location[k] = at[k];
    item->owner = pawn;
    auto set_owner_only = [&](uint32_t idx, const DataValue& v) {
        item->Set(idx, -1, v);
        for (auto& f : item->fields) if (f.idx == idx) f.owner_only = true;
    };
    item->fields.clear();
    set_owner_only(kOwner, DataValue::Actor(pawn));
    item->Set(kInstigator, -1, DataValue::Actor(pawn));
    set_owner_only(kNetOwner, DataValue::Bool(true));
    DataValue assoc; assoc.kind = DataValue::Kind::kName;
    assoc.s = pawn->inventory_association; assoc.i = pawn->inventory_association_number;
    item->Set(kInvAssociation, -1, assoc);
    item->Set(kOriginalTeam, -1, DataValue::Int(team));
    if (pawn->inv_manager) set_owner_only(kInvManager, DataValue::Actor(pawn->inv_manager));
    if (const DataField* ai = carry_.proto.Get(kAttachInfo)) item->Set(kAttachInfo, -1, ai->v);
    DataValue given; given.kind = DataValue::Kind::kStruct;          // ClientGivenTo(NewOwner, bDoNotActivate)
    given.members = {DataValue::Bool(true), DataValue::Actor(pawn), DataValue::Bool(false)};
    set_owner_only(kClientGivenTo, given);
    item->Set(kAttachedActor, -1, DataValue::Actor(pawn));
    objective_actors_.push_back(std::move(item));
    carry_.item = objective_actors_.back().get();
    carry_.carrier = pawn;
    carry_.dropped = nullptr;
    carry_.settle_at = -1.0;

    carry_.factory->Set(kFactoryCollideActors, -1, DataValue::Bool(false));
    carry_.factory->Set(kFactoryBlockActors, -1, DataValue::Bool(false));
    carry_.factory->Set(kFactoryInv, -1, DataValue::Actor(nullptr));
    if (carry_.marker) carry_.marker->Set(kMarkerHideSAIIcon, -1, DataValue::Bool(true));
    ++pc.objective_pickup_serial;
    LOG_INFO("%s picked up the objective%s", pc.pri ? pc.pri->player_name.c_str() : "?",
             from_ground ? " (from the ground)" : "");
    return true;
}

// The carrier is killed or leaves: the briefcase lets go and an
// AA_DroppedObjective falls where the carrier was.
bool World::DropObjectiveByChoice(ServerPlayerController& pc) {
    if (!carry_.active || !pc.pawn || carry_.carrier != pc.pawn) return false;
    DropObjective(*pc.pawn, 50.0f);
    return true;
}

void World::DropObjective(ServerPawn& carrier, float ahead) {
    if (!carry_.active || carry_.carrier != &carrier) return;
    ServerRoundActor& item = *carry_.item;
    item.fields.erase(std::remove_if(item.fields.begin(), item.fields.end(),
                                     [](const DataField& f) { return f.idx == kClientGivenTo; }),
                      item.fields.end());
    for (auto& f : item.fields) f.owner_only = false;
    item.Set(kOwner, -1, DataValue::Actor(nullptr));
    item.Set(kInstigator, -1, DataValue::Actor(nullptr));
    item.Set(kNetOwner, -1, DataValue::Bool(false));
    item.Set(kAttachedActor, -1, DataValue::Actor(nullptr));
    item.owner = nullptr;                            // Owner/bNetOwner now go out (None/False) to all

    auto drop = std::make_unique<ServerRoundActor>();
    drop->class_name = "AA_DroppedObjective";
    drop->max_index = kMaxDroppedObjective;
    drop->cdo_package = "AA3Baseline";
    drop->cdo_net_index = kCdoDroppedObjective;
    drop->object_index = BaselineIndex(kCdoDroppedObjective);
    drop->transient = true;
    drop->label = drop->class_name;
    for (int k = 0; k < 3; ++k) drop->location[k] = carrier.location[k];
    if (ahead > 0.0f) {                              // SafeDropObjective: 50 units out, at the feet
        const double yaw = carrier.rotation_yaw * 3.14159265358979 / 32768.0;
        drop->location[0] += static_cast<float>(std::cos(yaw) * ahead);
        drop->location[1] += static_cast<float>(std::sin(yaw) * ahead);
    }
    DataValue rot; rot.kind = DataValue::Kind::kRotator; rot.v3[1] = carrier.rotation_yaw;
    drop->Set(kDropRotation, -1, rot);
    const float fall[3] = {0.0f, 0.0f, -70.0f};
    drop->Set(kDropVelocity, -1, Vector(fall));
    drop->Set(kDropPhysics, -1, Byte(kPhysFalling, 4));
    drop->Set(kDropCollisionType, -1, Byte(3, 4));   // ECollisionType is 4 bits (retail)
    drop->Set(kDropBlockActors, -1, DataValue::Bool(true));
    drop->Set(kDropInventoryClass, -1, Obj("AA3Baseline", kClassBriefcaseUS));
    DataValue mesh; mesh.kind = DataValue::Kind::kRaw; mesh.s = "111111111111";   // retail's bits
    drop->Set(kDropMesh, -1, mesh);
    if (carry_.marker) drop->Set(kDropMarker, -1, DataValue::Actor(carry_.marker));
    drop->Set(kDropFactory, -1, Obj(carry_.factory_pkg, carry_.factory_net));
    objective_actors_.push_back(std::move(drop));
    carry_.dropped = objective_actors_.back().get();
    carry_.settle_at = clock_ + 0.07;                 // retail: lands 0.07 s later
    carry_.carrier = nullptr;
    for (auto& c : controllers_)
        if (c->pawn == &carrier || carrier.controller == c.get()) ++c->objective_drop_serial;
    LOG_INFO("Objective dropped at (%.0f, %.0f, %.0f)", carrier.location[0], carrier.location[1],
             carrier.location[2]);
}

void World::UpdateObjective() {
    if (!carry_.active) return;
    if (carry_.carrier && (carry_.carrier->torn_off || carry_.carrier->destroyed)) DropObjective(*carry_.carrier);
    if (carry_.dropped && carry_.settle_at >= 0.0 && clock_ >= carry_.settle_at) {
        carry_.settle_at = -1.0;
        const float still[3] = {0.0f, 0.0f, 0.0f};
        carry_.dropped->Set(kDropVelocity, -1, Vector(still));
        carry_.dropped->Set(kDropPhysics, -1, Byte(kPhysNone, 4));
    }
}

}  // namespace game
}  // namespace aa3
