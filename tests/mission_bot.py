#!/usr/bin/env python3
"""A soldier that plays Bridge AI (Bridge_inf_co_large_ai) start to finish,
against a retail AA3Game server (--retail NAME) or aa3_server.

  spawn      join Black, take a role (default 10: the DMR with the ACOG,
             as the real client did on retail), draw the rifle; raise and
             lower the sights once (the replies are logged);
  move       ServerMove at ~30 Hz with the real client's input: InAccel of
             magnitude 7680 (the client sends Acceleration * 10) toward the
             next waypoint, ClientLoc = our prediction, corrected by every
             Client*AdjustPosition; routes from aa3_navgrid (the bundle's
             collision: floors, stairs, crouch passages, doors);
  fight      enemy NPCs (AA_NPCEnemyPawn) within --engage units: stop, aim
             at the head, fire semi-auto, reload after 25; give up on one
             after --give-up shots (no line of sight);
  mission    to the briefcase at Yankee, ServerRepExec('Pickup', factory),
             to Zulu (Black's extraction) until the round ends;
  death      F5 (ServerRequestRespawn) until a new soldier, then carry on.

Everything that matters is logged with timestamps; the run ends with a
summary (kills, deaths, pickup, extraction).
"""
import argparse
import json
import math
import subprocess
import sys
import threading
import time

import fake_client as fc
from fake_client import BW
from grenade_client import WeaponsClient, CDO_IM, CDO_M67

T0 = time.time()
CDO_NPC = 21100                 # Default__AA_NPCEnemyPawn (AA3Baseline NetIndex)
NPC_MAX = 139
RIFLES = {5157: 71, 5251: 71, 5153: 71, 5245: 72}    # DMR, M4A1, M16A4, M249 -> MaxIndex
GRENADES = (CDO_M67, 33062, 33088, 33070, 33037)
REP_STARTFIRE, REP_STOPFIRE = 31, 32
REP_RAISE, REP_LOWER = 170, 171
REP_REPEXEC, REP_USE, REP_RESPAWN = 189, 76, 252
REP_ASKFORPAWN, REP_ACKPOSSESSION, REP_LEVEL_VISIBILITY = 42, 44, 100
PAWN_TRYRELOAD = 124
FACTORY = (2905.0, -739.0, 25.0)
PICKUP_SPOT = (2960.0, -739.0, -20.0)       # beside the briefcase's table, on the ground
ZULU_GROUND = (11860.0, -745.0, -20.0)
RETAIL_FACTORY_OBJ = 212307
ZULU = (11860.0, -745.0, 30.0)
ZULU_BOX = ((11671, -1071), (12056, -419))
XRAY_GROUND = (-6120.0, -896.0, -20.0)
XRAY_BOX = ((-6299, -1192), (-5941, -599))
ACCEL = 7680.0                  # |InAccel| the real client sends while walking
SPEED = 200.0                   # walking GroundSpeed (retail 200)
SPRINT_SPEED = 316.0            # sprinting (retail GroundSpeed 316)
REP_HOLDSPRINT, REP_STOPSPRINT = 182, 187
EYE = 31.0


def log(*a):
    print(f"[{time.time() - T0:7.2f}]", *a, flush=True)


