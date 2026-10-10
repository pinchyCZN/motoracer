# MotoRacer (PC, 1997)  -  Reverse Engineering & Modernization

## Mission

Decompile `MOTO.EXE`, reconstruct readable, maintainable source, and recompile
it for modern 64-bit Windows.

The headline modernization target is the renderer. The original draws through
**DirectDraw surfaces + an internal software rasterizer**, with an optional
**Direct3D Immediate Mode (execute-buffer)** hardware path - an API that has not
existed in a shipping Windows SDK since the 1990s. It is also **hard-locked to
16-bit colour depth**: the game refuses to start on any other display mode. Both
of those go.

---

## Hard rules

### 1. Git is read-only

Enforced in `.claude/settings.json`. Read commands (`status`, `log`, `diff`,
`show`, `blame`, `ls-files`, ...) are allowed. Everything that mutates a repo,
an index, a working tree, or a remote is denied - including `add`, `commit`,
`push`, `pull`, `fetch`, `checkout`, `reset`, `rebase`, `merge`, `stash`, `tag`,
`remote`, `config`, `init`.

**Commits, branches, and pushes are the user's job, never the agent's.** If a
change is ready to commit, say so and stop.

> This is a guardrail against accident, not a security sandbox. Don't route
> around it - no `sh -c`, no aliases, no scripts that shell out to git. If a
> git write is genuinely needed, ask for it.

### 2. Ghidra is the source of truth for the binary

Never guess at an address, offset, field, or signature. Read it out of the
Ghidra DB via the `ghidra` MCP server. If the MCP is down, stop and say so - do
not substitute recollection for a lookup.

The database is live and shared with the user's open Ghidra GUI. Renames,
retypes, and comments applied through MCP are real edits to their project. Batch
them deliberately; don't churn.

### 3. Write down what you discover

A finding that exists only in a chat transcript is lost. Anything learned about
a struct, a subsystem, a file format, or an algorithm goes into `docs/` in the
same session it was found. See **Documentation conventions** below.

### 4. Separate fact from inference

Every claim in `docs/` carries a tag:

- **`[verified]`** - read directly out of the binary or the DB, or observed at
  runtime.
- **`[inferred]`** - a reasoned conclusion from verified evidence. State the
  evidence.
- **`[observed]`** - seen in a recording of a retail build, typically a
  longplay video. Empirical, but weaker than `[verified]` in two specific
  ways: the recording may be of 3.20 rather than our 3.22, and it is
  somebody else's capture rather than our binary. STRONGER than `[inferred]`
  for anything visual - no amount of static reading tells you which way a
  logo slides. This exists because rule 8 forbids running the game, so
  without it runtime behaviour has no evidence path at all.
- **`[guess]`** - a hypothesis worth testing. Never build on one without saying
  so.

Renaming a Ghidra symbol is an assertion. Don't assert what you haven't earned.

### 5. Don't refactor the reconstruction toward "nice"

Reconstructed source should match observed behaviour, including the ugly parts.
Fixed-point math, goto-heavy control flow, and global state stay as they are
until a subsystem is fully understood and there is a concrete reason to change
it. Modernization is a later, deliberate phase - not a side effect of
transcription.

### 5a. Never fake the retail look

**Match the original's CODE, not screenshots of it.** When the
reconstruction looks wrong next to retail footage, the fix is to find
the code that produces the right result - not to add something that
produces the right pixels.

Banned, however tempting:

- hardcoding a position, colour or timing that "looks right" instead of
  reading it out of the binary;
- inventing an animation to stand in for one that is gated on an
  unreconstructed struct;
- special-casing a page so it resembles a video.

The reason is that a visual hack is indistinguishable from a correct
implementation *until* the real code lands, and then it silently fights
it. Every such hack is a future session's confusing bug.

**What to do instead:** implement what the code says, let it look wrong,
and write down the gap. A page that draws one sprite because the phase
counter is legitimately stuck at zero is more useful than one that
fakes three - the first tells you what is missing.

`[observed]` evidence (rule 4) is for *deciding what to go and read*,
and for catching mistakes like a wrong colour. It is never a licence to
implement the observation directly.

> A DEBUG OVERRIDE IS NOT A HACK, provided it is off by default, named
> in the ini, and does not change what the game does when unset.

### 5b. Discoveries flow BOTH ways - never let the two sides diverge

**The Ghidra DB and `src/` describe the same program. A fact learned on
one side must be recorded on the other, in the same session.**

The failure this exists to prevent is specific and it is easy to walk
into. Work moves to the port; a struct in `src/` turns out to be wrong;
the agent goes back to Ghidra, finds the missing field, **fixes the C
and not the DB** - and from then on the two disagree, with nothing
saying which is right. The next session reads the DB, believes it, and
reintroduces the bug.

So, whenever the port work turns up something new about the binary:

| Learned while... | Also required |
|---|---|
| fixing `src/` against the binary | apply the field/name/type in Ghidra |
| reading Ghidra for a port question | write it into `docs/` AND the C |
| a struct gains or loses a member | update the Ghidra struct to match |
| a function's real name is settled | rename it in the DB, not just in C |

**A rename or retype that exists only in `src/` is a divergence, not a
discovery.** The DB is the source of truth for the binary (rule 2), so
leaving it stale makes it a source of falsehood.

> This does NOT mean `src/` must mirror the DB's layouts. The
> reconstruction is allowed to hold a different, simpler struct - see
> `MR_ModelInstance` - as long as the difference is deliberate and
> written down. What is banned is the two drifting apart by accident.

### 6. Plain ASCII only

Every byte in every document, script, and source comment must be <= 0x7F.
No em dashes, no curly quotes, no arrows, no box-drawing characters, no
accented letters, no ellipsis glyph. Write `->` not an arrow, `-` not an em
dash, `...` not an ellipsis, `x` not a multiplication sign, `+`/`-`/`|` for
diagrams rather than box-drawing.

### 7. Documents are RFC-style plain text, not markdown

Documents live in `docs/` and `tools/` as `.txt`. This file is the one
exception - Claude Code requires the name `CLAUDE.md`, and markdown is correct
here because the harness renders it - but everything else is laid out like an
old RFC:

```
MotoRacer PC Reverse Engineering                           Analysis Note


                          CODE MAP AND TRIAGE


Abstract

   Body text indented three spaces, wrapped at 72 columns.

Table of Contents

   1. First Section
      1.1. A Subsection

1.  First Section

   +--------------+-------+
   | Column       | Count |
   +==============+=======+
   | aligned with | 2542  |
   +--------------+-------+
```

Rules that follow from that:

- **No markdown syntax in a `.txt` file.** No `#` headings, no `**bold**`, no
  backticks, no `*emphasis*`, no `[text](link)`, no `|`-delimited markdown
  tables. If it would need a renderer to look right, it is wrong.
