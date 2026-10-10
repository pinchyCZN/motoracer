MotoRacer PC Reverse Engineering                           Analysis Note


                    MRHOOK, THE CAPTURE HARNESS


Abstract

   A DLL that loads into the original MOTO.EXE, answers the install
   check in memory, and dumps the values the reconstruction cannot
   derive - wheel positions, the racer matrix, the posed vertex pool,
   the camera - to a text file on a hotkey.

   IT CHANGES NO BYTES IN THE EXE ON DISK. The loader maps it because
   MOTO.EXE statically imports DirectPlay; everything after that
   happens in memory and stops when the process does.

   IT ALSO LISTENS ON A NAMED PIPE. The same DLL runs an agent
   thread answering STATUS and GET_MEM, so a value can be read out
   of the running game on demand instead of only at an F11
   snapshot. See section 8.

   Why this exists at all is in docs/runtime-capture.txt.

Table of Contents

   1. Building it
   2. Deploying it
   3. Using it
   4. Removing it
   5. Why it is dplayx.dll
   6. What it patches, and what it does not
   7. If it does not work
   8. The agent, and the MCP bridge
      8.1. The commands
      8.2. Talking to it
      8.3. What it deliberately cannot do
   9. Trace hooks
      9.1. Arming one
      9.2. The five ways it could be silently wrong
      9.3. One record pool per slot, and why that had to change
      9.4. The watchpoints got the same treatment
      9.5. Proven against the configuration that used to fail
   10. SEND_INPUT - driving the game from the agent
      10.1. The command
      10.2. The bits, read out of Input_OnKeyDown
      10.3. Pause, through the game's own toggle
      10.4. Counting frames
      10.5. Smoke tested against a live race
      10.6. Two things testing changed
      10.7. What still has to be decided
   11. SET_MEM - the one command that writes
      11.1. The command
      11.2. Why the reply carries both was and now
      11.3. What it refuses
      11.4. HOLD, and why it is not optional
      11.5. The bridge tool
      11.6. Verified before use
      11.7. What the first live session established
   12. MasterVolume - turning the game down
      12.1. Where it lives
      12.2. The setting
      12.3. Two things it has to get right
   13. Scripting the agent - five composed tools
      13.1. They share one map, on purpose
   14. Data watchpoints - SET_DATABP and GET_DATABP
   15. Function replacement - running our code inside MOTO
      15.1. The commands
      15.2. Why it is better evidence than the two-process A/B
      15.3. What it refuses, and why one refusal differs from SET_MEM
      14.1. The commands
      14.2. Four things that will bite
      14.3. Limits
      14.4. Reading a hit
      13.1. What it does, and why it needs no patch
      13.2. Telling a race from a menu
      13.3. Measured


1.  Building it

   tools\mrhook\build.bat

   Output is build\mrhook\mrhook.dll. Release|Win32 only, the same
   constraint CLAUDE.md rule 9 puts on the reconstruction.

   It finds MSBuild with vswhere rather than a hardcoded path,
   because the VS2017 directory differs between Community,
   Professional and BuildTools and a wrong one reads as a broken
   script rather than as a missing edition. It prints the built
   DLL's SIZE AND TIMESTAMP, which is the thing to check before
   believing a capture - a stale harness answers normally and
   silently lacks whatever was just added.

   By hand it is:

       msbuild tools\mrhook\mrhook.vcxproj /p:Configuration=Release
               /p:Platform=Win32

1.1.  The hotkeys

   +-------+------------------------------------------------------+
   | key   | what it does                                         |
   +=======+======================================================+
   | F11   | capture a snapshot                                   |
   +-------+------------------------------------------------------+
   | 2     | restart the race - posts ESC then ENTER              |
   +-------+------------------------------------------------------+
   | 1     | toggle the 90-degree side view                       |
   +-------+------------------------------------------------------+

   1 AND 2 ARE FREE KEYS AND THAT WAS CHECKED, not assumed.
   g_actionBindings (0x0053eae0) binds Z, X, the arrows, A and C,
   and Input_OnKeyDown's hotkey switch has no case for 0x31 or
   0x32. Read live before either key was chosen.

   Both are turned off individually in mrhook.ini - RestartKey=0
   and SideCamKey=0.

1.2.  The restart key

   PRESS 2 AND THE RACE RESTARTS. The DLL polls for it beside the
   F11 capture key and posts ESC then ENTER to the game window,
   which is what the pause menu restarts on. It is the loop F1
   gives on the port: crash, press one key, test again.

   IT ONLY GETS YOU TO THE GRID. The six-second countdown still
   runs unless it has been patched out - either once, with

       python tools/mr_cheats.py --on nocountdown

   or for every launch, with NoCountdown=1 in mrhook.ini. Both
   zero the 0x1770 immediate at 0x004112a3 so maybe_Race_TickClock
   takes the timer negative on its first tick and starts the race
   itself. The two together are what makes a retest instant.

   THE INI FORM PATCHES AT LOAD, from startup() and not from a
   hook. DllMain runs before the EXE entry point, so nothing is
   executing that code yet and no thread suspend is needed - which
   it could not have there anyway, the thread it would suspend
   being the one doing the patching.

   IT CHANGES WHAT mr_reset.py SEES, and that script was taught
   about it rather than left to guess. It used to wait for the
   countdown flag to go UP as its proof the restart landed; with
   the patch that flag rises and falls inside one tick, and a poll
   every 10 ms against a 33 ms frame misses it more often than not.
   A restart that worked perfectly then reported "ESC+ENTER did not
   restart it". mr_reset.py now reads the four bytes first and
   skips both the wait and the timer drive when they are zero.

   F1 is not used because MOTO binds it itself.

1.3.  The side camera

   PRESS 1 AND THE VIEW SWINGS 90 DEGREES, again. It is a view the
   game already has: camera+0x21c is an INDEX, not a flag, and its
   eight slots are listed in docs/subsystems/camera.txt 1.2. Slot 1
   is TAB, slot 2 is the left side and slot 3 the right, and nothing
   in the game reaches 2 or 3.

   IT IS RE-ASSERTED WHILE ON, ten milliseconds apart, so it
   survives both TAB and the race restart that the 2 key causes -
   each of which writes the same field.

   NOT camera+0x3c. That field looks like the view angle because
   Input_OnKeyDown writes pi into it alongside the slot, and it is
   not one: setting it to pi/2 gives slot 1, a flat 180, with the
   pi/2 discarded. This is how the first version of the key was
   wrong.

   tools/mr_cheats.py has the same thing as --on cam90, plus cam90r
   for the other side. The script version is a one-shot write and
   does not survive a restart; the key does.
2.  Deploying it

   RUN decomp\deploy-mrhook.bat. It BUILDS FIRST and copies only
   if that succeeded, so a deploy can never install a stale DLL over
   a failed edit - the old one stays and it says so. Then it copies
   under the right name, overwriting any previous one.

   MOTO.EXE must be closed; a running one holds dplayx.dll open, and
   the copy fails loudly rather than half-writing it.

   By hand it is:

       copy build\mrhook\mrhook.dll  decomp\dplayx.dll
       copy tools\mrhook\mrhook.ini  decomp\mrhook.ini     (optional)

   The name matters - the loader looks for dplayx.dll by name, and the
   application directory is searched before the system one because
   dplayx is not a KnownDLL.

   USE decomp\MOTO.EXE, NOT ORIGINAL_MOTO.EXE. The patched copy has
   both colour-depth gates open AND the protection satisfied, and that
   second part is not cosmetic: with the protection failing,
   Carac_Load silently subtracts 1 from every bike's NbSpeed, so a
   carac dump from the untouched binary would be wrong. None of the
   seven patches touches the geometry path.

