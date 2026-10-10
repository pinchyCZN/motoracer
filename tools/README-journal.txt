MotoRacer PC Reverse Engineering                           Analysis Note


                        THE AGENT'S DEV JOURNAL


Abstract

   tools/journal.py keeps the project's work state - what was done,
   what is open, what to do next - in a gitignored SQLite database
   at .journal.db.  It replaced docs/comment-audit.txt, which is
   deleted.

   It is FOR THE AGENT.  Nothing in it is laid out for a human
   reader, nothing in it is committed, and all of it is meant to be
   thrown away when the port is finished.

Table of Contents

   1. What goes in it, and what does not
      1.1. Bodies are plain prose
   2. Schema
      2.1. The defect table
      2.2. The sweep table
   3. Commands
   4. Why a database
   5. Disposal
   6. Open questions

1.  What goes in it, and what does not

   THE JOURNAL IS NOT A HOME FOR FACTS.  Rule 14.1 gives every fact
   exactly one home, and none of those homes is this database:

   +--------------------------------------+------------------------+
   | What                                 | Where it lives         |
   +======================================+========================+
   | true about the binary - addresses,   | the Ghidra plate       |
   | instruction evidence, byte findings  |                        |
   +--------------------------------------+------------------------+
   | a format, a struct, a subsystem      | docs/                  |
   +--------------------------------------+------------------------+
   | why the C is shaped this way; a      | the src/ comment       |
   | deliberate divergence                |                        |
   +--------------------------------------+------------------------+
   | project rules                        | CLAUDE.md              |
   +--------------------------------------+------------------------+
   | WHAT WAS DONE, WHAT IS OPEN, WHAT    | HERE                   |
   | TO DO NEXT                           |                        |
   +--------------------------------------+------------------------+
   | what the comment sweep keeps         | HERE - see 2.1         |
   | hitting, and where                   |                        |
   +--------------------------------------+------------------------+

   THE TEST IS WHETHER IT SURVIVES THE PORT.  If a sentence would
   still be true and still be wanted once the renderer is replaced,
   it belongs in one of the first four rows.  If it is only about
   the state of the work, it belongs here.

   That test is what keeps the database disposable.  Get it wrong
   and purging the journal loses something, which means it stops
   being purgeable, which means it turns back into another document
   to maintain.

1.1.  Bodies are plain prose

   NO TABLES, no aligned columns, no hanging indents, one space
   after a period.  Nothing reads a body but the agent, so layout
   buys nothing and costs a re-pad on every edit.

   The rule exists because a table in a body is a
   read-modify-write: updating one cell meant rewriting the whole
   body, and the first such edit produced a row one column wider
   than its border.  ANYTHING THAT WANTS COLUMNS WANTS A TABLE OF
   ITS OWN - see 2.1.

   `add` warns when a body looks like a table.  It does not refuse
   one, because a pasted fragment of somebody else's output is
   sometimes the clearest thing to record.

   A done ENTRY IS A RECORD, NOT AN ESSAY - about six lines, and
   `add` warns past 600 characters.  The warning exists because the
   surplus is nearly always a FACT: a defect pattern, a rule of
   thumb, something the next reader of src/ would want.  Those
   belong in docs/comment-defects.txt or a plate, and writing them
   here instead schedules them for deletion.  Promote as they
   arise; triaging sixty entries at purge time is the same work
   under worse conditions.

   THE IMPORTED ROWS KEEP THEIR RFC LAYOUT and are not worth
   converting; they came from a document that was formatted for a
   human reader.

2.  Schema

   Three tables and a full-text index.

       entry   id, created, updated, kind, status, pri, evidence,
               title, body, superseded_by
       ref     entry_id, kind, value
       sweep   path, state, resume, lines, lines_before,
               comment_pct, note, updated
       defect      slug, title, guidance, created, updated
       defect_hit  slug, path, note, logged

   kind      finding | task | done | trap | decision | question |
             state
   status    open | done | withdrawn | blocked | superseded
   pri       1 high, 2 normal, 3 background
   evidence  verified | inferred | observed | guess   - rule 4's own
             tags, so an entry cannot quietly launder a guess into a
             fact

   ref.kind  file | addr | func | doc | rule | tag

   REFS ARE WHAT MAKE THIS BEAT grep.  "Everything about
   0x0048c6d0" or "everything touching racerdraw.c" is one query
   rather than a section you read yourself.  They are extracted
   from the title and body automatically on every add and edit, and
   can be given explicitly as well.

   BARE AND QUALIFIED SPELLINGS NORMALISE TO ONE VALUE.  Prose in
   this project says "prim.c", not "src/prim.c"; both are stored as
   src/prim.c so a lookup by either finds the same entries.
   Matching only the qualified form found almost nothing, which is
   how this was noticed.

   THERE ARE NO SECTION NUMBERS, and that is deliberate.  The
   4.12/4.16 numbering was an artifact of the RFC layout, it was
   maintained by hand, and renumbering is what corrupted the file it
   came from.  The integer id is the stable handle; grouping comes
   from tags.  The imported entries carry their old number as a tag
   - sec-4.16 - so an old reference can still be found.

