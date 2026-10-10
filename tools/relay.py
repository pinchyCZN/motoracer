#!/usr/bin/env python3
"""MotoRacer netplay relay. See README-relay.txt.

    python tools/relay.py [--port 27500] [--bind ADDR] [--verbose]
"""

import argparse
import logging
import selectors
import socket
import struct
import sys
import time

HDR = 8
PAYLOAD_MAX = 1024
FRAME_MAX = HDR + PAYLOAD_MAX
ROOM_MAX = 9                  # 0xcb carries 9 slots; netplay.txt 2.1
NAME_MAX = 9                  # racer+0x2dd4 playerName
LIST_MAX = 24                 # rooms per ROOM_LIST
VERSION = 2
IDLE_SECONDS = 60
RATE_FRAMES = 400             # per peer per second

FLAG_RELIABLE, FLAG_BROADCAST = 0x01, 0x02
(PAYLOAD, HELLO, WELCOME, PEER_JOINED, PEER_LEFT, ROOM_GONE, REFUSED,
 KEEPALIVE, LIST_REQ, ROOM_LIST, CREATE_ROOM, JOIN_ROOM, LEAVE_ROOM,
 ROOM_STATE) = range(14)
REFUSE_VERSION, REFUSE_NOROOM, REFUSE_FULL, REFUSE_STATE = 1, 2, 3, 4
WAITING, RACING = 0, 1
MSG_JOIN, MSG_LOBBY, MSG_READY, MSG_STATE = 1, 3, 4, 8
STATE_SLOT = 4                # MR_NetMsgState.slot, after the head

log = logging.getLogger('relay')


def frame(kind, flags, dst, src, payload=b''):
    return struct.pack('<BBHHH', kind, flags, dst, src,
                       len(payload)) + payload


def fixed(text, n):
    b = text.encode('ascii', 'replace')[:n - 1]
    return b + b'\0' * (n - len(b))


class Peer:
    def __init__(self, sock, addr):
        self.sock, self.addr = sock, addr
        self.rx, self.tx = bytearray(), bytearray()
        self.room, self.pid = None, 0
        self.name = ''
        self.hello = False
        self.last = time.monotonic()
        self.window, self.count = int(self.last), 0

    def send(self, data, droppable=False):
        # An unreliable frame is DROPPED rather than queued once the
        # backlog is real - netplay-relay.txt 2.4.
        if droppable and len(self.tx) > FRAME_MAX * 4:
            return
        self.tx += data

    def throttled(self):
        now = int(time.monotonic())
        if now != self.window:
            self.window, self.count = now, 0
        self.count += 1
        return self.count > RATE_FRAMES


class Room:
    next_id = 1

    def __init__(self, host):
        self.rid = Room.next_id
        Room.next_id += 1
        self.peers = {}
        self.host = host
        self.state = WAITING
        self.name = host.name

    def add(self, peer):
        for pid in range(1, ROOM_MAX):
            if pid not in self.peers:
                self.peers[pid] = peer
                peer.pid, peer.room = pid, self
                return pid
        return 0



class MirrorPeer(Peer):
    """A virtual guest that rides the host's own telemetry back at it.

    Debug only, and the one place the relay reads a game payload.
    See README-relay.txt.
    """

    def __init__(self, host, name, delay):
        Peer.__init__(self, None, ('mirror', 0))
        self.host, self.name, self.delay = host, name, delay
        self.hello, self.joined = True, False
        self.held = []

    # The relay thinks it is SENDING to us; react instead of queueing.
    def send(self, data, droppable=False):
        kind, _, _, _, length = struct.unpack_from('<BBHHH', data)
        if kind != PAYLOAD or length < 4:
            return
        body = data[HDR:HDR + length]
        mtype = struct.unpack_from('<H', body)[0]
        if mtype == MSG_LOBBY:
            self.announce()
        elif mtype == MSG_STATE and length >= STATE_SLOT + 4:
            self.echo(body)

    def to_host(self, body):
        self.host.send(frame(PAYLOAD, FLAG_RELIABLE, self.host.pid,
                             self.pid, body))

    def announce(self, again=False):
        # ONLY AFTER THE FIRST LOBBY. menu_net_on_join drops a JOIN
        # that arrives before the host has published one.
        if self.joined and not again:
            return
        self.joined = True
        self.to_host(struct.pack('<HHHHi', MSG_JOIN, 0, VERSION, 0, 0)
                     + fixed(self.name, NAME_MAX) + b'\0' * 3)
        self.to_host(struct.pack('<HHii', MSG_READY, 0, 1, 0))
        log.info('mirror "%s" joined room %d as peer %d',
                 self.name, self.room.rid, self.pid)

    def echo(self, body):
        out = bytearray(body)
        struct.pack_into('<i', out, STATE_SLOT, self.pid)
        self.held.append(bytes(out))
        if len(self.held) > self.delay:
            if self.joined == 1:
                self.joined = 2
                log.info('mirror echoing state, %d frame(s) behind',
                         self.delay)
            self.to_host(self.held.pop(0))


