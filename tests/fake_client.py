#!/usr/bin/env python3
"""Minimal scripted AA3 client for exercising aa3_server end to end.

Speaks just enough of the protocol to reach ESTABLISHED (the server's
handshake stages key on packet size: Hello 40-70 B, bundle 90-140 B, then a
reliable Ch=0/ChType=1 "JOIN" FString), then sends real ServerMove RPCs on
the PC channel and decodes what the server sends back: channel opens,
remote players' Pawn Location updates, ClientAckGoodMove.

  fake_client.py --two-player [--host 127.0.0.1 --port 8777 --seconds 4]

Two-player mode runs clients A and B: A walks +X, B stands still. PASS
requires B to see a remote Pawn channel open and receive Location updates
that track A's movement, and A to receive move acks (doc sec 21).

Like a UE3 client it acks every packet it receives, numbers its reliable
bunches per channel, and processes the server's reliable bunches in
ChSequence order (dropping duplicates, holding ones that arrive early).
Reliability checks (doc sec 25):
  --loss P     drop each incoming packet with probability P (unacked, as if
               lost on the wire); PASS additionally requires every reliable
               gap to have been filled by a server retransmission
  --reorder    A sends its crouch RPCs out of order and one twice; the server
               must apply each exactly once, in ChSequence order
Every run plays the round first (join, role, ready, spawn; doc sec 28-29):
start the server with short RoleSelectionTime/DeploymentTime (e.g. 2 and 1).
"""
import random
import argparse
import socket
import struct
import sys
import threading
import time

PC_CH = 2                  # PlayerController channel (same as retail)
OWN_PAWN_CH = 11           # this client's own Pawn
PROXY_CH = 12               # this client's AA_PlayerStorageProxy
FIRST_REMOTE_CH = 13       # other players' PRI/Pawn
PC_MAX = 491               # rep::AGP_PC::kMaxIndex (AGP_PlayerController)
PAWN_MAX = 131             # rep::Pawn::kMaxIndex
PRI_MAX = 212
REP_SERVERMOVE = 61        # rep::PC::ServerMove
REP_ACKGOODMOVE = 66       # rep::PC::ClientAckGoodMove
REP_LOCATION = 22          # Actor.Location
REP_ROLE = 17
REP_SETCROUCH = 180        # rep::PC::ServerSetCrouch
REP_POSTURE = 309          # rep::PC::NotifyPostureChange
REP_BISCROUCHED = 42       # Pawn.bIsCrouched (non-owners only)
REP_JOINTEAM = 229         # rep::PC::ServerJoinTeam(int iTeamIndex)
REP_TEAMMESSAGE = 53       # rep::PC::TeamMessage (the join reply)
REP_SETREADY = 206         # rep::PC::ServerSetPlayerReady(bool)
REP_SELECTROLE = 273       # rep::PC::ServerSelectClassRole_ByIndex(int RoleIndex)
REP_CLIENTRESTART = 84     # rep::PC::ClientRestart: leads the possession sequence
                           # (GivePawn itself follows ClientSetViewTarget in its bunch)
# Round-phase announcements on the PC channel (doc sec 28), in order.
PHASE_RPCS = {261: "EnterPrePlanningTool", 41: "GotoState", 260: "DeploymentWarning",
              262: "ClosePrePlanningTool"}


