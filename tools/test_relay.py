#!/usr/bin/env python3
"""Check the relay's mirror peer. See README-relay.txt 7.

    python tools/test_relay.py

A FRESH PORT PER RUN, deliberately: Windows SO_REUSEADDR lets a
second socket bind a port that is already listening, so an
orphaned relay silently steals the connection and every check
fails with nothing in the log.
"""

import socket, struct, subprocess, sys, time

PY = sys.executable
HDR = 8
PAYLOAD, HELLO, WELCOME, PEER_JOINED = 0, 1, 2, 3
CREATE_ROOM, ROOM_STATE, JOIN_ROOM, LEAVE_ROOM = 10, 13, 11, 12
REFUSED, ROOM_LIST = 6, 9
REFUSE_STATE = 4
MSG_JOIN, MSG_LOBBY, MSG_READY, MSG_STATE = 1, 3, 4, 8
FLAG_RELIABLE, FLAG_BROADCAST = 1, 2
import random
PORT = random.randint(29000, 31000)


def frame(kind, flags, dst, src, body=b''):
    return struct.pack('<BBHHH', kind, flags, dst, src, len(body)) + body


def fixed(t, n):
    b = t.encode('ascii')[:n - 1]
    return b + bytes(n - len(b))


class Conn:
    def __init__(self, port):
        self.s = socket.create_connection(('127.0.0.1', port), 3)
        self.s.settimeout(1.5)
        self.buf = b''

    def send(self, data):
        self.s.sendall(data)

    def frames(self, want, seconds=1.5):
        end = time.time() + seconds
        out = []
        while time.time() < end and len(out) < want:
            try:
                got = self.s.recv(4096)
            except socket.timeout:
                break
            if not got:
                break
            self.buf += got
            while len(self.buf) >= HDR:
                k, f, d, sr, n = struct.unpack_from('<BBHHH', self.buf)
                if len(self.buf) < HDR + n:
                    break
                out.append((k, f, d, sr, self.buf[HDR:HDR + n]))
                self.buf = self.buf[HDR + n:]
        return out


def payloads(frames):
    out = []
    for k, f, d, sr, body in frames:
        if k == PAYLOAD and len(body) >= 2:
            out.append((struct.unpack_from('<H', body)[0], sr, body))
    return out


def state_msg(slot, x, tick=1):
    # head, slot, tick, speed, aimDir[3], position[3], then 7 ints,
    # crash, crashDir[3] - MR_NetMsgState, pack(1)
    return (struct.pack('<HHiIf', MSG_STATE, 0, slot, tick, 50.0)
            + struct.pack('<3f', 0.0, 0.0, 1.0)
            + struct.pack('<3f', x, 2.0, 3.0)
            + struct.pack('<8i', 0, 0, 0, 0, 0, 0, 0, 0)
            + struct.pack('<3f', 0.0, 0.0, 0.0))


def run(delay):
    global PORT
    PORT += 1
    p = subprocess.Popen([PY, 'tools/relay.py', '--port', str(PORT),
                          '--mirror', 'GHOST', '--mirror-delay', str(delay)],
                         stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL)
    time.sleep(0.8)
    fails = []
    try:
        c = Conn(PORT)
        c.send(frame(HELLO, FLAG_RELIABLE, 0, 0,
                     struct.pack('<BB', 2, 0) + fixed('HOST', 9)))
        got = c.frames(2)
        if not any(g[0] == WELCOME for g in got):
            fails.append('no WELCOME')

        c.send(frame(CREATE_ROOM, FLAG_RELIABLE, 0, 0))
        got = c.frames(3)
        joined = [g for g in got if g[0] == PEER_JOINED]
        if not joined:
            fails.append('mirror did not announce itself as a peer')
        else:
            pid = struct.unpack_from('<H', joined[0][4])[0]
            if pid != 2:
                fails.append('mirror pid %d, expected 2' % pid)

        # the host publishes a lobby; the mirror should JOIN and READY
        c.send(frame(PAYLOAD, FLAG_RELIABLE | FLAG_BROADCAST, 0, 0,
                     struct.pack('<HHi', MSG_LOBBY, 0, 1)))
        msgs = payloads(c.frames(2))
        kinds = [m[0] for m in msgs]
        if MSG_JOIN not in kinds:
            fails.append('mirror sent no JOIN, got %s' % kinds)
        if MSG_READY not in kinds:
            fails.append('mirror sent no READY, got %s' % kinds)
        for mtype, src, body in msgs:
            if mtype == MSG_JOIN:
                ver = struct.unpack_from('<H', body, 4)[0]
                name = body[12:21].split(b'\0')[0].decode()
                if ver != 2:
                    fails.append('JOIN version %d' % ver)
                if name != 'GHOST':
                    fails.append('JOIN name %r' % name)
                if src != 2:
                    fails.append('JOIN src %d, expected 2' % src)

        # one state per "frame"; with delay=D the first D are held
        for i in range(delay + 1):
            c.send(frame(PAYLOAD, FLAG_BROADCAST, 0, 0,
                         state_msg(1, float(i))))
        msgs = [m for m in payloads(c.frames(delay + 2))
                if m[0] == MSG_STATE]
        if len(msgs) != 1:
            fails.append('delay %d released %d states, expected 1'
                         % (delay, len(msgs)))
        else:
            mtype, src, body = msgs[0]
            slot = struct.unpack_from('<i', body, 4)[0]
            x = struct.unpack_from('<f', body, 28)[0]
            if slot != 2:
                fails.append('echoed slot %d, expected 2' % slot)
            if src != 2:
                fails.append('echoed src %d, expected 2' % src)
            if abs(x - 0.0) > 1e-6:
                fails.append('released x=%g, expected the oldest (0)' % x)
        c.s.close()
    finally:
        p.terminate()
        p.wait(timeout=5)
    return fails