2.1.  The defect table

   THE COMMENT SWEEP'S DEFECT CLASSES, one row each, with every
   sighting a row of its own in defect_hit.

       python tools/journal.py defect              by frequency
       python tools/journal.py defect show SLUG    guidance + sightings
       python tools/journal.py defect hit SLUG --file F --note "..."
       python tools/journal.py defect add SLUG --title T --body-file -

   SEPARATING THE PATTERN FROM ITS SIGHTINGS IS THE WHOLE POINT.
   docs/comment-defects.txt kept both in one paragraph, so every new
   instance lengthened the class - 40 classes averaged 19 lines, 755
   in all, and the file grew 924 -> 1107 lines in a single sweep.
   That is the defect the sweep removes from src/, committed by the
   document that catalogues it.  Here a sighting is one INSERT and
   the guidance never moves.

   ls ORDERS BY HIT COUNT, which is what rule 16 asks for and what
   nobody was maintaining by hand.

   IS THIS WORK STATE OR A FACT?  Section 1's test says work state: a
   defect class is about how the sweep is going, not about the game,
   and none of it survives the port.  What DOES survive - how to run
   each check and what it cannot see - stayed in
   docs/comment-defects.txt section 2.


2.2.  The sweep table

   ONE ROW PER FILE for the rule 16 comment sweep, because the
   per-file state is the one thing here that genuinely has columns.

   state     done | begun | todo
   resume    where the next session picks up, e.g.
             "racer_surface_at_point, line 1578"
   lines     the current count; lines_before is the count when the
             sweep on that file started, where it is known
   comment_pct  from the ratio measurement, which is what decides
             the order to take the remaining files in

   --next IS THE ONE A BLOCK OPENS WITH.  It prints the resume
   point AND every open entry referencing that file, which is what
   stops findings being write-only: refs made them findable and
   nothing made them found, so eleven sat untouched while the sweep
   walked past the files they name.

   Without --next it is listed begun first, then todo by comment
   percentage descending - biggest target next - then done.  The layout is
   computed on read, so it cannot drift the way the hand-padded
   table it replaced did.

3.  Commands

       python tools/journal.py ls --status open --pri 1
       python tools/journal.py ls --tag graphics --full
       python tools/journal.py show 42
       python tools/journal.py find "palette livery"
       python tools/journal.py about 0x0048c6d0
       python tools/journal.py about racerdraw.c
       python tools/journal.py done 42 --note "user confirmed on Debug"
       python tools/journal.py sweep --next
       python tools/journal.py sweep
       python tools/journal.py sweep-set src/racer.c --state begun
       python tools/journal.py sweep-set src/racer.c --lines 8569
       python tools/journal.py sweep-set src/racer.c --resume "line 1578"
       python tools/journal.py stats
       python tools/journal.py export > /tmp/journal.md

   Adding one, with the body on stdin:

       python tools/journal.py add --kind finding --pri 1 \
           --evidence verified \
           --title "the wheels are never shaded" \
           --file src/racerdraw.c --addr 0x0048c9b2 --tag graphics \
           --body-file -

   PASS THE BODY ON STDIN OR IN A FILE, NEVER AS AN ARGUMENT.  Long
   prose through a shell argument is how the heredoc backslash trap
   bites, and it has already cost this project a mangled code
   sample.  --body-file - reads stdin.

   --json on ls, show, find and about when the output is going to be
   parsed rather than read.

   MR_JOURNAL_DB overrides the database path.

4.  Why a database

   A text file forces read-modify-write of the whole document, so
   every append risks the rest of it.  That is not hypothetical
   here: 4.16 ended up cross-referenced from a stale 4.13, a title
   claimed four rows over a table of five, and one line-ending slip
   turned a 140-line edit into a 4,461-line diff.  An INSERT touches
   one row and cannot disturb its neighbours.

   The second gain is that closing an item is an UPDATE rather than
   a hand edit that has to find and rewrite the right paragraph.
   Rule 14.2 - correct in place, never append - is free here.

5.  Disposal

   .journal.db is gitignored, along with its -wal and -shm
   companions.  Purging it is deleting the file.

   NEVER CITE AN ENTRY ID FROM src/ OR docs/.  A comment reading
   "see journal 63" dangles the moment the database is deleted, and
   it will be deleted - that is the whole point of it.  The journal
   may cite src/ and docs/ freely; the reverse is a one-way
   dependency on something disposable.

   Nothing enforces this either.  It was nearly broken once, in a
   src/ comment marking a dead function, and caught only because
   the id looked odd next to an address.

   BEFORE A PURGE, RUN export AND READ IT.  Anything that turns out
   to be a fact rather than work state should be promoted to its
   proper home first - a trap into CLAUDE.md, a finding about a
   format into docs/.  The entries tagged trap are the likely ones.

6.  Open questions

   o Nothing enforces section 1's test.  It is a discipline, and
     the failure mode is silent: a fact written only here is lost
     at the purge.
