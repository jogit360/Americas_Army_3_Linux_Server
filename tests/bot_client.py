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
REP_ASKFORPAWN = 42             # PlayerController.AskForPawn()
REP_ACKPOSSESSION = 44          # PlayerController.ServerAcknowledgePossession(P)
D_M67 = 33082 - 8103            # Default__AA_ThrowWeapon_M67 relative to Default__AA_Pawn
THROW_MAX = 66                  # AA_ThrowWeapon MaxIndex
COOK_IT = 59                    # AA_ThrowWeapon.CookIt()
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


REP_LEVEL_VISIBILITY = 100      # PlayerController.ServerUpdateLevelVisibility(PackageName, bIsVisible)


def pc_call(c, rep_index, build=None):
    p = fc.BW(); p.int_max(rep_index, fc.PC_MAX)
    if build:
        build(p)
    reliable(c, fc.PC_CH, p)


def retail_main(a, c):
    """A soldier on a retail server for as long as --seconds, round after
    round, the way the real client plays it: confirm level streaming
    (ServerUpdateLevelVisibility -- without it the server drops us at the next
    round's re-stream), pick the role and ready at each role selection,
    AskForPawn until given one, acknowledge it, and keep walking (retail
    kills soldiers that stand idle). No shooting."""
    c.send_join_team(a.team)
    t_end = time.time() + a.seconds
    t_join = time.time()
    picks_done = 0
    pick_at = None
    ask_until_pawn = False
    next_ask = 0.0
    pawn_ch = None
    me = None
    t_spawn = 0.0
    equip_at = None
    ts = 1.0
    while time.time() < t_end:
        now = time.time()
        with c.lock:
            pend, c.pending_visibility = c.pending_visibility, []
            selections = sum(1 for _, n in c.phase_rpcs if n == "EnterPrePlanningTool") + c.role_select_open
            got = c.own_pawn_ch if c.gave_pawn and c.own_pawn_ch in c.ch_class else None
        for name, visible in pend:
            def build(p, name=name, visible=visible):
                p.bit(1)                                    # PackageName: Send, AA3 FName
                for b in fc.fstring(name):
                    p.bitsn(b, 8)
                p.u32(0)                                    #   Number
                p.bit(visible)                              # bIsVisible
            pc_call(c, REP_LEVEL_VISIBILITY, build)
        # Role pick + ready for each role selection (or 5 s after joining if
        # none came, e.g. joining a round in progress).
        if selections > picks_done or (picks_done == 0 and pick_at is None and now - t_join > 5.0):
            picks_done = max(selections, 1)
            pick_at = now + 0.5
        if pick_at is not None and now >= pick_at:
            pick_at = None
            c.send_int_rpc(fc.REP_SELECTROLE, a.role)
            time.sleep(0.3)
            c.send_ready(True)
            ask_until_pawn = True
            print(f"picked role {a.role}, ready")
        if ask_until_pawn and not got and now >= next_ask:
            pc_call(c, REP_ASKFORPAWN)
            next_ask = now + 2.0
        if got and got != pawn_ch:
            pawn_ch = got
            ask_until_pawn = False
            me = c.open_locs.get(pawn_ch)
            t_spawn = now
            pc_call(c, REP_ACKPOSSESSION, lambda p: (p.bit(1), p.bit(1), p.int_max(pawn_ch, 0x800)))
            print("spawned at", me)
            equip_at = now + 1.0
        # Draw the rifle (InventoryManager.ServerSetCurrentWeapon): the
        # initial reload it starts is what sets AA_Pawn.bFirstReloadFinished,
        # and until then retail's TakeDamage ignores every hit ("no damage
        # during initial reload to avoid spawn campers").
        if got and equip_at and now >= equip_at:
            with c.lock:
                objs = dict(c.ch_class)
            pawn_obj = objs.get(got)
            ch_of = {o: ch for ch, o in objs.items()}
            weapon_ch = next((ch_of[pawn_obj + d] for d in D_WEAPONS if pawn_obj and pawn_obj + d in ch_of), None)
            invmgr_ch = ch_of.get(pawn_obj + D_INVMGR) if pawn_obj else None
            if weapon_ch and invmgr_ch:
                q = fc.BW(); q.int_max(SET_CURRENT_WEAPON, INVMGR_MAX)
                q.bit(1); q.bit(1); q.int_max(weapon_ch, 0x800)
                reliable(c, invmgr_ch, q)
                print(f"rifle drawn (ch {weapon_ch})")
                equip_at = None
            elif now - t_spawn > 15.0:
                print("no rifle found to draw")
                equip_at = None
            else:
                equip_at = now + 0.5
        if not got and pawn_ch is not None:
            print("lost soldier (died or round over)")
            pawn_ch = None
            me = None
        ts += 0.033
        accel = None
        if me and a.wander:
            dirx = 1.0 if int((now - t_spawn) / 2.0) % 2 == 0 else -1.0
            accel = (dirx * 1024.0, 0.0, 0.0)
            me = (me[0] + dirx * 150.0 * 0.033, me[1], me[2])
        c.send_move(ts, me or (0, 0, 0), 0, 0, accel)
        time.sleep(0.033)
    c.stop = True


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
    ap.add_argument("--cookoff-after", type=float, default=0.0, metavar="SEC",
                    help="SEC after spawning: take out the M67, pull the pin, drop the spoon "
                         "(CookIt) and hold it until it goes off in hand")
    ap.add_argument("--wander", action="store_true",
                    help="walk back and forth near the spawn (a retail server kills idle soldiers)")
    ap.add_argument("--retail", metavar="NAME",
                    help="log in the way the real client does (a retail AA3Game server), as NAME")
    a = ap.parse_args()
    goto = tuple(float(v) for v in a.goto.split(",")) if a.goto else None

    c = fc.Client("Bot", a.host, a.port)
    if a.retail:
        if not c.retail_handshake(a.retail):
            print("retail login failed; control messages:", c.control_msgs[:5])
            return
        print(f"logged in as {a.retail}: {len([m for m in c.control_msgs if m.startswith('USES')])} packages")
        c.recv_all(1.0)
    else:
        c.handshake(); c.recv_all(1.0)
    threading.Thread(target=lambda: [c.recv_all(0.05) for _ in iter(lambda: c.stop, True)],
                     daemon=True).start()
    print("connected; joining team", a.team)
    if a.retail:
        retail_main(a, c)
        return
    c.send_join_team(a.team)
    if a.retail:
        # The real client's order (retail GamePlayPCAP_Legit_Server): the role
        # pick waits for role selection (EnterPrePlanningTool); a pick before
        # it lands out of bounds on the retail server.
        t_wait = time.time()
        while time.time() - t_wait < 600 and not any(n == "EnterPrePlanningTool" for _, n in c.phase_rpcs):
            time.sleep(0.2)
        time.sleep(0.5)
    else:
        time.sleep(1.5)
    c.send_int_rpc(fc.REP_SELECTROLE, a.role)
    time.sleep(0.3)
    c.send_ready(True)
    if a.retail:
        # ...then PlayerController.AskForPawn() until the server gives one.
        def ask():
            while not c.gave_pawn and not c.stop:
                p = fc.BW(); p.int_max(REP_ASKFORPAWN, fc.PC_MAX); reliable(c, fc.PC_CH, p)
                time.sleep(2.0)
        threading.Thread(target=ask, daemon=True).start()

    # Keep the connection alive (moves) while waiting for the round to start.
    ts = 1.0
    deadline = time.time() + 900
    while time.time() < deadline and not (c.gave_pawn and c.own_pawn_ch in c.ch_class):
        time.sleep(0.2)
    own_ch = c.own_pawn_ch
    pawn_obj = c.ch_class[own_ch]
    fc.PAWN_OBJ = pawn_obj
    me = c.open_locs.get(own_ch)
    print("spawned at", me)
    # PlayerController.ServerAcknowledgePossession(P): the real client's
    # reply to getting a pawn (retail capture, after AskForPawn).
    p = fc.BW(); p.int_max(REP_ACKPOSSESSION, fc.PC_MAX)
    p.bit(1); p.bit(1); p.int_max(own_ch, 0x800)          # Send, bDynamic, channel
    reliable(c, fc.PC_CH, p)

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
    if weapon_ch and invmgr_ch:
        p = fc.BW()
        p.int_max(SET_CURRENT_WEAPON, INVMGR_MAX)
        p.bit(1); p.bit(1); p.int_max(weapon_ch, 0x800)   # Send, bDynamic, channel
        reliable(c, invmgr_ch, p)
    else:
        print("no rifle found: standing without firing")
        a.no_fire = True

    def target():
        """Latest location of another soldier (Location updates, else spawn)."""
        with c.lock:
            locs = [loc for ch, loc in c.remote_locs if ch != own_ch]
            if locs:
                return locs[-1]
            pawns = [ch for ch, o in c.ch_class.items() if o == pawn_obj and ch != own_ch]
            return c.open_locs.get(pawns[-1]) if pawns else None

    def cook_off():
        """Hold a cooked grenade: out, pin pulled, spoon dropped, never thrown."""
        m67_ch = channel_of(pawn_obj + D_M67)
        if not (m67_ch and invmgr_ch):
            print("cook-off: no M67 found")
            return
        print(f"cook-off: taking out M67 (ch {m67_ch})")
        q = fc.BW(); q.int_max(SET_CURRENT_WEAPON, INVMGR_MAX)
        q.bit(1); q.bit(1); q.int_max(m67_ch, 0x800)
        reliable(c, invmgr_ch, q)
        time.sleep(1.5)                                     # in hand (retail +0.65 s)
        q = fc.BW(); q.int_max(START_FIRE, THROW_MAX); reliable(c, m67_ch, q)
        print("cook-off: pin pulled")
        time.sleep(0.8)
        q = fc.BW(); q.int_max(COOK_IT, THROW_MAX); reliable(c, m67_ch, q)
        print("cook-off: spoon dropped; holding it")
    if a.cookoff_after > 0:
        a.no_fire = True
        threading.Timer(a.cookoff_after, cook_off).start()

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
        accel = None
        if a.wander and me:
            # 2 s one way, 2 s back along X at walking acceleration; the server
            # moves the pawn from InAccel, ClientLoc follows our estimate.
            phase = int((time.time() - t0) / 2.0) % 2
            dirx = 1.0 if phase == 0 else -1.0
            accel = (dirx * 1024.0, 0.0, 0.0)
            me = (me[0] + dirx * 150.0 * 0.033, me[1], me[2])
        c.send_move(ts, me or (0, 0, 0), yaw, pitch, accel)
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