- **Tables are drawn with `+---+` borders and space-aligned columns** so they
  line up in any editor. `+===+` under the header row.
- **Numbered sections** (`1.`, `1.1.`) with a generated Table of Contents.
- **Body text indented three spaces, wrapped at 72 columns.** Tables may run
  to 120 including their indent.
- Blocks indented four or more spaces in a draft are preformatted - address
  maps, listings, diagrams - and are never rewrapped.

Two tools enforce this:

```
python tools/asciify.py --check    ASCII, line width, table alignment
python tools/asciify.py --fix      transliterate and rewrap in place
python tools/rfcfmt.py  --fix P    render a markdown-ish draft as RFC text
```

`rfcfmt.py` is a **one-way converter, not a round-trip formatter.** It refuses
a file that is already in RFC form, because re-rendering its own output would
flatten the layout back into paragraphs. Author new documents in RFC form
directly; use `rfcfmt` only to convert a draft.

Run `asciify --check` before treating any document as finished. If it reports
an UNMAPPED character, add a deliberate entry to `XLAT` rather than letting a
`?` through. It is keyed by codepoint integer so it passes its own check.

It also checks that **every `|` row is exactly as wide as the `+---+` border
above it**, because a table that does not line up is a table that needed a
renderer. Editing a cell without re-padding the row is the easiest mistake in
this format to make and the hardest to see in a diff - 141 rows across 31
documents had drifted before the check existed. `--fix` does not repair these;
widen or rewrap the cell by hand so the text stays inside 80 columns.

### 8. Never run MOTO.EXE

**Do not launch the game.** Not `MOTO.EXE`, not `ORIGINAL_MOTO.EXE`, not under
a debugger, not windowed, not with `-NoD3D`, not "just to see if it starts".
It does not run correctly as-is, so a launch proves nothing and costs the
user a hung window or a wedged display.

That also means: **there is no runtime evidence available.** Every claim in
this project is static - read out of the binary, the Ghidra DB, or the
extracted assets in `decomp/DATA/`. When a question can only be settled by
running the game, the correct answer is to say so and stop, not to
approximate it by launching something.

- A prediction that would need runtime confirmation stays tagged as a
  prediction. Do not quietly upgrade it to `[verified]`.
- Asking the user to run it and report back is fine. Running it is not.
- The extracted assets are the empirical fallback and they are genuinely
  good: file headers, `.bpt` records via `tools/bpt.py`, `.map` bank
  indices, and the size formulas in `docs/formats/` have settled several
  questions that looked like they needed a debugger.

### 9. Build `Release|Win32` to CHECK; the user tests `Debug`

**Only `Release|Win32` may be built by the agent, and only to find
compile errors.** It is a syntax and type check, nothing more. `x64` is
off limits entirely.

```
msbuild src\mracer.sln /p:Configuration=Release /p:Platform=Win32
```

**`Debug|Win32` IS THE BUILD THAT GETS TESTED, and the user builds and
runs it.** So the binary behind the agent pipe is always Debug, and a
Release build proves only that the code compiles - never that a change
is live.

**CHECK WHICH BINARY IS RUNNING BEFORE BELIEVING A MEASUREMENT.** A
change built into Release and then measured through the pipe is measured
against the OLD Debug binary, and the result looks like a failed fix. If
a capture contradicts a change that just went in, suspect this first and
ask for a rebuild rather than re-deriving the code.

**Do not execute anything either build produces.** Not `mracer.exe`, not
a test harness, not a one-off. The user runs it and reports back. This
is rule 8 widened from `MOTO.EXE` to everything in `build/`.

**`GenerateMapFile` is ON for Debug and OFF for Release**, and that is
deliberate rather than an oversight - the .map is what
`mracer_get_mem` resolves symbols against, and it is wanted only on the
configuration that actually runs. Do not turn it on for Release to make
a tool work; fix the tool's expectations instead.

If a Debug or x64 build breaks, the user will say so. Do not go looking.

### 10. The deliverable is a drop-in EXE - no pre-extracted assets

**The target is an executable someone can drop into an existing
MotoRacer installation and run.** Nothing else ships with it. So the
reconstruction must read the game's assets **exactly where a stock
install puts them**, and that means out of the `.BKF` archives.

`decomp/DATA/extracted/` **is not shipped data.** It was produced by an
extraction tool the user ran years ago, and it will not exist on anyone
else's machine. Neither will any other unpacked tree we create.

| Stock install has | We must read | Not |
|---|---|---|
| `DATA\PAGES.BKF` | the archive, by member name | `DATA\extracted\PAGES\...` |
| `DATA\MENU.BKF`, `ILE01.BKF`, ... | likewise | any unpacked mirror |
| `DATA\*.BPT`, `DATA\ileNN\*.trk` | directly - these are loose already | |

**DONE, as of `src/bkf.c` and `src/assets.c`.** Every loader resolves
through `asset_load()`, which looks inside the archives first and falls
back to the extracted tree last. All 209 menu resources and every other
asset the build requests resolve from a `.BKF`. What remains of this
rule is to keep it that way.

The `.bkf` format was fully decoded before either file was written, and
`tools/bkf.py` is the oracle - verified byte-for-byte against
`DATA/extract/ile01`. See `docs/data-files.txt` and
`docs/asset-inventory.txt`. The directory is:

```
u32          entry count
entry[count] char name[36]; u32 offset; u32 size     44 bytes each
```

Member names carry their subdirectory with a **backslash** -
`TITLEDSI\BNK00_00.RAW`, `BANK\BNK00_02.RAW` - so a member name is
already the relative path the extracted tree mirrors. That is what
makes the mapping trivial: split the first component off a logical
name, use it as the archive stem, look the rest up as a member.

**The rules that keep this true:**

- **Never call `fopen` on a game asset.** Go through `asset_load()`.
  If a new loader needs a file, it needs a logical name, not a path.
- **Logical names, not paths, in code and in the ini.** `[PATHS]`
  settings are archive-relative prefixes now.
- The extracted tree stays as the LAST fallback while testing, and
  `assets.c` prints a line whenever it fires. A run that reports zero
  from the extracted tree is a run that would work on a stock install
  - watch for that line.

The same rule applies to any asset we might be tempted to pre-convert
offline - decompressed `.lez`, `.raw` turned into `.png`, a generated
table. If the game can compute it at run time from shipped data, the
reconstruction computes it too. `src/detail_table.inc` is the one
sanctioned exception: it is generated from a **document**, not from
game data, and it is checked in as source.

### 11. C by default, C++ only for COM

**The reconstruction is C.** Every engine subsystem - geometry, queue,
physics, menus, tracks, fonts, audio mixing, file formats - is C.