3.  Using it

   Start the game. THE DUMP IS WRITTEN BESIDE THE DLL, NOT IN THE
   WORKING DIRECTORY - deployed as above that is

       decomp\mrcapture.txt

   The path is absolute and derived from GetModuleFileName on the DLL
   itself, so it does not follow the game's current directory. The
   first lines are written before the game's entry point runs, so the
   file existing at all confirms the DLL loaded.

   mrhook.ini IS OPTIONAL and normally unnecessary: DataPath and
   SavingPath default to the DLL's own folder, which is already the
   install root. The header echoes what they resolved to - check
   there rather than guessing.

   ONE SETTING IS NOT A PATH. NoD3D=1 forces the software renderer,
   and is the same switch as -NoD3D on the command line:

       [mrhook]
       NoD3D=1

   IT IS NOT A CONVENIENCE. On a machine where the 16-bit colour gate
   leaves the hardware path unusable, the software renderer is the
   difference between a visible game and a blank one - which is the
   difference between having a retail reference to compare against
   and having none.

   WHY IT CAN BE SET FROM DllMain. -NoD3D stores 1 at 0x004dc198
   (Main_InitAndParseCommandLine, 0x004148ac) and Settings_LoadAll
   reads it back at 0x0042bc40. THOSE ARE THE ONLY TWO REFERENCES IN
   THE IMAGE. Our DllMain runs during process init, before the EXE's
   entry point and therefore before both; and since the parser only
   writes when the flag is actually on the command line, an absent
   -NoD3D cannot overwrite what we put there. No hook, no timing, no
   race - a single store before anything reads it.

   The startup header prints "NoD3D forced" when it fires, so a
   session that did not take the setting says so.

   Navigate to the bike-select page and press F11. Each press appends
   a numbered capture block. The block records which menu page was up,
   so a capture taken on the wrong page is obvious rather than
   misleading.

   Press it again after stepping to a different bike - two captures
   that differ only in the bike index are worth more than one.

4.  Removing it

   Delete decomp\dplayx.dll. That is the whole uninstall: nothing is
   written to the registry, no file is modified, and the game returns
   to stock behaviour exactly.

5.  Why it is dplayx.dll

   MOTO.EXE statically imports two exports from ddraw.dll, two from
   dplayx.dll by ORDINAL, and one from dsound.dll. Any of the three
   would load, and dplayx wins twice:

   o NOTHING ON THE PATH TO PAGE 10 NEEDS DIRECTPLAY, so stubs that
     fail both calls cost nothing. A ddraw proxy would have to return
     a working IDirectDraw or the game dies - real work, no benefit.

   o DIRECTPLAY IS AN OPTIONAL WINDOWS FEATURE AND IS OFF BY DEFAULT
     on Windows 10. Without dplayx.dll present the loader refuses to
     start the process at all, before any of our code could run. So
     the stub is also the fix for a failure that would otherwise come
     first.

   The exports carry ordinals 1 and 2 through mrhook.def, because the
   import is by ordinal and a name-only export would not bind.

6.  What it patches, and what it does not

   IT REPLACES TWO FUNCTIONS, both in memory, both five bytes:

   +------------+---------------------------+------------------------+
   | Address    | Original                  | Replaced by            |
   +============+===========================+========================+
   | 0x0045a330 | reads a REG_SZ from HKLM  | answers DataPath,      |
   |            | SOFTWARE\DelphineSoft\    | SavingPath and         |
   |            | MotoRacer\CurrentVersion\ | Language in-process    |
   |            | Moto                      |                        |
   +------------+---------------------------+------------------------+
   | 0x0045a3f0 | the same for a DWORD      | answers Version 1 and  |
   |            |                           | InstallType 3          |
   +------------+---------------------------+------------------------+

   Both are __stdcall with three arguments - RET 0xc - and both are
   REPLACED rather than hooked, so there is no trampoline to get
   wrong. Nothing here wants the original behaviour for any value.

   o ANSWERING IN-PROCESS BEATS CREATING THE KEY. Writing HKLM needs
     administrator, and from a 32-bit process it lands in
     WOW6432Node, which is one more thing to be wrong about. This
     needs neither and leaves nothing on the machine.

   IT DETOURS NOTHING ELSE. Every value the capture wants persists in
   a global for as long as the page is up, so a snapshot on a hotkey
   gets all of it without touching the frame loop. Detours were the
   original design and were dropped as unnecessary risk.

   THE ADDRESSES ARE REBASED. They are written for the preferred base
   0x00400000 and every one goes through rebase(). A 1997 binary
   carries no DYNAMIC_BASE flag so it will load there, but the image
   has a .reloc section and one subtraction is cheaper than debugging
   a wild pointer. The capture header prints the delta.

7.  If it does not work

   o NO mrcapture.txt AT ALL. The DLL did not load. Check the name is
     exactly dplayx.dll and that it is beside MOTO.EXE, and that the
     DLL is 32-bit - a 64-bit build will be ignored silently.

   o THE AGENT ANSWERS, BUT WITH THE WRONG VERSION - a command you
     just added comes back "ERR unknown command" while STATUS works.
     A SECOND MOTO.EXE IS RUNNING. The pipe is created with
     nMaxInstances 1, so the process that started FIRST owns it and
     a later one's agent thread exits quietly. The symptom is
     indistinguishable from a failed deploy, and the deploy is where
     everyone looks first. Check the task list for more than one
     MOTO.EXE before rebuilding anything.

   o THE INSTALL MESSAGE BOX STILL FIRES. The capture header says
     whether the two patches were applied. If they were, the check
     that fires is a different one from the two registry reads and
     wants finding.

   o THE VERTEX DUMP SAYS "not dumping". Either the model is not
     loaded - check the menu page in the header - or one of the two
     MR_Mesh offsets is wrong. The count and pointer are printed
     either way, so a wrong offset shows as an implausible pair.

   o EVERY FLOAT LOOKS LIKE GARBAGE. Check the delta in the header. A
     non-zero delta with garbage after it means a hardcoded address
     was missed somewhere in this file.

8.  The agent, and the MCP bridge

   A second thread opens

       \\.\pipe\mrhook

   as soon as the DLL loads, and answers one command per line. The
   startup block in mrcapture.txt names it, so the pipe being up is
   visible without a client.

   The design follows the agent thread in
   N:\DEV\CDINO\cdino_pc\cdino_pc.c - duplex byte-mode pipe in
   PIPE_NOWAIT, one client, newline framing, one line back, a 25 ms
   poll. Anyone who knows that file knows this one.

8.1.  The commands

   +--------------------------+------------------------------------+
   | STATUS                   | OK STATUS base=.. delta=.. page=.. |
   |                          | bike=.. captures=..                |
   +--------------------------+------------------------------------+
   | GET_MEM <hexaddr> <size> | OK GET_MEM addr=.. size=.. <hex>   |
   +--------------------------+------------------------------------+
   | SET_MEM <addr> <hex>     | OK SET_MEM addr=.. was=.. now=..   |
   |         [HOLD <frames>]  | the ONLY command that writes; 11   |
   +--------------------------+------------------------------------+
   | SET_HOOK s a len n [cap] | OK SET_HOOK slot=.. addr=.. ...    |
   +--------------------------+------------------------------------+
   | CLEAR_HOOK [slot]        | OK CLEAR_HOOK ..                   |
   +--------------------------+------------------------------------+
   | GET_TRACE                | OK GET_TRACE hits=N |h0#1 esp=..   |
   +--------------------------+------------------------------------+
   | LOAD_MOD <filename>      | OK LOAD_MOD name=.. base=..    15  |
   +--------------------------+------------------------------------+
   | HOOK_FUNC <a> <exp> <on> | OK HOOK_FUNC addr=.. dst=..    15  |
   |                          | REPLACES a MOTO function           |
   +--------------------------+------------------------------------+
   | LIST_FUNC                | OK LIST_FUNC mod=.. <addr>=<exp>   |
   +--------------------------+------------------------------------+
   | UNLOAD_MOD               | OK UNLOAD_MOD name=.. unhooked=..  |
   |                          | unhooks everything, then frees     |
   +--------------------------+------------------------------------+

   Anything else, and every failure, is a single line beginning ERR.

   THE ADDRESS IS A PREFERRED-BASE VA - the form used in Ghidra and
   throughout docs/. It is rebased inside the game, so an address is
   pasted unchanged. Size is 1..4096.

   THE 4096 IS THE REPLY BUFFER, NOT A ROUND NUMBER. A reply is hex,
   so it costs two bytes per byte read plus a 34-byte header: 4096
   comes to 8226 of the 16384 available and 8192 would not fit. A
   compile-time assert in mrhook.c ties the two constants together,
   because agent_get_mem truncates rather than failing - too large a
   cap would return a SHORT reply that still says OK.

   Two guards, and they are not the same guard. The rebased address
   AND the last byte of the run must both lie in 0x00400000 to
   0x7FFFFFFF; then the copy itself runs under __try/__except, which
   catches an unmapped page inside a range that passed. Without the
   second, one bad address ends the session - the exact cost this
   harness exists to avoid.

