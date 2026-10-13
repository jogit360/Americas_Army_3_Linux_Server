#!/usr/bin/env python3
"""A medic bot for a retail AA3Game server (doc sec 39): logs in like the real
client (bot_client.py --retail), joins a team, spawns and stands still. When a
teammate comes within --reach (retail's GetVisiblePawnFromSoldierName wants
120 units) it treats them the way the retail client does (doc sec 39.1):

  ServerStartTreatment(sVictimSoldierName) + ServerStartPassiveTreatment()
  + ServerUse(), held, then ServerStopTreatment().

It retries every 3 s until the server accepts (SetTreatmentOnRequestingClient
names a patient). Walking to the patient doesn't work: the retail server
corrects scripted moves (ClientAdjustPosition), so the patient walks over.
Tested 2026-10-12 on retail Bridge AI: treated, +20 score.

  retail_medic.py --patient DrAxEl --team 0 [--hold 12]
"""
import argparse
import math
import threading
import time

import fake_client as fc
from bot_client import reliable, pc_call, REP_ASKFORPAWN, REP_ACKPOSSESSION, REP_LEVEL_VISIBILITY

REP_START_TREATMENT, REP_STOP_TREATMENT = 213, 214
REP_START_PASSIVE = 216
REP_SERVER_USE = 76


class Medic(fc.Client):
    def __init__(self, *a):
        super().__init__(*a)
        self.pc_names = []
        self.adjusted = None        # latest server correction of our Location

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
                    if short not in ("ClientAckGoodMove()", "ClientHearSound()"):
                        with self.lock:
                            self.pc_names.append((round(time.time() - fc.T0, 1), short, val))
            except Exception:
                pass
        super().parse_pc_retail(data, start, bdb)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8777)
    ap.add_argument("--name", default="Medic")
    ap.add_argument("--team", type=int, default=0)
    ap.add_argument("--role", type=int, default=1)
    ap.add_argument("--patient", required=True, help="the patient's soldier name (their player name)")
    ap.add_argument("--reach", type=float, default=100.0, help="stop this close to the patient")
    ap.add_argument("--hold", type=float, default=12.0, help="seconds to keep treating")
    ap.add_argument("--seconds", type=float, default=600.0, help="stay connected this long in all")
    a = ap.parse_args()

    c = Medic("Bot", a.host, a.port)
    if not c.retail_handshake(a.name):
        print("retail login failed; control messages:", c.control_msgs[:5])
        return 1
    c.recv_all(1.0)
    threading.Thread(target=lambda: [c.recv_all(0.05) for _ in iter(lambda: c.stop, True)],
                     daemon=True).start()
    print(f"logged in as {a.name}; joining team {a.team}")
    c.send_join_team(a.team)

    t_end = time.time() + a.seconds
    t_join = time.time()
    picked = False
    next_ask = 0.0
    pawn_ch = None
    me = None
    ts = 1.0
    phase = "spawn"             # spawn -> walk (wait in reach) -> check -> treat -> done
    t_phase = 0.0
    yaw = 0
    t_report, mark = 0.0, 0
    while time.time() < t_end:
        now = time.time()
        with c.lock:
            pend, c.pending_visibility = c.pending_visibility, []
            got = c.own_pawn_ch if c.gave_pawn and c.own_pawn_ch in c.ch_class else None
        for name, visible in pend:
            def build(p, name=name, visible=visible):
                p.bit(1)
                for b in fc.fstring(name):
                    p.bitsn(b, 8)
                p.u32(0)
                p.bit(visible)
            pc_call(c, REP_LEVEL_VISIBILITY, build)
        if not picked and now - t_join > 3.0:
            picked = True
            c.send_int_rpc(fc.REP_SELECTROLE, a.role)
            time.sleep(0.3)
            c.send_ready(True)
            print(f"picked role {a.role}, ready")
        if picked and not got and now >= next_ask:
            pc_call(c, REP_ASKFORPAWN)
            next_ask = now + 2.0
        if got and got != pawn_ch:
            pawn_ch = got
            me = c.open_locs.get(pawn_ch)
            with c.lock:
                fc.PAWN_OBJ = c.ch_class.get(pawn_ch)      # every soldier's pawn class
            pc_call(c, REP_ACKPOSSESSION, lambda p: (p.bit(1), p.bit(1), p.int_max(pawn_ch, 0x800)))
            print("spawned at", me)
            phase, t_phase = "walk", now + 2.0
        if not got and pawn_ch is not None:
            print("lost soldier (died or round over)")
            pawn_ch, me = None, None
            phase = "spawn"

        accel = None
        with c.lock:
            adj, c.adjusted = c.adjusted, None
        if me and adj:
            me = adj                                    # the server's word on where we are
        # Wait for a teammate to come within reach (retail: 120 units), face
        # them, treat; retry every 3 s until the server accepts
        # (SetTreatmentOnRequestingClient names a patient).
        if me and phase == "walk" and now >= t_phase:
            with c.lock:
                last = {}
                for ch, loc in c.remote_locs:
                    if ch != pawn_ch:
                        last[ch] = loc
            near = min(((math.hypot(l[0] - me[0], l[1] - me[1]), l) for l in last.values()), default=None)
            if near:
                d, target = near
                yaw = int(math.atan2(target[1] - me[1], target[0] - me[0]) * 32768 / math.pi) & 0xFFFF
                if now >= t_report:
                    print(f"nearest teammate {d:.0f} units away at {tuple(round(v) for v in target)}; me at {tuple(round(v) for v in me)}")
                    t_report = now + 5.0
                if d <= a.reach:
                    print(f"patient in reach ({d:.0f} units); treating")
                    mark = len(c.pc_names)
                    def treat(p):
                        p.bit(1)                                    # sVictimSoldierName
                        for b in fc.fstring(a.patient):
                            p.bitsn(b, 8)
                    pc_call(c, REP_START_TREATMENT, treat)
                    pc_call(c, REP_START_PASSIVE)
                    pc_call(c, REP_SERVER_USE)
                    phase, t_phase = "check", now + 1.0
        if phase == "check" and now >= t_phase:
            with c.lock:
                reply = [v for _, n, v in c.pc_names[mark:] if n == "SetTreatmentOnRequestingClient()"]
            if reply and reply[-1]:
                print("server accepted the treatment:", reply[-1])
                phase, t_phase = "treat", now + a.hold
            else:
                print("server refused the treatment (out of reach or not treatable); retrying")
                pc_call(c, REP_STOP_TREATMENT)
                phase, t_phase = "walk", now + 3.0
        if phase == "treat" and now >= t_phase:
            pc_call(c, REP_STOP_TREATMENT)
            print("stopped treatment")
            time.sleep(1.0)
            with c.lock:
                calls = c.pc_names[mark:]
            for t, name, val in calls:
                print(f"  t={t} {name} {val if val not in (None, {}) else ''}"[:200])
            phase = "done"
        ts += 0.033
        c.send_move(ts, me or (0, 0, 0), yaw, 0, accel)
        time.sleep(0.033)
    c.stop = True
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