**C++ is allowed only where it earns its place: COM, and the Win32
shell.** `d3d9rt.cpp` is C++ because `IDirect3DDevice9` in C means
`lpVtbl->` on every call plus an explicit `this`, which buries the logic
in punctuation. A future DirectSound or XAudio2 back end gets the same
exemption. An input layer on raw Win32 does not.

**Every C++ file presents a C header.** `render.h` is the model:
`extern "C"`, plain structs, no C++ in the interface. The C++ stays
inside one translation unit.

**No MFC and no exceptions.** Both are droppable and this is measured,
not assumed - see `src/README.txt` section 24:

- 81 of 2038 game functions are `__thiscall`, about 4 per cent.
- Of 36 RTTI descriptors in the image, **35 are MFC and exactly one is
  the game's** - `CdsiError`, an error type. A codebase using
  polymorphism as a design tool would have dozens.
- The 415 vtable-shaped indirect calls are **DirectDraw, Direct3D and
  DirectSound**, not the game's own virtual functions.
- The one real hierarchy is the **font family**: a base plus five
  variants, seven vtable slots of which only three ever differ and four
  are the same function in all six classes. In C that is a struct with
  three function pointers.

So the original is C with classes used as namespaces. Writing C is not
fighting it.

### 12. Tabs for indentation, spaces for alignment

**In `src/` only.** One indent level is one TAB, displayed as four
columns. Everything that is not structural indentation stays SPACES:

- a continuation line lined up under an open paren;
- the `* ` of a block comment;
- an ASCII diagram or an address listing inside a comment;
- columns in an initialiser or a run of aligned assignments.

That split is the whole point - it makes the source render correctly at
any tab width instead of only at four.

```
python tools/retab.py --check    report, exit 1 if anything drifted
python tools/retab.py --fix      convert in place
```

`retab.py` decides the split as `tabs = min(brace_depth, spaces // 4)`,
so a line indented past its own depth keeps the excess as alignment. It
verifies every file by expanding its own output back and comparing, and
refuses to write one that does not round-trip - so a scanner bug is
caught rather than committed.

**`docs/` and `tools/` do NOT change.** The RFC documents are specified
in spaces (rule 7) and `asciify.py` counts their columns; the Python is
PEP 8. `.editorconfig` encodes all three cases.

Two traps, both already hit:

- **`asciify.py` measures DISPLAY columns**, expanding tabs to four. It
  used to count characters, which would have let a deeply nested line
  run well past 80 real columns and still pass.
- **A generator that emits spaces silently undoes this.**
  `tools/gen_detail_table.py` writes `src/detail_table.inc` and now
  emits a tab. Check any new generator the same way.

---

### 13. The gameplay file set is fixed - do not add to it

**Five files in `src/` hold all gameplay logic.** New gameplay code
goes into one of them. Creating a sixth is a decision to raise with
the user, not one to make in passing.

| File | Owns | Test - does it belong here? |
|---|---|---|
| `physics.c/h` | the simulation | its output feeds a speed or a force |
| `race.c/h` | the race; the camera | it concerns **more than one** racer |
| `racer.c/h` | one competitor | it operates on a **single** `MR_Racer` |
| `bikes.c/h` | the carac table | it serves bike characteristics |
| `grid.c/h` | grid geometry | it is about grid slots |

In full: `physics.c` holds torque, drag, grip, brake, slope, the
wheelie and slip speed - anything `tools/check_physics.py` can hold
to a measured number. `race.c` holds load, grid build, the clock,
the countdown, the time limit, the per-racer loop, the dispatch, the
chase camera and draw orchestration. `racer.c` holds placement,
per-rider state decay, the tick order and logging.

**This rule exists because it was broken.** Two consecutive sessions
added `racetick.c` and `ridertick.c` without checking whether `race.c`
or `racer.c` was the right home - and both already existed, already
owning exactly those concepts. Four files, 178 lines of code between
them, six hundred lines of comment, four include guards, four project
entries. `camera.c` was the same shape from earlier. All are now
folded back in and deleted.

> **A header nothing else includes is usually not a module.** All
> three merged headers were included by their own `.c` and by `race.c`
> alone. If the only consumer is the file next to it, the split is
> imaginary. Check `grep -l 'include "x.h"' src/*` before writing
> `x.h`.

The borderline call, stated so it is not re-litigated: **the wheelie
is a state machine like fork-dive, but its output is a torque
multiplier, so it lives in `physics.c`.** Follow the output, not the
shape.

**The rule governs GAMEPLAY LOGIC, not everything in `src/`.** Four
files sit outside it deliberately and keep their own homes:
`timing.c` (the frame pacer), `model.c` (`camera_look_at`, which
`menu.c` also uses), `autopilot.c` (a debug driver - its output feeds
an INPUT, which is the opposite end of the pipe from a force) and
`agent.c` (the debug pipe the rule 15.2 verification loop runs on,
`ORIGIN NONE - ours`). `autopilot.h` argues its own case in its
header and records that it was agreed before it was written. Do not
fold either of the last two into `racer.c` the way `racetick.c` was
folded back.

**A SIXTH GAMEPLAY FILE NEEDS A MAJOR FEATURE THAT DOES NOT EXIST
YET** - not a new corner of one that does. The only candidate named
so far is **network play**: DirectPlay in the original, with no
reconstruction at all. Everything short of that goes into a file
that is already here.

**Every transcribed function carries an `ORIGIN` tag** naming the
Ghidra function it came from, so a suspect line can be traced back
without a search:

```c
/* ORIGIN  Engine_TorqueAt @ 0x0048ee30 */
float engine_torque_at(...)

/* ORIGIN  maybe_Physics_TickDrivetrain @ 0x0048ef30, at 0x0048f962 */
float physics_brake_force(...)

/* ORIGIN  NONE - ours */
void racer_log(...)
```

- **Always give the address**, even when the names match - a rename in
  the DB must not orphan the tag.
- **Name the enclosing function first, then the site**, when the C
  function is a slice of a bigger original. Six of `physics.c`'s
  functions are pieces of one 4284-byte function; the tag says which
  part.
- **Say `NONE - ours`** for code with no original, so an untagged
  function reads as an oversight rather than as ours by default.
- Audit with `grep -c "^/\* ORIGIN" src/*.c` against the function
  count.

Section 11's mirror-the-original-tree plan still applies to the
subsystems yet to be transcribed - `screen3d/`, `Dsi_d3d.cpp`,
`frame3d.cpp`. The five files above are **ours**: the original spread
the same logic across `player.cpp`, `event.cpp` and `frame3d.cpp`
according to a structure we cannot see, and inventing files to guess
at it would be worse than a flat set. See `src/README.txt` 11.1.


### 14. Record facts, not the journey - and record each one ONCE

Documentation says **what the code does and why**. It does not narrate
the search that found it.

**Measured, 2026-09-06:** 60,297 lines of `docs/` against 33,138 lines
of `src/`. `docs/subsystems/racers.txt` alone is 8,225 lines - a quarter
of the whole source tree, for one subsystem. That ratio is the problem
this rule exists to correct.

