#!/usr/bin/env python3
"""Dev journal for the agent: what was done, what is open, what next.

NOT a home for facts.  A fact about the binary belongs in a Ghidra
plate, about a format or subsystem in docs/, about why the C is shaped
this way in the src/ comment.  This holds WORK STATE, which is why the
database is gitignored and disposable.  See tools/README-journal.txt
and CLAUDE.md rule 16.

    python tools/journal.py ls --status open --pri 1
    python tools/journal.py about 0x0048c6d0
    python tools/journal.py add --kind finding --title "..." --body-file -
"""

import argparse
import datetime
import os
import re
import sqlite3
import sys

DB = os.environ.get('MR_JOURNAL_DB') or os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))), '.journal.db')

KINDS = ('finding', 'task', 'done', 'trap', 'decision', 'question', 'state')
STATUS = ('open', 'done', 'withdrawn', 'blocked', 'superseded')
EVIDENCE = ('verified', 'inferred', 'observed', 'guess')

# About six lines.  Soft: add warns and still writes.
DONE_BODY_SOFT_MAX = 600

SCHEMA = """
PRAGMA foreign_keys=ON;
CREATE TABLE IF NOT EXISTS entry (
  id            INTEGER PRIMARY KEY,
  created       TEXT NOT NULL,
  updated       TEXT NOT NULL,
  kind          TEXT NOT NULL,
  status        TEXT NOT NULL,
  pri           INTEGER NOT NULL DEFAULT 2,
  evidence      TEXT,
  title         TEXT NOT NULL,
  body          TEXT,
  superseded_by INTEGER REFERENCES entry(id)
);
CREATE TABLE IF NOT EXISTS ref (
  entry_id INTEGER NOT NULL REFERENCES entry(id) ON DELETE CASCADE,
  kind     TEXT NOT NULL,
  value    TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS ref_lookup ON ref(kind, value);
CREATE INDEX IF NOT EXISTS ref_entry ON ref(entry_id);
CREATE INDEX IF NOT EXISTS entry_status ON entry(status, pri);
CREATE TABLE IF NOT EXISTS sweep (
  path         TEXT PRIMARY KEY,
  state        TEXT NOT NULL,
  resume       TEXT,
  lines        INTEGER,
  lines_before INTEGER,
  comment_pct  REAL,
  note         TEXT,
  updated      TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS defect (
  slug     TEXT PRIMARY KEY,
  title    TEXT NOT NULL,
  guidance TEXT NOT NULL,
  created  TEXT NOT NULL,
  updated  TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS defect_hit (
  slug   TEXT NOT NULL REFERENCES defect(slug) ON DELETE CASCADE,
  path   TEXT,
  note   TEXT,
  logged TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS defect_hit_slug ON defect_hit(slug);
CREATE VIRTUAL TABLE IF NOT EXISTS entry_fts
  USING fts5(title, body, content='entry', content_rowid='id');
CREATE TRIGGER IF NOT EXISTS entry_ai AFTER INSERT ON entry BEGIN
  INSERT INTO entry_fts(rowid, title, body)
    VALUES (new.id, new.title, new.body);
END;
CREATE TRIGGER IF NOT EXISTS entry_ad AFTER DELETE ON entry BEGIN
  INSERT INTO entry_fts(entry_fts, rowid, title, body)
    VALUES ('delete', old.id, old.title, old.body);
END;
CREATE TRIGGER IF NOT EXISTS entry_au AFTER UPDATE ON entry BEGIN
  INSERT INTO entry_fts(entry_fts, rowid, title, body)
    VALUES ('delete', old.id, old.title, old.body);
  INSERT INTO entry_fts(rowid, title, body)
    VALUES (new.id, new.title, new.body);
END;
"""

# Refs the body mentions.  Cheap, and being wrong is cheap too: a bad
# ref costs one noisy row in `about`, a missing one costs a lookup.
#
# THE BARE FORM IS THE COMMON ONE.  Prose in this project says
# "prim.c", not "src/prim.c", so matching only the qualified spelling
# found almost nothing.  Both normalise to the qualified value so a
# lookup by either finds the same entries.
REF_PATTERNS = (
    ('addr', re.compile(r'0x00[0-9a-fA-F]{6}')),
    ('file', re.compile(r'(?:src/)?([A-Za-z0-9_]+\.(?:c|h|cpp|inc))\b')),
    ('file', re.compile(r'(?:tools/)?([A-Za-z0-9_]+\.py)\b')),
    ('doc', re.compile(r'(?:docs/)?((?:[a-z0-9-]+/)*[a-z0-9_-]+\.txt)\b')),
    ('rule', re.compile(r'\brule (\d+[a-z]?(?:\.\d+)?)')),
)