def restarts():
    """Three races in one room: the echo and the name must survive.

    THE NAME IS THE FRAGILE HALF.  The mirror announced itself once
    per room, so a host that rebuilt its lobby between races showed
    the peer as COMPUTER from then on - seen live, racer+0x2dd4 read
    back as "COMPUTER" while states were still arriving.
    """
    global PORT
    PORT += 1
    p = subprocess.Popen([PY, "tools/relay.py", "--port", str(PORT),
                          "--mirror", "GHOST", "--mirror-delay", "2"],
                         stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL)
    time.sleep(0.8)
    fails = []
    tick = 1000
    try:
        c = Conn(PORT)
        c.send(frame(HELLO, FLAG_RELIABLE, 0, 0,
                     struct.pack("<BB", 2, 0) + fixed("HOST", 9)))
        c.frames(2)
        c.send(frame(CREATE_ROOM, FLAG_RELIABLE, 0, 0))
        c.frames(3)
        c.send(frame(PAYLOAD, FLAG_RELIABLE | FLAG_BROADCAST, 0, 0,
                     struct.pack("<HHi", MSG_LOBBY, 0, 1)))
        c.frames(2)
        for race in (1, 2, 3):
            c.send(frame(ROOM_STATE, FLAG_RELIABLE, 0, 0, bytes([1])))
            joins = [m for m in payloads(c.frames(2, 0.5))
                     if m[0] == MSG_JOIN]
            if not joins:
                fails.append("race %d: no re-JOIN, the peer stays"
                             " COMPUTER" % race)
            echoed = 0
            for _ in range(10):
                tick += 1
                c.send(frame(PAYLOAD, FLAG_BROADCAST, 0, 0,
                             state_msg(1, float(tick), tick)))
                echoed += len([m for m in payloads(c.frames(1, 0.12))
                               if m[0] == MSG_STATE])
            if echoed == 0:
                fails.append("race %d: no echo" % race)
            c.send(frame(ROOM_STATE, FLAG_RELIABLE, 0, 0, bytes([0])))
            c.frames(1, 0.3)
        c.s.close()
    finally:
        p.terminate()
        p.wait(timeout=5)
    return fails


def room_state():
    """A guest must be able to rejoin once the host is back in the
    lobby, and must still be refused mid-race.

    BEGIN sent ROOM_STATE(1) and nothing ever sent 0, so one race
    made a room unjoinable for its whole life - the host had to
    leave and recreate it.  page8_enter sends WAITING now.
    """
    global PORT
    PORT += 1
    p = subprocess.Popen([PY, "tools/relay.py", "--port", str(PORT)],
                         stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL)
    time.sleep(0.8)
    fails = []
    try:
        host = Conn(PORT)
        host.send(frame(HELLO, FLAG_RELIABLE, 0, 0,
                        struct.pack("<BB", 2, 0) + fixed("HOST", 9)))
        host.frames(2)
        host.send(frame(CREATE_ROOM, FLAG_RELIABLE, 0, 0))
        host.frames(2)

        def guest():
            g = Conn(PORT)
            g.send(frame(HELLO, FLAG_RELIABLE, 0, 0,
                         struct.pack("<BB", 2, 0) + fixed("GUEST", 9)))
            got = g.frames(3) + g.frames(1, 0.3)
            lists = [b for k, f, d, sr, b in got if k == ROOM_LIST]
            rid, state = 0, -1
            if lists and lists[-1][0] >= 1:
                rid, state, _ = struct.unpack_from("<HBB", lists[-1], 1)
            g.send(frame(JOIN_ROOM, FLAG_RELIABLE, 0, 0,
                         struct.pack("<H", rid)))
            fr = g.frames(2, 0.6)
            ok = any(k == WELCOME for k, f, d, sr, b in fr)
            why = [b[0] for k, f, d, sr, b in fr if k == REFUSED]
            if ok:
                g.send(frame(LEAVE_ROOM, FLAG_RELIABLE, 0, 0))
                g.frames(1, 0.3)
            g.s.close()
            return ok, state, (why[0] if why else 0)

        ok, state, _ = guest()
        if not ok:
            fails.append("cannot join a fresh room")

        host.send(frame(ROOM_STATE, FLAG_RELIABLE, 0, 0, bytes([1])))
        host.frames(1, 0.3)
        host.send(frame(ROOM_STATE, FLAG_RELIABLE, 0, 0, bytes([0])))
        host.frames(1, 0.3)
        ok, state, _ = guest()
        if not ok:
            fails.append("cannot rejoin after the host returns to lobby")
        if state != 0:
            fails.append("still listed racing, state %d" % state)

        host.send(frame(ROOM_STATE, FLAG_RELIABLE, 0, 0, bytes([1])))
        host.frames(1, 0.3)
        ok, state, why = guest()
        if ok:
            fails.append("joined a racing room")
        elif why != REFUSE_STATE:
            fails.append("refused mid-race with %d, wanted %d"
                         % (why, REFUSE_STATE))
        host.s.close()
    finally:
        p.terminate()
        p.wait(timeout=5)
    return fails


bad = []
for d in (0, 3):
    f = run(d)
    print('delay %d: %s' % (d, 'ok' if not f else '; '.join(f)))
    bad += f
f = restarts()
print('restarts:  %s' % ('ok' if not f else '; '.join(f)))
bad += f
f = room_state()
print('room state: %s' % ('ok' if not f else '; '.join(f)))
bad += f
print('FAILED' if bad else 'all relay checks passed')
sys.exit(1 if bad else 0)
