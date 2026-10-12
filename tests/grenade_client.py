#!/usr/bin/env python3
"""Scripted weapons run against aa3_server (doc sec 37), on fake_client.py.

Two players join Black. Before the match A picks the 2x ACOG on the main rail
and drops the M95 light (ServerSetRailMod, as the role menu does); both vote
ready and spawn. Then A, looking up and away from B, draws the rifle, takes an M67
out, pulls the pin, cooks it and throws, and goes back to the rifle --
retail's sequence from GamePlayPCAP_Legit_Server t=444.92-450.70.

PASS requires:
  - A's rifle mod is the 2x ACOG (and no light), B's the role default iron
    sights + M95 light;
  - the PRI's iFragGrenades = the role's 2 at spawn, 1 once the M67 is in hand;
  - ClientSendHUDNotifyChangeInventoryCount (leading the take-out bunch);
  - ClientStartCookingDelayedRestoreTimer on CookIt;
  - an AA_ThrowProjectile_M67 channel on both A and B, a new M67 for A, and
    ClientSwitchToBestWeapon on A's PC after the throw;
  - UpdateGrenadeCounts (leading the switch-back bunch);
  - the projectile's channel closed again at its 6 s LifeSpan;
  - InventoryManager.SetCurrentWeapon(rifle) from the server 6.15 s after
    the cook (retail t=453.94);
  - ES2 perk pick-ups (ServerES2Use): grenades added up to the cap;
  - a second M67 cooked and held past its fuse: ClientHolderDied and a
    projectile (it goes off in the hand).

Start the server standalone with short round timers, e.g. an ini with
RoleSelectionTime = 2 and DeploymentTime = 1.
"""
import argparse
import sys
import threading
import time

import fake_client as fc
from fake_client import BW, BR, Client

# AA3Baseline default-object NetIndex values (class export + 1). Object
# indices on the wire are ObjectBase(AA3Baseline) + these, so the test works
# with differences from the rifle's.
CDO_M4A1 = 5251
CDO_IM = 13669
CDO_M67 = 33082
CDO_PROJ_M67 = 32787
CDO_IRON = 35980
CDO_ACOG2X = 35944
CDO_M95 = 35844
CDO_INSTAPERK = 8617
IM_MAX = 38
THROW_MAX = 66
REP_SETCURRENTWEAPON = 25          # InventoryManager.ServerSetCurrentWeapon
REP_STARTFIRE, REP_STOPFIRE, REP_COOKIT = 31, 32, 59
REP_COOK_TIMER = 63                # AA_ThrowWeapon.ClientStartCookingDelayedRestoreTimer
REP_HOLDER_DIED = 64               # AA_ThrowWeapon.ClientHolderDied
REP_IM_SETCURRENTWEAPON = 24       # InventoryManager.SetCurrentWeapon (client)
REP_SETRAILMOD = 321               # AA_PlayerController.ServerSetRailMod
PC_HUD_COUNT, PC_GRENADE_COUNTS, PC_CHANGED_WEAPON, PC_SWITCH_BEST = 164, 163, 157, 28
PRI_FRAG = 148
PRI_STUN = 145
REP_ES2USE = 232                   # AA_PlayerController.ServerES2Use(aaES2Obj)


