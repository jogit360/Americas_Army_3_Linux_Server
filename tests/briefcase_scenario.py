#!/usr/bin/env python3
"""Carried objective (the briefcase) on aa3_server, Bridge AI
(Bridge_inf_co_large_ai; retail capture 2026-10-12), on medic_client.py.

  Carrier (Black) walks to the briefcase's factory and picks it up:
    ServerRepExec('Pickup', factory) + ServerUse.
  Enemy (Gold) shoots the carrier in the head -> incapacitated (the server
    says ClientNotifyIncapacitatedWithObjective); the downed carrier hands
    the case over: ServerDropObjective -> it drops beside him; then he
    withdraws (ServerSuicide).
  The dead carrier presses F5 (ServerRequestRespawn): queued, deployed by
    the next reinforcement wave (<= 10 s) at a Black respawn point.
  Mate (Black) picks the dropped case up (ServerRepExec('Pickup', its
    channel)) and walks into Zulu, Black's extraction zone -> Black wins.

PASS requires: the carrier's SetNotificationMessage (265) and
ReceiveLocalizedMessage (46) at the pickup; the mate seeing the briefcase
re-sent (closed and opened again) and an AA_DroppedObjective open after the
kill; the carrier's ClientSendObjectiveDroppedNotification (225); the carrier's
reinforcement messages (ClientMessage, 52) and a new pawn near a Black
respawn point; the round won by extraction (server log, checked by the
caller).

Run the server on Bridge_inf_co_large_ai (MapName) with short round timers.
"""
import argparse
import sys
import threading
import time

import fake_client as fc
from fake_client import BW
from medic_client import MedicClient, REP_STARTFIRE, REP_STOPFIRE, draw_rifle
from medic_scenario import RIFLES, find_base
from grenade_client import CDO_IM

REP_REPEXEC, REP_USE, REP_SUICIDE, REP_RESPAWN = 189, 76, 77, 252
REP_DROP_OBJECTIVE, PC_INCAP_WITH_OBJECTIVE = 239, 240
PC_CLIENT_MESSAGE = 52
BLACK_RESPAWN = (-7735.0, 1886.0, 48.0)   # the first of Black's AA_MCPlayerRespawnMarkers
CDO_MARKER, CDO_BRIEFCASE, CDO_DROPPED = 15660, 26202, 7016
FACTORY = (2905.0, -739.0, 25.0)
FIGHT_CARRIER, FIGHT_ENEMY = (11585.0, -3308.0, 65.0), (11385.0, -3308.0, 65.0)
ZULU = (11860.0, -745.0, 30.0)            # inside Black's extraction box
PC_NOTIFY, PC_LOCALIZED, PC_DROPPED = 265, 46, 225