class Bot(WeaponsClient):
    def __init__(self, *a):
        super().__init__(*a)
        self.pc_events = []         # (t, name, val)
        self.adjusted = None
        self.npc_loc = {}           # NPC channel -> latest location
        self.decoded = 0

    # Every PC call (both servers, via decode_rep), adjustments separately.
    def parse_pc_retail(self, data, start, bdb):
        dr = fc._decode_rep()
        if dr is not None:
            r = dr.BitReader(data); r.pos = start
            try:
                for _, _, name, val in dr.decode_fields(dr.Decoder(r), "AGP_PlayerController", start + bdb):
                    short = name.rsplit(".", 1)[-1]
                    if short.endswith("ClientAdjustPosition()") and isinstance(val, dict) and "NewLocX" in val:
                        with self.lock:
                            self.adjusted = (val["NewLocX"], val["NewLocY"], val["NewLocZ"])
                        continue
                    if short.endswith("ClientAdjustPosition()") and isinstance(val, dict) and "NewLoc" in val:
                        with self.lock:
                            self.adjusted = tuple(val["NewLoc"])
                        continue
                    if short in ("ClientAckGoodMove()", "ClientHearSound()", "NotifyPostureChange()",
                                 "NotifyTacticalCombatPostureChange()", "ClientRemovePlayerFromScoreboard()",
                                 "ClientNotifyHiddenAchievementProgress()", "ClientUpdateScoreboard()"):
                        continue
                    with self.lock:
                        self.pc_events.append((round(time.time() - T0, 2), short, val))
            except Exception:
                pass
        super().parse_pc_retail(data, start, bdb)

    def deliver(self, ch, b_open, b_close, payload):
        # NPC movement: Location (22) / Velocity (4) vectors, Rotation (21).
        if not b_open and payload and self.base is not None:
            with self.lock:
                obj = self.ch_class.get(ch)
            if obj == self.base + CDO_NPC:
                w = BW(); w.bits = payload
                r = fc.BR(bytes(w.packet()), len(payload))
                try:
                    while r.pos < r.end:
                        idx = r.int_max(NPC_MAX)
                        if idx in (22, 4):
                            v = r.vec()
                            if idx == 22:
                                with self.lock:
                                    self.npc_loc[ch] = v
                        elif idx == 21:
                            for _ in range(3):
                                if r.bit():
                                    r.bitsn(8)
                        else:
                            break
                except IndexError:
                    pass
        if b_close:
            with self.lock:
                self.npc_loc.pop(ch, None)
        super().deliver(ch, b_open, b_close, payload)

    def pc(self, idx, build=None):
        p = BW(); p.int_max(idx, fc.PC_MAX)
        if build:
            build(p)
        self.reliable(fc.PC_CH, p)

    def events_since(self, n):
        with self.lock:
            return list(self.pc_events[n:])