**Do not write:**

- "we first thought X, then found Y" - just write Y.
- "this took three sessions", "the first sweep was wrong", "an earlier
  note said" - the reader does not care how the fact was obtained.
- restating what a previous section already established.
- congratulation, summary-of-the-summary, or which tool was used.

**The one exception - a trap worth marking.** If an approach looks
obviously right and is actually wrong, say so in a sentence or two and
say why. "Do not lerp these as plain floats, the angles wrap" is a fact
about the problem. "I lerped them as plain floats and it broke" is a
diary entry. Write the first.

#### 14.1. One fact, one home

The same fact in two files means one of them goes stale, and only the
other gets updated. This has already happened repeatedly.

| What | Where it lives |
|---|---|
| addresses, instruction evidence, byte-level findings | the Ghidra plate comment |
| what a subsystem does, struct layouts, formats | `docs/` |
| why the C is shaped this way; divergences from the original | the `src/` comment |
| project rules | this file |

Everything else **points at** that home - `see racers.txt 9.40`,
`see MR_Racer.txt 3.8` - and does not restate its contents.

A constant's value belongs in the code that uses it, with its address.
It does not also need a table in `docs/` repeating the same number.

#### 14.2. Correct in place; never append

When a fact changes, **edit the sentence that is now wrong.** Do not
leave it standing and add a correction beside it - that produces two
statements where one is false, and the reader cannot tell which.

A correction is worth its own paragraph only when the WRONG reading is
one a future reader would independently arrive at. Otherwise just make
the text true and move on.

#### 14.3. Proportion

A subsystem note should be shorter than the code it describes. If a
section is longer than the function it documents, it is too long.

Prefer a table to a paragraph, a formula to a description of a formula,
and silence to a restatement.

---

### 15. Import WHOLE functions, not slices

**One C function per Ghidra function, carrying the original's name.**
Start from the decompiler's C and fix it up. Do not transcribe the
part of a function the current task happens to need.

| Subsystem | Rule |
|---|---|
| gameplay, physics, racers, track, AI | whole function, no exceptions |
| file formats, audio, input | whole function |
| **menus** | slices are tolerated |

The menu exemption is narrow and is a judgement about cost, not a
statement that slicing is fine: the pages are simple, most of the
important ones already work, and nobody is going to breakpoint
`pageMainMenu.cpp` against MOTO.EXE. Everything the bike touches is
the opposite on all three counts.

**Why: a slice silently drops every branch nobody knew to look for.**
Four defects in the wall response were traced one at a time across a
session and all four were already inside
`maybe_Racer_UpdateGroundContact` - the 3D bounce/scrape gate, the
damping order, the two-pass clip, and the pending-yaw recovery.
Importing that one function would have brought all four across for
free. Two were got wrong on the way instead.

It also breaks the debug infrastructure. Hooks, watchpoints and
`mr_place.py` exist so the same event can be produced on both sides
and compared - and that needs a function on our side corresponding
to the one being hooked in MOTO.EXE. **A slice has no counterpart,
so the comparison has nowhere to attach.**

**A two-address ORIGIN tag is the tell.** `@ 0xA, at 0xB` names a
site inside a bigger function; that is a slice, and it now has to be
justified rather than being the default.

#### 15.1. When the decompilation is bad

**First assume it is the DB, not the function.** "The decompiler
cannot handle this" is usually "the callees have no prototypes".
`maybe_Racer_UpdateGroundContact` carried a plate comment saying to
read it from disassembly because its calls rendered with empty
argument lists; six `set_function_prototype` calls fixed that, and
they improve every other function calling those helpers too. Check
the four DB defects (`ghidra-db-repairs.txt`) in the same breath.

**If it is still unusable, import a SKELETON and fill the gaps from
the asm.** Case by case. What is not negotiable:

- the function exists, whole, under the original's name;
- a branch that is not understood is still **transcribed and still
  runs**, marked in place with its address - never dropped;
- the gaps are named, so the next reader knows what was filled by
  hand and what came from the decompiler.

A skeleton with three marked holes is worth more than a clean slice
of a third of the function, because the holes are visible and the
missing two-thirds is not.

#### 15.2. The verification loop is the point

```
python tools/mr_place.py --to both --spot NAME    same place, same heading
moto_set_hook    by ADDRESS   - MOTO has no symbols
mracer_set_hook  by NAME      - ours has a .map
drive both the same number of frames, diff the traces
```

Both halves work in `Release|Win32`. **`MR_Racer` is byte-exact** -
the DB's layout verbatim, `#pragma pack(1)`, offset assertions, 0x3209
bytes - so `moto_get_mem` and `mracer_get_mem` read the same
field at the same offset and imported code uses the offsets the
decompiler gives it. Keep it that way: a field placed by guesswork
is worse than one left as named padding.

See `docs/whole-function-import.txt` for the procedure and the
import order.

---

### 16. `src/` comments - `docs/` is the default, not `src/`

**Measured: `race.c` was 55 per cent comment**, 5357 lines against
4267 of code - which is why this rule exists. Rule 14 governs `docs/`;
this governs `src/`.

**AN EXPLANATION GOES TO `docs/` AND LEAVES A ONE-LINE POINTER.** That
is the default and the first thing to reach for. A `src/` comment is
the EXCEPTION, and it earns its place only by being one of these six:

| Keep | Why |
|---|---|
| **`ORIGIN` + the address** | non-negotiable; address first, name second - names drift, addresses do not |
| **a landmine** | looks obviously right, is wrong. State the PROBLEM as a fact, never the story |
| **a deliberate divergence** | so nobody "fixes" it back |
| **`[open]` / not implemented** | what is missing and what it would take |
| **the ORIGINAL's key binding** | `F5`, `TAB` - it is how you find the handler in Ghidra |
| **a non-obvious WHY** | something reading the code cannot give you |

**Everything else belongs in `docs/` or nowhere.** These are the
shapes that get written anyway, in the order they are hit:

- **The dev journal.** "we first thought X", "this took three
  sessions", "an earlier version said". The highest-value deletions
  in the tree. The ONE exception is rule 14's: an approach that looks
  right and is wrong keeps a sentence, written as a fact.
- **Restatement of the code.** If the line below says it, the comment
  goes. Do not explain C to the reader.
- **OUR keybindings.** `"on '6'"`, `"the '3' key"` - ours change and
  the comment is stale the same week. The original's stay.
- **Inline assembly listings.** The `ORIGIN` address is the listing.
- **Measured retail values `docs/` already holds.** Replace with a
  pointer - `camera.txt 3.2`. The exception is rule 14.1's: a
  constant the code MULTIPLIES BY keeps its number and its address
  inline.