REF_DIR = {'c': 'src/', 'h': 'src/', 'cpp': 'src/', 'inc': 'src/',
           'py': 'tools/', 'txt': 'docs/'}


def qualify(value):
    """src/prim.c for both "prim.c" and "src/prim.c"."""
    if '/' in value:
        return value
    ext = value.rsplit('.', 1)[-1]
    return REF_DIR.get(ext, '') + value


def now():
    return datetime.datetime.now().replace(microsecond=0).isoformat(' ')


def connect():
    conn = sqlite3.connect(DB)
    conn.row_factory = sqlite3.Row
    conn.execute('PRAGMA journal_mode=WAL')
    conn.executescript(SCHEMA)
    return conn


def autorefs(text):
    out = set()
    for kind, rx in REF_PATTERNS:
        for m in rx.finditer(text or ''):
            v = (m.group(1) if rx.groups else m.group(0)).lower()
            out.add((kind, qualify(v) if kind in ('file', 'doc') else v))
    return out


def add_refs(conn, eid, pairs):
    have = set()
    for r in conn.execute('SELECT kind, value FROM ref WHERE entry_id=?',
                          (eid,)):
        have.add((r['kind'], r['value']))
    conn.executemany('INSERT INTO ref(entry_id, kind, value) VALUES(?,?,?)',
                     [(eid, k, v) for (k, v) in pairs if (k, v) not in have])


def read_body(arg):
    if arg is None:
        return None
    if arg == '-':
        return sys.stdin.read()
    with open(arg, encoding='utf-8') as f:
        return f.read()


def cli_refs(a):
    out = set()
    for kind, vals in (('file', a.file), ('addr', a.addr), ('func', a.func),
                       ('doc', a.doc), ('rule', a.rule), ('tag', a.tag)):
        for v in (vals or []):
            v = v.lower()
            out.add((kind, qualify(v) if kind in ('file', 'doc') else v))
    return out


def refs_of(conn, eid):
    return ['%s:%s' % (r['kind'], r['value']) for r in conn.execute(
        'SELECT kind, value FROM ref WHERE entry_id=? ORDER BY kind, value',
        (eid,))]


def render(conn, rows, full=False, as_json=False):
    if as_json:
        import json
        out = []
        for r in rows:
            d = dict(r)
            d['refs'] = refs_of(conn, r['id'])
            out.append(d)
        print(json.dumps(out, indent=1))
        return
    for r in rows:
        print('#%-4d %-8s %-10s p%d  %s' % (r['id'], r['kind'], r['status'],
                                            r['pri'], r['title']))
        if not full:
            continue
        print('      %s%s' % (r['created'],
                              '  ' + r['evidence'] if r['evidence'] else ''))
        refs = refs_of(conn, r['id'])
        if refs:
            print('      refs: ' + '  '.join(refs))
        if r['body']:
            print()
            for line in r['body'].rstrip().split('\n'):
                print(('      ' + line) if line.strip() else '')
        print()


# --------------------------------------------------------------- commands

def warn_formatted(body):
    """BODIES ARE PLAIN PROSE.  Nothing reads them but the agent, so
    layout buys nothing and costs a re-pad on every edit.  A table is
    the form that did real damage - a row one column wider than its
    border - so it is the one worth saying out loud."""
    if not body:
        return
    if '+---' in body or any(
            ln.strip().startswith('|') and ln.rstrip().endswith('|')
            for ln in body.split('\n')):
        sys.stderr.write('journal: body looks like a table. Bodies are '
                         'plain prose - see tools/README-journal.txt 1.1.\n')


def warn_long(kind, body):
    """A done entry records that something happened and why, in a
    few lines.  Anything longer is usually a FACT, and a fact's home
    is docs/ or a plate - not a database built to be deleted."""
    if kind == 'done' and body and len(body) > DONE_BODY_SOFT_MAX:
        sys.stderr.write(
            'journal: done body is %d chars (soft max %d). If the'
            ' surplus is a fact rather than a record, promote it -'
            ' README-journal.txt 1.1.' % (len(body),
                                          DONE_BODY_SOFT_MAX)
            + chr(10))


