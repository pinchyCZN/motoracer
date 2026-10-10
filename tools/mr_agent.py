"""Talk to the running mracer.exe debug agent.

The game opens \\.\pipe\mracer_agent while it runs (src/agent.c).
This is the client half: it sends one command line and prints the
reply.

    python tools/mr_agent.py STATUS
    python tools/mr_agent.py GET_MEM name=s_race_drive len=64
    python tools/mr_agent.py GET_MEM name=s_race_racers offset=0x2e1d len=4
    python tools/mr_agent.py GET_MEM name=One obj=libucrt:87disp.obj len=8

Options:
    -x      print the raw hex instead of a hex dump
    -f FMT  unpack the bytes with struct.unpack, e.g. -f 12f

Symbol names are the C names; the map's leading '_' is stripped.
obj= only matters for duplicate statics.

NEEDS A Debug|Win32 BUILD. The agent reads its symbols from the
linker .map beside the .exe, and that is the only configuration that
links one; under Release STATUS reports symbols=0.

This is the standalone client. The MCP route to the same agent is
tools/bridge_mcp_mrhook.py, tools mracer_status and mracer_get_mem.
"""
import argparse
import struct
import sys

PIPE = r'\.\pipe\mracer_agent'


def send(line, timeout_s=20.0):
    """One command, one reply. Returns the reply text."""
    import time

    deadline = time.time() + timeout_s
    while True:
        try:
            f = open(PIPE, 'r+b', buffering=0)
            break
        except OSError as exc:
            if time.time() > deadline:
                raise SystemExit('cannot open %s: %s' % (PIPE, exc))
            time.sleep(0.1)

    with f:
        f.write((line.rstrip('\n') + '\n').encode('ascii'))
        buf = b''
        want = None
        while True:
            # THE HEADER DECLARES ITS OWN PAYLOAD as lines=N, so this
            # does not have to know which command was sent.
            if want is None:
                nl = buf.find(b'\n')
                if nl >= 0:
                    head = buf[:nl].decode('ascii', 'replace')
                    want = 0
                    for tok in head.split():
                        if tok.startswith('lines='):
                            try:
                                want = max(0, int(tok[6:]))
                            except ValueError:
                                want = 0
            if want is not None and buf.count(b'\n') >= want + 1:
                break
            b = f.read(65536)
            if not b:
                break
            buf += b
    return buf.decode('ascii', 'replace')


def hexdump(data, base):
    out = []
    for off in range(0, len(data), 16):
        row = data[off:off + 16]
        hx = ' '.join('%02x' % c for c in row)
        txt = ''.join(chr(c) if 32 <= c < 127 else '.' for c in row)
        out.append('%08x  %-47s  %s' % (base + off, hx, txt))
    return '\n'.join(out)


def main():
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument('words', nargs='+', help='the command line to send')
    ap.add_argument('-x', action='store_true', help='raw hex')
    ap.add_argument('-f', metavar='FMT', help='struct.unpack format')
    args = ap.parse_args()

    reply = send(' '.join(args.words))
    lines = reply.split('\n')
    head = lines[0] if lines else ''

    if not head.startswith('OK') or len(lines) < 2 or not lines[1]:
        print(reply.rstrip())
        return 0 if head.startswith('OK') else 1

    print(head)
    data = bytes.fromhex(lines[1].strip())
    addr = 0
    for tok in head.split():
        if tok.startswith('addr='):
            addr = int(tok[5:], 16)

    if args.f:
        vals = struct.unpack_from('<' + args.f, data)
        print('  ' + '  '.join(repr(v) for v in vals))
    elif args.x:
        print(lines[1].strip())
    else:
        print(hexdump(data, addr))
    return 0


if __name__ == '__main__':
    sys.exit(main())