class WeaponsClient(Client):
    def __init__(self, *a):
        super().__init__(*a)
        self.pc_calls = []          # first RepIndex of each PC-channel bunch received
        self.throw_calls = []       # (ch, RepIndex) on grenade channels
        self.frag_counts = []       # PRI iFragGrenades values
        self.stun_counts = []       # PRI iStunGrenades values
        self.im_calls = []          # first RepIndex of each InventoryManager bunch
        self.closed = set()

    def deliver(self, ch, b_open, b_close, payload):
        if ch >= 2 and payload and not b_open:
            w = BW(); w.bits = payload
            r = BR(bytes(w.packet()), len(payload))
            try:
                if ch == fc.PC_CH:
                    with self.lock:
                        self.pc_calls.append(r.int_max(fc.PC_MAX))
                elif ch == 10:
                    while r.pos < len(payload):
                        idx = r.int_max(fc.PRI_MAX)
                        if idx < 144 or idx > 148:      # grenade counts are 144..148
                            break
                        v = r.bitsn(32)
                        if idx == PRI_FRAG:
                            with self.lock:
                                self.frag_counts.append(v)
                        elif idx == PRI_STUN:
                            with self.lock:
                                self.stun_counts.append(v)
                else:
                    obj = self.ch_class.get(ch)
                    if obj is not None and self.base is not None and obj - self.base == CDO_M67:
                        with self.lock:
                            self.throw_calls.append((ch, r.int_max(THROW_MAX)))
                    elif obj is not None and self.base is not None and obj - self.base == CDO_IM:
                        with self.lock:
                            self.im_calls.append(r.int_max(IM_MAX))
            except IndexError:
                pass
        if b_close:
            with self.lock:
                self.closed.add((ch, self.ch_class.get(ch)))
        super().deliver(ch, b_open, b_close, payload)

    base = None                     # ObjectBase(AA3Baseline) on this connection

    def channels_of(self, cdo):
        """Open channels whose object is the default object `cdo`."""
        with self.lock:
            if self.base is None:
                return []
            return sorted(ch for ch, o in self.ch_class.items() if o - self.base == cdo)

    def reliable(self, ch, payload):
        chseq = self.next_chseq(ch)
        with self.lock:
            w = BW()
            self.next_seq(w)
            w.bit(0); w.bit(0); w.bit(1)
            w.int_max(ch, 0x800); w.int_max(chseq, 0x400); w.int_max(2, 8)
            w.int_max(len(payload.bits), 0x1000)
            w.extend(payload)
            self.send_bits(w)

    def call(self, ch, max_index, rep_index):
        p = BW(); p.int_max(rep_index, max_index)
        self.reliable(ch, p)

    def set_current_weapon(self, im_ch, weapon_ch):
        p = BW(); p.int_max(REP_SETCURRENTWEAPON, IM_MAX)
        p.bit(1); p.bit(1); p.int_max(weapon_ch, 0x800)     # Send, bDynamic, channel
        self.reliable(im_ch, p)

    def es2_use(self, obj_ch):
        p = BW(); p.int_max(REP_ES2USE, fc.PC_MAX)
        p.bit(1); p.bit(1); p.int_max(obj_ch, 0x800)        # Send, bDynamic, channel
        self.reliable(fc.PC_CH, p)

    def set_rail_mod(self, weapon, rail, index):
        p = BW(); p.int_max(REP_SETRAILMOD, fc.PC_MAX)
        for s in (weapon, rail):
            p.bit(1)
            for b in fc.fstring(s):
                p.bitsn(b, 8)
        p.bit(index != 0)
        if index != 0:
            p.u32(index)
        self.reliable(fc.PC_CH, p)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8777)
    a = ap.parse_args()

    A = WeaponsClient("A", a.host, a.port)
    B = WeaponsClient("B", a.host, a.port)
    A.handshake(); A.recv_all(1.0)
    B.handshake(); B.recv_all(1.0)

    def pump(c):
        while not c.stop:
            c.recv_all(0.05)
    for c in (A, B):
        threading.Thread(target=pump, args=(c,), daemon=True).start()

    A.send_join_team(0); B.send_join_team(0)
    time.sleep(1.5)
    A.send_int_rpc(fc.REP_SELECTROLE, 1)                    # T0F0R1, Fireteam Leader
    B.send_int_rpc(fc.REP_SELECTROLE, 2)
    time.sleep(0.3)
    A.set_rail_mod("PrimaryOne", "Main", 2)                 # role list: iron, CCO, 2x ACOG
    A.set_rail_mod("PrimaryOne", "Right", -1)               # no light
    time.sleep(0.3)
    A.send_ready(True); B.send_ready(True)
    deadline = time.time() + 30
    while time.time() < deadline and not all(c.gave_pawn for c in (A, B)):
        time.sleep(0.1)
    time.sleep(0.8)

    # AA3Baseline's ObjectBase: the spawn inventory opens the five grenades
    # on consecutive channels, M67, M83, M84, M106, AN-M14 (retail frame 365).
    grenades = (CDO_M67, 33062, 33088, 33070, 33037)
    for c in (A, B):
        with c.lock:
            by_ch = dict(c.ch_class)
        for ch, o in by_ch.items():
            base = o - CDO_M67
            if all(by_ch.get(ch + k) == base + g for k, g in enumerate(grenades)):
                c.base = base
                break
    rifle_a = A.channels_of(CDO_M4A1)
    im_a = A.channels_of(CDO_IM)
    m67_a = A.channels_of(CDO_M67)
    print(f"A: rifle ch {rifle_a}, IM ch {im_a}, M67 ch {m67_a}")
    mods_a = (A.channels_of(CDO_ACOG2X), A.channels_of(CDO_IRON), A.channels_of(CDO_M95))
    mods_b = (B.channels_of(CDO_ACOG2X), B.channels_of(CDO_IRON), B.channels_of(CDO_M95))
    print(f"A mods (2x ACOG, iron, M95): {mods_a}; B mods: {mods_b}")
    mods_ok = bool(mods_a[0]) and not mods_a[1] and not mods_a[2] and bool(mods_b[1]) and bool(mods_b[2])
    if not (rifle_a and im_a and m67_a):
        print("FAIL: inventory not found")
        return 1
    rifle, im, m67 = rifle_a[0], im_a[0], m67_a[0]

    # Stand still looking north (+Y, away from B) and 45 degrees up: the
    # first grenade lands well clear of both players.
    stop_moving = threading.Event()
    def mover():
        ts = 1.0
        while not stop_moving.is_set():
            ts += 0.05
            A.send_move(ts, (11385.0, -3308.0, 65.0), yaw=16384, pitch=8192)
            time.sleep(0.05)
    threading.Thread(target=mover, daemon=True).start()

    A.set_current_weapon(im, rifle)
    time.sleep(1.0)
    n_pc = len(A.pc_calls)
    A.set_current_weapon(im, m67)
    time.sleep(1.0)
    take_calls = A.pc_calls[n_pc:]
    A.call(m67, THROW_MAX, REP_STARTFIRE)
    time.sleep(0.4)
    A.call(m67, THROW_MAX, REP_COOKIT)
    time.sleep(1.0)
    n_pc = len(A.pc_calls)
    A.call(m67, THROW_MAX, REP_STOPFIRE)
    time.sleep(1.2)
    proj_a, proj_b = A.channels_of(CDO_PROJ_M67), B.channels_of(CDO_PROJ_M67)
    new_m67 = [ch for ch in A.channels_of(CDO_M67) if ch != m67]
    throw_calls = A.pc_calls[n_pc:]
    n_pc = len(A.pc_calls)
    A.set_current_weapon(im, rifle)
    time.sleep(0.8)
    back_calls = A.pc_calls[n_pc:]
    time.sleep(5.5)                                          # LifeSpan 6 s from the throw
    proj_open_after = A.channels_of(CDO_PROJ_M67) + B.channels_of(CDO_PROJ_M67)
    # The cook's restore: InventoryManager.SetCurrentWeapon(rifle) 6.15 s
    # after CookIt (retail t=453.94).
    restore_ok = REP_IM_SETCURRENTWEAPON in A.im_calls

    # ES2 perks (AA_ES2InstaPerk.PerformES2Use): take from each, 4 times.
    # Bridge classic has two M84 cases (3 each): the stun count climbs until
    # the 12-grenade cap (MAX_GRENADES) stops it.
    perks = A.channels_of(CDO_INSTAPERK)
    for _ in range(4):
        for ch in perks:
            A.es2_use(ch)
            time.sleep(0.15)
    time.sleep(0.8)
    perks_left = A.channels_of(CDO_INSTAPERK)
    print(f"perks {perks} -> {perks_left}; stun counts {A.stun_counts}")
    perks_ok = bool(perks) and len(A.stun_counts) >= 2 and A.stun_counts[-1] > A.stun_counts[0]

    # Second M67, cooked and held past its fuse: it goes off in the hand.
    m67b = new_m67[0] if new_m67 else m67
    A.set_current_weapon(im, m67b)
    time.sleep(1.0)
    A.call(m67b, THROW_MAX, REP_STARTFIRE)
    time.sleep(0.3)
    A.call(m67b, THROW_MAX, REP_COOKIT)
    time.sleep(5.6)
    cookoff_ok = (m67b, REP_HOLDER_DIED) in A.throw_calls and len(A.channels_of(CDO_PROJ_M67)) >= 1
    stop_moving.set()
    A.stop = B.stop = True

    print(f"PRI iFragGrenades seen by A: {A.frag_counts}")
    print(f"take-out PC calls {take_calls}; throw PC calls {throw_calls}; back PC calls {back_calls}")
    print(f"M67 channel calls: {A.throw_calls}")
    print(f"projectile ch: A {proj_a} B {proj_b}; new M67 {new_m67}; open after LifeSpan {proj_open_after}")
    checks = {
        "rail mods": mods_ok,
        "frag counts": A.frag_counts[:2] == [2, 1],
        # (first call of each bunch: HUD count + grenade counts + weapon change go as one)
        "take-out": take_calls[:1] == [PC_HUD_COUNT],
        "cook timer": (m67, REP_COOK_TIMER) in A.throw_calls,
        "projectile": bool(proj_a) and bool(proj_b),
        "replacement": bool(new_m67),
        "switch best": PC_SWITCH_BEST in throw_calls,
        "switch back": back_calls[:1] == [PC_GRENADE_COUNTS],
        "lifespan close": not proj_open_after,
        "cook restore": restore_ok,
        "cook-off in hand": cookoff_ok,
        "ES2 perks": perks_ok,
    }
    for k, v in checks.items():
        print(f"  {k}: {'ok' if v else 'FAILED'}")
    ok = all(checks.values())
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