class Relay:
    def __init__(self, port, mirror='', delay=6, bind=''):
        self.mirror, self.delay = mirror, delay
        self.rooms, self.sel = {}, selectors.DefaultSelector()
        self.srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.srv.bind((bind, port))
        self.srv.listen(16)
        self.srv.setblocking(False)
        self.sel.register(self.srv, selectors.EVENT_READ, None)
        log.info('listening on %s:%d', bind or '0.0.0.0', port)

    # -- outbound ------------------------------------------------------
    def welcome(self, peer):
        room = peer.room
        if room is None:
            body = struct.pack('<HBHHB', peer.pid, 0, 0, 0, 0)
        else:
            others = [p for p in room.peers if p != peer.pid]
            body = struct.pack('<HBHHB', peer.pid,
                               1 if peer is room.host else 0,
                               room.host.pid, room.rid, len(others))
            for pid in others:
                body += struct.pack('<H', pid)
        peer.send(frame(WELCOME, FLAG_RELIABLE, peer.pid, 0, body))

    def room_list(self, peer):
        rooms = list(self.rooms.values())[:LIST_MAX]
        body = struct.pack('<B', len(rooms))
        for room in rooms:
            body += struct.pack('<HBB', room.rid, room.state,
                                len(room.peers))
            body += fixed(room.name, NAME_MAX)
        peer.send(frame(ROOM_LIST, FLAG_RELIABLE, peer.pid, 0, body))

    def announce_list(self):
        # Everyone browsing sees a room appear or go without asking.
        for key in list(self.sel.get_map().values()):
            other = key.data
            if other is not None and other.hello and other.room is None:
                self.room_list(other)

    def refuse(self, peer, reason):
        peer.send(frame(REFUSED, FLAG_RELIABLE, 0, 0, bytes([reason])))

    # -- inbound -------------------------------------------------------
    def on_hello(self, peer, body):
        if len(body) < 2 + NAME_MAX or body[0] != VERSION:
            self.refuse(peer, REFUSE_VERSION)
            return
        peer.name = body[2:2 + NAME_MAX].split(b'\0')[0].decode(
            'ascii', 'replace')
        peer.hello = True
        log.info('%s connected as "%s"', peer.addr, peer.name)
        self.welcome(peer)
        self.room_list(peer)

    def on_create(self, peer):
        if peer.room is not None:
            return
        room = Room(peer)
        self.rooms[room.rid] = room
        room.add(peer)
        log.info('room %d "%s" opened by %s', room.rid, room.name,
                 peer.addr)
        self.welcome(peer)
        if self.mirror:
            ghost = MirrorPeer(peer, self.mirror, self.delay)
            if room.add(ghost):
                peer.send(frame(PEER_JOINED, FLAG_RELIABLE, 0, 0,
                                struct.pack('<H', ghost.pid)))
        self.announce_list()

    def on_join(self, peer, body):
        if peer.room is not None or len(body) < 2:
            return
        rid = struct.unpack_from('<H', body)[0]
        room = self.rooms.get(rid)
        if room is None:
            self.refuse(peer, REFUSE_NOROOM)
            return
        if room.state != WAITING:
            self.refuse(peer, REFUSE_STATE)
            return
        if not room.add(peer):
            self.refuse(peer, REFUSE_FULL)
            return
        log.info('peer %d "%s" joined room %d', peer.pid, peer.name, rid)
        note = frame(PEER_JOINED, FLAG_RELIABLE, 0, 0,
                     struct.pack('<H', peer.pid))
        for pid, other in room.peers.items():
            if pid != peer.pid:
                other.send(note)
        self.welcome(peer)
        self.announce_list()

    def on_room_state(self, peer, body):
        room = peer.room
        if room is None or peer is not room.host or len(body) < 1:
            return
        room.state = RACING if body[0] else WAITING
        # A HELD FRAME OUTLIVES THE RACE otherwise, and the next
        # one opens by replaying the end of the last.
        for other in room.peers.values():
            if isinstance(other, MirrorPeer):
                other.held = []
                # THE NAME IS RE-SENT ONCE PER RACE. The host may
                # rebuild its lobby between races, and a mirror
                # that announced only once is COMPUTER from then
                # on. Not on every LOBBY: the host republishes on
                # each JOIN, so that would ping-pong.
                if room.state == RACING:
                    other.announce(again=True)
        log.info('room %d now %s', room.rid,
                 'racing' if room.state else 'waiting')
        self.announce_list()

    def on_payload(self, peer, kind, flags, dst, body):
        # The relay NEVER reads body. It stamps src, because the game's
        # echo guard trusts it - netplay-relay.txt 2.3.
        if peer.room is None:
            return
        out = frame(kind, flags, dst, peer.pid, body)
        droppable = not (flags & FLAG_RELIABLE)
        if flags & FLAG_BROADCAST:
            for pid, other in peer.room.peers.items():
                if pid != peer.pid:
                    other.send(out, droppable)
        else:
            other = peer.room.peers.get(dst)
            if other is not None:
                other.send(out, droppable)

    def on_frame(self, peer, kind, flags, dst, body):
        if kind == HELLO:
            if not peer.hello:
                self.on_hello(peer, body)
            return
        if not peer.hello:
            return                      # nothing before a HELLO
        if kind == PAYLOAD:
            self.on_payload(peer, kind, flags, dst, body)
        elif kind == LIST_REQ:
            self.room_list(peer)
        elif kind == CREATE_ROOM:
            self.on_create(peer)
        elif kind == JOIN_ROOM:
            self.on_join(peer, body)
        elif kind == LEAVE_ROOM:
            self.leave_room(peer)
            self.welcome(peer)
            self.room_list(peer)
        elif kind == ROOM_STATE:
            self.on_room_state(peer, body)
        # Everything else is relay-to-client only; a client sending one
        # is confused, not dangerous. Ignore it.

    # -- plumbing ------------------------------------------------------
    def leave_room(self, peer):
        room, pid = peer.room, peer.pid
        peer.room, peer.pid = None, 0
        if room is None:
            return
        # Removed by identity, not by the id we just cleared.
        for slot, other in list(room.peers.items()):
            if other is peer:
                del room.peers[slot]
        if peer is room.host:
            log.info('room %d closed, host left', room.rid)
            # UNLISTED FIRST. Telling the guests to rebrowse while the
            # dying room is still in self.rooms shows them a game that
            # cannot be joined.
            self.rooms.pop(room.rid, None)
            gone = frame(ROOM_GONE, FLAG_RELIABLE, 0, 0)
            for other in list(room.peers.values()):
                other.send(gone)
                other.room, other.pid = None, 0
                self.room_list(other)
            room.peers.clear()
        else:
            note = frame(PEER_LEFT, FLAG_RELIABLE, 0, 0,
                         struct.pack('<H', pid))
            for other in room.peers.values():
                other.send(note)
        self.announce_list()

    def drop(self, peer, why):
        self.leave_room(peer)
        if peer.sock is None:
            return
        log.info('peer %s dropped: %s', peer.addr, why)
        try:
            self.sel.unregister(peer.sock)
        except KeyError:
            pass
        peer.sock.close()

    def readable(self, peer):
        try:
            got = peer.sock.recv(4096)
        except (ConnectionError, OSError):
            got = b''
        if not got:
            self.drop(peer, 'closed')
            return
        peer.last = time.monotonic()
        peer.rx += got

        # TCP is a stream: a frame can arrive split or several at once.
        while len(peer.rx) >= HDR:
            kind, flags, dst, _, length = struct.unpack_from('<BBHHH',
                                                             peer.rx)
            if length > PAYLOAD_MAX:
                self.drop(peer, 'frame of %d bytes' % length)
                return
            if len(peer.rx) < HDR + length:
                break
            body = bytes(peer.rx[HDR:HDR + length])
            del peer.rx[:HDR + length]
            if peer.throttled():
                self.drop(peer, 'rate limit')
                return
            self.on_frame(peer, kind, flags, dst, body)

    def flush(self, peer):
        if not peer.tx:
            return
        try:
            sent = peer.sock.send(peer.tx)
        except BlockingIOError:
            return
        except (ConnectionError, OSError):
            self.drop(peer, 'send failed')
            return
        del peer.tx[:sent]

    def run(self):
        while True:
            for key, _ in self.sel.select(timeout=1.0):
                if key.data is None:
                    sock, addr = self.srv.accept()
                    sock.setblocking(False)
                    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                    peer = Peer(sock, addr)
                    self.sel.register(sock, selectors.EVENT_READ, peer)
                else:
                    self.readable(key.data)
            now = time.monotonic()
            for key in list(self.sel.get_map().values()):
                peer = key.data
                if peer is None:
                    continue
                if now - peer.last > IDLE_SECONDS:
                    self.drop(peer, 'idle')
                else:
                    self.flush(peer)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--port', type=int, default=27500)
    ap.add_argument('--bind', metavar='ADDR', default='',
                    help='interface to listen on; default every one.'
                         ' 127.0.0.1 pairs with an SSH tunnel')
    ap.add_argument('--verbose', action='store_true')
    ap.add_argument('--mirror', metavar='NAME', default='',
                    help='add a virtual peer echoing the host')
    ap.add_argument('--mirror-delay', type=int, default=6,
                    metavar='FRAMES', help='how far it trails')
    a = ap.parse_args()
    logging.basicConfig(level=logging.DEBUG if a.verbose else logging.INFO,
                        format='%(asctime)s %(message)s',
                        datefmt='%H:%M:%S')
    try:
        Relay(a.port, a.mirror[:NAME_MAX - 1], a.mirror_delay,
              a.bind).run()
    except KeyboardInterrupt:
        return 0
    return 0


if __name__ == '__main__':
    sys.exit(main())
