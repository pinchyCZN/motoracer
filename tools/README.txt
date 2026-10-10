MotoRacer PC Reverse Engineering                           Analysis Note


                                 TOOLS/


Abstract

   Scripts for analyzing the binary, automating the Ghidra database, and
   extracting data.

   Python 3.10: C:\Program Files\Python310\python.exe

Table of Contents

   1. Rules
   2. Existing tools
   3. Suggested first tools
   4. mr_pose_ab.py - the pose A/B
   5. mr_wheel_ab.py - the wheel A/B, and why it forces the spin
      5.1. What the three phases check
   6. mr_steer_ab.py - the front wheel against steering


1.  Rules

   o Anything run more than twice becomes a script here.
   o Every script starts with a header comment: what it does, how to run
     it, what it expects.
   o Scripts that write to the Ghidra DB say so loudly in that header
     and support a --dry-run that prints intended changes without
     applying them. The DB is shared live with the user's open Ghidra
     GUI; a bad bulk rename is expensive to undo.
   o Output that is a finding goes to docs/. Output that is bulk data
     goes to a gitignored scratch path, not into the repo.
   o Every script must itself pass asciify.py --check.

2.  Existing tools

   +----------------------+---------------------------------------------------+
   | Script               | Purpose                                           |
   +======================+===================================================+
   | asciify.py           | Plain-ASCII + line width. --check / --fix         |
   +----------------------+---------------------------------------------------+
   | rfcfmt.py            | Render a markdown-ish draft as RFC text (one-way) |
   +----------------------+---------------------------------------------------+
   | ghidra_tu_fence.java | Ghidra: LIBRARY tag + TU anchor comments          |
   +----------------------+---------------------------------------------------+
   | mr_pose_ab.py        | Pose diff, MOTO vs port - see section 4           |
   +----------------------+---------------------------------------------------+
   | mr_wheel_ab.py       | Wheel matrix diff, MOTO vs port - see section 5   |
   +----------------------+---------------------------------------------------+
   | mr_steer_ab.py       | Front wheel placement vs steer - see section 6    |
   +----------------------+---------------------------------------------------+

   asciify.py enforces the plain-ASCII rule: every byte <= 0x7F, prose
   within 80 columns, table rows within 120. It transliterates non-ASCII
   to ASCII (em dash to -, arrows to ->, box-drawing to +-|). Its mapping
   table is keyed by codepoint integer so the script passes its own
   check. An unmapped character is reported and written as ? rather than
   guessed at.

   rfcfmt.py renders a markdown-ish draft into RFC layout: numbered
   sections, generated table of contents, body text indented three
   spaces and wrapped at 72 columns, and tables redrawn with +---+
   borders and space-aligned columns. All inline markup is stripped.

   rfcfmt is ONE-WAY. It refuses a file already in RFC form, because
   re-rendering its own output would flatten the layout back into
   paragraphs. Pass --force only if you mean it. Author new documents in
   RFC form directly and use rfcfmt only to convert a draft.

   ghidra_tu_fence.java applies the two triage operations described in
   docs/code-map.txt. It writes to the Ghidra DB; it is idempotent and
   has a DRY_RUN flag at the top.

3.  Suggested first tools

   Not yet written - listed so the intent is recorded:

   +------------------------+-------------------------------------------------------------------------------+
   | Script                 | Purpose                                                                       |
   +========================+===============================================================================+
   | find_565_constants.py  | Scan .text for RGB565 masks/shifts, group by function, report to renderer.txt |
   +------------------------+-------------------------------------------------------------------------------+
   | tag_runtime_funcs.py   | Identify MFC/CRT boilerplate and tag it in Ghidra so it drops out of searches |
   +------------------------+-------------------------------------------------------------------------------+
   | attribute_tus.py       | Map functions to translation units via leaked assert paths + call graph       |
   +------------------------+-------------------------------------------------------------------------------+
   | dump_config_globals.py | Walk the command-line parser and name every config global it sets             |
   +------------------------+-------------------------------------------------------------------------------+

4.  mr_pose_ab.py - the pose A/B

   Places MOTO and our port at the same spot with the same state,
   settles both, then diffs all 17 posed parts and prints the angle,
   pivot and matrix dmax for each.

       python tools/mr_pose_ab.py
       python tools/mr_pose_ab.py --settle 8 --racer 1

   Both games must already be running with their mrhook agent up -
   the script drives them, it does not start them, and CLAUDE.md rule
   8 forbids launching either.

   IT VERIFIES THE START STATE AND ABORTS IF IT DIFFERS. Position,
   orientation, moveDir, engine, speed and gearbox are written to both
   sides and read back before anything is measured. A pose diff taken
   from two different starting states is noise that looks like a
   finding, and this is the guard against reporting one.

   THE SETTLE LOOP IS NOT PADDING. headingSmoothed converges by
   delta/retardCadre and ours starts off-target, so a read one frame
   after the reset compares a transient rather than the model. Five
   rounds of 30 frames is enough on both sides; --settle raises it.

   READ THE PARTS, NOT THE WORST NUMBER. Parts 15 and 16 are the bike
   - front bodywork/fork and rear wheel - and are labelled in the
   output because they are what the draw work targets. 0 and 1 are the
   roots. Everything between is the rider.

   AN ANGLE dmax OF 6.28319 IS NOT AN ERROR. The trig table is indexed
   by (int)(a * 2607.5945876) & 0x3fff and 2 pi maps to exactly one
   full table, so a and a + 2 pi select the same entry. Parts carrying
   the wrap can and do report a matrix dmax of 0.00000. The matrix
   column is the one that means anything. See
   docs/racer-draw-audit.txt section 39.

   It was a scratchpad script for several sessions before it was
   promoted, which is the reason its defaults are a specific spot and
   racer rather than arguments.

