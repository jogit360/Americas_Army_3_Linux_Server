// Wounds, bleeding and IFAK treatment (doc sec 39), from the retail capture
// of 2026-10-11 (captures/ifak_decoded_2026-10-11) and AA3Baseline's
// AA_PlayerController / AA_Pawn treatment script:
//
//   hit        Health -= ~43% of the hit at once; HealthTarget = Health - the
//              rest (the bleed); bIsBleeding; a wound at the hit location.
//   bleeding   Health drains ~1.2/s to HealthTarget, then stops; at
//              IncapacitationHealth (25) the soldier goes down
//              (AA_DamageType_Bleeding).
//   treatment  ServerStartTreatment(name) links medic and patient
//              (CurrentTreatmentTreater[0]); ServerStartPassiveTreatment
//              stops all bleeding (HealthTarget = Health) and works the most
//              critical wound, WoundTreatmentProgress +1/60 per 33 ms (2 s),
//              then the wound is treated (level 1); ServerStopTreatment ends it.
//   revive     ServerGiveActiveTreatment on a downed patient uses an IFAK --
//              the patient's own first -- and recapacitates: Health 25,
//              HealthTarget 25 minus the bleed still owed, ClientRecapacitate.
#include <algorithm>
#include <cmath>
#include "game/world.h"
#include "util/logging.h"