def cmd_add(conn, a):
    body = read_body(a.body_file)
    warn_formatted(body)
    warn_long(a.kind, body)
    stamp = now()
    # kind=done with the default status=open reads as a contradiction
    # and sorts into the worklist.  An explicit --status still wins.
    if a.kind == 'done' and a.status == 'open' and '--status' not in sys.argv:
        a.status = 'done'
    cur = conn.execute(
        'INSERT INTO entry(created, updated, kind, status, pri, evidence,'
        ' title, body) VALUES(?,?,?,?,?,?,?,?)',
        (stamp, stamp, a.kind, a.status, a.pri, a.evidence, a.title, body))
    eid = cur.lastrowid
    add_refs(conn, eid,
             cli_refs(a) | autorefs('%s\n%s' % (a.title, body or '')))
    conn.commit()
    print(eid)


def cmd_edit(conn, a):
    if not conn.execute('SELECT id FROM entry WHERE id=?', (a.id,)).fetchone():
        sys.exit('no entry %d' % a.id)
    body = read_body(a.body_file)
    cols, vals = [], []
    for name, v in (('kind', a.kind), ('status', a.status), ('pri', a.pri),
                    ('evidence', a.evidence), ('title', a.title),
                    ('body', body), ('superseded_by', a.superseded_by)):
        if v is not None:
            cols.append(name + '=?')
            vals.append(v)
    if cols:
        cols.append('updated=?')
        vals.append(now())
        conn.execute('UPDATE entry SET %s WHERE id=?' % ', '.join(cols),
                     vals + [a.id])
    add_refs(conn, a.id, cli_refs(a) | autorefs(body or ''))
    conn.commit()
    print(a.id)


def cmd_done(conn, a):
    row = conn.execute('SELECT body FROM entry WHERE id=?', (a.id,)).fetchone()
    if not row:
        sys.exit('no entry %d' % a.id)
    body = row['body'] or ''
    if a.note:
        body = ('%s\n\n%s  %s' % (body.rstrip(), now(), a.note)).strip()
    conn.execute('UPDATE entry SET status=?, body=?, updated=? WHERE id=?',
                 (a.status, body, now(), a.id))
    conn.commit()
    print(a.id)


def cmd_rm(conn, a):
    conn.execute('DELETE FROM entry WHERE id=?', (a.id,))
    conn.commit()
    print(a.id)


def cmd_ls(conn, a):
    q = 'SELECT * FROM entry WHERE 1=1'
    v = []
    for col, val in (('status', a.status), ('kind', a.kind)):
        if val:
            q += ' AND %s=?' % col
            v.append(val)
    if a.pri:
        q += ' AND pri<=?'
        v.append(a.pri)
    if a.tag:
        q += ' AND id IN (SELECT entry_id FROM ref WHERE kind=? AND value=?)'
        v += ['tag', a.tag.lower()]
    q += ' ORDER BY pri, id DESC LIMIT ?'
    v.append(a.limit)
    render(conn, conn.execute(q, v).fetchall(), a.full, a.json)


def cmd_show(conn, a):
    rows = conn.execute('SELECT * FROM entry WHERE id IN (%s) ORDER BY id'
                        % ','.join('?' * len(a.id)), a.id).fetchall()
    render(conn, rows, True, a.json)


def cmd_find(conn, a):
    # EVERY TOKEN IS QUOTED unless --raw.  FTS5 reads a bare hyphen as
    # a column filter, so "comment-defects" errors with "no such
    # column: defects" - and hyphenated terms are everywhere here.
    # Quoting per token keeps the AND-of-terms behaviour a phrase
    # quote would lose.
    q = a.query if a.raw else ' '.join(
        '"%s"' % t.replace('"', '""') for t in a.query.split())
    rows = conn.execute(
        'SELECT e.* FROM entry_fts f JOIN entry e ON e.id=f.rowid'
        ' WHERE entry_fts MATCH ? ORDER BY rank LIMIT ?',
        (q, a.limit)).fetchall()
    render(conn, rows, a.full, a.json)


