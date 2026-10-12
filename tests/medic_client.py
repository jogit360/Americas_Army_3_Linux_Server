#!/usr/bin/env python3
"""Scripted IFAK treatment run against aa3_server (doc sec 39), on
fake_client.py / grenade_client.py.

A and B join Black; A stands 200 units from B, draws its rifle, and after the
initial reload:
  1. shoots B in the upper leg -> B is wounded and bleeding;
  2. treats B (ServerStartTreatment('Player2') + ServerStartPassiveTreatment,
     2.5 s, ServerStopTreatment) -> bleeding stops, the wound is treated;
  3. shoots B in the head -> B is incapacitated;
  4. revives B (ServerStartTreatment + ServerGiveActiveTreatment('Saline')).

PASS requires, on A's connection: SetTreatmentOnRequestingClient (215)
leading the reply bunch to each start and stop; and on B's: the revive's
ClientRecapacitate (pawn RPC 110). The server log (checked by the caller)
shows wound / bleeding / treated / revived lines.

Start the server standalone with short round timers (RoleSelectionTime 2,
DeploymentTime 1).
"""
import argparse
import sys
import threading
import time

import fake_client as fc
from fake_client import BW
from grenade_client import WeaponsClient, CDO_M67

CDO_M4A1 = 5251
CDO_IM = 13669
RIFLE_MAX = 71
REP_STARTFIRE, REP_STOPFIRE = 31, 32
REP_START_TREATMENT, REP_STOP_TREATMENT = 213, 214
REP_START_PASSIVE, REP_GIVE_ACTIVE = 216, 219
REP_SET_TREATMENT = 215                   # SetTreatmentOnRequestingClient (to the medic)
PAWN_CLIENT_RECAPACITATE = 110            # AA_Pawn.ClientRecapacitate


class MedicClient(WeaponsClient):
    def __init__(self, *a):
        super().__init__(*a)
        self.own_pawn_calls = []

    def deliver(self, ch, b_open, b_close, payload):
        if ch == fc.OWN_PAWN_CH and payload and not b_open:
            w = BW(); w.bits = payload
            r = fc.BR(bytes(w.packet()), len(payload))
            try:
                with self.lock:
                    self.own_pawn_calls.append(r.int_max(fc.PAWN_MAX))
            except IndexError:
                pass
        super().deliver(ch, b_open, b_close, payload)

    def pc_string(self, rep_index, *strings):
        p = BW(); p.int_max(rep_index, fc.PC_MAX)
        for s in strings:
            p.bit(1)
            for b in fc.fstring(s):
                p.bitsn(b, 8)
        self.reliable(fc.PC_CH, p)

    def give_active(self, treatment, name):
        p = BW(); p.int_max(REP_GIVE_ACTIVE, fc.PC_MAX)
        p.bit(1)                                            # TreatmentType: AA3 FName
        for b in fc.fstring(treatment):
            p.bitsn(b, 8)
        p.u32(0)
        p.bit(0)                                            # intendedWoundTreatment: Send=0 (retail)
        p.bit(1)                                            # sVictimSoldierName
        for b in fc.fstring(name):
            p.bitsn(b, 8)
        self.reliable(fc.PC_CH, p)

    def pc_call(self, rep_index):
        p = BW(); p.int_max(rep_index, fc.PC_MAX)
        self.reliable(fc.PC_CH, p)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8777)
    a = ap.parse_args()

    A = MedicClient("A", a.host, a.port)
    B = MedicClient("B", a.host, a.port)
    A.handshake(); A.recv_all(1.0)
    B.handshake(); B.recv_all(1.0)
    for c in (A, B):
        threading.Thread(target=lambda c=c: [c.recv_all(0.05) for _ in iter(lambda: c.stop, True)],
                         daemon=True).start()
    A.send_join_team(0); B.send_join_team(0)
    time.sleep(1.5)
    A.send_int_rpc(fc.REP_SELECTROLE, 1); B.send_int_rpc(fc.REP_SELECTROLE, 2)
    time.sleep(0.3)
    A.send_ready(True); B.send_ready(True)
    t = time.time() + 30
    while time.time() < t and not (A.gave_pawn and B.gave_pawn):
        time.sleep(0.1)
    time.sleep(0.8)
    grenades = (CDO_M67, 33062, 33088, 33070, 33037)
    with A.lock:
        by_ch = dict(A.ch_class)
    for ch, o in by_ch.items():
        if all(by_ch.get(ch + k) == o - CDO_M67 + g for k, g in enumerate(grenades)):
            A.base = o - CDO_M67
    rifle, im = A.channels_of(CDO_M4A1), A.channels_of(CDO_IM)
    print("A rifle", rifle, "IM", im)
    if not (rifle and im):
        print("FAIL: no rifle")
        return 1

    aim = {"pitch": 0}
    stop = threading.Event()
    def mover():
        ts = 1.0
        while not stop.is_set():
            ts += 0.05
            A.send_move(ts, (11385.0, -3308.0, 65.0), yaw=0, pitch=aim["pitch"] & 0xFFFF)
            B.send_move(ts, (11585.0, -3308.0, 65.0), yaw=32768)
            time.sleep(0.05)
    threading.Thread(target=mover, daemon=True).start()

    A.set_current_weapon(im[0], rifle[0])
    time.sleep(8.0)                                        # initial reload

    def shoot(pitch):
        aim["pitch"] = pitch
        time.sleep(0.4)
        A.call(rifle[0], RIFLE_MAX, REP_STARTFIRE); time.sleep(0.1)
        A.call(rifle[0], RIFLE_MAX, REP_STOPFIRE); time.sleep(0.5)

    shoot(-2849)                                           # upper leg (z ~40 at 200 units)
    time.sleep(1.0)
    n = len(A.pc_calls)
    A.pc_string(REP_START_TREATMENT, "Player2")
    A.pc_call(REP_START_PASSIVE)
    time.sleep(2.6)
    A.pc_call(REP_STOP_TREATMENT)
    time.sleep(0.6)
    treat_calls = A.pc_calls[n:]

    shoot(469)                                             # head (z ~105)
    time.sleep(1.5)
    n = len(A.pc_calls)
    A.pc_string(REP_START_TREATMENT, "Player2")
    time.sleep(0.3)
    A.give_active("Saline", "Player2")
    time.sleep(0.5)
    A.pc_call(REP_STOP_TREATMENT)
    time.sleep(1.0)
    revive_calls = A.pc_calls[n:]
    stop.set()
    A.stop = B.stop = True

    print(f"A PC calls around treatment: {treat_calls}; around revive: {revive_calls}")
    print(f"B own-pawn calls: {B.own_pawn_calls[-15:]}")
    checks = {
        "treatment start/stop reply": treat_calls.count(REP_SET_TREATMENT) >= 2,
        "revive start reply": REP_SET_TREATMENT in revive_calls,
        "patient ClientRecapacitate": PAWN_CLIENT_RECAPACITATE in B.own_pawn_calls,
    }
    for k, v in checks.items():
        print(f"  {k}: {'ok' if v else 'FAILED'}")
    ok = all(checks.values())
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