class BW:
    def __init__(self):
        self.bits = []

    def bit(self, b):
        self.bits.append(1 if b else 0)

    def bitsn(self, v, n):
        for i in range(n):
            self.bit((v >> i) & 1)

    def int_max(self, v, mx):  # UE3 SerializeInt / WriteIntMax
        val = 0
        m = 1
        while val + m < mx and m:
            b = 1 if (v & m) else 0
            self.bit(b)
            if b:
                val |= m
            m <<= 1

    def u32(self, v):
        self.bitsn(v & 0xFFFFFFFF, 32)

    def f32(self, f):
        self.u32(struct.unpack("<I", struct.pack("<f", f))[0])

    def vec(self, x, y, z):  # FVector::SerializeCompressed
        ix, iy, iz = (int(round(c)) for c in (x, y, z))
        mx = max(abs(ix), abs(iy), abs(iz))
        n, cl = 1 + mx, 0
        v = 1
        while v < n:
            v <<= 1
            cl += 1
        cl = min(max(cl, 1), 20)
        bits = cl - 1
        self.int_max(bits, 20)
        bias = 1 << (bits + 1)
        m = 1 << (bits + 2)
        for c in (ix, iy, iz):
            self.int_max(c + bias, m)

    def extend(self, other):
        self.bits.extend(other.bits)

    def packet(self):
        bits = self.bits + [1]  # terminator
        out = bytearray((len(bits) + 7) // 8)
        for i, b in enumerate(bits):
            if b:
                out[i >> 3] |= 1 << (i & 7)
        return bytes(out)


class BR:
    def __init__(self, data, end=None):
        self.d = data
        self.pos = 0
        if end is None:
            end = len(data) * 8
            for i in range(len(data) - 1, -1, -1):
                if data[i]:
                    hi = 7
                    while not (data[i] >> hi) & 1:
                        hi -= 1
                    end = i * 8 + hi
                    break
        self.end = end

    def bit(self):
        if self.pos >= self.end:
            raise IndexError
        b = (self.d[self.pos >> 3] >> (self.pos & 7)) & 1
        self.pos += 1
        return b

    def bitsn(self, n):
        return sum(self.bit() << i for i in range(n))

    def int_max(self, mx):
        v, m = 0, 1
        while v + m < mx and m:
            if self.bit():
                v |= m
            m <<= 1
        return v

    def vec(self):
        bits = self.int_max(20)
        bias = 1 << (bits + 1)
        m = 1 << (bits + 2)
        return tuple(self.int_max(m) - bias for _ in range(3))


_DR = []


def _decode_rep():
    """aa3-tools/map_manifest/decode_rep (layouts from the cooked packages),
    loaded on first use; None if it isn't there."""
    if not _DR:
        import importlib
        import os
        tools = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..",
                             "aa3-tools", "map_manifest")
        sys.path.insert(0, os.path.abspath(tools))
        try:
            _DR.append(importlib.import_module("decode_rep"))
        except Exception:
            _DR.append(None)
    return _DR[0]


def fstring(s):
    b = s.encode() + b"\0"
    return struct.pack("<I", len(b)) + b