def cmd_about(conn, a):
    v = a.value.lower()
    rows = conn.execute(
        'SELECT DISTINCT e.* FROM entry e JOIN ref r ON r.entry_id=e.id'
        ' WHERE r.value=? OR r.value LIKE ? ORDER BY e.status, e.id',
        (v, '%' + v + '%')).fetchall()
    render(conn, rows, a.full, a.json)


# ONE ROW PER FILE, AND NO TABLE IN A BODY.  The per-file sweep state
# used to be an ASCII table inside one entry, so updating one file
# rewrote the whole body - the read-modify-write this database exists
# to avoid - and the columns had to be re-padded by hand.  Rendering
# from real columns cannot drift.
SWEEP_ORDER = {'begun': 0, 'todo': 1, 'done': 2}
SWEEP_STATES = tuple(SWEEP_ORDER)


def cmd_sweep(conn, a):
    if a.path:
        rows = conn.execute('SELECT * FROM sweep WHERE path=?',
                            (qualify(a.path.lower()),)).fetchall()
    else:
        rows = conn.execute('SELECT * FROM sweep').fetchall()
    rows = sorted(rows, key=lambda r: (SWEEP_ORDER.get(r['state'], 9),
                                       -(r['comment_pct'] or 0)))
    if a.next:
        # THE ONE COMMAND A BLOCK OPENS WITH.  Findings were
        # write-only until this existed: refs made them findable and
        # nothing made them found, so eleven sat untouched while the
        # sweep walked past the files they name.
        rows = rows[:1]
        if rows:
            print('resume  %s  %s' % (rows[0]['path'], rows[0]['resume'] or ''))
            open_here = conn.execute(
                'SELECT DISTINCT e.* FROM entry e JOIN ref r ON r.entry_id=e.id'
                " WHERE e.status='open' AND r.kind='file' AND r.value=?"
                ' ORDER BY e.pri, e.id', (rows[0]['path'],)).fetchall()
            if open_here:
                print('\nopen against this file:')
                render(conn, open_here, False, False)
            else:
                print('\nno open findings against this file')
        return
    if a.json:
        import json
        print(json.dumps([dict(r) for r in rows], indent=1))
        return
    if not rows:
        print('no sweep rows')
        return
    w = max(len(r['path']) for r in rows)
    print('%-*s  %-5s %6s %6s %5s  %s'
          % (w, 'file', 'state', 'lines', 'was', 'pct', 'resume'))
    for r in rows:
        print('%-*s  %-5s %6s %6s %5s  %s'
              % (w, r['path'], r['state'],
                 r['lines'] if r['lines'] is not None else '-',
                 r['lines_before'] if r['lines_before'] is not None else '-',
                 ('%.1f' % r['comment_pct']) if r['comment_pct'] else '-',
                 r['resume'] or ''))
        if a.full and r['note']:
            for line in r['note'].rstrip().split('\n'):
                print('    ' + line)


def cmd_sweep_set(conn, a):
    path = qualify(a.path.lower())
    row = conn.execute('SELECT * FROM sweep WHERE path=?', (path,)).fetchone()
    if row is None:
        conn.execute('INSERT INTO sweep(path, state, updated) VALUES(?,?,?)',
                     (path, a.state or 'todo', now()))
    cols, vals = [], []
    for name, v in (('state', a.state), ('resume', a.resume),
                    ('lines', a.lines), ('lines_before', a.lines_before),
                    ('comment_pct', a.pct), ('note', a.note)):
        if v is not None:
            cols.append(name + '=?')
            vals.append(v)
    cols.append('updated=?')
    vals.append(now())
    conn.execute('UPDATE sweep SET %s WHERE path=?' % ', '.join(cols),
                 vals + [path])
    conn.commit()
    print(path)


