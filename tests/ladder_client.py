#!/usr/bin/env python3
"""Ladder-from-the-top and vault replies of aa3_server (retail Bridge AI
capture 2026-10-12, 13:37:53), on fake_client.py / medic_client.py.

A soldier spawns, then sends what the retail client sends to climb down a
ladder: AA_Pawn.ServerStartMoveToTarget(TargetLocation, TargetRotation,
FinishMoveCallback = this pawn's 'FinishLadderMoveToTarget') (97), then
ServerClimbLadder (103) and ServerEndClimbLadder (104). Then a vault: the
same move with 'FinishVaultMantleMoveToTarget'.

PASS requires, on the own pawn channel: ClientFinishLadderMoveToTarget (102)
and no ClientFinishVaultMantleMoveToTarget (100) after the ladder move, and
100 after the vault move.
"""
import argparse
import sys
import threading
import time

import fake_client as fc
from fake_client import BW
from medic_client import MedicClient

START_MOVE, CLIMB, END_CLIMB = 97, 103, 104
FINISH_VAULT, FINISH_LADDER = 100, 102


def move_to_target(c, target, callback):
    p = BW(); p.int_max(START_MOVE, fc.PAWN_MAX)
    p.bit(1)                                        # TargetLocation: compressed vector
    bits = 14
    p.int_max(bits, 20)
    for v in target:
        p.int_max(int(v) + (1 << (bits + 1)), 1 << (bits + 2))
    p.bit(1)                                        # TargetRotation: yaw only
    p.bit(0); p.bit(1); p.bitsn(197, 8); p.bit(0)
    p.bit(1)                                        # FinishMoveCallback: the pawn, then the FName
    p.bit(1); p.int_max(fc.OWN_PAWN_CH, 0x800)
    for b in fc.fstring(callback):
        p.bitsn(b, 8)
    p.u32(0)
    p.bit(0)                                        # fMinDistanceToTarget: not sent
    c.reliable(fc.OWN_PAWN_CH, p)


def ladder(c, idx):
    p = BW(); p.int_max(idx, fc.PAWN_MAX)
    p.bit(1); p.bit(0); p.int_max(1, 0x80000000)    # L: a static LadderVolume
    c.reliable(fc.OWN_PAWN_CH, p)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8777)
    a = ap.parse_args()

    c = MedicClient("Climber", a.host, a.port)
    c.handshake(); c.recv_all(1.0)
    threading.Thread(target=lambda: [c.recv_all(0.05) for _ in iter(lambda: c.stop, True)],
                     daemon=True).start()
    c.send_join_team(0)
    time.sleep(1.5)
    c.send_int_rpc(fc.REP_SELECTROLE, 1)
    time.sleep(0.3)
    c.send_ready(True)
    t = time.time() + 30
    while time.time() < t and not c.gave_pawn:
        time.sleep(0.1)
    if not c.gave_pawn:
        print("FAIL: no pawn")
        return 1
    stop = threading.Event()
    def mover():
        ts = 1.0
        while not stop.is_set():
            ts += 0.05
            c.send_move(ts, (2913.0, -1217.0, 399.0), yaw=197 << 8)
            time.sleep(0.05)
    threading.Thread(target=mover, daemon=True).start()
    time.sleep(1.0)

    n = len(c.own_pawn_calls)
    print("ladder: move to the top of the ladder")
    move_to_target(c, (2913, -1217, 399), "FinishLadderMoveToTarget")
    time.sleep(0.6)
    after_move = c.own_pawn_calls[n:]
    ladder(c, CLIMB)
    time.sleep(1.5)
    ladder(c, END_CLIMB)
    time.sleep(0.5)

    n = len(c.own_pawn_calls)
    print("vault")
    move_to_target(c, (2950, -1217, 420), "FinishVaultMantleMoveToTarget")
    time.sleep(0.8)
    after_vault = c.own_pawn_calls[n:]
    stop.set()
    c.stop = True

    print(f"own-pawn RPCs after the ladder move: {after_move}; after the vault: {after_vault}")
    checks = {
        "ladder move -> ClientFinishLadderMoveToTarget": FINISH_LADDER in after_move,
        "ladder move -> no vault finish": FINISH_VAULT not in after_move,
        "vault -> ClientFinishVaultMantleMoveToTarget": FINISH_VAULT in after_vault,
    }
    for k, v in checks.items():
        print(f"  {k}: {'ok' if v else 'FAILED'}")
    ok = all(checks.values())
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
