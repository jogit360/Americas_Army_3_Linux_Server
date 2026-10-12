#!/usr/bin/env python3
"""Three-bot IFAK scenario against aa3_server (doc sec 39), on medic_client.py.

  Medic   (Black) stands 150 units beside the patient, rifle lowered.
  Patient (Black) stands still, 200 units in front of the enemy.
  Enemy   (Gold)  draws its rifle and, after the initial reload:
    1. shoots the patient in the upper leg -> wounded, bleeding;
       the medic treats (ServerStartTreatment + passive, 2.6 s, stop);
    2. shoots the patient in the head -> incapacitated;
       the medic revives (ServerStartTreatment + Saline, stop).

PASS requires: the medic got the whole MOS set (6 qualification opens,
Combat Medic included; doc sec 39.7), SetTreatmentOnRequestingClient (215)
replies to each start and stop, and the patient's ClientRecapacitate (110).
The server log shows the wound / treated / revived lines.
"""
import argparse
import sys
import threading
import time

import fake_client as fc
from grenade_client import CDO_M67, CDO_IM
from medic_client import (MedicClient, REP_START_TREATMENT, REP_STOP_TREATMENT, REP_START_PASSIVE,
                          REP_SET_TREATMENT, PAWN_CLIENT_RECAPACITATE, REP_STARTFIRE, REP_STOPFIRE)

RIFLES = {5251: 71, 5153: 71, 5157: 71, 5245: 72}   # kit weapon CDO -> MaxIndex
MOS_CDOS = {18071: "BCT", 18079: "DRMAIT", 18091: "GrenadierAIT", 18032: "CombatMedic",
            18044: "ARAIT", 18099: "RiflemanAIT"}
GRENADES = (CDO_M67, 33062, 33088, 33070, 33037)


def find_base(c):
    with c.lock:
        by_ch = dict(c.ch_class)
    for ch, o in by_ch.items():
        if all(by_ch.get(ch + k) == o - CDO_M67 + g for k, g in enumerate(GRENADES)):
            c.base = o - CDO_M67
            return True
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8777)
    a = ap.parse_args()

    medic = MedicClient("Medic", a.host, a.port)
    patient = MedicClient("Patient", a.host, a.port)
    enemy = MedicClient("Enemy", a.host, a.port)
    bots = (medic, patient, enemy)
    for c in bots:
        c.handshake(); c.recv_all(1.0)
    for c in bots:
        threading.Thread(target=lambda c=c: [c.recv_all(0.05) for _ in iter(lambda: c.stop, True)],
                         daemon=True).start()
    medic.send_join_team(0); patient.send_join_team(0); enemy.send_join_team(1)
    time.sleep(1.5)
    medic.send_int_rpc(fc.REP_SELECTROLE, 1); patient.send_int_rpc(fc.REP_SELECTROLE, 2)
    enemy.send_int_rpc(fc.REP_SELECTROLE, 1)
    time.sleep(0.3)
    for c in bots:
        c.send_ready(True)
    t = time.time() + 30
    while time.time() < t and not all(c.gave_pawn for c in bots):
        time.sleep(0.1)
    time.sleep(0.8)
    if not all(find_base(c) for c in bots):
        print("FAIL: no grenade set (no pawn?)")
        return 1

    with medic.lock:
        medic_objs = set(medic.ch_class.values())
    mos = [n for cdo, n in MOS_CDOS.items() if medic.base + cdo in medic_objs]
    print("medic MOS qualifications:", mos)

    rifle = next(((ch, mx) for cdo, mx in RIFLES.items() for ch in enemy.channels_of(cdo)), None)
    im = enemy.channels_of(CDO_IM)
    print("enemy rifle", rifle, "IM", im)
    if not (rifle and im):
        print("FAIL: enemy has no rifle")
        return 1

    aim = {"pitch": 0}
    stop = threading.Event()
    def mover():
        ts = 1.0
        while not stop.is_set():
            ts += 0.05
            enemy.send_move(ts, (11385.0, -3308.0, 65.0), yaw=0, pitch=aim["pitch"] & 0xFFFF)
            patient.send_move(ts, (11585.0, -3308.0, 65.0), yaw=32768)
            medic.send_move(ts, (11585.0, -3158.0, 65.0), yaw=49152)
            time.sleep(0.05)
    threading.Thread(target=mover, daemon=True).start()

    enemy.set_current_weapon(im[0], rifle[0])
    time.sleep(8.0)                                        # initial reload

    def shoot(pitch):
        aim["pitch"] = pitch
        time.sleep(0.4)
        enemy.call(rifle[0], rifle[1], REP_STARTFIRE); time.sleep(0.1)
        enemy.call(rifle[0], rifle[1], REP_STOPFIRE); time.sleep(0.5)

    print("enemy: leg shot")
    shoot(-2849)                                           # upper leg (z ~40 at 200 units)
    time.sleep(1.0)
    n = len(medic.pc_calls)
    print("medic: start treatment + passive")
    medic.pc_string(REP_START_TREATMENT, "Player2")
    medic.pc_call(REP_START_PASSIVE)
    time.sleep(2.6)
    medic.pc_call(REP_STOP_TREATMENT)
    time.sleep(0.6)
    treat_calls = medic.pc_calls[n:]

    print("enemy: head shot")
    shoot(469)                                             # head (z ~105)
    time.sleep(1.5)
    n = len(medic.pc_calls)
    print("medic: start treatment + Saline")
    medic.pc_string(REP_START_TREATMENT, "Player2")
    time.sleep(0.3)
    medic.give_active("Saline", "Player2")
    time.sleep(0.5)
    medic.pc_call(REP_STOP_TREATMENT)
    time.sleep(1.0)
    revive_calls = medic.pc_calls[n:]
    stop.set()
    for c in bots:
        c.stop = True

    print(f"medic PC calls around treatment: {treat_calls}; around revive: {revive_calls}")
    print(f"patient own-pawn calls: {patient.own_pawn_calls[-15:]}")
    checks = {
        "medic has all 6 MOS (Combat Medic)": len(mos) == 6,
        "treatment start/stop reply": treat_calls.count(REP_SET_TREATMENT) >= 2,
        "revive start reply": REP_SET_TREATMENT in revive_calls,
        "patient ClientRecapacitate": PAWN_CLIENT_RECAPACITATE in patient.own_pawn_calls,
    }
    for k, v in checks.items():
        print(f"  {k}: {'ok' if v else 'FAILED'}")
    ok = all(checks.values())
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