- **Anything stale.** Highest priority of all, because a stale
  comment actively misleads where a missing one merely omits.

`docs/` and `src/` are married; expect to edit both in one session.

**WHAT THE SWEEP ACTUALLY FINDS IS IN THE JOURNAL** - 40 defect
classes, ordered by how often each is hit, with every sighting a row
of its own:

```
python tools/journal.py defect              by frequency
python tools/journal.py defect show SLUG    guidance + sightings
python tools/journal.py defect hit SLUG --file F --note "..."
```

**`docs/comment-defects.txt` keeps the standing checks** - how to run
each one and, more valuably, WHAT EACH IS BLIND TO. This rule is the
criteria; those two are the evidence behind it, and most of the
"everything else goes" list above was derived from the counts. Read
both before sweeping a file you have not swept before.

> **STALENESS IS THE POINT OF DOING THIS BY HAND.** The first 110
> lines of `race.c` carried three dead claims: a `camera.c` that rule
> 13 deleted, an orphaned comment for a variable that does not exist,
> and a cross-reference to a `README` section that was never written.
> None of that is findable by grep. Read every comment.

**One block per session, top down**, so the diff stays reviewable.

**EVERY CHANGE CLOSES WITH THESE FOUR**, a sweep block included:

```
python tools/retab.py --check
python tools/asciify.py --check
python tools/commentbudget.py
msbuild src\mracer.sln /p:Configuration=Release /p:Platform=Win32
```

**THE BUDGET IS A NUMBER BECAUSE THE CRITERIA ARE NOT.** The six
above are a judgement, and an author applies them to their own fresh
work where everything feels earned - so they filter somebody else's
old comment well and your own new one badly. `commentbudget` only
counts: **net comment may not exceed net code**, added minus
removed, so shortening a comment reports as the reduction it is.

Hitting it is the signal to move the explanation to `docs/`, never to
delete something true. It is blind to a comment that is simply WRONG,
to one fact restated in two files, and to code padded to buy
allowance - `tools/README-commentbudget.txt` says so.

> **THE SURROUNDING FILE IS NOT THE STANDARD.** `commentbudget
> --blocks` counts 1023 comment runs over twelve lines in `src/`. A
> dense neighbour reads as house style and is the single strongest
> pull toward writing more; it is evidence of the problem, not
> licence.

**PROGRESS, FINDINGS AND THE STANDING AUDITS LIVE IN THE JOURNAL,
NOT HERE.** This rule is the criteria; the journal is the state - it
records which files are done, the defect patterns the sweep keeps
hitting, the ratios that decide what to sweep next, and, importantly,
everything found that needs a CODE change and was therefore left
alone. Update it in the same session, the way rule 14 expects of any
other finding.

```
python tools/journal.py sweep                        which file next
python tools/journal.py ls --status open --pri 1     what else is open
python tools/journal.py about racerdraw.c            what we know
python tools/journal.py done 42 --note "..."         close one
```

`sweep` is one row per file - state, where to resume, and the comment
percentage that decides the order. Close a block with `sweep-set`.

### 16.1. The journal is `tools/journal.py`, and it is NOT `docs/`

**A gitignored SQLite database at `.journal.db`, for the agent, and
meant to be thrown away.** `tools/README-journal.txt` is the manual.

It exists because a text file forces read-modify-write of the whole
document, so every append risks the rest of it - which is not
hypothetical: one entry ended up cross-referenced from a stale
neighbour, a title claimed four rows over a table of five, and a
single line-ending slip turned a 140-line edit into a 4,461-line
diff. An `INSERT` touches one row.

**IT IS NOT A HOME FOR FACTS.** Rule 14.1 still stands and the
journal is not one of its homes:

| What | Where |
|---|---|
| true about the binary | the Ghidra plate |
| a format, a struct, a subsystem | `docs/` |
| why the C is shaped this way | the `src/` comment |
| project rules | this file |
| **what was done, what is open, what next** | **the journal** |

**The test is whether it survives the port.** Still true and still
wanted once the renderer is replaced? Then it goes in one of the
first four. Only about the state of the work? The journal.

Getting that wrong is what makes the database un-purgeable, and an
un-purgeable journal is just another document to maintain - which is
the thing this replaced.

> `docs/comment-audit.txt` IS GONE. Its progress, findings and
> defect catalogue are all journal rows now; the standing audits are
> `docs/comment-defects.txt`. A reference to it anywhere is stale.

---

## Repository layout

```
MOTORACER/
  CLAUDE.md              this file
  .claude/settings.json  permissions (read-only git)
  decomp/                Ghidra project + game binaries  [not committed]
    MOTO.EXE               the modified copy we analyse (7 patched sites)
    ORIGINAL_MOTO.EXE      untouched; the reference for every byte claim
    motoracer.gpr / motoracer.rep/
  docs/                  everything we discover (plain ASCII .txt)
    README.txt           doc conventions and evidence tagging
    structs/             one file per reconstructed struct
    subsystems/          one file per subsystem (renderer, audio, input, ...)
    binary-facts.txt     verified facts about the executable
    original-layout.txt  recovered original source tree
    glossary.txt         naming decisions and conventions
    comment-defects.txt  the sweep's standing checks, and their blind spots
  .journal.db            the agent's work state  [not committed]
  tools/                 scripts: analysis, Ghidra automation, data extraction
    journal.py           the dev journal - see README-journal.txt
    README-journal.txt   what goes in the journal and what does not
    asciify.py           plain-ASCII rule: --check / --fix
    commentbudget.py     no more comment than code - README-commentbudget.txt
    diff_original.py     byte-diff MOTO.EXE vs ORIGINAL_MOTO.EXE
    ghidra_restore_original_bytes.java   un-patch the 7 sites in the DB
    ghidra_grep_comments.java   search every comment in the DB
  src/                   reconstructed source - see src/README.txt
    mracer.sln             MSVC 2017, v141, no precompiled header
    main.cpp               the Win32 shell; builds and runs
  build/                 compiler output  [not committed]
```

`decomp/` is ~51 MB and holds the copyrighted game binary. It is gitignored, as
are extracted game assets. **Do not commit the binary or its assets.**

---

## Verified facts

### Binary

| | |
|---|---|
| Format | PE32, x86, little-endian, 32-bit |
| Image base | `0x00400000` |
| Functions in `.text` | 3,019 after boundary repairs (2,542 as imported) |
| Imported / external | ~282 |
| Of which game code | 2,070 (below `0x004b0000`); 1,328 over 16 bytes |
| Of which MFC/CRT | 949 (at/above `0x004b0000`) |
| Symbols | 13,728 |
| Total mapped | 3,324,776 bytes |