5.  mr_wheel_ab.py - the wheel A/B, and why it forces the spin

   Diffs the matrix each wheel mesh is actually drawn with, MOTO
   against the port.

       python tools/mr_wheel_ab.py
       python tools/mr_wheel_ab.py --rear 0.7 --front 1.3

   It hooks 0x0048c70e, inside maybe_Inst_DrawWithMatrix, where EBX
   holds the matrix and EBP names the instance - racer+0x1a8 is the
   front wheel and racer+0x32c the rear.  Our side is read from the
   g_rdRearMat and g_rdFrontWheelMat mirrors.

   IT WRITES THE SPIN ANGLE INTO BOTH GAMES RATHER THAN DRIVING THEM
   TO IT.  racer+0x2e3d and +0x2e49 accumulate with distance, and the
   two engines do not agree on what a frame is, so any attempt to
   drive both to the same angle diverges - the same trap mr_ab warns
   about.  Forcing the angle removes the timebase from the comparison
   and leaves only the question being asked: given this spin, does the
   port build the same matrix?

   WHY IT EXISTS.  mr_pose_ab settles the bike to a standstill before
   comparing, so it is structurally blind to any term that is zero at
   rest.  The wheel spin is exactly such a term, and two separate bugs
   hid behind that - the spin never being accumulated at all, and then
   the spin leaking out of the wheel matrix into the swingarm and the
   fork pivot.  Both were found by eye and neither could have been
   caught by a comparison taken at rest.  See
   docs/racer-draw-audit.txt 52 and 53.

   THE LAST LINE IS THE REGRESSION GUARD.  rearFrameW must stay at
   identity while the wheels are spun: it is the frame the swingarm
   and the arm angle are built from, and if the spin ever leaks back
   into it that number moves off zero.

5.1.  What the three phases check

   +----------------+------------------------------------------------+
   | matrix         | the spin is FORCED equal on both, then the two |
   |                | drawn matrices are diffed. Isolates the build  |
   |                | from the physics entirely.                     |
   +----------------+------------------------------------------------+
   | rate           | spin advance divided by distance travelled,    |
   |                | which is timebase-free: the two engines reach  |
   |                | different speeds in the same nominal frames, so|
   |                | comparing raw advance would always fail.       |
   +----------------+------------------------------------------------+
   | brake          | input bit 1 must freeze the spin on both while |
   |                | the bike is still sliding. That is the gate at |
   |                | the head of Racer_SpinWheels.                  |
   +----------------+------------------------------------------------+

   THE RATE TOLERANCE IS 1 PER CENT AND THE MEASURED ERROR IS ABOUT
   0.04.  The residual is not a defect in the formula: distance is
   measured as a CHORD between two positions while the spin integrates
   the per-frame path, so a longer or more curved run reads slightly
   differently on each side.  Do not tighten it without changing how
   distance is measured.

6.  mr_steer_ab.py - the front wheel against steering

   Forces the same steering angle into both games, with fork dive and
   suspension zeroed, and diffs the offset the front wheel is drawn
   at - the vector maybe_Inst_DrawWithMatrix adds to the contact
   point, captured from ESP+0xc0 at the hook.

       python tools/mr_steer_ab.py

   IT FORCES THE ANGLE FOR THE SAME REASON mr_wheel_ab does.  Driving
   both sides with the same keys leaves them at different speeds and
   different steering, and the comparison then measures the physics
   gap rather than the placement maths.  A first attempt at this ran
   the two independently and reported errors up to 18 units that were
   almost entirely state divergence.

   WHAT THE SHAPE OF THE OUTPUT TELLS YOU, which is more than the
   dmax:

   o EXACT AT ZERO STEER AND WRONG OFF-CENTRE means the steer is
     entering the placement somewhere it should not.
   o Y MUST NOT MOVE.  The steer is a rotation about the bike's own
     Y axis, so the drawn wheel keeps its height - MOTO reads 30.576
     at every angle.  A Y that drifts means something angle-dependent
     is scaling the fork axis, which in practice means `reach` is
     moving because the pivot moved.
   o A CONSTANT RATIO in the x swing is the giveaway for a doubled
     rotation.  1.91 found the fork pivot being turned by the steer
     as well as the axis.

   IT RE-PLACES BOTH GAMES BEFORE EVERY ROW, and that is not
   optional.  Each row ticks both sides, which moves the bike and its
   wheel contacts, and the placement is measured from a contact - so
   without it the later rows read worse purely from their position in
   the sweep.  That artefact was once mistaken for a left/right
   asymmetry.  IF A ROW IS EXACT WHEN IT RUNS FIRST AND NOT WHEN IT
   RUNS FIFTH, the tool is measuring itself.