namespace aa3 {
namespace game {

namespace {
constexpr float kBleedPerSecond = 1.2f;       // retail: 84.88 -> 79.08 in 4.84 s
constexpr float kTreatPerSecond = 0.5f;       // 1/60 per 33 ms
constexpr float kTreatRange = 250.0f;         // medic to patient (guess; GetVisiblePawnFromSoldierName)
constexpr uint32_t kBleedingDamage = 6419;    // AA3Baseline AA_DamageType_Bleeding

float Dist(const ServerPawn& a, const ServerPawn& b) {
    const float dx = a.location[0] - b.location[0], dy = a.location[1] - b.location[1],
                dz = a.location[2] - b.location[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}
}  // namespace

void World::AddWound(ServerPawn& victim, int location, float bleed, const std::string& attacker) {
    if (victim.torn_off || victim.incapacitated) return;
    location = std::max(0, std::min(ServerPawn::kHitLocations - 1, location));
    auto& slots = victim.wounds[static_cast<size_t>(location)];
    for (auto& w : slots)
        if (!w.present) { w = ServerPawn::Wound{}; w.present = true; break; }
    victim.bleed_rate[static_cast<size_t>(location)] = 1.0f;
    victim.health_target = std::max(0.0f, (victim.bleeding ? victim.health_target : victim.health) - bleed);
    victim.bleeding = true;
    victim.recapacitated = false;
    victim.last_attacker = attacker;
    LOG_INFO("%s wounded at location %d: health %.1f, bleeding to %.1f",
             victim.pri ? victim.pri->player_name.c_str() : "?", location, victim.health, victim.health_target);
}

ServerPawn* World::FindPatient(const ServerPawn& medic, const std::string& name) {
    if (!medic.pri || !medic.pri->team) return nullptr;
    for (auto& p : pawns_) {
        ServerPawn& o = *p;
        if (&o == &medic || o.torn_off || o.destroyed || !o.pri || o.pri->team != medic.pri->team) continue;
        if (o.body_soldier_name != name && o.pri->player_name != name) continue;
        if (Dist(o, medic) > kTreatRange) return nullptr;
        return &o;
    }
    return nullptr;
}

ServerPawn* World::StartTreatment(ServerPawn* medic, const std::string& patient_name) {
    if (!medic || medic->torn_off || medic->incapacitated) return nullptr;
    ServerPawn* patient = FindPatient(*medic, patient_name);
    if (!patient) return nullptr;
    if (medic->patient && medic->patient != patient) StopTreatment(medic);
    medic->patient = patient;
    medic->passive_treatment = false;
    patient->treater = medic;
    LOG_INFO("%s starts treating %s", medic->pri ? medic->pri->player_name.c_str() : "?", patient_name.c_str());
    return patient;
}

bool World::StartPassiveTreatment(ServerPawn* medic) {
    if (!medic || !medic->patient || medic->patient->incapacitated) return false;
    const ServerPawn& p = *medic->patient;
    if (!p.bleeding && !p.HasInjury(true) && !p.HasInjury(false)) return false;   // CanBeTreatedBySlow
    medic->passive_treatment = true;
    return true;
}

void World::StopTreatment(ServerPawn* medic) {
    if (!medic) return;
    if (medic->patient && medic->patient->treater == medic) medic->patient->treater = nullptr;
    medic->patient = nullptr;
    medic->passive_treatment = false;
}

bool World::GiveActiveTreatment(ServerPawn* medic, const std::string& type, const std::string& patient_name) {
    if (!medic || medic->torn_off || medic->incapacitated) return false;
    ServerPawn* patient = FindPatient(*medic, patient_name);
    if (!patient || !patient->incapacitated) return false;   // only the revive is modelled
    // The IFAK: the patient's own first, else the medic's.
    auto take_ifak = [&](ServerPawn& from) {
        auto it = pawn_inventory_.find(&from);
        if (it == pawn_inventory_.end()) return false;
        for (auto& item : it->second)
            if (item->class_name == "AA_IFAK" && !item->gone) { item->gone = true; return true; }
        return false;
    };
    const bool own = take_ifak(*patient);
    if (!own && !take_ifak(*medic)) {
        LOG_INFO("%s has no IFAK to treat %s", medic->pri ? medic->pri->player_name.c_str() : "?",
                 patient_name.c_str());
        return false;
    }
    // AA_IFAK's removal updates the owner's bHasReviveKit.
    for (ServerPawn* p : {patient, medic}) {
        if (!p->pri) continue;
        p->pri->has_revive_kit = false;
        auto it = pawn_inventory_.find(p);
        if (it != pawn_inventory_.end())
            for (auto& item : it->second)
                if (item->class_name == "AA_IFAK" && !item->gone) p->pri->has_revive_kit = true;
    }
    patient->recap_used_own_ifak = own;
    Recapacitate(*patient);
    LOG_INFO("%s revived %s (%s)", medic->pri ? medic->pri->player_name.c_str() : "?", patient_name.c_str(),
             type.c_str());
    return true;
}

// Retail t=204.78 (patient's own channel): ClientRecapacitate(), Health 25,
// RepGraphStates[0] {134}, [2] {74 -> 1}, IncapacitationType None,
// HealthTarget 25 - the bleed still owed (18.73 - 6.59), bRecapacitated,
// bResetWeaponIK; GroundSpeed 124 at +0.26 s.
void World::Recapacitate(ServerPawn& p) {
    const float owed = std::max(0.0f, p.health - p.target_before_incap);
    p.incapacitated = false;
    p.incap_stage = 0;
    p.incap_at = -1.0;
    p.ragdoll = false;
    p.health = kIncapacitationHealth;
    p.health_target = std::max(0.0f, kIncapacitationHealth - owed);
    p.incap_type_index = 0;
    p.recapacitated = true;
    p.reset_weapon_ik = true;
    p.SetGraph(0, 134, 134);
    p.SetGraph(2, 74, 1);
    p.speed_at = clock_ + 0.26;
    p.speed_after = 124.0f;
    ++p.recap_serial;
    for (auto& c : controllers_)
        if (c->pawn == &p) {
            if (c->pri) c->pri->b_incapacitated = false;
            if (c->proxy) c->proxy->incapacitated = false;
        }
}

void World::UpdateInjuries(double dt) {
    const float fdt = static_cast<float>(dt);
    for (auto& up : pawns_) {
        ServerPawn& p = *up;
        if (p.torn_off || p.destroyed) continue;
        // Bleeding toward HealthTarget (a downed soldier's bleed-out is
        // UpdateIncapacitated's). A revived soldier doesn't bleed back down
        // past 25 (retail's held at 25 until treated).
        if (p.bleeding && !p.incapacitated) {
            float rate = 0.0f;
            for (float r : p.bleed_rate) rate += r;
            if (rate <= 0.0f) rate = 1.0f;
            float next = std::max(p.health_target, p.health - kBleedPerSecond * rate * fdt);
            if (p.recapacitated) next = std::max(next, kIncapacitationHealth);
            if (next < p.health) {
                p.health = next;
                p.movement_speed_modifier = InjuredSpeedModifier(p.health);
            }
            if (p.health <= p.health_target + 1e-3f) {
                p.bleeding = false;
                p.bleed_rate.fill(0.0f);
            } else if (!p.recapacitated && p.health <= kIncapacitationHealth) {
                Incapacitate(p, kBleedingDamage, p.last_attacker.empty() ? "Unknown" : p.last_attacker);
            }
        }
        // A medic at work: patient still there and in reach, else stop.
        if (ServerPawn* pat = p.patient) {
            if (pat->torn_off || pat->destroyed || p.incapacitated || Dist(p, *pat) > kTreatRange) {
                StopTreatment(&p);
                continue;
            }
            if (!p.passive_treatment || pat->incapacitated) continue;
            // AA_Pawn.ReceivePassiveTreatment: bleeding off, then the most
            // critical untreated wound's progress.
            if (pat->bleeding) {
                pat->bleeding = false;
                pat->bleed_rate.fill(0.0f);
                pat->health_target = pat->health;
                LOG_INFO("Treatment stopped %s's bleeding at %.1f",
                         pat->pri ? pat->pri->player_name.c_str() : "?", pat->health);
            }
            bool worked = false;
            for (auto& loc : pat->wounds) {
                for (auto& w : loc)
                    if (w.present && !w.treated) {
                        w.progress = std::min(1.0f, w.progress + kTreatPerSecond * fdt);
                        if (w.progress >= 1.0f) {
                            w.treated = true;
                            LOG_INFO("%s's wound treated", pat->pri ? pat->pri->player_name.c_str() : "?");
                        }
                        worked = true;
                        break;
                    }
                if (worked) break;
            }
        }
    }
}

}  // namespace game
}  // namespace aa3