def cmd_defect(conn, a):
    """The comment-sweep's defect classes, ordered by how often hit.

    THE PATTERN AND ITS INSTANCES ARE SEPARATE ROWS on purpose.  The
    catalogue this replaced kept both in one paragraph, so every new
    sighting lengthened the class and the file grew 924 -> 1107 lines
    in one sweep.  Here a sighting is a defect_hit and the guidance
    never moves.
    """
    ts = now()
    if a.op == 'add':
        g = read_body(a.body_file) or a.title
        conn.execute('INSERT OR REPLACE INTO defect'
                     ' (slug, title, guidance, created, updated)'
                     ' VALUES (?, ?, ?,'
                     '   COALESCE((SELECT created FROM defect WHERE slug=?),'
                     '            ?), ?)',
                     (a.slug, a.title, g.strip(), a.slug, ts, ts))
        conn.commit()
        print(a.slug)
        return 0
    if a.op == 'hit':
        r = conn.execute('SELECT 1 FROM defect WHERE slug=?',
                         (a.slug,)).fetchone()
        if not r:
            print('journal: no defect class %r - add it first' % a.slug)
            return 1
        conn.execute('INSERT INTO defect_hit (slug, path, note, logged)'
                     ' VALUES (?, ?, ?, ?)',
                     (a.slug, qualify(a.file.lower()) if a.file else None,
                      a.note, ts))
        conn.execute('UPDATE defect SET updated=? WHERE slug=?',
                     (ts, a.slug))
        conn.commit()
        n = conn.execute('SELECT COUNT(*) c FROM defect_hit WHERE slug=?',
                         (a.slug,)).fetchone()['c']
        print('%s: %d hit(s)' % (a.slug, n))
        return 0
    if a.op == 'rm':
        conn.execute('DELETE FROM defect WHERE slug=?', (a.slug,))
        conn.commit()
        return 0
    if a.op == 'show':
        d = conn.execute('SELECT * FROM defect WHERE slug=?',
                         (a.slug,)).fetchone()
        if not d:
            print('journal: no defect class %r' % a.slug)
            return 1
        print('%s  %s' % (d['slug'], d['title']))
        print()
        for line in d['guidance'].split('\n'):
            print('   ' + line)
        hits = conn.execute('SELECT * FROM defect_hit WHERE slug=?'
                            ' ORDER BY logged', (a.slug,)).fetchall()
        if hits:
            print()
            print('   seen %d time(s):' % len(hits))
            for h in hits:
                print('     %-22s %s' % (h['path'] or '', h['note'] or ''))
        return 0
    rows = conn.execute(
        'SELECT d.slug, d.title, COUNT(h.slug) n FROM defect d'
        ' LEFT JOIN defect_hit h ON h.slug=d.slug'
        ' GROUP BY d.slug ORDER BY n DESC, d.slug').fetchall()
    if not rows:
        print('no defect classes recorded.')
        return 0
    w = max(len(r['slug']) for r in rows)
    for r in rows:
        print('%3d  %-*s  %s' % (r['n'], w, r['slug'], r['title']))
    print()
    print('%d class(es), %d sighting(s).'
          % (len(rows), sum(r['n'] for r in rows)))
    return 0


def cmd_stats(conn, a):
    print('db: %s' % DB)
    for r in conn.execute('SELECT status, kind, COUNT(*) n FROM entry'
                          ' GROUP BY status, kind ORDER BY status, kind'):
        print('  %-11s %-9s %d' % (r['status'], r['kind'], r['n']))
    n = conn.execute('SELECT COUNT(*) n FROM entry').fetchone()['n']
    print('  %-11s %-9s %d' % ('TOTAL', '', n))


def cmd_export(conn, a):
    for r in conn.execute('SELECT * FROM entry ORDER BY id'):
        print('## #%d  %s' % (r['id'], r['title']))
        print('%s | %s | p%d | %s%s' % (
            r['kind'], r['status'], r['pri'], r['created'],
            ' | ' + r['evidence'] if r['evidence'] else ''))
        refs = refs_of(conn, r['id'])
        if refs:
            print('refs: ' + ' '.join(refs))
        print()
        if r['body']:
            print(r['body'].rstrip())
            print()


# ------------------------------------------------- the one-off import

HEADING = re.compile(r'^(\d+(?:\.\d+)*)\.\s\s(.*)$')

TITLE_MAP = (
    ('DONE', 'done', 'done'),
    ('WITHDRAWN', 'decision', 'withdrawn'),
    ('CHECKED, NOT A DEFECT', 'done', 'done'),
    ('IN PROGRESS', 'task', 'open'),
    ('A STANDING CHECK', 'trap', 'open'),
    ('FOUND, NOT APPLIED', 'finding', 'open'),
    ('FOUND', 'finding', 'open'),
)


