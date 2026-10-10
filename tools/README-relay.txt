MotoRacer PC Reverse Engineering                           Analysis Note


                      THE TEST RELAY, relay.py


Abstract

   A dumb relay for local netplay testing.  It implements the frame
   format of docs/subsystems/netplay-relay.txt 2 and nothing else: it
   never reads a payload, keeps no game state, and does not decide who
   may race.

   Python rather than a second MSVC target on purpose.  It is a test
   fixture, it changes often while the protocol settles, and a
   scripting language is far easier to instrument than an exe we would
   also have to build.  A shipping relay can be rewritten later; the
   wire format is what has to stay still.

Table of Contents

   1. Running it
   2. Testing two players on one machine
   3. What it enforces
   4. What it deliberately does not do
   5. The mirror peer - debugging a remote bike alone
   6. Tests
   7. Running it on a VPS
      7.1. C is not worth writing
   8. The systemd unit
      8.1. Letting only one address in

1.  Running it

       python tools/relay.py                  port 27500
       python tools/relay.py --port 27599     another port
       python tools/relay.py --verbose        every frame
       python tools/relay.py --bind 127.0.0.1 loopback only

   Single-threaded and select-based, so a breakpoint stops the whole
   relay and nothing races while you look at it.

2.  Testing two players on one machine

   Three processes: the relay, and two copies of the game.  The second
   copy needs its OWN FOLDER with its own DATA, because the ghost file
   and the ini are written beside the exe - see rule 10 and
   formats/timatak.txt.  Sharing one folder makes two instances fight
   over both.

       1. python tools/relay.py
       2. set RelayHost to 127.0.0.1 in both inis
       3. start copy A, host, read the five-character code
       4. start copy B, join with that code

   Loopback hides exactly the faults this design is most exposed to -
   reordering, loss and jitter are all absent - so a clean local run
   is necessary and not sufficient.

3.  What it enforces

   o a payload of 0..1024 bytes; a longer frame drops the link rather
     than being truncated
   o nine peers a room, which is the protocol's own cap
   o 400 frames a second a peer, then the link drops
   o a 60-second idle timeout
   o unreliable frames are DROPPED once a peer's backlog is real,
     never queued behind reliable traffic

4.  What it deliberately does not do

   o read any payload byte, unless --mirror is on - section 5.
     Otherwise it stamps src and routes on the header.
   o trust the src a client sends.  It overwrites it, because the
     game's echo guard trusts that field - netplay-relay.txt 2.3.
   o interpret 0xce or any other game message.  The host measures
     round trips through the relay, and that IS the path the traffic
     takes, so shortening it would make the measurement wrong rather
     than better.  journal 466.
   o survive a host leaving.  The room closes with ROOM_GONE; there is
     no host migration.

5.  The mirror peer - debugging a remote bike alone

   A netplay bug in the REMOTE half needs two machines, two logs and
   two agent pipes, and the pipe cannot easily address both.  The
   mirror collapses that to one instance:

       python tools/relay.py --mirror GHOST --mirror-delay 6

   The relay puts a virtual guest in every room that is created.  It
   has no socket and no simulation; it holds the host's own STATE
   frames, rewrites the slot to its own, and hands them back a few
   frames later.  So the host races against a bike that is riding
   the host's own telemetry.

   WHAT IT TELLS YOU.  The mirror should trace your exact line a
   fixed distance behind, upright, at your speed.  Anything else is
   a defect in build_state, on_state or racer_remote_apply_state,
   and one machine can see it:

   +--------------------------+--------------------------------------+
   | What the mirror does     | Where to look                        |
   +==========================+======================================+
   | faces the wrong way      | the heading sent - netplay-relay     |
   |                          | 3.1                                  |
   +--------------------------+--------------------------------------+
   | jumps or teleports       | the chase and the snap distance in   |
   |                          | racer_remote_apply_state             |
   +--------------------------+--------------------------------------+
   | speed jitters            | one speed feeds both fields; or the  |
   |                          | wall damp firing every frame         |
   +--------------------------+--------------------------------------+
   | drifts off your line     | moveDir, rebuilt from the last two   |
   |                          | received positions                   |
   +--------------------------+--------------------------------------+

   A DELAY, NOT AN OFFSET.  The echo is held for --mirror-delay
   frames rather than being nudged sideways, so every position the
   mirror is given is one the host actually occupied - on the road,
   inside the walls.  Offsetting it would put the bike wherever the
   offset landed and the wall response would then produce jumping of
   its own, which is the symptom being investigated.

   It reads two payload fields and no others: the message type, to
   recognise a STATE and the host's first LOBBY, and the slot it
   rewrites.  That is the only game knowledge in the file, it is
   behind a flag that is off by default, and section 4 still holds
   when the flag is unset.

   The mirror announces itself only after the host's first LOBBY,
   because menu_net_on_join drops a JOIN that arrives before the
   host has published one.

