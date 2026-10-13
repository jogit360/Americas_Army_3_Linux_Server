#!/usr/bin/env python3
"""Enemy NPC soldiers on aa3_server, Bridge AI (Bridge_inf_co_large_ai;
retail capture 2026-10-12), on medic_client.py.

A Black soldier spawns; the server's enemy NPCs (AA_NPCEnemyPawn, CDO
21100) open on its connection. The soldier steps out near one NPC
(trying a few spots around it until the NPC can see it), draws its rifle
and, after the initial reload, fires at the NPC's head.

PASS requires: NPC pawn channels opened to the player; the NPC shooting the
player (the player's pawn gets hurt -- the server log's "<NPC> hit
Player1", checked here through the PC's ClientSendHitNotification, 223);
and the player's ClientNotifyNeutralizedNME (406) after killing it.

Run the server on Bridge_inf_co_large_ai with short round timers.
"""
import argparse
import math
import sys
import threading
import time

import fake_client as fc
from medic_client import MedicClient, REP_STARTFIRE, REP_STOPFIRE
from mission_bot import Bot
from medic_scenario import RIFLES, find_base
from grenade_client import CDO_IM

CDO_NPC = 21100
PC_HIT, PC_NEUTRALIZED = 223, 406


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8777)
    a = ap.parse_args()

    c = Bot("Soldier", a.host, a.port)            # tracks NPCs' live Location (they move now)
    c.handshake(); c.recv_all(1.0)
    threading.Thread(target=lambda: [c.recv_all(0.05) for _ in iter(lambda: c.stop, True)], daemon=True).start()
    c.send_join_team(0)
    time.sleep(1.5)
    c.send_int_rpc(fc.REP_SELECTROLE, 1)
    time.sleep(0.3)
    c.send_ready(True)
    t = time.time() + 30
    while time.time() < t and not c.gave_pawn:
        time.sleep(0.1)
    time.sleep(1.5)
    if not find_base(c):
        print("FAIL: no pawn")
        return 1
    with c.lock:
        npcs = {ch: c.open_locs.get(ch) for ch, o in c.ch_class.items() if o == c.base + CDO_NPC}
    print(f"NPC pawns open: {len(npcs)}")
    if not npcs:
        print("FAIL: no NPCs")
        return 1
    with c.lock:
        me0 = c.open_locs.get(fc.OWN_PAWN_CH)
    # The NPC nearest the player's spawn.
    ch, npc = min(npcs.items(), key=lambda kv: math.dist(kv[1][:2], me0[:2]))
    print(f"target NPC ch {ch} at {npc}")

    state = {"pos": (npc[0] + 500.0, npc[1], npc[2]), "yaw": 32768, "pitch": 0}
    stop = threading.Event()
    def mover():
        ts = 1.0
        while not stop.is_set():
            ts += 0.05
            c.send_move(ts, state["pos"], yaw=state["yaw"], pitch=state["pitch"] & 0xFFFF)
            time.sleep(0.05)
    threading.Thread(target=mover, daemon=True).start()
    rifle = next(((rch, mx) for cdo, mx in RIFLES.items() for rch in c.channels_of(cdo)), None)
    im = c.channels_of(CDO_IM)
    n0 = len(c.pc_calls)          # hits taken count from here
    if rifle and im:
        c.set_current_weapon(im[0], rifle[0])
    time.sleep(8.0)               # the initial reload: no damage taken before it ends (doc sec 34)

    # Step to spots around the NPC; at each, aim at its head and fire once.
    # Being shot (by it or its neighbours) is recorded along the way.
    neutralized = False
    for ang in range(0, 360, 30):
        if not rifle:
            break
        r = math.radians(ang)
        state["pos"] = (npc[0] + 450 * math.cos(r), npc[1] + 450 * math.sin(r), npc[2])
        with c.lock:
            now_at = c.npc_loc.get(ch, npc)              # where it is now
        state["pos"] = (now_at[0] + 450 * math.cos(r), now_at[1] + 450 * math.sin(r), now_at[2])
        dx, dy = now_at[0] - state["pos"][0], now_at[1] - state["pos"][1]
        dz = (now_at[2] + 22) - (state["pos"][2] + 31)  # head, standing or crouched
        state["yaw"] = int(math.atan2(dy, dx) * 32768 / math.pi) & 0xFFFF
        state["pitch"] = int(math.atan2(dz, math.hypot(dx, dy)) * 32768 / math.pi)
        time.sleep(0.3)
        n = len(c.pc_calls)
        c.call(rifle[0], rifle[1], REP_STARTFIRE); time.sleep(0.1)
        c.call(rifle[0], rifle[1], REP_STOPFIRE); time.sleep(1.2)
        if PC_NEUTRALIZED in c.pc_calls[n:]:
            print(f"neutralized it from angle {ang}")
            neutralized = True
            break
    # Being shot: stand within 400 units of the other NPCs in turn (all-round
    # awareness that close) and give each a while -- they react slower and
    # miss more than they used to.
    if PC_HIT not in c.pc_calls[n0:]:
        with c.lock:
            others = [(ch2, l) for ch2, o in c.ch_class.items() if o == c.base + CDO_NPC and ch2 != ch
                      for l in [c.open_locs.get(ch2)] if l]
        others.sort(key=lambda kv: math.dist(kv[1][:2], state["pos"][:2]))
        for ch2, l in others[:4]:
            for ang in (0, 90, 180, 270):
                r = math.radians(ang)
                state["pos"] = (l[0] + 400 * math.cos(r), l[1] + 400 * math.sin(r), l[2])
                t = time.time() + 6
                while time.time() < t and PC_HIT not in c.pc_calls[n0:]:
                    time.sleep(0.5)
                if PC_HIT in c.pc_calls[n0:]:
                    break
            if PC_HIT in c.pc_calls[n0:]:
                break
    hit = PC_HIT in c.pc_calls[n0:]
    stop.set()
    c.stop = True
    checks = {"NPCs opened": bool(npcs), "NPC shot the player": hit, "player neutralized the NPC": neutralized}
    for k, v in checks.items():
        print(f"  {k}: {'ok' if v else 'FAILED'}")
    ok = all(checks.values())
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