def classify(title):
    up = title.upper()
    for prefix, kind, status in TITLE_MAP:
        if up.startswith(prefix):
            return kind, status
    return 'state', 'open'


def dedent(lines):
    body = [ln[3:] if ln.startswith('   ') else ln for ln in lines]
    while body and not body[0].strip():
        body.pop(0)
    while body and not body[-1].strip():
        body.pop()
    return '\n'.join(body)


def split_bullets(text):
    """(title, body) per top-level "o " bullet, title being its first
    sentence.  A bullet with no "o " prefix keeps the whole block."""
    out, cur = [], None
    for ln in text.split('\n'):
        if ln.startswith('o '):
            cur = [ln[2:].strip()]
            out.append(cur)
        elif cur is not None:
            cur.append(ln[2:] if ln.startswith('  ') else ln)
    blocks = ['\n'.join(b).strip() for b in out] if out else [text.strip()]
    res = []
    for b in blocks:
        flat = ' '.join(b.split())
        cut = flat.find('.  ')
        title = flat[:cut + 1] if 0 < cut < 110 else flat[:110].rstrip()
        res.append((title, b))
    return res


def cmd_import_audit(conn, a):
    with open(a.path, encoding='ascii', errors='replace') as f:
        lines = f.read().replace('\r\n', '\n').split('\n')

    secs, cur = [], None
    for ln in lines:
        m = HEADING.match(ln)
        if m:
            cur = [m.group(1), m.group(2).strip(), []]
            secs.append(cur)
        elif cur is not None:
            cur[2].append(ln)

    # a heading's title wraps onto the following over-indented line
    for s in secs:
        while s[2] and s[2][0].startswith('       ') and s[2][0].strip():
            s[1] = s[1] + ' ' + s[2].pop(0).strip()

    # fold x.y.z into x.y; 1, 2, 3, 4.n, 5.n stay entries of their own
    keep = []
    for num, title, body in secs:
        parts = num.split('.')
        if len(parts) >= 3:
            parent = '.'.join(parts[:2])
            for k in keep:
                if k[0] == parent:
                    k[2] += ['', '   %s. %s' % (num, title), ''] + body
                    break
            continue
        keep.append([num, title, list(body)])

    stamp = now()
    n = 0

    def insert(kind, status, title, text, num, pri=None):
        cur = conn.execute(
            'INSERT INTO entry(created, updated, kind, status, pri, title,'
            ' body) VALUES(?,?,?,?,?,?,?)',
            (stamp, stamp, kind, status,
             pri if pri else (2 if status == 'open' else 3), title, text))
        add_refs(conn, cur.lastrowid,
                 {('tag', 'imported'), ('tag', 'comment-sweep'),
                  ('tag', 'sec-' + num)} | autorefs(title + '\n' + text))

    for num, title, body in keep:
        text = dedent(body)
        top = num.split('.')[0]
        if not text.strip():
            continue
        if top == '6':
            # ONE ENTRY PER BULLET.  Section 6 is a list of unrelated
            # open items - the ones a file's own sweep has to pick up -
            # and as a single row none of them can be closed alone.
            for title2, text2 in split_bullets(text):
                insert('finding', 'open', title2, text2, num, pri=1)
                n += 1
            continue
        if top == '4':
            kind, status = classify(title)
        elif top == '5':
            kind, status = 'trap', 'open'
        else:
            kind, status = 'state', 'open'
        insert(kind, status, title, text, num)
        n += 1
    conn.commit()
    print('imported %d entries from %s' % (n, a.path))


# --------------------------------------------------------------- argparse

def refargs(q):
    q.add_argument('--file', action='append')
    q.add_argument('--addr', action='append')
    q.add_argument('--func', action='append')
    q.add_argument('--doc', action='append')
    q.add_argument('--rule', action='append')
    q.add_argument('--tag', action='append')


def viewargs(q):
    q.add_argument('--full', action='store_true')
    q.add_argument('--json', action='store_true')


