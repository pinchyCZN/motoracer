"""Dump DATA\\BESTSCOR.DAT - the all-time track records.

    python tools/bestscor.py [PATH] [--all] [--raw]

Default PATH is decomp/DATA/BESTSCOR.DAT.  Prints one line per entry:
the key, record 0's holder and lap time, and a MINE flag on any entry
carrying a holder that is not DSI - the file is written at run time, so
those rows are somebody's play and not shipped data.  --all also prints
the eight table records under each key.

A holder that is not DSI is a real person's name, so it prints as
(player) unless --raw is given.  Output from here has been pasted into
docs/ before; masking by default is what keeps it out.

Layout and the key suffixes: docs/formats/bestscor.txt.
"""

import sys

MAGIC = b"00.1"
KEY_LEN = 0x20
REC = 0xF9
PER_KEY = 9
INIT_OFF = 9
RACE_OFF = 0xF1
LAP_OFF = 0xF5
NO_TIME = 0x7F7F7F7F
STRIDE = KEY_LEN + PER_KEY * REC

EMPTY = chr(0x7F)


def cstr(b):
    return b.split(b"\0")[0].decode("latin1")


def u32(b, off):
    return int.from_bytes(b[off:off + 4], "little")


def ms(v):
    return "-" if v == NO_TIME else "%d" % v


def theirs(holder):
    """True for a shipped row - empty, sentinel, or a DSI name."""
    return not holder or holder.startswith("DSI") or holder[0] == EMPTY


def shown(holder, raw):
    return holder if raw or theirs(holder) else "(player)"


def records(blob):
    if blob[:4] != MAGIC:
        sys.exit("bad magic %r" % blob[:4])
    for off in range(4, len(blob) - STRIDE + 1, STRIDE):
        key = cstr(blob[off:off + KEY_LEN])
        recs = [blob[off + KEY_LEN + i * REC:off + KEY_LEN + (i + 1) * REC]
                for i in range(PER_KEY)]
        yield key, recs


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    every = "--all" in sys.argv
    raw = "--raw" in sys.argv
    path = args[0] if args else "decomp/DATA/BESTSCOR.DAT"
    blob = open(path, "rb").read()

    total = mine = 0
    for key, recs in records(blob):
        total += 1
        holders = [cstr(r[:INIT_OFF]) for r in recs]
        dirty = any(not theirs(h) for h in holders)
        mine += dirty
        print("%-24s %-9s %8s %s"
              % (key, shown(holders[0], raw), ms(u32(recs[0], LAP_OFF)),
                 "MINE" if dirty else ""))
        if not every:
            continue
        for i, r in enumerate(recs[1:], 1):
            print("    %d %-9s %-4s race %-8s lap %s"
                  % (i, shown(holders[i], raw), cstr(r[INIT_OFF:KEY_LEN]),
                     ms(u32(r, RACE_OFF)), ms(u32(r, LAP_OFF))))

    print("\n%d entries, %d with a holder that is not DSI" % (total, mine))


if __name__ == "__main__":
    main()