8.2.  Talking to it

   tools/bridge_mcp_mrhook.py is an MCP server over stdio, with no
   package dependencies. .mcp.json registers it; Claude Code prompts
   once to approve it and the session must be restarted before the
   tools appear.

   IT ALSO RUNS STANDALONE, and that is the first thing to try:

       python tools\bridge_mcp_mrhook.py --cli STATUS
       python tools\bridge_mcp_mrhook.py --cli GET_MEM 004d102c 4

   --cli exercises the pipe with no MCP involved. From inside Claude
   Code the two failure modes look alike, and nothing else separates
   them.

   A known-answer test: 004d102c is 96.0f, the units-per-metre
   constant, so GET_MEM 004d102c 4 must return 0000c042. That checks
   the whole chain including the rebase.

8.3.  What it deliberately cannot do

   IT READS AND WRITES MEMORY, AND NOTHING ELSE. There is no break
   and no step. SET_MEM (section 11) is the only write, it refuses
   .text outright, and it is capped at 64 bytes.

   It runs on its own thread with no synchronisation, so NEITHER a
   read NOR a write is atomic with the frame loop - fine for statics
   and for anything stable while a menu page is up, useless for a
   value that changes within a frame. That is what SET_MEM's HOLD
   exists to work around, and it works around it by repetition
   rather than by synchronisation: it does not make one write
   atomic, it makes the value true for a stretch of frames.

   THAT IS THE SEAM. cdino carries a single-slot mailbox - a
   critical section, an interlocked pending flag, a reply event - so
   that a command can execute on the emulator thread at a safe
   point. Nothing here needs it yet. The first command that does -
   stepping, or reading a stack slot while a chosen function is
   executing - needs that mailbox brought over wholesale rather than
   bolted onto this loop.

   In particular GET_MEM CANNOT READ THE STACK SLOTS the front wheel
   work needs. [ESP+0x180] in maybe_Racer_Draw is meaningful only
   while that frame is live; by the time a pipe command arrives the
   frame is gone. That needs a detour at 0x0045fdb3, which is a
   separate change - see docs/runtime-capture.txt.

9.  Trace hooks

   GET_MEM reads an address. A stack slot is not an address - it
   means nothing once the frame has returned - so reading one needs
   code that runs at a chosen instruction. That is what a hook is.

   FOUR SLOTS. The instruction is replaced with a five-byte jump to
   a naked stub which saves the registers, records what was asked
   for, restores, runs the original instruction out of a trampoline
   and jumps back. The game executes the same instructions in the
   same order with the same register and FPU state.

   IT ONLY READS. Nothing is written into the game, and a hook
   disarms itself once its hit budget is spent, so a forgotten one
   cannot run forever.

9.1.  Arming one

       SET_HOOK <slot> <hexaddr> <len> <hits> [cap[,cap...]]

   len is the FULL BYTE LENGTH of the instruction being replaced,
   read off a disassembly listing. hits is 1..8. A capture is
   REG+hexoffset:size, up to four of them, 32 bytes each; the
   registers are EAX ECX EDX EBX ESP EBP ESI EDI.

   The worked example, and the reason this exists:

       SET_HOOK 0 0045fdb3 7 4 ESP+180:4,ESP+ac:4
       GET_TRACE

   0x0045fdb3 is FDIV [ESP+0xac] in maybe_Racer_Draw, seven bytes.
   [ESP+0x180] is A and [ESP+0xac] is B in S = |d| * A / B, the
   front wheel scalar that four attempts at static analysis failed
   to place. Both are named in racerdraw_fork_scale now; see
   docs/racer-draw-audit.txt 55 to 57.

9.2.  The five ways it could be silently wrong

   Each is handled, and each is worth knowing before hooking
   somewhere new.

   o INSTRUCTION LENGTH. Whole instructions must be relocated and
     the length cannot be known without decoding, so THE CALLER
     SUPPLIES IT. Give it wrong and the game is corrupted. Take it
     from the listing, never from a guess.

   o RELATIVE BRANCHES. E8, E9, EB and Jcc mean something different
     once moved to a trampoline. A site starting with one is
     refused. A branch in the MIDDLE of a longer stolen run is not
     detected - pick sites off the listing.

   o THE ESP ARITHMETIC. Entry is by JMP so no return address is
     pushed, then pushfd takes four bytes before pushad records the
     stack pointer. The site's ESP is therefore regs->esp + 4. The
     trace prints the corrected value; ESP+180 means what it says
     in the listing.

   o THE x87 STACK. pushad does not save it, and maybe_Racer_Draw
     is full of FPU work with live values in ST0. NOTHING HERE
     TOUCHES THE FPU - captures are memory reads only. Reading an
     FPU register would need fnsave/frstor and is a different tool.

   o OUTPUT. A hook on a draw function fires every frame, and
     logf() opens and closes the file per call. Hits go to a ring
     buffer instead and are drained by GET_TRACE.

   One more, not a correctness issue but worth stating: arming
   patches live code, so the game thread is suspended across the
   write. Its id is taken in DllMain, which runs on that thread.


9.3.  One record pool per slot, and why that had to change

   EVERY SLOT HAS ITS OWN POOL OF EIGHT. It did not always, and the
   way it failed is worth keeping because the symptom looked exactly
   like the opposite of the cause.

   Three hooks were armed for one race: a crash, a rider collision,
   and a checkpoint write. Only the COLLISION reported. The other
   two showed nothing at all, which reads as "those events did not
   happen" - and they had both happened, several times.

   THE RING WAS ONE ARRAY OF EIGHT SHARED BY ALL FOUR SLOTS, claimed
   first-come-first-served. maybe_Game_CollideRacers runs several
   times a frame, so it took all eight entries inside the first
   frame and there was nowhere for a rare hook's hit to land.

   And it was worse than crowding. The overflow path also did

       h->budget = 0;

   so the hook that arrived to a full ring DISARMED ITSELF having
   recorded nothing. A rare hook could be killed outright by an
   unrelated hot one, and nothing in the reply said so.

   What it is now:

   +---------------------+------------------------------------------+
   | s_hookRing          | [HOOK_MAX][HOOK_HITS_MAX] - per slot     |
   +---------------------+------------------------------------------+
   | s_hookCount[slot]   | recorded, written by the game thread     |
   +---------------------+------------------------------------------+
   | s_hookDrained[slot] | already sent, the pipe thread's cursor   |
   +---------------------+------------------------------------------+
   | s_hookDropped[slot] | defensive, reported as WARN hN dropped N |
   +---------------------+------------------------------------------+

   o A SLOT CAN ONLY FILL ITS OWN POOL, because the budget ceiling
     and the pool depth are the same number. What one hook does can
     no longer be seen by another.

   o THE OVERFLOW NO LONGER TOUCHES THE BUDGET. It counts, and the
     count is printed. A hook is now disarmed only by spending its
     budget, which is the one reason it should ever stop.

   o GET_TRACE DRAINS INCREMENTALLY. It prints from s_hookDrained
     and only recycles a slot once that slot is fully sent, so a
     reply that runs out of room ends with

         |MORE 7 hit(s) not shown, call GET_TRACE again

     rather than silently discarding the rest. Four full slots is
     32 hits, which can exceed one 16 KB reply.

   o THE RECYCLE IS A COMPARE-EXCHANGE, not a store. The game thread
     can append between the last record printed and the reset; a
     plain store would drop that hit and rewind the write index onto
     it. If the count moved, the cursor is left alone and the next
     call carries it.