def main():
    p = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = p.add_subparsers(dest='cmd', required=True)

    q = sub.add_parser('add', help='new entry; body via --body-file - on stdin')
    q.add_argument('--kind', choices=KINDS, required=True)
    q.add_argument('--title', required=True)
    q.add_argument('--status', choices=STATUS, default='open')
    q.add_argument('--pri', type=int, default=2)
    q.add_argument('--evidence', choices=EVIDENCE)
    q.add_argument('--body-file')
    refargs(q)
    q.set_defaults(fn=cmd_add)

    q = sub.add_parser('edit')
    q.add_argument('id', type=int)
    q.add_argument('--kind', choices=KINDS)
    q.add_argument('--title')
    q.add_argument('--status', choices=STATUS)
    q.add_argument('--pri', type=int)
    q.add_argument('--evidence', choices=EVIDENCE)
    q.add_argument('--body-file')
    q.add_argument('--superseded-by', type=int, dest='superseded_by')
    refargs(q)
    q.set_defaults(fn=cmd_edit)

    q = sub.add_parser('done', help='close one, optionally appending a note')
    q.add_argument('id', type=int)
    q.add_argument('--note')
    q.add_argument('--status', choices=STATUS, default='done')
    q.set_defaults(fn=cmd_done)

    q = sub.add_parser('rm')
    q.add_argument('id', type=int)
    q.set_defaults(fn=cmd_rm)

    q = sub.add_parser('ls')
    q.add_argument('--status', choices=STATUS)
    q.add_argument('--kind', choices=KINDS)
    q.add_argument('--pri', type=int, help='at least this urgent, 1 highest')
    q.add_argument('--tag')
    q.add_argument('--limit', type=int, default=60)
    viewargs(q)
    q.set_defaults(fn=cmd_ls)

    q = sub.add_parser('show')
    q.add_argument('id', type=int, nargs='+')
    viewargs(q)
    q.set_defaults(fn=cmd_show)

    q = sub.add_parser('find', help='full-text search of title and body')
    q.add_argument('query')
    q.add_argument('--raw', action='store_true',
                   help='pass the query to FTS5 unquoted')
    q.add_argument('--limit', type=int, default=20)
    viewargs(q)
    q.set_defaults(fn=cmd_find)

    q = sub.add_parser('about', help='entries referencing an address or file')
    q.add_argument('value')
    viewargs(q)
    q.set_defaults(fn=cmd_about)

    q = sub.add_parser('sweep', help='per-file comment-sweep state')
    q.add_argument('path', nargs='?')
    q.add_argument('--next', action='store_true',
                   help='the resume point plus what is open against it')
    viewargs(q)
    q.set_defaults(fn=cmd_sweep)

    q = sub.add_parser('sweep-set', help='update one file\'s sweep row')
    q.add_argument('path')
    q.add_argument('--state', choices=SWEEP_STATES)
    q.add_argument('--resume', help='where the next session picks up')
    q.add_argument('--lines', type=int)
    q.add_argument('--lines-before', type=int, dest='lines_before')
    q.add_argument('--pct', type=float, help='per cent comment lines')
    q.add_argument('--note')
    q.set_defaults(fn=cmd_sweep_set)

    q = sub.add_parser('defect', help='comment-sweep defect classes')
    q.add_argument('op', nargs='?', default='ls',
                   choices=['ls', 'add', 'hit', 'show', 'rm'])
    q.add_argument('slug', nargs='?')
    q.add_argument('--title', default='')
    q.add_argument('--file')
    q.add_argument('--note')
    q.add_argument('--body-file')
    q.set_defaults(fn=cmd_defect)

    q = sub.add_parser('stats')
    q.set_defaults(fn=cmd_stats)

    q = sub.add_parser('export', help='markdown to stdout, before a purge')
    q.set_defaults(fn=cmd_export)

    q = sub.add_parser('import-audit',
                       help='the retired comment-audit.txt; for a'
                            ' re-import from an old checkout')
    q.add_argument('path')
    q.set_defaults(fn=cmd_import_audit)

    a = p.parse_args()
    a.fn(connect(), a)


if __name__ == '__main__':
    try:
        main()
    except (BrokenPipeError, OSError) as e:
        # `journal ls | head` closes the pipe under us.  On Windows
        # that surfaces as OSError 22 rather than BrokenPipeError, and
        # the interpreter then reports it a second time while flushing
        # stdout at exit - hence devnull rather than a bare pass.
        if isinstance(e, OSError) and not isinstance(e, BrokenPipeError):
            if e.errno not in (22, 32):
                raise
        os.dup2(os.open(os.devnull, os.O_WRONLY), sys.stdout.fileno())
        sys.exit(0)