class Client:
    def __init__(self, name, host, port):
        self.name = name
        self.addr = (host, port)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.settimeout(0.05)
        self.seq = 0
        self.ch_class = {}          # ch -> object index from open
        self.ch_open_bits = {}
        self.remote_locs = []       # (ch, loc) Location updates on non-own pawn channels
        self.roles = {}             # ch -> Role value from initial bunch
        self.acks = 0
        self.postures = []          # NotifyPostureChange names received on the PC channel
        self.remote_crouch = []     # bIsCrouched values seen on remote pawn channels
        self.lock = threading.RLock()
        self.stop = False
        # Reliability state (UNetConnection/UChannel on the client side).
        self.out_rel = {}           # ch -> last ChSequence we sent
        self.queued_acks = []       # server packets to ack in our next packet
        self.resend_acks = []       # ...and once more in the one after
        self.last_send = 0.0
        self.in_rel = {}            # ch -> last in-order reliable ChSequence processed
        self.in_rec = {}            # ch -> {seq: (b_open, b_close, payload bits)}
        self.loss = 0.0
        self.rng = random.Random(1)
        self.dropped = self.received = self.dup_in = self.held_in = 0
        self.server_acked = set()   # our packet seqs the server acked
        self.pc_opens = []          # (ch, NetPlayerIndex) per PlayerController open
        self.team_messages = 0      # TeamMessage RPCs (ServerJoinTeam replies)
        self.phase_rpcs = []        # (t, name) round-phase announcements
        self.gave_pawn = False      # own pawn channel opened (possession)
        self.open_locs = {}         # ch -> spawn Location from a transient actor's open
        self.control_msgs = []      # Ch=0 control messages received (retail login)
        self.retail = False         # logged in to a retail server (retail_handshake)
        self.own_pawn_ch = OWN_PAWN_CH   # from ClientRestart(NewPawn)
        self.pending_visibility = []     # (level, visible) to confirm (retail)
        self.role_select_open = 0        # OpenRoleSelectOnTeamJoin() calls (retail)

    def send_bits(self, w):
        with self.lock:
            self.sock.sendto(w.packet(), self.addr)
            self.last_send = time.time()

    def next_seq(self, w):
        """Start a packet: PacketSequence + owed acks (each sent twice, UE3-style)."""
        with self.lock:
            w.int_max(self.seq & 0x3FFF, 0x4000)
            self.seq += 1
            for a in self.resend_acks + self.queued_acks:
                w.bit(1); w.int_max(a, 0x4000)
            self.resend_acks, self.queued_acks = self.queued_acks, []

    def next_chseq(self, ch):
        with self.lock:
            self.out_rel[ch] = self.out_rel.get(ch, 0) + 1
            return self.out_rel[ch]

    def flush_acks(self):
        with self.lock:
            if not self.queued_acks or time.time() - self.last_send < 0.03:
                return
            w = BW()
            self.next_seq(w)
            self.send_bits(w)

    def recv_all(self, dur):
        t = time.time() + dur
        while time.time() < t:
            try:
                data, _ = self.sock.recvfrom(4096)
            except socket.timeout:
                self.flush_acks()
                continue
            if self.loss and self.rng.random() < self.loss:
                self.dropped += 1                            # "lost": not seen, not acked
                continue
            self.received += 1
            self.parse(data)
            self.flush_acks()

    def control_bunch(self, text, pad_to=0, b_open=False):
        """One packet with a reliable Ch=0 ChType=1 FString bunch."""
        payload = fstring(text)
        if pad_to:
            payload += b"\0" * max(0, pad_to - len(payload))
        w = BW()
        self.next_seq(w)
        w.bit(0)
        w.bit(1 if b_open else 0)
        if b_open:
            w.bit(1); w.bit(0)
        w.bit(1)                                             # reliable
        w.int_max(0, 0x800); w.int_max(self.next_chseq(0), 0x400); w.int_max(1, 8)
        w.int_max(len(payload) * 8, 0x1000)
        for byte in payload:
            w.bitsn(byte, 8)
        return w

    def handshake(self):
        # Real packets in the sizes the server's stage classifier expects
        # (Hello 40-70 B, bundle 90-140 B), on Ch=0 ChSeq 1 and 2 like retail.
        self.send_bits(self.control_bunch("HELLO P=1 REVISION=0 MINVER=4206 VER=4206",
                                          b_open=True))
        self.recv_all(0.3)
        self.send_bits(self.control_bunch("NETSPEED 10000", pad_to=100))
        self.recv_all(0.5)
        self.send_bits(self.control_bunch("JOIN"))

    def retail_handshake(self, name, timeout=20.0):
        """The real client's login, which a retail AA3Game server requires
        (GamePlayPCAP_Legit_Server frames 1-70): HELLO -> CHALLENGE; NETSPEED
        + LOGIN RESPONSE=0 URL=...?Name=... (0 = no auth, LAN) -> USES x N +
        WELCOME; HAVE GUID=<g> GEN=<n> for every USES, then JOIN. Returns
        False if the server never welcomes us."""
        def wait_for(prefix, secs):
            end = time.time() + secs
            while time.time() < end:
                with self.lock:
                    if any(m.startswith(prefix) for m in self.control_msgs):
                        return True
                self.recv_all(0.1)
            return False
        self.retail = True
        self.send_bits(self.control_bunch("HELLO P=1 REVISION=0 MINVER=3077 VER=4206", b_open=True))
        if not wait_for("CHALLENGE", timeout):
            return False
        self.send_bits(self.control_bunch("NETSPEED 10000"))
        self.send_bits(self.control_bunch(
            f"LOGIN RESPONSE=0 URL=ui_bink_master?Name={name}?MAXCLIENTNETSPEED=25000?team=0?Face=0"))
        if not wait_for("WELCOME", timeout):
            return False
        self.recv_all(0.5)                                   # trailing USES/DLMGR
        with self.lock:
            uses = [m for m in self.control_msgs if m.startswith("USES ")]
        for m in uses:
            f = dict(kv.split("=", 1) for kv in m.split()[1:] if "=" in kv)
            if "GUID" in f:
                self.send_bits(self.control_bunch(f"HAVE GUID={f['GUID']} GEN={f.get('GEN', '1')}"))
                self.recv_all(0.005)
        self.send_bits(self.control_bunch("JOIN"))
        return True

    def send_move(self, ts, loc, yaw, pitch=0, accel=None):
        p = BW()
        p.int_max(REP_SERVERMOVE, PC_MAX)
        p.bit(1); p.f32(ts)                                  # TimeStamp
        if accel:                                            # InAccel: the walk input
            p.bit(1); p.vec(*accel)
        else:
            p.bit(0)                                         # InAccel = 0
        p.bit(1); p.vec(*loc)                                # ClientLoc
        p.bit(0); p.bit(0)                                   # MoveFlags, ClientRoll
        p.bit(1); p.u32(((yaw & 0xFFFF) << 16) | (pitch & 0xFFFF))  # View
        with self.lock:   # seq + acks + send as one step: packets leave in seq order
            w = BW()
            self.next_seq(w)
            w.bit(0); w.bit(0); w.bit(0)                     # bunch, no OC, UNRELIABLE
            w.int_max(PC_CH, 0x800)
            w.int_max(len(p.bits), 0x1000)
            w.extend(p)
            self.send_bits(w)

    def send_crouch(self, crouch, chseq=None):
        """Reliable ServerSetCrouch. `chseq` overrides the sequence (to send
        out of order or retransmit); returns the ChSequence used."""
        p = BW()
        p.int_max(REP_SETCROUCH, PC_MAX)
        p.bit(crouch)                                        # bool param: no Send flag
        if chseq is None:
            chseq = self.next_chseq(PC_CH)
        with self.lock:
            w = BW()
            self.next_seq(w)
            w.bit(0); w.bit(0); w.bit(1)                     # reliable bunch
            w.int_max(PC_CH, 0x800); w.int_max(chseq, 0x400); w.int_max(2, 8)
            w.int_max(len(p.bits), 0x1000)
            w.extend(p)
            self.send_bits(w)
        return chseq

    def send_join_team(self, team):
        p = BW()
        p.int_max(REP_JOINTEAM, PC_MAX)
        p.bit(team != 0)                                     # iTeamIndex: Send flag
        if team != 0:
            p.u32(team)
        chseq = self.next_chseq(PC_CH)
        with self.lock:
            w = BW()
            self.next_seq(w)
            w.bit(0); w.bit(0); w.bit(1)
            w.int_max(PC_CH, 0x800); w.int_max(chseq, 0x400); w.int_max(2, 8)
            w.int_max(len(p.bits), 0x1000)
            w.extend(p)
            self.send_bits(w)

    def send_int_rpc(self, rep_index, value):
        """Reliable PC RPC with one int parameter (Send flag + 32 bits)."""
        p = BW()
        p.int_max(rep_index, PC_MAX)
        p.bit(value != 0)
        if value != 0:
            p.u32(value)
        chseq = self.next_chseq(PC_CH)
        with self.lock:
            w = BW()
            self.next_seq(w)
            w.bit(0); w.bit(0); w.bit(1)
            w.int_max(PC_CH, 0x800); w.int_max(chseq, 0x400); w.int_max(2, 8)
            w.int_max(len(p.bits), 0x1000)
            w.extend(p)
            self.send_bits(w)

    def send_ready(self, ready):
        p = BW()
        p.int_max(REP_SETREADY, PC_MAX)
        p.bit(ready)                                         # bool: no Send flag
        chseq = self.next_chseq(PC_CH)
        with self.lock:
            w = BW()
            self.next_seq(w)
            w.bit(0); w.bit(0); w.bit(1)
            w.int_max(PC_CH, 0x800); w.int_max(chseq, 0x400); w.int_max(2, 8)
            w.int_max(len(p.bits), 0x1000)
            w.extend(p)
            self.send_bits(w)

    def parse(self, data):
        r = BR(data)
        try:
            pkt = r.int_max(0x4000)
            with self.lock:
                self.queued_acks.append(pkt)
            while r.pos < r.end:
                if r.bit():
                    a = r.int_max(0x4000)
                    with self.lock:
                        self.server_acked.add(a)
                    continue
                b_open = b_close = False
                if r.bit():
                    b_open, b_close = bool(r.bit()), bool(r.bit())
                rel = r.bit()
                ch = r.int_max(0x800)
                seq = None
                if rel:
                    seq = r.int_max(0x400); r.int_max(8)
                elif b_open:
                    r.int_max(8)
                bdb = r.int_max(0x1000)
                start = r.pos
                if start + bdb > r.end:
                    return
                bits = BR(data, start + bdb)
                bits.pos = start
                payload = [bits.bit() for _ in range(bdb)]
                r.pos = start + bdb
                if seq is None:
                    self.deliver(ch, b_open, b_close, payload)
                else:
                    self.receive_reliable(ch, seq, b_open, b_close, payload)
        except IndexError:
            pass

    def receive_reliable(self, ch, raw_seq, b_open, b_close, payload):
        """UChannel::ReceivedRawBunch: in order, no duplicates."""
        with self.lock:
            last = self.in_rel.get(ch, 0)
            seq = last + (((raw_seq - last + 512) & 1023) - 512)   # MakeRelative
            if seq <= last:
                self.dup_in += 1
                return
            if seq != last + 1:
                self.held_in += 1
                self.in_rec.setdefault(ch, {})[seq] = (b_open, b_close, payload)
                return
            self.in_rel[ch] = seq
        self.deliver(ch, b_open, b_close, payload)
        while True:
            with self.lock:
                q = self.in_rec.get(ch, {})
                nxt = self.in_rel.get(ch, 0) + 1
                if nxt not in q:
                    return
                item = q.pop(nxt)
                self.in_rel[ch] = nxt
            self.deliver(ch, *item)

    def gaps(self):
        """Channels still holding reliable bunches behind a missing one."""
        with self.lock:
            return {ch: sorted(q) for ch, q in self.in_rec.items() if q}

    def deliver(self, ch, b_open, b_close, payload):
        if ch == 0 and payload:
            # Control channel: FString messages (CHALLENGE, USES, WELCOME...).
            data = bytes(sum(payload[i + k] << k for k in range(8) if i + k < len(payload))
                         for i in range(0, len(payload) - 7, 8))
            pos = 0
            while pos + 4 <= len(data):
                n = struct.unpack_from("<i", data, pos)[0]
                if n <= 0 or pos + 4 + n > len(data):
                    break
                text = data[pos + 4:pos + 4 + n].rstrip(b"\0").decode("latin1")
                with self.lock:
                    self.control_msgs.append(text)
                pos += 4 + n
        if ch >= 2 and payload:
            w = BW()
            w.bits = payload
            self.parse_actor(bytes(w.packet()), ch, 0, len(payload), b_open)
        if b_close:
            with self.lock:
                self.ch_class.pop(ch, None)

    def parse_pc_retail(self, data, start, bdb):
        """Every call in a retail PC bunch (retail packs the phase RPCs
        behind others, and gives the pawn through Controller.Pawn, not
        ClientRestart's argument), via aa3-tools' decode_rep."""
        dr = _decode_rep()
        if dr is None:
            return
        r = dr.BitReader(data)
        r.pos = start
        dec = dr.Decoder(r)
        try:
            for _, _, name, val in dr.decode_fields(dec, "AGP_PlayerController", start + bdb):
                short = name.rsplit(".", 1)[-1]
                with self.lock:
                    if short == "ClientAckGoodMove()":
                        self.acks += 1
                    elif short == "Pawn":
                        if isinstance(val, str) and val.startswith("<dynamic ch "):
                            self.own_pawn_ch = int(val.split()[-1].rstrip(">"))
                            self.gave_pawn = True
                        else:                               # None: died / round over
                            self.own_pawn_ch = None
                            self.gave_pawn = False
                    elif short == "ClientUpdateLevelStreamingStatus()" and isinstance(val, dict):
                        name = str(val.get("PackageName", "")).strip("'")
                        if name:
                            self.pending_visibility.append((name, bool(val.get("bNewShouldBeVisible"))))
                    elif short == "OpenRoleSelectOnTeamJoin()":
                        self.role_select_open += 1
                    elif short == "TeamMessage()":
                        self.team_messages += 1
                    else:
                        for idx, phase in PHASE_RPCS.items():
                            if short == f"Client{phase}()":
                                self.phase_rpcs.append((round(time.time() - T0, 1), phase))
        except Exception:
            pass

    def parse_actor(self, data, ch, start, bdb, b_open):
        r = BR(data, start + bdb)
        r.pos = start
        try:
            if b_open:
                r.bit()
                obj = r.int_max(0x80000000)
                with self.lock:
                    self.ch_class[ch] = obj
                    if ch == OWN_PAWN_CH and not self.retail:
                        self.gave_pawn = True      # possession opens the own pawn (doc sec 29)
                    if ch != PC_CH:
                        # Transient actors start with SerializeCompressedInitial
                        # (Location); harmless garbage for static ones.
                        save = r.pos
                        try:
                            self.open_locs[ch] = r.vec()
                        except IndexError:
                            pass
                        r.pos = save
                    if ch == PC_CH:
                        # What AA3's UActorChannel::ReceivedBunch does with a
                        # PC: transient preamble (Location; PCs don't send
                        # rotation; then AA3's 7 extra bits), then
                        # NetPlayerIndex. Only index 0 on
                        # the main connection becomes the local player.
                        r.vec()
                        r.bitsn(7)    # AA3: 2 bits + SerializeInt(32) (doc sec 26)
                        self.pc_opens.append((ch, r.bitsn(8)))
                return  # property payload after the preamble: not needed here
            obj = self.ch_class.get(ch)
            if ch == PC_CH and self.retail:
                self.parse_pc_retail(data, start, bdb)
                return
            if ch == PC_CH:
                idx = r.int_max(PC_MAX)
                if idx == REP_ACKGOODMOVE:
                    with self.lock:
                        self.acks += 1
                elif idx == REP_TEAMMESSAGE:
                    with self.lock:
                        self.team_messages += 1
                elif idx == REP_CLIENTRESTART:
                    # ClientRestart(NewPawn): Send, bDynamic, the pawn's channel
                    # (retail numbers it dynamically, e.g. 159; ours is 11).
                    ch_pawn = None
                    if r.bit() and r.bit():
                        ch_pawn = r.int_max(0x800)
                    with self.lock:
                        if ch_pawn:
                            self.own_pawn_ch = ch_pawn
                        self.gave_pawn = True
                elif idx in PHASE_RPCS:
                    with self.lock:
                        self.phase_rpcs.append((round(time.time() - T0, 1), PHASE_RPCS[idx]))
                elif idx == REP_POSTURE and r.bit():         # Send, then AA3 FName
                    n = r.bitsn(32)
                    name = bytes(r.bitsn(8) for _ in range(n)).rstrip(b"\0").decode()
                    r.bitsn(32)
                    with self.lock:
                        self.postures.append(name)
            elif ch >= FIRST_REMOTE_CH and obj == PAWN_OBJ:
                idx = r.int_max(PAWN_MAX)
                if idx == REP_LOCATION:
                    loc = r.vec()
                    with self.lock:
                        self.remote_locs.append((ch, loc))
                elif idx == REP_BISCROUCHED:
                    with self.lock:
                        self.remote_crouch.append(r.bit())
        except IndexError:
            pass