def pickup(c, static_obj=None, ch=None):
    p = BW(); p.int_max(REP_REPEXEC, fc.PC_MAX)
    p.bit(1)                                        # ExecAction: FName 'Pickup'
    for b in fc.fstring("Pickup"):
        p.bitsn(b, 8)
    p.u32(0)
    p.bit(1)                                        # ContextActor
    if ch is not None:
        p.bit(1); p.int_max(ch, 0x800)
    else:
        p.bit(0); p.int_max(static_obj, 0x80000000)
    c.reliable(fc.PC_CH, p)
    c.pc_call(REP_USE)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8777)
    a = ap.parse_args()

    carrier = MedicClient("Carrier", a.host, a.port)
    mate = MedicClient("Mate", a.host, a.port)
    enemy = MedicClient("Enemy", a.host, a.port)
    bots = (carrier, mate, enemy)
    for c in bots:
        c.handshake(); c.recv_all(1.0)
    for c in bots:
        threading.Thread(target=lambda c=c: [c.recv_all(0.05) for _ in iter(lambda: c.stop, True)],
                         daemon=True).start()
    carrier.send_join_team(0); mate.send_join_team(0); enemy.send_join_team(1)
    time.sleep(1.5)
    carrier.send_int_rpc(fc.REP_SELECTROLE, 1); mate.send_int_rpc(fc.REP_SELECTROLE, 2)
    enemy.send_int_rpc(fc.REP_SELECTROLE, 1)
    time.sleep(0.3)
    for c in bots:
        c.send_ready(True)
    t = time.time() + 30
    while time.time() < t and not all(c.gave_pawn for c in bots):
        time.sleep(0.1)
    time.sleep(0.8)
    if not all(find_base(c) for c in bots):
        print("FAIL: no pawns")
        return 1

    with carrier.lock:
        by_obj = {o: ch for ch, o in carrier.ch_class.items()}
        objs = dict(carrier.ch_class)
    marker_ch = by_obj.get(carrier.base + CDO_MARKER)
    factory_obj = objs.get(marker_ch - 1) if marker_ch else None
    print("marker ch", marker_ch, "factory obj", factory_obj)
    if not factory_obj:
        print("FAIL: no objective marker / factory")
        return 1

    pos = {"carrier": FACTORY, "mate": (2905.0, -600.0, 25.0), "enemy": FIGHT_ENEMY}
    aim = {"pitch": 0}
    stop = threading.Event()
    def mover():
        ts = 1.0
        while not stop.is_set():
            ts += 0.05
            carrier.send_move(ts, pos["carrier"], yaw=32768)
            mate.send_move(ts, pos["mate"], yaw=0)
            enemy.send_move(ts, pos["enemy"], yaw=0, pitch=aim["pitch"] & 0xFFFF)
            time.sleep(0.05)
    threading.Thread(target=mover, daemon=True).start()
    rifle = next(((ch, mx) for cdo, mx in RIFLES.items() for ch in enemy.channels_of(cdo)), None)
    im = enemy.channels_of(CDO_IM)
    if rifle and im:
        enemy.set_current_weapon(im[0], rifle[0])
    draw_rifle(carrier); draw_rifle(mate)                  # damage immunity ends with the initial reload
    time.sleep(1.0)

    def briefcase_chs(c):
        with c.lock:
            return sorted(ch for ch, o in c.ch_class.items() if o == c.base + CDO_BRIEFCASE)
    before = briefcase_chs(mate)
    n = len(carrier.pc_calls)
    print("carrier: pick up the briefcase")
    pickup(carrier, static_obj=factory_obj)
    time.sleep(1.0)
    pickup_calls = carrier.pc_calls[n:]
    after = briefcase_chs(mate)
    print(f"mate's briefcase channels: before {before}, after {after}; carrier PC calls {pickup_calls[:8]}")

    print("carrier to the fight; enemy shoots after its reload")
    pos["carrier"] = FIGHT_CARRIER
    time.sleep(7.0)
    if rifle:
        aim["pitch"] = 469                                          # head (as medic_scenario)
        time.sleep(0.4)
        enemy.call(rifle[0], rifle[1], REP_STARTFIRE); time.sleep(0.1)
        enemy.call(rifle[0], rifle[1], REP_STOPFIRE)
    time.sleep(1.5)
    incap_calls = list(carrier.pc_calls)
    n = len(carrier.pc_calls)
    p = BW(); p.int_max(REP_DROP_OBJECTIVE, fc.PC_MAX); p.bit(0)    # bDebugDropping False
    carrier.reliable(fc.PC_CH, p)                                   # hand the case over
    time.sleep(1.0)
    drop_calls = carrier.pc_calls[n:]
    carrier.pc_call(REP_SUICIDE)                                    # then withdraw
    time.sleep(1.0)
    with mate.lock:
        dropped = [ch for ch, o in mate.ch_class.items() if o == mate.base + CDO_DROPPED]
    print(f"mate sees dropped case on {dropped}; carrier PC calls {drop_calls[:8]}")

    print("carrier: F5 (request respawn)")
    with carrier.lock:
        carrier.gave_pawn = False
    n = len(carrier.pc_calls)
    carrier.pc_call(REP_RESPAWN)
    t = time.time() + 15
    while time.time() < t and not carrier.gave_pawn:
        time.sleep(0.2)
    respawn_calls = carrier.pc_calls[n:]
    with carrier.lock:
        back_at = carrier.open_locs.get(fc.OWN_PAWN_CH) if carrier.gave_pawn else None
    print(f"carrier back: {back_at}; ClientMessages {respawn_calls.count(PC_CLIENT_MESSAGE)}")
    if back_at:
        pos["carrier"] = tuple(float(v) for v in back_at)

    if dropped:
        print("mate: pick up the dropped case, walk into Zulu")
        pos["mate"] = FIGHT_CARRIER
        time.sleep(0.5)
        pickup(mate, ch=dropped[0])
        time.sleep(1.0)
        pos["mate"] = ZULU
        time.sleep(3.0)
    stop.set()
    for c in bots:
        c.stop = True

    checks = {
        "carrier: pickup notices": PC_NOTIFY in pickup_calls and PC_LOCALIZED in pickup_calls,
        "mate: briefcase re-sent": bool(before) and bool(after) and after != before,
        "mate: dropped case opened": bool(dropped),
        "carrier: told he can drop it (down)": PC_INCAP_WITH_OBJECTIVE in incap_calls,
        "carrier: dropped notice": PC_DROPPED in drop_calls,
        "carrier: reinforcement messages": respawn_calls.count(PC_CLIENT_MESSAGE) >= 1,
        "carrier: respawned at a Black respawn point": bool(back_at) and
            abs(back_at[0] - BLACK_RESPAWN[0]) < 600 and abs(back_at[1] - BLACK_RESPAWN[1]) < 600,
    }
    for k, v in checks.items():
        print(f"  {k}: {'ok' if v else 'FAILED'}")
    ok = all(checks.values())
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