6.  Tests

       python tools/test_relay.py

   Covers the mirror: that it is added as a peer, that it sends a
   JOIN carrying the protocol version and its name, that it sends
   READY, that an echoed STATE has the slot and src rewritten, and
   that the delay releases the OLDEST frame first.

   And the ROOM STATE, which is the half that bit: a guest must be
   able to join a fresh room, must be able to REJOIN once the host
   is back in the lobby, and must still be refused mid-race with
   REFUSE_STATE.  BEGIN sends ROOM_STATE(1) and nothing used to
   send 0, so one race made a room unjoinable for its whole life
   and the host had to leave and recreate it.  page8_enter sends
   WAITING on entry now - netplay-relay.txt 2.5.

   A FRESH PORT PER RUN, deliberately.  Windows SO_REUSEADDR lets a
   second socket bind a port that is already listening, so an
   orphaned relay from an earlier run steals the connection and
   every check fails with an empty log.  That cost a debugging pass
   once already.

   [open] the handshake itself has no checked-in test.  src
   stamping, broadcast exclusion, direct addressing, a frame split
   across two sends, two in one send, both refusal paths and the
   host leaving were all exercised from a scratchpad script that was
   never kept.

7.  Running it on a VPS

   IT BINDS EVERY INTERFACE BY DEFAULT - the empty string is
   0.0.0.0, not loopback - so the moment it starts on a box with a
   public address it is reachable from the internet.  --bind picks
   one interface instead:

       python tools/relay.py --bind 127.0.0.1

   Verified: with that set, a connection to 127.0.0.1 succeeds and
   one to the machine's LAN address is refused.

7.1.  C is not worth writing

   The question comes up because the relay is the one component
   meant to run for weeks.  The arithmetic says no.

   A STATE frame is 92 bytes on the wire - an 8-byte transport
   header plus MR_NetMsgState - and each peer sends one a frame:

   +---------+----------+-------------+------------+
   | players | inbound  | relayed out | sends/sec  |
   +=========+==========+=============+============+
   | 2       | 11 KB/s  | 11 KB/s     | 120        |
   +---------+----------+-------------+------------+
   | 4       | 22 KB/s  | 65 KB/s     | 720        |
   +---------+----------+-------------+------------+
   | 9 (max) | 49 KB/s  | 388 KB/s    | 4320       |
   +---------+----------+-------------+------------+

   THE SYSCALL IS THE WORK, not the Python.  Per frame the relay
   does one struct.unpack, one 92-byte concat and one send(); a C
   rewrite removes none of the send() calls, so it would optimise
   the small half.  Memory is 8 MB on Windows and 10-14 MB for
   CPython on Debian, against 512 MB of VPS.

   WHAT ACTUALLY MATTERS FOR A LONG RUN is none of that: it needs
   restarting when it dies, and its log needs somewhere to go.
   That is section 8, and it is worth more than a rewrite.

   One real limit, noted rather than hit: the loop is select-based,
   which caps at 1024 descriptors on glibc.  The protocol caps a
   room at nine.

8.  The systemd unit

   tools/mracer-relay.service.  Install it, edit User,
   WorkingDirectory and the --bind address, then:

       cp tools/mracer-relay.service /etc/systemd/system/
       systemctl daemon-reload
       systemctl enable --now mracer-relay
       journalctl -u mracer-relay -f

   NOT ROOT.  27500 is above 1024, so the relay needs no capability:
   adduser --system --no-create-home mracer.

   RESTART=ALWAYS IS SAFE BECAUSE THERE IS NO STATE TO LOSE.  Rooms
   live in memory and the clients reconnect, so a restart costs an
   interrupted race and nothing else.  StartLimitBurst stops a
   crash-loop on a bad argument from spinning for ever.

   ProtectSystem=strict WITH NO ReadWritePaths IS DELIBERATE.  The
   relay reads one .py and writes nothing but its log, which goes to
   the journal.  Add a writable path only when that stops being
   true.

8.1.  Letting only one address in

       ufw allow from <HOME_IP> to any port 27500 proto tcp
       ufw deny 27500/tcp

   TWO THINGS TO KNOW BEFORE RELYING ON IT.

   o ONLY MACHINES AT THAT ADDRESS CAN PLAY, and netplay needs two
     peers.  One rule means testing two local instances against a
     remote relay - a real test of the path, not of playing with
     somebody else.  A second player needs a second rule.
   o A HOME ADDRESS IS USUALLY DYNAMIC.  A DHCP renew locks you out
     silently, and the symptom reads as the relay being down.

   The SSH tunnel avoids both.  --bind 127.0.0.1 on the VPS, then
   from the client:

       ssh -L 27500:localhost:27500 vps

   and point the game at 127.0.0.1:27500.  Nothing is exposed and
   there is no allowlist to go stale.  A provider-side cloud
   firewall, if there is one, needs the port opened too - ufw does
   not know about it.