PAWN_OBJ = None  # filled from the own-pawn open: same class for every pawn
T0 = time.time()


def main():
    global PAWN_OBJ
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8777)
    ap.add_argument("--seconds", type=float, default=4.0)
    ap.add_argument("--two-player", action="store_true", default=True)
    ap.add_argument("--leave-test", type=float, default=0.0,
                    help="after the walk, A goes silent; wait this long and require "
                         "B to receive close bunches for A's channels (server must run "
                         "with a short AA3_CONNECTION_TIMEOUT_S)")
    ap.add_argument("--loss", type=float, default=0.0,
                    help="drop this fraction of incoming packets on both clients")
    ap.add_argument("--reorder", action="store_true",
                    help="A sends its crouch RPCs out of order plus a duplicate")
    ap.add_argument("--round", action="store_true",
                    help="A joins a team and votes ready; require the round phases "
                         "(role selection -> deployment warning -> match start)")
    a = ap.parse_args()

    A = Client("A", a.host, a.port)
    B = Client("B", a.host, a.port)
    A.loss = B.loss = a.loss
    B.rng = random.Random(2)
    A.handshake(); A.recv_all(1.5 if a.loss else 1.0)
    B.handshake(); B.recv_all(1.5 if a.loss else 1.0)

    def pump(c):
        while not c.stop:
            c.recv_all(0.05)
    threads = [threading.Thread(target=pump, args=(c,), daemon=True) for c in (A, B)]
    for t in threads:
        t.start()

    # The round (doc sec 28-29): both join Black, A takes retail's role pick,
    # both vote ready (ends role selection), deployment runs, then the server
    # spawns each player's pawn and hands it over (GivePawn). Run the server
    # with short RoleSelectionTime/DeploymentTime to keep this quick.
    A.send_join_team(0)
    B.send_join_team(0)
    time.sleep(1.5)                                          # role selection starts ~0.8s after
    A.send_int_rpc(REP_SELECTROLE, 1)                        # T0F0R1, Fireteam Leader
    time.sleep(0.3)
    A.send_ready(True)
    B.send_ready(True)
    deadline = time.time() + 30
    while time.time() < deadline and not all(
            c.gave_pawn and OWN_PAWN_CH in c.ch_class for c in (A, B)):
        time.sleep(0.1)
    time.sleep(0.6)                                          # other players' pawns open next tick
    PAWN_OBJ = A.ch_class.get(OWN_PAWN_CH)
    print(f"A channels: {sorted(A.ch_class)[:12]}...  B channels: {sorted(B.ch_class)[:14]}...  "
          f"pawn obj={PAWN_OBJ}; possessed A={A.gave_pawn} B={B.gave_pawn}")
    PRI_OBJ = A.ch_class.get(10)

    def remote_channels(c):
        """Other players' PRI/Pawn channels (round actors also live >= 13)."""
        with c.lock:
            return sorted(ch for ch, obj in c.ch_class.items()
                          if ch >= FIRST_REMOTE_CH and obj in (PAWN_OBJ, PRI_OBJ))

    x0, y0, z0 = 11385.0, -3308.0, 30.0
    t0 = time.time()
    ts = 1.0
    while time.time() - t0 < a.seconds:
        ts += 0.023
        x = x0 + 300.0 * (time.time() - t0)   # 300 uu/s along +X
        A.send_move(ts, (x, y0, z0), yaw=16384)
        B.send_move(ts, (x0 - 500, y0, z0), yaw=0)
        time.sleep(0.023)
    if a.reorder:
        # crouch(False) as ChSeq k+1 first: the server must hold it until
        # k arrives, then apply True, False -- and drop the duplicate k.
        k = A.next_chseq(PC_CH)
        k1 = A.next_chseq(PC_CH)
        A.send_crouch(False, chseq=k1)
        time.sleep(0.3)
        A.send_crouch(True, chseq=k)
        time.sleep(0.3)
        A.send_crouch(True, chseq=k)                         # retransmission
        time.sleep(0.9)
    else:
        A.send_crouch(True)
        time.sleep(0.6)                                      # > one 200ms reliable tick
        A.send_crouch(False)
        time.sleep(0.6)
    if a.round:
        print(f"A round-phase RPCs: {A.phase_rpcs}")
    if a.loss:
        time.sleep(1.5)                                      # let retransmissions land
    A.stop = True
    remote_b = remote_channels(B)
    closed_ok = True
    if a.leave_test:
        remote_before = remote_channels(B)
        t_end = time.time() + a.leave_test
        while time.time() < t_end:   # B keeps talking so only A times out
            ts += 0.023
            B.send_move(ts, (x0 - 500, y0, z0), yaw=0)
            time.sleep(0.05)
        remote_after = remote_channels(B)
        closed_ok = bool(remote_before) and not remote_after
        print(f"leave test: B remote channels before={remote_before} after={remote_after}")
    B.stop = True

    locs = [l for ch, l in B.remote_locs]
    xs = [l[0] for l in locs]
    print(f"B sees remote channels {sorted(remote_b)}; {len(locs)} remote Location updates")
    if locs:
        print(f"  first {locs[0]}  last {locs[-1]}")
    print(f"A received {A.acks} ClientAckGoodMove; B received {B.acks}")
    ok = (len(remote_b) >= 2 and len(locs) >= 10 and xs[-1] - xs[0] > 0.5 * 300 * a.seconds
          and all(abs(l[1] - y0) <= 1 for l in locs) and A.acks >= 10)
    print(f"A postures received: {A.postures}; B saw A's bIsCrouched: {B.remote_crouch}")
    if a.reorder:
        # Applied back to back in one tick, so B can't see the crouch; A
        # must get exactly one reply per distinct RPC, in sequence order.
        stance_ok = A.postures == ["EndWalk", "EndCrouch"]
    else:
        stance_ok = A.postures[:2] == ["EndWalk", "EndCrouch"] and B.remote_crouch[:2] == [1, 0]
    for c in (A, B):
        print(f"{c.name}: PlayerController opens (ch, NetPlayerIndex): {c.pc_opens}")
    # Exactly one PC, on the PC channel, NetPlayerIndex 0 -- anything else
    # leaves the real client on its "Connecting" screen (doc sec 26).
    pc_ok = all(c.pc_opens == [(PC_CH, 0)] for c in (A, B))
    print(f"A got {A.team_messages} TeamMessage(s) after ServerJoinTeam(0)")
    pc_ok = pc_ok and A.team_messages >= 1 and A.gave_pawn and B.gave_pawn
    if a.round:
        names = [n for _, n in A.phase_rpcs]
        want = ["EnterPrePlanningTool", "GotoState", "DeploymentWarning", "ClosePrePlanningTool"]
        pc_ok = pc_ok and names == want
    rel_ok = pc_ok
    for c in (A, B):
        g = c.gaps()
        acked = len(c.server_acked)
        print(f"{c.name}: received {c.received} pkts, dropped {c.dropped}, "
              f"reliable dups {c.dup_in}, held out-of-order {c.held_in}, unfilled gaps {g}; "
              f"server acked {acked}/{c.seq} of our packets")
        rel_ok = rel_ok and not g and acked >= 0.9 * c.seq * (1 - a.loss)
    ok = ok and closed_ok and stance_ok and rel_ok
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