class Router:
    """aa3_navgrid --serve: one route per query."""
    def __init__(self, tool, bundle):
        self.p = subprocess.Popen([tool, bundle, "--serve"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL, text=True, bufsize=1)

    def block(self, p, r=48):
        self.p.stdin.write(f"block {p[0]:.0f} {p[1]:.0f} {p[2]:.0f} {r:.0f}\n")
        self.p.stdin.flush()
        while True:
            line = self.p.stdout.readline()
            if not line or line.strip() == "ok":
                return

    def route(self, src, dst):
        self.p.stdin.write(f"{src[0]:.0f} {src[1]:.0f} {src[2] - 48:.0f} {dst[0]:.0f} {dst[1]:.0f} {dst[2]:.0f}\n")
        self.p.stdin.flush()
        while True:
            line = self.p.stdout.readline()
            if not line:
                return []
            if line.startswith("[[") or line.strip() == "[]":    # (log lines start "[2026-")
                return [tuple(v) for v in json.loads(line)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8777)
    ap.add_argument("--retail", metavar="NAME", help="log in like the real client (a retail server)")
    ap.add_argument("--role", type=int, default=10)
    ap.add_argument("--navgrid", default="../build/aa3_navgrid")
    ap.add_argument("--bundle", default="../data/maps/Bridge_inf_co_large_ai.aa3map.zst")
    ap.add_argument("--engage", type=float, default=1200.0)
    ap.add_argument("--give-up", type=int, default=4)
    ap.add_argument("--minutes", type=float, default=15.0)
    a = ap.parse_args()
    router = Router(a.navgrid, a.bundle)        # builds while we log in (~25 s)

    c = Bot("MissionBot", a.host, a.port)
    if a.retail:
        if not c.retail_handshake(a.retail):
            log("retail login failed", c.control_msgs[:3]); return 1
    else:
        c.handshake()
        c.retail = True            # decode every PC call the same way on both servers
    c.recv_all(1.0)
    threading.Thread(target=lambda: [c.recv_all(0.05) for _ in iter(lambda: c.stop, True)], daemon=True).start()
    log("logged in; joining Black")
    c.send_join_team(0)

    stats = {"kills": 0, "deaths": 0, "shots": 0, "pickup": False, "extracted": False, "round_end": None}
    state = {"phase": "spawn", "me": None, "pawn_ch": None, "yaw": 0, "pitch": 0, "path": [], "ts": 1.0,
             "best": 1e9, "best_at": 0.0}
    t_end = time.time() + a.minutes * 60
    t_join = time.time()
    picked = False
    next_ask = next_f5 = 0.0
    rifle = im = None
    rounds_in_mag = 0
    reload_until = equip_at = 0.0
    target = None; shots_at_target = 0; ignore = {}
    next_shot = 0.0
    sights_done = False
    seen_events = 0
    last_tick = time.time()

    def tick_move(accel_dir):
        nonlocal last_tick
        now = time.time()
        dt = min(0.1, now - last_tick); last_tick = now
        state["ts"] += dt
        with c.lock:
            adj_loc, c.adjusted = c.adjusted, None
        if adj_loc and state["me"]:
            state["me"] = tuple(float(v) for v in adj_loc)
        if not state["me"]:
            return                    # no soldier yet: the real client sends no moves
        me = state["me"]
        accel = None
        if accel_dir and state["me"]:
            ux, uy = accel_dir
            v = SPRINT_SPEED if state.get("sprinting") else SPEED
            me = (me[0] + ux * v * dt, me[1] + uy * v * dt, me[2])
            state["me"] = me
            accel = (ux * ACCEL, uy * ACCEL, 0.0)
        c.send_move(state["ts"], me, state["yaw"], state["pitch"] & 0xFFFF, accel)

    while time.time() < t_end:
        now = time.time()
        with c.lock:
            pend, c.pending_visibility = c.pending_visibility, []
            selections = sum(1 for _, n in c.phase_rpcs if n == "EnterPrePlanningTool") + c.role_select_open
            got = c.own_pawn_ch if c.gave_pawn and c.own_pawn_ch in c.ch_class else None
        for name, visible in pend:
            def build(p, name=name, visible=visible):
                p.bit(1)
                for b in fc.fstring(name):
                    p.bitsn(b, 8)
                p.u32(0); p.bit(visible)
            c.pc(REP_LEVEL_VISIBILITY, build)
        # Notable PC events.
        for t, name, val in c.events_since(seen_events):
            seen_events += 1
            if name == "ClientNotifyNeutralizedNME()":
                stats["kills"] += 1
                log(f"NEUTRALIZED {val.get('sOtherPlayer') if isinstance(val, dict) else val} (kills {stats['kills']})")
                target = None
            elif name in ("ClientGameEnded()", "ClientNotifyPawnDied()", "SetNotificationMessage()",
                          "ReceiveLocalizedMessage()", "ClientSendObjectiveDroppedNotification()",
                          "ClientMessage()", "ClientNotifyIncapacitated()", "ClientSendHitNotification()",
                          "ClientDisplayScoreboard()", "ClientRejectRaiseSights()", "ClientRaiseSights()",
                          "ClientLowerSights()") or "Sight" in name:
                log(f"PC {name} {str(val)[:150]}")
                if name == "ClientGameEnded()" and state["phase"] == "zulu":
                    stats["round_end"] = val            # the round we carried the case in
                if name == "ClientNotifyIncapacitated()" and isinstance(val, dict) and val.get("bIncapacitateStart"):
                    state["withdraw_at"] = time.time() + 2.0       # down: withdraw, then F5
                if name == "SetNotificationMessage()" and "picked up" in str(val):
                    stats["pickup"] = True
        if stats["round_end"] is not None:
            # Retail tells the extractor bIsWinner=False even on a win; the
            # extraction is ours if we were in our zone when the round ended.
            zc = state["zone"][0]
            stats["extracted"] = bool(me := state["me"]) and math.dist(me[:2], zc[:2]) < 450
            log(f"ROUND OVER while carrying: {'WON (extracted)' if stats['extracted'] else 'lost'}")
            if stats["extracted"]:
                break
            stats["round_end"] = None; stats["pickup"] = False      # next round, from the start
            state["phase"] = "spawn"; state["path"] = []
        if state.get("withdraw_at") and now >= state["withdraw_at"]:
            state["withdraw_at"] = None
            c.pc(77)                                            # ServerSuicide: withdraw (MEDEVAC)
            log("withdrawing (down)")
        # Role pick / ready / pawn requests (retail_main's flow).
        if not picked and (selections > 0 or now - t_join > 5.0):
            picked = True
            time.sleep(0.5)
            c.send_int_rpc(fc.REP_SELECTROLE, a.role); time.sleep(0.3); c.send_ready(True)
            log(f"picked role {a.role}, ready")
        if picked and not got and now >= next_ask and state["phase"] == "spawn":
            c.pc(REP_ASKFORPAWN); next_ask = now + 2.0
        if got and got != state["pawn_ch"]:
            state["pawn_ch"] = got
            if c.base is None:
                with c.lock:
                    by_ch = dict(c.ch_class)
                for ch, o in by_ch.items():
                    if all(by_ch.get(ch + k) == o - CDO_M67 + g for k, g in enumerate(GRENADES)):
                        c.base = o - CDO_M67
            with c.lock:
                loc = c.open_locs.get(got)
            state["me"] = tuple(float(v) for v in loc) if loc else None
            c.pc(REP_ACKPOSSESSION, lambda p: (p.bit(1), p.bit(1), p.int_max(got, 0x800)))
            log(f"SPAWNED at {state['me']} (pawn ch {got})")
            state["sprinting"] = False
            state["phase"] = "goto_yankee" if not stats["pickup"] else "zulu"
            # Black extracts at Zulu from the west spawn; on swapped rounds it
            # spawns east and extracts at X-ray (the override swaps).
            if state["me"] and abs(state["me"][0]) > 5000:   # a spawn at one end: sides swap between rounds
                state["zone"] = (ZULU_GROUND, ZULU_BOX) if state["me"][0] < 0 else (XRAY_GROUND, XRAY_BOX)
                log(f"extraction zone {'Zulu' if state['me'][0] < 0 else 'X-ray'}")
            state["path"] = []
            rifle = im = None
            equip_at = now + 1.0
            target = None
        if got is None and state["pawn_ch"] is not None:
            stats["deaths"] += 1
            if stats["pickup"]:
                stats["pickup"] = False                 # the case drops with us
                log("dropped the briefcase (died carrying it)")
            log(f"DIED (deaths {stats['deaths']}); F5 for reinforcements")
            state["pawn_ch"] = None
            state["phase"] = "dead"
            next_f5 = now + 3.0
        if state["phase"] == "dead":
            if now >= next_f5:
                c.pc(REP_RESPAWN); next_f5 = now + 6.0
                log("F5 (ServerRequestRespawn)")
            tick_move(None); time.sleep(0.033); continue
        if not got:
            tick_move(None); time.sleep(0.033); continue
        # Draw the rifle.
        if equip_at and now >= equip_at and c.base is not None:
            rifle = next(((ch, mx) for cdo, mx in RIFLES.items() for ch in c.channels_of(cdo)), None)
            ims = c.channels_of(CDO_IM)
            if rifle and ims:
                im = ims[0]
                c.set_current_weapon(im, rifle[0])
                rounds_in_mag = 0
                reload_until = now + 8.0       # the initial reload
                log(f"rifle drawn (ch {rifle[0]})")
                equip_at = 0.0
            else:
                equip_at = now + 0.5
        # Sights, once: raise, then lower (log the replies).
        if rifle and not sights_done and now >= reload_until:
            sights_done = True
            n = len(c.pc_events)
            c.pc(REP_RAISE); time.sleep(1.5); c.pc(REP_LOWER); time.sleep(1.0)
            log(f"sights up/down; PC replies: {[e[1] for e in c.events_since(n)][:12]}")

        # Fight: the nearest NPC within range not being ignored.
        me = state["me"]
        if rifle and now >= reload_until and me:
            with c.lock:
                npcs = dict(c.npc_loc)
                for ch, o in c.ch_class.items():
                    if o == c.base + CDO_NPC and ch not in npcs and ch in c.open_locs:
                        npcs[ch] = c.open_locs[ch]
            reach = 0.0 if state["phase"] == "zulu" else a.engage    # carrying: run, don't stop to fight
            cands = [(math.dist(l[:2], me[:2]), ch, l) for ch, l in npcs.items()
                     if ignore.get(ch, 0) < now and math.dist(l[:2], me[:2]) < reach]
            if target and target not in npcs:
                target = None
            if not target and cands:
                _, target, _ = min(cands)
                shots_at_target = 0
                log(f"engaging NPC ch {target} at {npcs[target]} ({min(cands)[0]:.0f} units)")
            if target:
                l = npcs[target]
                dx, dy = l[0] - me[0], l[1] - me[1]
                dz = (l[2] + 38) - (me[2] + EYE)
                state["yaw"] = int(math.atan2(dy, dx) * 32768 / math.pi) & 0xFFFF
                state["pitch"] = int(math.atan2(dz, math.hypot(dx, dy)) * 32768 / math.pi)
                if now >= next_shot:
                    c.call(rifle[0], rifle[1], REP_STARTFIRE); time.sleep(0.05)
                    c.call(rifle[0], rifle[1], REP_STOPFIRE)
                    stats["shots"] += 1; shots_at_target += 1; rounds_in_mag += 1
                    next_shot = now + 0.4
                    if rounds_in_mag >= 18:
                        p = BW(); p.int_max(PAWN_TRYRELOAD, fc.PAWN_MAX)
                        p.bit(1); p.bitsn(2, 2)                 # ReloadType = 2, as the real client
                        c.reliable(state["pawn_ch"], p)
                        reload_until = now + 4.0; rounds_in_mag = 0
                        log("reloading")
                    if shots_at_target >= a.give_up:
                        log(f"giving up on NPC ch {target} (no kill in {shots_at_target} shots)")
                        ignore[target] = now + 20.0
                        target = None
                tick_move(None); time.sleep(0.033); continue

        # Sprint while carrying the case.
        want_sprint = state["phase"] == "zulu"
        if want_sprint != state.get("sprinting", False):
            c.pc(REP_HOLDSPRINT if want_sprint else REP_STOPSPRINT)
            state["sprinting"] = want_sprint
            log("sprinting" if want_sprint else "walking")
        # Mission movement.
        dropped = None
        if state["phase"] == "goto_yankee" and c.base is not None:
            with c.lock:
                dropped = next(((ch, c.open_locs.get(ch)) for ch, o in c.ch_class.items()
                                if o == c.base + 7016 and c.open_locs.get(ch)), None)   # AA_DroppedObjective
        if state["phase"] == "zulu" and state.get("routed_zone") != state["zone"][0]:
            state["routed_zone"] = state["zone"][0]; state["path"] = []
        if state["phase"] == "goto_yankee" and dropped:
            goal = (float(dropped[1][0]), float(dropped[1][1]), float(dropped[1][2]) - 10)
            if state.get("goal") != goal:
                state["goal"] = goal; state["path"] = []
                log(f"going for the dropped briefcase at {tuple(round(v) for v in goal)}")
        elif state["phase"] == "goto_yankee":
            goal = PICKUP_SPOT
        elif state["phase"] == "zulu":
            goal = state["zone"][0]
        else:
            tick_move(None); time.sleep(0.033); continue
        if not state["path"]:
            pts = router.route(me, goal)
            # every 3rd grid point (96 units), keeping crouch/door ones and the last
            state["path"] = [p for k, p in enumerate(pts) if k % 3 == 0 or p[3] or p[4] >= 0 or k == len(pts) - 1]
            if not state["path"]:
                state["path"] = [goal + (0, -1)]
            state["best"], state["best_at"] = 1e9, now
            log(f"route to {state['phase']}: {len(pts)} grid points, {len(state['path'])} waypoints")
        wp = state["path"][0]
        d = math.dist(wp[:2], me[:2])
        crouch_now = bool(wp[3]) if len(wp) > 3 else False
        if crouch_now != state.get("crouched", False):
            c.send_crouch(crouch_now); state["crouched"] = crouch_now
            log(f"{'crouch' if crouch_now else 'stand'} at {tuple(round(v) for v in me)}")
        if d < 50 and len(state["path"]) > 1:
            state["path"].pop(0); state["best"], state["best_at"] = 1e9, now
            tick_move(None); time.sleep(0.033); continue
        # Arrived at the goal.
        if math.dist(goal[:2], me[:2]) < 70:
            if state["phase"] == "goto_yankee":
                if not stats["pickup"]:
                    with c.lock:
                        objs = dict(c.ch_class)
                    factory = RETAIL_FACTORY_OBJ if a.retail else None
                    if factory is None:
                        marker = next((ch for ch, o in objs.items() if o == c.base + 15660), None)
                        factory = objs.get(marker - 1) if marker else None
                    def build(p, factory=factory):
                        p.bit(1)
                        for b in fc.fstring("Pickup"):
                            p.bitsn(b, 8)
                        p.u32(0)
                        if dropped:                                     # from the ground: its channel
                            p.bit(1); p.bit(1); p.int_max(dropped[0], 0x800)
                        else:
                            p.bit(1); p.bit(0); p.int_max(factory, 0x80000000)
                    c.pc(REP_REPEXEC, build); c.pc(REP_USE)
                    log(f"PICKUP sent (factory obj {factory})")
                    time.sleep(1.0)
                if stats["pickup"]:
                    state["phase"] = "zulu"; state["path"] = []
                    log("carrying the briefcase to Zulu")
            else:
                box = state["zone"][1]
                inside = box[0][0] <= me[0] <= box[1][0] and box[0][1] <= me[1] <= box[1][1]
                if inside and state["phase"] == "zulu":
                    log("inside Zulu, waiting for the round to end")
            tick_move(None); time.sleep(0.033); continue
        # Stuck: no progress toward the waypoint for 4 s -> next waypoint / re-plan.
        if d < state["best"] - 10:
            state["best"], state["best_at"] = d, now
        elif now - state["best_at"] > 4.0:
            # Blocked in play where our collision shows nothing: route around the
            # cells just ahead (logged, to find what the collision misses).
            ahead = (me[0] + (wp[0] - me[0]) / max(d, 1e-3) * 40, me[1] + (wp[1] - me[1]) / max(d, 1e-3) * 40, wp[2])
            router.block(ahead, 40)
            log(f"stuck at {tuple(round(v) for v in me)} going to {tuple(round(v) for v in wp[:3])}; "
                f"BLOCKED {tuple(round(v) for v in ahead)}, side-step, re-plan")
            state["path"] = []
            state["side"] = -state.get("side", 1)
            state["side_until"] = now + 0.8
            state["side_dir"] = (-(wp[1] - me[1]) / max(d, 1e-3) * state["side"], (wp[0] - me[0]) / max(d, 1e-3) * state["side"])
            state["best"], state["best_at"] = 1e9, now
        ux, uy = (wp[0] - me[0]) / max(d, 1e-3), (wp[1] - me[1]) / max(d, 1e-3)
        if now < state.get("side_until", 0):
            ux, uy = state["side_dir"]
        state["yaw"] = int(math.atan2(uy, ux) * 32768 / math.pi) & 0xFFFF
        state["pitch"] = 0
        tick_move((ux, uy))
        time.sleep(0.033)

    c.stop = True
    log(f"SUMMARY {json.dumps(stats, default=str)}")
    return 0 if stats["extracted"] else 1


if __name__ == "__main__":
    sys.exit(main())