| Segment | Range |
|---|---|
| `.text` | `0x00401000`-`0x004cf5ff` |
| `.rdata` | `0x004d0000`-`0x004dadff` |
| `.data` | `0x004db000`-`0x0070c567` |
| `.idata` | `0x0070d000`-`0x0070edff` |
| `.rsrc` | `0x0070f000`-`0x007167ff` |
| `.reloc` | `0x00717000`-`0x0072e7ff` |

### Original source tree `[verified]`

Assert macros leaked 13 original paths under `D:\Dsed\mr320\`. This is real
structure - use it to attribute functions to their original translation units.

```
D:\Dsed\mr320\
    Dsi_d3d.cpp          animDecor.cpp     event.cpp
    frame3d.cpp          player.cpp
    menu\
        menu.cpp         bitmap.cpp        menuTools.cpp
        pageMainMenu.cpp pageProtocol.cpp
    screen3d\
        screen3d.cpp     dd.cpp            bankw.Cpp
```

The `Dsi_` prefix and `Dsed` almost certainly stand for Delphine Software
`[inferred]`.

**`mr320` is NOT this build's version** `[verified]`. Our version resource says
**3.22**, and the build that actually shipped as 3.20 was compiled from
`C:\Dev\Projects\Moto\` - a different root entirely. `mr320` is a working
copy or branch named after the 3.20 release, and 3.22 was built out of it seven
months later. Three builds are now identified, all with the same thirteen
translation units under three different roots:

| Version | Source root | Where |
|---|---|---|
| 2.47 "MotoRacer Game" | `D:\Projects\Moto238\` | inside `DISK1\data.z` on every retail CD |
| 3.20 "Full Direct3d version" | `C:\Dev\Projects\Moto\` | the Patch3.20 folder on the re-release |
| **3.22 "Full Direct3d version"** | `D:\Dsed\mr320\` | **ours** |

See `docs/releases.txt`. **No retail CD examined contains our binary.**

### Graphics stack `[verified]`

- Imports **ddraw** (`DirectDrawCreate`, `DirectDrawEnumerateA`) *and* loads
  `DDRAW.DLL` dynamically (`"Couldn't LoadLibrary DDraw"`, `"Couldn't QI
  DDraw2"`).
- **No Direct3D import.** D3D is obtained by `QueryInterface` off the DirectDraw
  object - the DX2/DX3-era pattern.
- The hardware path is **Direct3D Immediate Mode with execute buffers**
  (`D3DERR_EXECUTE_*`, `"Error in D3DUnLockExecuteBuffer lpBufEnd OverFlow"`).
  That API is long dead and has no modern equivalent to port to mechanically. It
  must be replaced, not translated.
- Hardware detection covers Direct3D HAL, **3Dfx**, and **PowerVR**.
- Supported resolutions: **320x240, 512x384, 640x480**.

### The 16-bit lock `[verified]`

`"16 bit screen mode required !"` appears in six languages. The lock is **four
separate things**, and they are not equally hard to remove - see
`docs/subsystems/display-mode-gate.txt`:

| Layer | Where | Removable? |
|---|---|---|
| desktop depth gate | `maybe_Cdd_ValidateDisplayMode` `0x0045a660` | yes, one branch |
| surface depth gate | `Cdd_Init` `0x0047642d` | yes, one branch |
| mode enumeration filter | `Cdd_EnumModesCallback` | yes, a filter |
| **RGB565 packing** | throughout `screen3d\` | **no - the real work** |

The primary gate is a **modal Abort/Retry/Ignore retry loop**, not a refusal:
it asks the user to change Windows' colour depth and re-reads the mode.

RGB565 pixel packing is baked into the software rasterizer, palette conversion,
blending, and texture upload - not confined to one mode-set function
`[inferred]`. Expect hardcoded 5/6/5 shifts and masks throughout `screen3d\`.
**Cataloguing every one of them is a primary early goal, and must happen before
either gate is opened** - a build that starts at 32 bpp and draws garbage is
harder to debug than one that refuses to start.

> **Both depth gates are patched in our copy** (`0x0047642d`, `0x0045a7c9`:
> `bpp == 16` flipped to `bpp >= 16`). Read the *original* bytes when
> cataloguing the lock, or you will record somebody else's edit as the game's
> design. Enumeration and RGB565 are untouched. See
> `docs/binary-modifications.txt`.

### Other subsystems `[verified]`

- **Audio** - DirectSound, plus `mciSendCommandA` (CD audio) and the `mixer*`
  API.
- **Input** - **no DirectInput.** Joystick via `winmm` (`joyGetPosEx`,
  `joyGetDevCapsA`), keyboard via `GetKeyState`.
- **Networking** - DirectPlay (`DirectPlayCreate`, imported by ordinal).
- **Config** - `.ini` files (`GetPrivateProfileStringA`) and the registry.
- **MFC and the CRT are statically linked** `[verified]` - visible both in the
  import table (`GrayStringA`, `TabbedTextOutA`, printer and common-control
  APIs) and in the named-function cliff at `0x004b0000`. All 949 functions at
  or above that address are library; the 2,070 below it are game code. See
  `docs/code-map.txt`.

### Command-line flags

The binary contains its own full flag documentation at `0x004dc664`. It is the
single best map of the game's subsystems - read it before exploring blind. It
covers language, windowed/fullscreen, 3Dfx/PowerVR/D3D selection, texture format
and size limits, dithering, palettes, Z-buffer, DirectDraw blit behaviour, MMX,
gouraud shading, CPU detection, AGP, thread priority, framerate cap, and
screenshots.

### This MOTO.EXE is modified - 7 sites, 14 bytes `[verified]`

`decomp/ORIGINAL_MOTO.EXE` is the untouched binary. The one we analyse differs
at **seven** sites. Verified by byte-diff, not inference:

```
python tools/diff_original.py     exits non-zero if anything else differs
```

| Address | Was | Now | Effect |
|---|---|---|---|
| `0x00414331` | `75 0c` | `90 90` | protection: insert-CD message box |
| `0x00442785` | `33 c0` | `eb f4` | protection: disc check returns 1 |
| `0x00442f44` | `74 91` | `90 90` | protection: CD-present local |
| `0x00444e73` | `74 5a` | `90 90` | protection: CD-audio playback |
| `0x0045a7ca` | `84` | `8d` | **colour depth**: `JZ` -> `JGE` |
| `0x0047642d` | `74` | `7d` | **colour depth**: `JZ` -> `JGE` |
| `0x0047e8db` | `0f 84 ...` | `e9 ... 90` | protection: nag overlay |

**Two of these are not copy protection.** At `0x0047642d` in `Cdd_Init`, and
again at `0x0045a7c9` in `FUN_0045a660`, a `CMP EAX,0x10` / `JZ` - *bpp == 16* -
was flipped to `JGE`, admitting 24- and 32-bit modes. So **part of the 16-bit
lock has already been removed in our copy.** The mode-enumeration filter and
the RGB565 rasterizer are untouched, and whether it actually runs at 32 bpp is
untested - do not assume it does.

The protection also has teeth that are *not* a refusal, and there are far
more of them than the seven patched sites suggest. When `g_protectionPassed`
is 0, `Carac_Load` subtracts 1.0 from every bike's `NbSpeed` - one gear
fewer, silently.

**And there are FOUR `"ILLEGAL VERSION"` sites, not one.** The string is
assembled four bytes at a time from immediates so it never appears in a
strings dump; the immediates `0x454c4c49` / `0x204c4147` are the only
handle. Three of the four are **live in our copy** - the crack neutralised
only the fourth:

| String at | In | Payload on failure |
|---|---|---|
| `0x0045d1fd` | `maybe_Racer_ApplyCornering` | zeroes `realSpeedMax` + `wheelieScale` |
| `0x00466e3b` | `FUN_00466d70` | draws only |
| `0x00468f10` | `maybe_Racer_UpdateGroundContact` | zeroes `active` |
| `0x0047e8f1` | `maybe_Race_UpdateProgress` | the block orphaned by the crack |

All payloads hit the **local racer only**. Zeroing `realSpeedMax` pins
both `speed` and `gearboxRoadSpeed` to 0 in `maybe_Physics_TickDrivetrain`,
so the bike cannot move; zeroing `wheelieScale` makes `Engine_TorqueAt`
return 0, so the engine makes no torque either - two independent kills
for one failed check. Zeroing `active` drops the racer out of
`Race_TickAllRacers` and the draw loop entirely.

Three *different* checks feed them, each with its own flag - **three
consecutive dwords** in `.data`, which is itself the tell:

| Flag | Address | Set by |
|---|---|---|
| `g_protectionPassed` | `0x004dc1f4` | the registry PId check |
| `g_codeCheckPassed` | `0x004dc1f8` | reads its **own code** at `0x00442761`, expecting `75 22` |
| `g_windowTokenOk` | `0x004dc1fc` | `GetWindowLongA(hwnd, GWL_USERDATA) + [0x00540488] == 0x7b5403` |

`FUN_00466d70` adds a fourth style: an eight-entry (offset, expected-byte)
table walked against `0x004b1830` - which is `maybe_CdAudio_GetTrackPos`,
the routine that asks MCI for the current track position. **That same
routine plants check C's token**: it stores the position into the window's
`GWL_USERDATA` as a side effect, which is the value check C reads back.
So all three checks guard the disc from three directions.

> **The `0x004b0000` cliff is not a clean library boundary.**
> `maybe_CdAudio_GetTrackPos` (mciSendCommandA plus a game error
> string), the FX trail emitter at `0x004b00e0` (1599 bytes,
> `__thiscall`, fifteen stack args), and `Rand_Next`/`Rand_Seed` are
> all game code above it. The cliff sorts by **link order**, not by
> origin. It is good for triage - deciding what not to read first -
> and is **not evidence** about any individual function. Read the
> function. See
> `docs/subsystems/copy-protection.txt` 5.3.2.

> **They are progression-gated, and that is the point.** Every one requires
> `g_nSelectedMode == 2` - championship - plus a championship-index test.
> A pirated copy plays perfectly through practice, single races and the
> first few championships, then quietly becomes unfinishable. It also means
> **no single-race capture can ever exercise them**, which is why every
> replay in this project has been blind to the whole mechanism.

**Our copy passes every check that can be tested statically** - `0x00442761`
reads `75 22` and all eight bytes at `0x004b1830` match, both verified. The
window-token check is runtime and cannot be settled under rule 8.

> **DO NOT PATCH `0x00442761`, `0x00442762`, OR `0x004b1830`-`0x004b18b6`.**
> They are watched bytes. We are two bytes away from an unplayable
> championship, and the symptom would appear hours later in a different
> subsystem. Both regions are bookmarked in the Ghidra DB under
> `copy-protection`. See `docs/subsystems/copy-protection.txt` 5.1-5.2.

The reconstruction restores the **original** bytes at all seven sites. Removing
the colour-depth gates is then a deliberate porting decision, not an inherited
patch.

> **The Ghidra DB already holds the original bytes at all seven sites.** So the
> decompiler shows the game's real logic, and the DB deliberately disagrees
> with `MOTO.EXE` on disk. Neither executable was written to, so
> `diff_original.py` is unaffected. Restoring site `0x0047e8db` also recovered
> the 153-byte `"ILLEGAL VERSION"` block into `FUN_0047e3d0` (2003 -> 2156
> bytes). A reimport brings the patched bytes back: re-run
> `tools/ghidra_restore_original_bytes.java`.

**This has already contaminated three documents.** In a modified binary,
"unconditional" is a claim that has to be earned: a flag with no conditional
writer, a comparison with no consumer, a branch with one edge, and a function
that always returns the same value are all things a patch produces. Check
`docs/binary-modifications.txt` before deep-reading anything on the
replace-list - `Cdd_Init` and `FUN_0045a660` both contain patched
instructions.

### The DATA folder is modified too

`decomp/DATA/SPEED.BKF` is **not stock** `[verified]`. Its directory and 61 of
its 62 members are byte-identical to the retail disc, but the archive has been
repacked - every name field's post-NUL padding is zeroed, which no other `.BKF`
of ours shows - and `CARAC2.BPT` inside it has **eight bike-statistics values
edited**, seven raised and one lowered.

So a conclusion drawn from `CARAC2.BPT` is a conclusion about somebody's cheat,
not about the game. This is the same hazard as the seven patched code sites,
one directory over. Everything else in `decomp/DATA` is clean: it either
matches a retail disc byte-for-byte, matches the 3.20 patch byte-for-byte, or
is an obvious runtime file. `docs/releases.txt` section 4.3.

A stock reference now exists at `B:\motoinstall\usa\DATA` - use it to settle
"is this ours or theirs" rather than guessing.

---

## Working method

### Triage before depth

3,019 functions in `.text`, of which 949 are MFC/CRT. Before any deep
analysis:

1. Fence off `0x004b0000`-`0x004cf5ff` as library so it stops polluting
   searches. That is 37% of the function count removed in one operation.
2. Attribute the remainder to the original translation units using the 13
   assert-path anchors in `docs/code-map.txt`.
3. Prioritize `screen3d\` and `Dsi_d3d.cpp` - that is where the mission lives.

The working set is **1,328 functions over 16 bytes**. The raw game count
is 2,070, but 742 of those are ten-byte SEH unwind thunks and fragments.
Filter by size before judging how much is left.

Two database defects were swept and fixed; see `docs/ghidra-db-repairs.txt`.
71 functions had been split into a stub plus an orphaned body, 68 KB of
code belonged to no function at all, and 30 more were never disassembled
because they are reached only through pointers in data. A fourth defect
was found later: Ghidra had promoted 32 switch **case bodies** to
functions, amputating them from their parents -
`MenuPage12_ControllerConfig_Tick` was showing 468 bytes of a 2230-byte
function. See `docs/ghidra-db-repairs.txt` section 10.

After any reimport run `tools/ghidra_check_pointer_tables.java`, then
`tools/ghidra_define_orphan_code.java`, then
`tools/ghidra_fix_split_functions.java`, then
`tools/ghidra_fix_case_functions.java`, repeating until all four are
quiet; then `tools/ghidra_restore_original_bytes.java` to undo the seven
patched sites in the DB, and finally
`tools/ghidra_commit_signatures.java` to rebuild prototypes. Distrust any
decompilation showing phantom inputs like `in_ZF`, and distrust any
function whose body looks too short for what it does.

### Spine first: depth follows the mission, not the call graph

Two halves of this binary need **different depths**, and the difference should
decide where time goes:

- **Replace-list** - `screen3d\dd.cpp`, `screen3d\screen3d.cpp`, `Dsi_d3d.cpp`.
  DirectDraw surfaces, the software rasterizer, D3D execute buffers, mode
  setting. No mechanical translation exists, so these need deep understanding:
  every hardcoded 5/6/5 shift, every surface assumption, every failure path.
- **Translate-list** - everything else. Physics, AI, menus, racers, file
  loaders. This is 32-bit C++ that will largely recompile as-is. It needs
  breadth and correctness, not depth. You do not need to understand the
  suspension model to port it.

So: **thin scripted layer everywhere, deep by hand only on the replace-list.**
Gameplay code stays at "named and bucketed" until the renderer is replaced.

**The rule that keeps this honest: when a call edge leaves the replace-list,
note the target and stop.** Following call edges depth-first is how a session
that began on the 16-bit lock ends up eight levels deep in bike characteristic
tables - every individual step locally sensible, the whole walk off-mission.
Record the edge as an open question and return to the spine.

Revisit this rule deliberately when the renderer is replaced, not before.

### Check `docs/` AND the Ghidra comments before naming anything

Grep `docs/` for an address before assigning a Ghidra name. This project's
notes now run ahead of the database in places, and it has already cost two
wrong names - `Trk_Load` was renamed to something invented while
`formats/trk.txt` had already walked the same function.

**Search the plate comments too.** They run ahead of `docs/` in other places,
and a global's identity is often already written in the comment of a function
nobody has read yet. `maybe_g_spanShade` was renamed three times across three
sessions - once in the wrong direction - while
`maybe_Screen3D_SpanShadedTex`'s plate comment said in capitals what it was
the whole time. Checking `docs/` alone is not enough:

```
tools/ghidra_grep_comments.java  006d1f80    # or any address / name
```

Searching comments for an address is cheap and belongs *before* reasoning
about what a global means, not after.

### Struct discovery is the core work

Reconstructing internal structs is the highest-value activity in this project;
everything else depends on it. When working a struct:

1. Find the allocation site and the size passed to it - that bounds the struct.
2. Cross-reference every field access; record offset, width, and observed
   values.
3. Apply the type in Ghidra so decompiler output improves for every caller.
4. Write it up in `docs/structs/` **in the same session**.
5. Re-read affected functions afterwards - a correct struct usually collapses a
   lot of noise and exposes further fields.

### Ghidra naming conventions

- Functions: `Subsystem_Verb` - `Screen3D_LockBackBuffer`,
  `D3D_CreateExecuteBuffer`.
- Use the original module prefix where the translation unit is known: `Dsi_`,
  `Menu_`.
- Unverified: prefix `maybe_` and leave a plate comment stating what is
  unconfirmed.
- Globals: `g_` prefix.
- Structs: `MR_` prefix (`MR_Player`, `MR_Surface`) to keep them distinct from
  Windows types.
- Never overwrite an existing meaningful name without saying why.

#### `FUN_`/`DAT_` IN `src/` IS A REFERENCE, NOT A STALE NAME

**A `FUN_00466940` or `DAT_004d0e58` written in a `src/` comment stays
as it is.** Do not "fix" it to whatever the Ghidra DB now calls that
address.

The name carries the address, and the address is the only thing that
survives. A Ghidra name is ours, it is revisable, and it lives in one
`.rep` on one machine - **lose the DB and `Racer_SpinWheels` points at
nothing, while `FUN_00466940` still points at the binary.** That is
the entire purpose of an ORIGIN tag (rule 13): to trace a line of the
reconstruction back to the bytes it came from, not back to a label we
invented.

So:

- **Leave `FUN_`/`DAT_` references alone** in comments and ORIGIN
  tags. They are not a defect and not a TODO.
- Writing the friendly name **as well** is fine and often useful -
  `Racer_SpinWheels @ 0x00466940` - because the address is still
  there. Writing it **instead** is the loss.
- Where a friendly name has already replaced a `FUN_` reference,
  leave it; converting back is churn for no gain.
- This does not change how functions are named **in the DB**. Name
  them; the conventions above stand. It governs what `src/` cites.

> A mass rename of `FUN_*` in `src/` was done once and reverted. It
> read as tidying and was a loss of provenance.

### Tools

Anything run more than twice becomes a script in `tools/`, with a header comment
saying what it does and how to run it. Python 3.10 lives at `C:\Program
Files\Python310\python.exe`.

---

## Documentation conventions

`docs/structs/<name>.txt` - RFC form like every other document, not
markdown. Rule 7 applies here too; this template used to show
markdown and contradicted it, and the struct docs written from it
had to be converted by hand:

```
MotoRacer PC Reverse Engineering                           Analysis Note


                             MR_PLAYER


Abstract

   Size: 0x1c4 [verified] - from the allocation at FUN_00401234.
   Freed at 0x00405678. Ghidra type applied: yes.

   One paragraph on what the thing IS.

Table of Contents

   1. The layout
   2. Open questions

1.  The layout

   +--------+------+-------+------------+-------------------------+
   | Offset | Size | Type  | Confidence | Notes                   |
   +========+======+=======+============+=========================+
   | +0x00  |   4  | int   | verified   | compared against 0..7   |
   |        |      |       |            | at 0x00412000           |
   +--------+------+-------+------------+-------------------------+
   | +0x04  |   4  | float | inferred   | fed to the same maths   |
   |        |      |       |            | as posY and posZ        |
   +--------+------+-------+------------+-------------------------+

2.  Open questions

   o 0x40..0x60 unexamined.
```

`docs/subsystems/<name>.txt`: what it does, entry points with addresses, the
structs it owns, how it talks to the rest of the game, and what still isn't
understood.

Keep an **Open questions** section in every document. An honest gap is more
useful than a confident guess.

---

## Legal note

A personal interoperability and preservation effort on a legally-owned copy. The
binary and game assets are not redistributed and are excluded from version
control. Reconstructed source is our own work product.