9.4.  The watchpoints got the same treatment

   SET_DATABP's staging was already a proper head/tail ring that
   REPORTED its drops, which is why it never produced the silent
   failure above - but it was still one ring of 64 shared by four
   slots. With the default cap of 8 the four could not between them
   overflow it; raise two caps to 64 and they can, and then a hot
   watch eats a rare one's evidence exactly as the hooks used to.

   It is now s_dbpStage[DBP_MAX][DBP_STAGE] with per-slot head,
   tail and dropped counters, reported per slot:

       |WARN d2 dropped 5 (that slot's staging full)
       |MORE 12 hit(s) not shown, call GET_DATABP again

   o SO THE TWO FACILITIES NOW BEHAVE THE SAME WAY, which matters
     more than it sounds: the reason the hook failure cost a race
     was that its behaviour did not match the mental model built
     from using the other one.


9.5.  Proven against the configuration that used to fail

   The fix was tested by rebuilding the exact arrangement that lost a
   race of evidence, made deliberately harsher: TWO hot hooks, not
   one, against two rare ones. Two minutes of a replay [verified]:

   +------+--------------------------+--------+------+
   | Slot | Site                     | Rate   | Hits |
   +======+==========================+========+======+
   |  0   | maybe_Game_CollideRacers | hot,   |  8/8 |
   |      | 0x0047ecd0               | many   |      |
   |      |                          | /frame |      |
   +------+--------------------------+--------+------+
   |  1   | Racer_UpdateSuspension   | hot,   |  8/8 |
   |      | 0x00466a80               | /frame |      |
   +------+--------------------------+--------+------+
   |  2   | maybe_Racer_BeginCrash   | RARE   |    1 |
   |      | 0x00467e30               |        |      |
   +------+--------------------------+--------+------+
   |  3   | 0x0047deec, the write of | RARE   |    0 |
   |      | race state 3             |        |      |
   +------+--------------------------+--------+------+

   SEVENTEEN HITS: 8 + 8 + 1 + 0, no WARN, no MORE. Both hot hooks
   saturated their own pools inside the first frames and the rare
   crash hook still recorded its hit.

   o UNDER THE OLD BUILD THIS WAS IMPOSSIBLE. Slot 0 alone would
     have taken all eight shared entries before slot 2 ever fired,
     and slot 2 would have recorded nothing AND zeroed its own
     budget on the way past. One hot hook was enough; two made it
     certain.

   o SLOT 3's ZERO IS A RESULT, NOT A MISS. No racer entered state 3
     in those two minutes, which is now a trustworthy statement
     rather than an ambiguous one - that is the whole point of the
     change. Before, a zero could equally have meant "crowded out".

   What slot 2 caught, decoded:

   +-----------------+-----------------------------------------------+
   | ECX = ESI       | 0x00584421 - racer 1, the player              |
   +-----------------+-----------------------------------------------+
   | +0x124 speed    | 109.5659, against RealSpeedMax 110.0 - at     |
   |                 | the cap, like every other crash caught so far |
   +-----------------+-----------------------------------------------+
   | +0x2ddd clock   | 85408 ms = 1:25.41                            |
   +-----------------+-----------------------------------------------+
   | +0x2de5 state   | 1 - RACING, which is what                     |
   |                 | subsystems/race-standings.txt predicts        |
   +-----------------+-----------------------------------------------+
   | EBX             | 0x005875e6 = racer1 + 0x31c5, the WALL        |
   |                 | NORMAL - so this crash was a wall hit         |
   +-----------------+-----------------------------------------------+

   o AND SLOT 1 SETTLED SOMETHING FOR FREE. Its eight hits walked
     ECX through 0x00584421, 0x0058762a, 0x0058a833 and on, each
     exactly 0x3209 above the last - racers 1 to 8 in order. So
     Racer_UpdateSuspension runs for EVERY racer, not just the
     player: the AI bikes carry the same chassis spring model. That
     matters for the port, and it fell out of a hook that was only
     armed to be a nuisance.

10.  SEND_INPUT - driving the game from the agent

   DESIGNED, NOT BUILT. Everything below was verified against a
   running MOTO.EXE before any code was written; the addresses and
   the bit assignments are read, not assumed.

   The point is a probe loop that does not need a human at the
   keyboard: pause, set an input, run a bounded number of frames,
   pause again, read the result.

10.1.  The command

       SEND_INPUT <hold> <frames> [tap ...]

   +----------+--------------------+------------------------------+
   | Field    | Form               | Meaning                      |
   +==========+====================+==============================+
   | hold     | 8 hex digits       | driving-action bitmask, held |
   |          |                    | for the whole run            |
   +----------+--------------------+------------------------------+
   | frames   | hex, 1 .. 12C      | 300 = five seconds at 60 fps |
   +----------+--------------------+------------------------------+
   | tap      | vk or mod+vk, hex  | pressed and released on the  |
   |          | repeatable         | first frame                  |
   +----------+--------------------+------------------------------+

   Hex throughout, no 0x. The reply is one line:

       OK SEND_INPUT held=00000001 frames=3c ran=3c paused=1

   ALWAYS ENDS PAUSED. Unpause if needed, run exactly N frames,
   pause. The steady state between commands is "stopped and safe to
   probe", so commands chain and a dropped connection cannot leave
   the bike accelerating into a wall.

       SEND_INPUT 00000001 3C          accelerate, 60 frames
       SEND_INPUT 00000005 1E          accelerate + left, 30 frames
       SEND_INPUT 00000000 01 74       tap F5, advance one frame
       SEND_INPUT 00000000 01 11+56    tap CTRL+V

10.2.  The bits, read out of Input_OnKeyDown

   There is no need to fake a keyboard. The game keeps a PLAYER
   INPUT WORD and the key handler only sets bits in it, so the agent
   can write the same word:

       racer + 0x31e9        (&DAT_00584401 + index * 0x3209)

   +-----+---------------+--------------------------------------+
   | Bit | Action        | What the handler does                |
   +=====+===============+======================================+
   | 0   | accelerate    | sets 1, and CLEARS bit 1             |
   | 1   | brake         | sets 2, and CLEARS bit 0             |
   | 2   | turn left     | sets 4, and CLEARS bit 3             |
   | 3   | turn right    | sets 8, and CLEARS bit 2             |
   | 4   | higher gear   | sets 0x10                            |
   | 5   | lower gear    | sets 0x20                            |
   +-----+---------------+--------------------------------------+

   o NO COMBINATION IS REFUSED ANY MORE. SEND_INPUT used to reject
     accelerate-with-brake and left-with-right, on the reasoning
     that the handler clears the opposite bit and a keyboard cannot
     produce them anyway.

     THE FIRST OF THOSE HID A REAL BUG. Gas and brake and a
     direction together is how a dirt bike does endless donuts, and
     the game has a code path for exactly that combination - the
     turnBrakeFlag gate at 0x0048f915, which skips the whole
     deceleration block when a human on a MotoType 1 bike brakes and
     turns at once. The guard made that path impossible to drive
     from here, so the port's version of it could not be compared
     against the original at all.

     The mask is still bounded to bits 0..6. What the game does with
     a combination is the game's business, and finding that out is
     the point of the tool.

     o THE CLAIM THAT THE HANDLER CLEARS THE OPPOSITE BIT IS NOT
       VERIFIED and was the premise of the guard. Worth settling
       when convenient - read racer+0x31e9 back after holding both.

   o TURBO IS NOT A BIT. Action 6 writes two separate dwords, at
     racer+0x31f9 and racer+0x3201, both set to 1. It needs its own
     handling and is deliberately left out of the mask.

   o STAGE 2 IS GATED. The driving switch runs only when
     DAT_004e9834 is zero, and most cases additionally require
     DAT_005429b0. Read live during a race both were 0 and 0
     respectively - so the gate is real and SEND_INPUT should report
     rather than silently do nothing when it is shut.

10.3.  Pause, through the game's own toggle

   Case 0x13 / 0x50 of Input_OnKeyDown [verified]:

       DAT_004dc038 ^= 1;  FUN_004096e0(state);  FUN_00409700(state);

   +------------------+--------------------------------------------+
   | DAT_004dc038     | the pause STATE. Read it, do not guess     |
   +------------------+--------------------------------------------+
   | VK_PAUSE 0x13    | toggles with NO modifier                   |
   +------------------+--------------------------------------------+
   | P 0x50           | toggles only with CTRL held                |
   +------------------+--------------------------------------------+
   | g_nSoloMode      | must be non-zero - no pausing in link play |
   +------------------+--------------------------------------------+

   Reading the state is what makes the command idempotent: "ensure
   paused" is a comparison, where a blind toggle would flip the
   wrong way half the time. And because it goes through the game's
   own routine, a human CTRL+P still works normally afterwards.

   o HELP.GEN ADVERTISES PAUSE AS CTRL+P AND THAT IS NOT THE WHOLE
     truth. Bare VK_PAUSE works too, and is the better key for an
     agent to use because it needs no modifier state.

10.4.  Counting frames

   maybe_Game_RenderFrame increments two counters, and both were
   watched advancing on a live game:

       DAT_004f0dc4      104396 -> 105946
       DAT_004e981c      104418 -> 105996

   So the agent thread can poll one of them and needs NO new hook
   for timing. The 300-frame cap is a safety limit, not a technical
   one: a runaway command should cost five seconds, not a race.

10.5.  Smoke tested against a live race

   Every path exercised on ile01 with the game paused mid-race
   [verified]:

   +---------------------------+----------------------------------+
   | Call                      | Result                           |
   +===========================+==================================+
   | hold 0, 1 frame           | ran=1, re-paused. Window found,  |
   |                           | pause read, counter advanced     |
   +---------------------------+----------------------------------+
   | hold 1, 60 frames         | z 2654.6 -> 8417.0, x unchanged  |
   | accelerate                | at -128. It accelerates straight |
   +---------------------------+----------------------------------+
   | hold 5, 30 frames         | x -128 -> -1241.3, and the racer |
   | accelerate + left         | matrix row0 became (0.536, ~0,   |
   |                           | 0.844), squaring to 1.000 - a    |
   |                           | real yaw, not a sideways slide   |
   +---------------------------+----------------------------------+
   | tap 74 (F5)               | camera+0x220 went 0 -> 1         |
   +---------------------------+----------------------------------+

   o AND IT CONFIRMS F5. The tap moved the boom-table index, which
     is what reading Input_OnKeyDown had predicted - so F5 CHANGE
     VIEW is the camera DISTANCE selector, cycling & 3 over the
     four-entry table {0, 128, 256, 512}.

   o A MEASURED SPEED, free from the accelerate run: 5762 units over
     62 frames is about 5576 units a second. src/race.c's
     placeholder auto-speed is 900, roughly six times too slow.

10.6.  Two things testing changed

   FRAMES ARE APPROXIMATE, over by two or three. A one-frame request
   advanced the counter from 45 to 48: the poll sleeps 2 ms and the
   re-pause takes a few frames to land. Fine for driving the bike;
   NOT fine for anything that needs an exact frame, and the reply's
   ran= is where counting stopped rather than where the game did.

   THE BRIDGE NEEDED ITS OWN TIMEOUT. Every other command returns in
   microseconds, so 2 s was plenty; SEND_INPUT BLOCKS for the length
   of the run and a 60-frame hold blew straight through it. The tool
   now budgets frames/60 plus room for the two pause round trips,
   and stays above the DLL's 20 s guard so a real failure arrives as
   an error line rather than as a timeout.

   o THE FIRST FAILED RUN WAS NOT A FAILURE. The command completed
     inside the game - the counter had advanced and the word was
     cleared - only the reply was cut off. A blocking command on a
     pipe built for instant ones is worth spotting before it looks
     like a bug in the game.

   o AND THE BIKE NOT MOVING THE FIRST TIME WAS THE COUNTDOWN, not
     the command. Input is accepted and the word is written, but
     nothing moves until the race actually starts. Check the racer
     position has left the grid before concluding an input did
     nothing.


10.7.  What still has to be decided

   o WHICH VK THE TAPS GO THROUGH. The driving bits bypass the
     keyboard entirely, but a hotkey like F5 has to reach
     Input_OnKeyDown. Either post WM_KEYDOWN/WM_KEYUP to the game
     window, or call Input_OnKeyDown directly with a synthesised
     (hWnd, vkey, lParam). Posting is safer - it runs on the game's
     own thread at its own time - and is the recommended route.

   o MODIFIER STATE. g_shiftDown and g_ctrlDown are latches SET by
     Input_OnKeyDown and CLEARED by maybe_Input_OnKeyUp at
     0x00432700. A tap of "11+56" therefore has to post the CTRL
     down, the V down, the V up and the CTRL up, in that order, or
     the latch is left stuck and every later bare key is read as a
     CTRL combination.

   o READ THE BINDINGS, DO NOT ASSUME THEM. g_actionBindings at
     0x0053eae0 was read live and did NOT match the documented
     defaults - accelerate was Z, brake X, gear-up A, gear-down
     unbound, turbo C. That does not affect the input word, which is
     indexed by ACTION, but it does affect any tap that names a
     driving key.

11.  SET_MEM - the one command that writes

   Everything else here reads. This writes, so it is built to be
   reversible and to refuse the cases that end a session.

11.1.  The command

   +-----------------------------------+---------------------------+
   | SET_MEM <hexaddr> <hexbytes>      | OK SET_MEM addr=.. size=..|
   |         [HOLD <frames>]           | was=<hex> now=<hex>       |
   |                                   | [frames=.. ran=.. paused=]|
   +-----------------------------------+---------------------------+

   The address is a PREFERRED-BASE VA through rebase(), identical to
   GET_MEM, so an address out of Ghidra or docs/ pastes unchanged.

   The value is RAW LITTLE-ENDIAN HEX - exactly the encoding GET_MEM
   returns, so a read pastes straight back as a write. Length is
   implied by the digit count. Typing lives in the Python bridge,
   which keeps float packing out of the DLL.

11.2.  Why the reply carries both was and now

   o "was" IS READ IMMEDIATELY BEFORE THE WRITE, so the undo is in
     the reply. No experiment has to have remembered to snapshot
     first, which is the failure mode that makes a write command
     dangerous in the first place.

   o "now" IS RE-READ AFTER THE WRITE, not echoed from the request.
     A write that did not land - a read-only page in .rdata, or a
     field the game recomputes in the same instant - shows up as a
     "now" that is not what was asked for, rather than as a false
     success. That distinction is the whole point: a silent no-op
     would be indistinguishable from a working write, and every
     conclusion drawn afterwards would be wrong.

11.3.  What it refuses

   +---------------------------------+-----------------------------+
   | .text, 0x00401000..0x004cf5ff   | refuses to write code       |
   +---------------------------------+-----------------------------+
   | odd hex digit count             | bad value, whole bytes      |
   +---------------------------------+-----------------------------+
   | 0 or more than 64 bytes         | bad value, 1..64            |
   +---------------------------------+-----------------------------+
   | outside 0x00400000..0x7FFFFFFF  | address out of range        |
   +---------------------------------+-----------------------------+
   | the write threw                 | write faulted at ..         |
   +---------------------------------+-----------------------------+
   | HOLD frames outside 1..0x12c    | frames must be 1..12c       |
   +---------------------------------+-----------------------------+

   REFUSING .text IS THE IMPORTANT ONE. Struct work never needs to
   write code, a mistyped address does the most damage there, and
   SET_HOOK already exists for deliberate patching. It is an OVERLAP
   test against the address AS TYPED - the section bounds in docs/
   and in Ghidra are preferred-base - so a run that merely clips the
   end of .text is refused too, not just one that starts inside it.

   64 BYTES, AGAINST GET_MEM's 4096. A write should be surgical; 64
   is exactly a 4x4 matrix and far too small to smear a struct by
   accident.

   ALIGNMENT IS DELIBERATELY NOT ENFORCED. The racer's tail fields
   sit at 4k+1 - +0x2e1d and +0x2e39 among them - so an alignment
   check would refuse precisely the fields this exists for.

   .rdata is ALLOWED but will normally fault on a read-only page.
   The guard catches it and reports rather than dying, so probing
   0x004d1220 or a clamp constant costs nothing but an ERR line.

11.4.  HOLD, and why it is not optional

   A BARE WRITE CANNOT TEST A PHYSICS FIELD. maybe_Racer_UpdateLean
   recomputes racer+0x2e1d every frame, so a one-shot write is gone
   before anything draws it - and that is true of most of what is
   worth writing.

   HOLD <frames> rewrites the value every poll for N frames, counted
   off the game's own frame counter. It is the same trick si_run
   uses to defeat the key handler clearing the input word, reused
   rather than reinvented.

   FRAMES ONLY ADVANCE WHILE THE GAME RUNS, so HOLD does the same
   pause dance SEND_INPUT does: prove the game is pausable, unpause,
   hold, re-pause. A bare SET_MEM never touches the pause state.

11.5.  The bridge tool

   set_mem(address, value, type="hex", hold_frames=None)

   type is one of hex, f32, i32, u32. The numeric forms are packed
   little-endian in Python, so a float is written as "0.5" rather
   than as 0000003f by hand - and hand-packing IEEE754 is exactly
   the class of transcription error this project has already paid
   for twice.

   The .text refusal is duplicated in the bridge. That is deliberate
   redundancy, not an oversight: it turns a typo into an immediate
   error instead of a pipe round trip, and the DLL still refuses on
   its own if anything talks to the pipe directly.

11.6.  Verified before use

   +------------------------------------+--------------------------+
   | f32 0.5, -3.25; i32 100            | packed correctly         |
   +------------------------------------+--------------------------+
   | .text write, and one clipping its  | both refused             |
   | last byte at 0x004cf5fe            |                          |
   +------------------------------------+--------------------------+
   | odd hex, non-hex, 65 bytes         | all refused              |
   +------------------------------------+--------------------------+
   | hold_frames 0 and 301              | both refused             |
   +------------------------------------+--------------------------+

   Still to do against a live game: the non-destructive round trip
   (read a field, write the identical bytes back, assert was equals
   now), an independent GET_MEM read-back, and the first real use -
   writing racer+0x2e1d under HOLD to see the bike lean.

11.7.  What the first live session established

   SET_MEM worked end to end on its first use. All four guards fired
   correctly against the running game and the process survived the
   fault, which is the whole reason the guard exists.

   +-------------------------------------+------------------------+
   | write 12.5f to a field, read back    | landed, confirmed by  |
   | with GET_MEM, restore from was=      | a separate command    |
   +-------------------------------------+------------------------+
   | unmapped but in-range 0x7ffffff0     | ERR read faulted,     |
   |                                      | GAME SURVIVED         |
   +-------------------------------------+------------------------+
   | 0x90000000                           | ERR out of range      |
   +-------------------------------------+------------------------+
   | 0x0045d580, inside .text             | refused at the bridge |
   +-------------------------------------+------------------------+

   THREE THINGS ABOUT METHOD came out of it, and they matter more
   than the command does.

   o THE GAME DOES NOT REDRAW WHILE PAUSED. A 300-unit write to the
     player's position - a verified field - changed nothing on
     screen until frames were allowed to run. So a bare write plus
     "does it look different" IS NOT A TEST: the screen is a frozen
     frame and will report no change no matter what is written.
     Two null results were collected this way before the position
     control caught it. Always run frames before believing a
     negative.

   o HOLD IS A RACE, NOT A LOCK. It rewrites every 2 ms against a
     field the game recomputes every 16 ms, so most frames show the
     written value and some show the game's. Forcing the lean angle
     produced a bike FLICKERING between full lean and upright
     rather than a steady lean. That is enough to identify a field
     and not enough to observe one cleanly. Dropping the Sleep(2)
     in agent_hold would narrow the window.

   o "now=" AFTER A HOLD IS MISLEADING. It is read after the hold
     stops and the game re-pauses, by which point the game has had
     the last write. A held value that worked perfectly for 180
     frames still reports the game's value in "now". The field is
     behaving as specified - "now" is reality after the command -
     but do not read it as "what was held".

12.  MasterVolume - turning the game down

   MotoRacer is loud because its MASTER VOLUME option is not an
   internal software gain: IT MOVES THE WINDOWS MIXER. The game
   restores whatever level was saved on the machine it was last
   run on, regardless of where the system volume sits now.

12.1.  Where it lives

   Settings_LoadAll (0x0042b2a0) reads three registry values and
   hands each straight to Mixer_SetVolume (0x00470cf0), which ends
   in mixerSetControlDetails [verified]:

   +------+----------------+------------+----------------------+
   | line | key            | at         | pushed at            |
   +======+================+============+======================+
   | 0    | "MasterVolume" | 0x004e7ee4 | 0x0042b2f0  PUSH 0   |
   +------+----------------+------------+----------------------+
   | 1    | "CDVolume"     | 0x004e7ed8 | 0x0042b322  PUSH 1   |
   +------+----------------+------------+----------------------+
   | 2    | "WaveVolume"   | 0x004e7ecc | 0x0042b354  PUSH 2   |
   +------+----------------+------------+----------------------+

   The mixer object is at 0x005d2958. Its first dword is non-zero
   once initialised and the second is the HMIXEROBJ. THE RANGE IS
   mixer[line + 0x119] AND READS 32 - read live rather than
   assumed, since it is filled in during mixer init.

   o THERE IS NO COMMAND-LINE FLAG. The binary documents its own
     flags at 0x004dc664 and the list ends at -FrameRateMax.
     -NoSound is the only audio switch and it disables DirectSound
     outright. This was checked rather than guessed.

   o THE VOLUMES ARE NOT IN THE REGISTRY - an earlier version of
     this section said they were and that was wrong. They are read
     by Settings_ReadInt (0x00425bb0), which goes through
     FUN_00425080 to Crypt_Xor and sscanf, and written back with
     fwrite: AN XOR-ENCRYPTED SETTINGS FILE.

     The registry holds only the install information - DataPath,
     SavingPath, Language, Version, InstallType - which is the
     separate helper section 6 already replaces. TWO DIFFERENT
     STORES ON TWO DIFFERENT CODE PATHS.

   o SO THE EXISTING REGISTRY INTERCEPTION CANNOT CATCH IT. The
     obvious idea - answer the MasterVolume read the way DataPath
     is answered - does not apply, because the read never reaches
     that helper.

     Intercepting Settings_ReadInt instead would work in
     principle, but it serves EVERY setting, so it would need a
     conditional detour with a trampoline to pass the other reads
     through untouched. That is more machinery than the read-only
     hooks in section 9 have, and it would sit in the path of the
     controls and the graphics options too. Calling the game's own
     setter once is smaller and cannot affect anything else.

12.2.  The setting

       [mrhook]
       MasterVolume=8

   0 is silent and 32 is full. A value above the range is clamped
   to it rather than refused. Absent, or -1, leaves the mixer
   exactly as the game set it.

   IT IS APPLIED BY CALLING THE GAME'S OWN SETTER, so nothing is
   written to the registry and the change lasts only for this run.

12.3.  Two things it has to get right

   o IT CANNOT BE DONE AT DLL LOAD. mrhook's startup runs during
     process init, before the EXE's entry point, and the mixer
     object is still null. It also must not run before the game
     has set its own value or the game simply overwrites it. So
     volume_service() is called from the agent thread's poll loop,
     does nothing until mixer[0] and mixer[1] are both non-zero,
     then fires ONCE and marks itself done - winning by being
     later.

   o __thiscall FROM C. MSVC will not declare a __thiscall pointer
     in a C file. The standard __fastcall stand-in is used: ECX
     takes the object, EDX takes a dummy the callee never reads,
     and the two real arguments go on the stack in the same order
     and are cleaned by the callee. Same machine code, no
     assembly. The call runs under __try/__except, and a fault
     marks it done rather than retrying every 25 ms forever.

   The startup log says "MasterVolume set to N of M" when it
   fires, so a silent run means the value was absent, the mixer
   never came up, or the setter faulted.


13.  Scripting the agent - five composed tools

   THE COMMANDS ARE PRIMITIVES AND ARE MEANT TO BE COMPOSED. A
   sequence built from SEND_INPUT, GET_MEM and SET_MEM belongs in a
   script beside the bridge, NOT as another command in the DLL:
   changing it then costs an edit rather than a rebuild, a redeploy
   and a restart of the game.

   tools/mr_reset.py is the first of these and the pattern for any
   others. It imports bridge_mcp_mrhook for the pipe rather than
   reimplementing it:

       python tools/mr_reset.py            restart, skip the countdown
       python tools/mr_reset.py --state    where is the game, no change

   tools/mr_dump_racer.py is the second. It lays out a whole racer -
   the four model instances, every scalar, the three matrices, the
   pose map, all seventeen parts and the shadow geometry:

       python tools/mr_dump_racer.py            racer 1, everything
       python tools/mr_dump_racer.py --parts    just the skeleton
       python tools/mr_dump_racer.py --slot 1   parts from block 1
       python tools/mr_dump_racer.py --raw 2ea9 64    hex at an offset

   ONE SNAPSHOT, THIRTEEN READS, EVERYTHING SLICED LOCALLY. The whole
   0x3209-byte record comes back in one pass and every table is cut
   from that, so the dump is self-consistent - two fields printed
   together were read in the same instant, which hand-slicing across
   separate GET_MEMs never guaranteed.

   o IT PRINTS A FLOAT BESIDE ANY "int" OVER A MILLION. A field typed
     by guesswork shows up as a huge integer with a clean float
     reading next to it, and that is how racer+0x31b1 was caught
     being a float that src/racer.h declared as an int - on the first
     run of the tool.

   tools/mr_diff.py is the third, and it is the struct-discovery loop
   made one command: snapshot, do exactly one thing, snapshot again,
   print only what moved.

       python tools/mr_diff.py snap                baseline racer 1
       python tools/mr_diff.py diff                what changed since
       python tools/mr_diff.py run --frames 5 --hold accel
       python tools/mr_diff.py snap --addr 5dabcc --size 64

   RUN MODE IS THE ONE THAT ANSWERS QUESTIONS. It snapshots, holds an
   input for a bounded number of frames through SEND_INPUT, pauses and
   snapshots again - so the only thing that happened between the two
   reads is the input asked for.

   o WHAT MAKES IT READABLE IS THE NAMING. A raw byte diff of a racer
     is a wall of offsets. Every run is resolved against the block map
     first, so a change reads as "slot 0 part 5 angles.x" rather than
     "+0x083c". Five frames of acceleration produced 60 changed runs
     and ZERO of them unnamed.

   o THE MAP COMES FROM mr_dump_racer, imported rather than copied, so
     the two cannot drift apart. Naming a new field in one names it in
     the other.

   o A DIFF IS NOT A CAUSE. Thirty frames of anything moves the clock,
     the RNG and every physics scalar. It is a candidate list to
     narrow, and one frame beats thirty whenever the question allows.

   o TWO THINGS IT REPRODUCED WITHOUT BEING TOLD, which is the check
     that the naming is right rather than merely plausible: a posed
     part comes out as exactly 60 changed bytes starting at the
     angles, which is the static/posed split in
     docs/structs/MR_RacerPart.txt 1.1; and twenty frames of
     acceleration advanced the visible segment list at 0x005dabcc by
     exactly four entries.

   tools/mr_find.py is the fourth: where does this value live.

       python tools/mr_find.py 84.0           float, named results
       python tools/mr_find.py 3              int 3 AND float 3.0
       python tools/mr_find.py --int 12000    integer only
       python tools/mr_find.py --str DSI      an ASCII run
       python tools/mr_find.py --at 124       what is there, then
                                              every other copy of it
       python tools/mr_find.py 80.0 --all     all eight racers
       python tools/mr_find.py -96.0 --addr 4d0e88 --size 64

   o IT SCANS EVERY BYTE OFFSET, NOT EVERY DWORD, and that is the
     default deliberately. The racer tail is byte-misaligned
     throughout, so a dword-aligned scan misses most of the
     interesting half of the record. --aligned restricts it when the
     noise is worse than the misses.

   o --at IS THE ALIAS FINDER. It reads whatever is at an offset and
     then finds every other copy, which is how a field that exists
     twice under two names gets caught. Run on +0x124 it turned up an
     undocumented second copy of the speed at +0x12c.

   o A MATCH IS A COINCIDENCE UNTIL IT REPEATS. Four bytes of anything
     will occasionally read as the float you asked for. The one that
     means something usually lands in a structure you recognise, or in
     the same place across two racers - which is what --all is for.

   tools/mr_freeze.py is the fifth, and the only one meant to be
   left running while you play:

       python tools/mr_freeze.py            hold it, Ctrl-C to stop
       python tools/mr_freeze.py --status   report, change nothing

   THE CHECKPOINT TIMER AND THE START COUNTDOWN ARE THE SAME WORD.
   0x004dc0a8 counts down in milliseconds; while 0x004dc0b4 is set it
   is the six-second countdown, and the moment that clears the game
   reloads the same word with the race limit. So topping it up is all
   it takes to stop the checkpoint timeout - see game-object.txt,
   RACE COUNTDOWN.

   o IT TOPS UP RATHER THAN PINNING, and it will not write at all
     while the countdown flag is set. The race BEGINS when that word
     goes negative, so a value that never moves is a value the game
     can never use to make the transition - pinning it would stop the
     race ever starting.

   o LEAVE IT RUNNING WHILE YOU WORK. The pipe has ONE instance, so
     two tools cannot be connected at once, and this one reconnects
     every half second - which used to lose about one command in
     four and kill a multi-call snapshot outright. open_pipe in the
     bridge now waits out a busy pipe against a deadline instead of
     giving up after three tries. Measured with mr_freeze running:
     12 of 12 commands succeeded and a full snapshot cost 3 per cent
     more than with the pipe idle.

   o WaitNamedPipe ALONE WAS NOT THE FIX. The agent runs a
     PIPE_NOWAIT loop and only re-listens on its 25 ms poll, so
     between one client disconnecting and the server queueing another
     instance there is a window with nothing for Wait to hand over.
     Retrying around a deadline covers it; a fixed attempt count does
     not.

   o IT IS A DIAGNOSTIC OVERRIDE and it is off unless you run it.
     Nothing in the DLL changes and nothing persists. It exists so a
     lap can be ridden slowly while something else is being measured,
     which is the only way some racer fields are ever reached.

13.1.  They share one map, on purpose

   mr_diff and mr_find both name their hits through mr_dump_racer's
   tables, imported rather than copied. Naming a field once names it
   in all three, and the three cannot drift apart. A hit that comes
   back unnamed is a gap in the map and should be filled there.

13.1.  What it does, and why it needs no patch

   THE COUNTDOWN IS TWO GLOBALS [verified]:

       0x00411293  MOV [0x004dc0b4], 1       countdown flag on
       0x0041129d  MOV [0x004dc0a8], 0x1770  6000 ms on the clock

   and FUN_0047dae0, the race tick, does

       DAT_004dc0a8 -= DAT_004e9810;         by the frame delta
       if (DAT_004dc0a8 < 0 && DAT_004dc0b4)
           DAT_004dc0b4 = 0;                 ... and the race begins

   SO DRIVING THE TIMER NEGATIVE MAKES THE GAME START THE RACE
   ITSELF. Clearing lap state, resetting the racers, arming the
   input gate - all of it runs exactly as it would have six seconds
   later. NOTHING IS REIMPLEMENTED, which is the reason to do it
   this way rather than by poking the flag.

   o BOTH TAPS GO IN ONE SEND_INPUT. After ESC the menu is up, and
     SEND_INPUT cannot pause with a menu up - a second call is
     refused before its tap is ever posted. Posting is ordered, so
     the game digests ESC then ENTER over the frames that follow.

   o THE TIMER IS HELD NEGATIVE, not written once. The tick
     subtracts the frame delta from whatever is there and the start
     branch only fires on a frame that actually runs.

   o WAITS ARE WALL CLOCK, NOT FRAMES. The menu may stop advancing
     the frame counter, and anything counting frames would then
     block until its guard expires on every reset.

   o THE GAME MUST BE RUNNING TO CONSUME THE WRITE, and SEND_INPUT
     RE-PAUSES AS SOON AS ITS RUN ENDS. The first version held the
     timer negative in a sleep loop, which wrote -1 over and over
     into a frozen game and never fired anything: the flag stayed
     set and the clock read 4294967295 - which is -1 unsigned, so
     the write had landed perfectly and the tick had simply never
     run. The loop now writes, hands the game six frames through
     SEND_INPUT, and looks again.

     A PAUSED GAME LOOKS EXACTLY LIKE A STOPPED ONE from the frame
     counter alone, which is why --state reads the pause flag at
     0x004dc038 as well. Every reading in a session like this is
     taken while paused, so that is the normal case rather than
     the fault.

13.3.  Measured

   +------------------------------+---------------------------+
   | ESC and ENTER to a restart   | countdown armed in 0.06s  |
   +------------------------------+---------------------------+
   | countdown skipped            | 0.43s                     |
   +------------------------------+---------------------------+
   | position after the reset     | (-128.0, -0.0, 5120.0),   |
   |                              | identical to a hand       |
   |                              | restart, and identical    |
   |                              | again on the second run   |
   +------------------------------+---------------------------+
   | 5-frame throttle bump after  | speed 5.99 - the input    |
   |                              | gate is armed and it      |
   |                              | drives straight away      |
   +------------------------------+---------------------------+

   About a second, against ESC, ENTER and six seconds of watching
   a countdown. [verified]

13.2.  Telling a race from a menu

   There was no reliable way to do this and now there is. STATUS
   reports page=0 both in and out of a race, so it is not the check.
   Two things are:

   o SEND_INPUT ANSWERING "could not pause" MEANS THE RACE IS OVER.
     Pause does not work in the menus.

   o FRAMES ADVANCING WHILE THE RACE TIMER IS FROZEN means a menu.
     That is what --state reports, and it is the one case the reset
     cannot dig itself out of: ESC and ENTER mean other things
     there, so the race has to be re-entered by hand.

14.  Data watchpoints - SET_DATABP and GET_DATABP

   WHAT THESE ANSWER THAT SET_HOOK CANNOT: "which instruction wrote
   this address". A hook traps a site you already suspect. A
   watchpoint traps the DATA, so it finds writers nobody thought of.

   THAT GAP IS REAL AND WE FELL IN IT TWICE. A Ghidra search for a
   field's offset only finds stores that NAME the offset. A store
   through a pointer names nothing, and is invisible to it.
   racer+0x2ea9, the shadow vertex array, has ZERO instructions
   referencing it in the whole image - it was found only because the
   racer's constructor happens to build it by name. Finding what
   wrote racer+0x319d took two runs of four hook slots; one
   watchpoint would have named them at once.

   The mechanism is the x86 debug registers: DR0-DR3 hold four
   addresses, DR7 says trap on write or on access, and the CPU raises
   a #DB. Four is the hardware limit, not a choice.

   Lifted from N:\DEV\CDINO\cdino_pc\cdino_hooks.c, which
   implements the same thing with its failure modes written down in
   place.

14.1.  The commands

       SET_DATABP clear
       SET_DATABP add <hexaddr> [len=1|2|4] [mode=w|rw] [cap=N]
                                [changed] [val=IMM] [val!=IMM]
                                [tag=label]
       GET_DATABP

   There is no CLEAR_DATABP; "SET_DATABP clear" does that job.

   +----------+----------------------------------------------------+
   | len=     | 1 (default), 2 or 4. 8 is x64 only.                |
   +----------+----------------------------------------------------+
   | mode=    | w (default) traps writes; rw traps read OR write.  |
   +----------+----------------------------------------------------+
   | cap=     | auto-disarm after N hits. DEFAULT 8, 0 = unlimited,|
   |          | 64 max.                                            |
   +----------+----------------------------------------------------+
   | changed  | only record when the value actually differs.       |
   +----------+----------------------------------------------------+
   | val=     | only record when the value after the access        |
   | val!=    | matches, or does not match, this hex immediate.    |
   +----------+----------------------------------------------------+
   | tag=     | a label echoed back by GET_DATABP.                 |
   +----------+----------------------------------------------------+

   GET_DATABP reports the ARM STATE FIRST and then drains the hits.
   The state line is not padding: Windows sanitizes DR7 on the way
   in, an attached debugger steals the registers, and a silently
   dropped arm is indistinguishable from "nothing writes this"
   unless you look at what the hardware actually holds. It is read
   back OFF THE THREAD, next to what was asked for.

14.2.  Four things that will bite

   o THE REPORTED EIP IS THE INSTRUCTION AFTER THE STORE. A data #DB
     is a TRAP, not a fault: it is raised once the storing
     instruction has RETIRED. A hit reading eip=0045e2c3 was written
     by the instruction ENDING at 0045e2c3, not the one starting
     there. A write made by the CRT - memset and memcpy compile to
     rep stos - reports inside the CRT, not in the caller.

   o THE ADDRESS MUST BE NATURALLY ALIGNED TO len, and this bites
     constantly on MOTO.EXE because the racer tail is byte-
     misaligned throughout. +0x2e1d, +0x2e55, +0x3085 and +0x319d
     are all 1 mod 4:

         racer 1 = 0x00584421, +0x319d -> 0x005875be   (2 mod 4)
             len=4   REFUSED
             len=2   ok
             len=1   ok, and works at ANY address

     A 1-byte watch on the low byte of a dword catches every normal
     store to that dword, so len=1 is the answer rather than a
     compromise. x86 leaves an unaligned data breakpoint undefined,
     which is why it is refused rather than tried.

   o x86 HAS NO READ-ONLY ENCODING. mode=rw is an ACCESS watch, not
     a read watch. Infer a read from the value not changing.

   o A WATCH ON A HOT ADDRESS COSTS A KERNEL ROUND TRIP PER ACCESS
     and will visibly slow the game. That is what cap= defends
     against, and why it defaults to 8 rather than unlimited.

14.3.  Limits

   o ONLY THE GAME THREAD IS WATCHED. Debug registers are per
     thread. MOTO.EXE runs its frame loop, its physics and its draw
     on the thread DllMain loaded on, so this covers everything the
     racer struct sees - but a write from another thread is
     invisible and looks exactly like "nothing writes this".

   o A DEBUGGER OWNS DR0-DR3 and re-applies its own set on every
     resume. SET_DATABP refuses outright when one is attached
     rather than working half the time.

   o FOUR SLOTS, and they are the CPU's.

14.4.  Reading a hit

       |d0#1 eip=0045e2c3 eax=00000001 ecx=00000011 edx=0051e1c4
             ebx=00000000 esp=0019fae8 ebp=00000000 esi=00584421
             edi=00000001 old=00000000 new=00000001

   Slot 0, first hit. All eight registers come free with the
   exception CONTEXT, which is why - unlike SET_HOOK - there is no
   capture syntax to learn: esi=00584421 already says which racer,
   and old/new say what the write did.

15.  Function replacement - running our code inside MOTO

   THE HEADER OF mrhook.c USED TO SAY IT DETOURED NOTHING. That was
   true until this. A trace hook reads and returns - the stolen bytes
   run from a trampoline and the game carries on. HOOK_FUNC does not
   return: the entry of a MOTO function is overwritten with a jump
   into an export of a module we load, and the original never runs.

   The module is tools/mrmod, built and copied beside MOTO.EXE by
   deploy-mrhook.bat. It is NOT loaded automatically.

15.1.  The commands

       LOAD_MOD  mrmod.dll
       HOOK_FUNC 004b10c0 mrmod_plane_distance_bad 1
       HOOK_FUNC 004b10c0 mrmod_plane_distance_bad 0
       LIST_FUNC

   o LOAD_MOD TAKES A BARE FILENAME beside MOTO.EXE. A path is
     refused.

   o UNLOAD_MOD IS THE OTHER HALF, and without it the loop is not
     usable: every rebuild would need MOTO restarted, losing the race
     and the position the experiment was set up in. The cycle is

         rebuild -> UNLOAD_MOD -> LOAD_MOD -> HOOK_FUNC

     and the game never notices.

     IT UNHOOKS EVERYTHING FIRST, and that is not a courtesy. Freeing
     the module while a MOTO function still jumps into it leaves an E9
     pointing at unmapped memory; for a function as hot as
     Plane_Distance the game dies within one frame. If any hook will
     not restore, the module stays loaded and the reply says so - the
     safer half of a bad situation.

   o IT CANNOT RUN AT DLL LOAD. LoadLibrary under the loader lock
     deadlocks, which is why this is a command rather than something
     startup() does - commands arrive on the agent thread, so the
     timing is right for nothing.

   o HOOK_FUNC ECHOES THE FIVE ORIGINAL BYTES, so a hook that did not
     land is visible in the reply rather than at the next measurement.
     Those bytes are kept and put back on on=0; the game thread is
     suspended across both directions.

   o SIXTEEN CAN BE LIVE AT ONCE. The HOOK_MAX of 4 does not apply -
     that limit exists because each trace hook needs a hand-written
     naked stub with its slot baked in, and a replacement needs none.

   o CHECK LIST_FUNC BEFORE BELIEVING A MEASUREMENT. A hook left
     installed from an earlier experiment is the quiet way to misread
     a whole session.

15.2.  Why it is better evidence than the two-process A/B

   tools/mr_ab.py states its own ceiling: MOTO frames and ours are
   not the same thing, so only the SHAPE of a track can be compared,
   never a value. Two more limits follow from running two games:

   o EVERY DIFFERENCE IS CUMULATIVE. If lean differs at frame 40 the
     cause could be steering, ground contact, the drivetrain, the
     timebase or the placement. Nothing isolates one function.

   o THE WORLDS DRIFT. After the first difference the two games are
     in different states, and every later frame compares two
     different situations.

   A replacement has neither problem. It is called by MOTO, with
   MOTO arguments, in MOTO state, on MOTO frame N. There is one
   world, so the function either matches on that call or it does
   not. The question stops being "do the two games end up in the
   same place" and becomes "given these bytes in, does our function
   produce the same bytes out".

   THE CONVENTION NEEDS NO ASSEMBLY. The originals are mostly
   __thiscall - ECX carries the implicit this, the rest go on the
   stack, callee purges. MSVC will not let a free function be
   __thiscall, but __fastcall puts its first two arguments in ECX
   and EDX, pushes the rest and purges them. So

       float __fastcall f(this, unused, a, b, ...)

   is the same contract, with a dead parameter absorbing the EDX the
   original never set. Float returns come back in ST0 either way.

15.3.  What it refuses, and why one refusal differs from SET_MEM

   o outside .text (00401000..004cf5ff);
   o an address already hooked;
   o an export the loaded module does not have;
   o A FIVE-BYTE PATCH OVERLAPPING THE WATCHED PROTECTION BYTES -
     00442761..00442762, or the eight-entry table over
     004b1830..004b18b6.

   THAT LAST ONE IS NOT WHAT SET_MEM DOES, and the difference is
   deliberate. SET_MEM writes those bytes as DATA and allows it,
   because the protection reads its own code as data and an
   experiment may need to change what it sees. A jump destroys the
   check outright, and the symptom is not a crash - it is a
   championship that quietly becomes unfinishable hours later, in a
   different subsystem. Different verb, different call. See
   docs/subsystems/copy-protection.txt 5.1-5.2.

   WHAT IT DOES NOT CHECK is whether five bytes split an instruction
   at the entry. For a pure replacement that does not matter - the
   original bytes never execute and are restored verbatim - but it
   would matter the moment anything wants to call the original, which
   needs a trampoline and an instruction-length decoder. There is no
   trampoline here on purpose.  [open]
