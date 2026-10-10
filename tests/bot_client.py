#!/usr/bin/env python3
"""Scripted AA3 player for live tests (doc sec 33): joins a team, takes a
role, spawns, equips its rifle like the real client (ServerSetCurrentWeapon
on its InventoryManager), then turns toward the nearest other soldier and
fires semi-auto -- a target that shoots back.

  bot_client.py --port 8777 --team 1 --role 1 [--seconds 600] [--rate 1.0]
  bot_client.py ... --no-fire --goto -6612,2900,40   (walk-in test: report that
                                                      location after --goto-after s)

Built on fake_client.Client (handshake, reliability, acks)."""
import argparse
import math
import threading
import time

import fake_client as fc

INVMGR_MAX = 38                 # AA_InventoryManager MaxIndex
WEAPON_MAX = 71                 # AA_BulletWeapon_M4A1 MaxIndex
SET_CURRENT_WEAPON = 25         # InventoryManager.ServerSetCurrentWeapon(Weapon)
START_FIRE, STOP_FIRE = 31, 32  # Weapon.ServerStartFire / ServerStopFire
# Class default objects relative to Default__AA_Pawn (all in AA3Baseline, so
# the offsets hold under any PackageMap): retail 46464 pawn, 43612 M4A1,
# 52030 InventoryManager.
# Rifles by kit (doc sec 34): default objects (AA3Baseline NetIndex) minus
# Default__AA_Pawn's 8103 -- M4A1 5251, M16A4 5153, M16DMR 5157, M249 5245.
D_WEAPONS = tuple(n - 8103 for n in (5251, 5153, 5157, 5245))
D_INVMGR = 52030 - 46464
EYE = 31.0                      # retail shot origin above the pawn location


def reliable(c, ch, payload):
    chseq = c.next_chseq(ch)
    with c.lock:
        w = fc.BW()
        c.next_seq(w)
        w.bit(0); w.bit(0); w.bit(1)
        w.int_max(ch, 0x800); w.int_max(chseq, 0x400); w.int_max(2, 8)
        w.int_max(len(payload.bits), 0x1000)
        w.extend(payload)
        c.send_bits(w)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8777)
    ap.add_argument("--team", type=int, default=1)
    ap.add_argument("--role", type=int, default=1)
    ap.add_argument("--seconds", type=float, default=600)
    ap.add_argument("--rate", type=float, default=1.0, help="shots per second")
    ap.add_argument("--no-fire", action="store_true")
    ap.add_argument("--shots", type=int, default=0,
                    help="stop after this many shots (0 = no limit); 2 torso hits incapacitate")
    ap.add_argument("--goto", help="x,y,z: report this location in ServerMove (the server "
                    "trusts ClientLoc) -- e.g. inside an extraction volume (doc sec 36)")
    ap.add_argument("--goto-after", type=float, default=5.0, help="seconds after spawning")
    a = ap.parse_args()
    goto = tuple(float(v) for v in a.goto.split(",")) if a.goto else None

    c = fc.Client("Bot", a.host, a.port)
    c.handshake(); c.recv_all(1.0)
    threading.Thread(target=lambda: [c.recv_all(0.05) for _ in iter(lambda: c.stop, True)],
                     daemon=True).start()
    print("connected; joining team", a.team)
    c.send_join_team(a.team)
    time.sleep(1.5)
    c.send_int_rpc(fc.REP_SELECTROLE, a.role)
    time.sleep(0.3)
    c.send_ready(True)

    # Keep the connection alive (moves) while waiting for the round to start.
    ts = 1.0
    deadline = time.time() + 900
    while time.time() < deadline and not (c.gave_pawn and fc.OWN_PAWN_CH in c.ch_class):
        time.sleep(0.2)
    pawn_obj = c.ch_class[fc.OWN_PAWN_CH]
    fc.PAWN_OBJ = pawn_obj
    me = c.open_locs.get(fc.OWN_PAWN_CH)
    print("spawned at", me)

    def channel_of(obj):
        with c.lock:
            return next((ch for ch, o in c.ch_class.items() if o == obj), None)

    weapon_ch = invmgr_ch = None
    t_wait = time.time()
    while (weapon_ch is None or invmgr_ch is None) and time.time() - t_wait < 10:
        weapon_ch = next((ch for ch in (channel_of(pawn_obj + d) for d in D_WEAPONS) if ch), None)
        invmgr_ch = channel_of(pawn_obj + D_INVMGR)
        time.sleep(0.1)
    print("weapon ch", weapon_ch, "inventory manager ch", invmgr_ch)
    p = fc.BW()
    p.int_max(SET_CURRENT_WEAPON, INVMGR_MAX)
    p.bit(1); p.bit(1); p.int_max(weapon_ch, 0x800)       # Send, bDynamic, channel
    reliable(c, invmgr_ch, p)

    def target():
        """Latest location of another soldier (Location updates, else spawn)."""
        with c.lock:
            locs = [loc for ch, loc in c.remote_locs if ch != fc.OWN_PAWN_CH]
            if locs:
                return locs[-1]
            pawns = [ch for ch, o in c.ch_class.items() if o == pawn_obj and ch != fc.OWN_PAWN_CH]
            return c.open_locs.get(pawns[-1]) if pawns else None

    t0 = time.time()
    next_shot = t0 + 8.0                                    # after the initial reload
    yaw = pitch = 0
    shots = 0
    while time.time() - t0 < a.seconds:
        tgt = target()
        if tgt and me:
            dx, dy, dz = tgt[0] - me[0], tgt[1] - me[1], (tgt[2] + 10) - (me[2] + EYE)
            yaw = int(math.atan2(dy, dx) * 32768 / math.pi) & 0xFFFF
            pitch = int(math.atan2(dz, math.hypot(dx, dy)) * 32768 / math.pi) & 0xFFFF
        if goto and time.time() - t0 >= a.goto_after and me != goto:
            print("moving to", goto)
            me = goto
        ts += 0.033
        c.send_move(ts, me or (0, 0, 0), yaw, pitch)
        if not a.no_fire and tgt and time.time() >= next_shot and not (a.shots and shots >= a.shots):
            q = fc.BW(); q.int_max(START_FIRE, WEAPON_MAX); reliable(c, weapon_ch, q)
            time.sleep(0.05)
            q = fc.BW(); q.int_max(STOP_FIRE, WEAPON_MAX); reliable(c, weapon_ch, q)
            next_shot = time.time() + 1.0 / a.rate
            shots += 1
            print(f"fire at {tgt} yaw={yaw} pitch={pitch}")
        time.sleep(0.033)
    c.stop = True


if __name__ == "__main__":
    main()
