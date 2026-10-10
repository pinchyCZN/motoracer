MotoRacer PC Reverse Engineering                           Analysis Note


                          THE MRACER SOURCE TREE


Abstract

   src/ holds the reconstruction: the source that is being rebuilt from
   the analysis in docs/, and the MSVC project that compiles it.

   As of this note it is a bare Win32 shell and nothing else. It exists
   to be a forcing function. Until there was something to compile, the
   question "which of the remaining functions matter?" could only be
   answered by ranking heuristics, and an early coverage note section
   10.3 records two occasions where those misled. A build turns that
   question into a list of undefined symbols.

Table of Contents

   1. Building it
   2. What is here
   3. Project settings, and why
   4. Win32 or x64
   5. The debug console
   6. Window placement and mracer.ini
   7. The back buffer and the present
   8. The data directory
      8.1. Relative to the EXECUTABLE, never the working directory
      8.2. data_path is for LOOSE files now
   9. The .LEZ decoder
   10. The sprite layer
      10.1. The four blit modes
      10.2. The tweening system
      10.3. One deliberate divergence
   11. Where the code goes as it arrives
      11.1. The gameplay file set is FIXED
   12. Menu page 1, and the page that was not reachable
      12.1. It was going to be the Delphine logo page
   13. The mesh loader
      13.1. The two scale factors
      13.2. What is deliberately missing
   14. The transform
      14.1. Three things that are easy to get wrong
      14.2. Checking it
   15. Texture pages and banks
      15.1. Two behaviours reproduced on purpose
      15.2. Checking it
   16. The D3D9 back end
      16.1. No DirectX redistributable, and how that was checked
      16.2. What the back end reproduces, and what it drops
      16.3. Three things that are ours, not the game's
   17. The first geometry: the Delphine logo
      17.1. Two things the data settled that guesses had wrong
      17.2. Checking it without running it
   18. The queue
      18.1. Three things in it that look like defects and are not
      18.2. The guard band, which is the nicest thing in the stage
      18.3. The detail table is GENERATED, not transcribed
      18.4. What is NOT reproduced, and what it costs
      18.5. Two hazards the exercise exposed
      18.6. Checking it without running it
   19. The .map flags to kind decode
      19.1. Why the feature-bit column is the whole point
      19.2. The tier2 selector, and who asks for it
      19.3. Two things measured rather than assumed
      19.4. What it changes on screen
   20. The shade ramp
      20.1. Three things worth knowing about it
      20.2. Two tables with similar names
      20.3. What is NOT reproduced, and why it is a real gap
      20.4. Checking it, and why Gouraud still is not visible
   21. The asset layer: reading the game's own archives
      21.1. One chokepoint, which is the whole point
      21.2. Measured, not hoped for
      21.3. What this does not cover yet
   22. The near-plane clipper
      22.1. What is transcribed and what is not
      22.2. Three things the edge clip actually does
      22.3. Where the pieces go
      22.4. Making it visible, and a wrong turn worth recording
   23. The two colour routes
      23.1. What that settles, and what it changed on screen
      23.2. An inference this file was carrying, now corrected
      23.3. The hardware path IS Gouraud shaded - a correction
      23.4. What is left of the shade work
   24. C or C++: what the binary actually requires
      24.1. The binary is C with classes used as namespaces
      24.2. The 415 "virtual calls" are COM, not the game
      24.3. The one real hierarchy is the fonts, and it is small
      24.4. The policy
      24.5. What is left of the engine
   25. The shaded transform, and Gouraud on screen
      25.1. Two surprises in the interface
      25.2. What is deliberately not reproduced
      25.3. Gouraud is finally visible
   26. The viewport clipper, and a hedge removed
      26.1. What it is
      26.2. The hedge it removes
      26.3. Why no code
   27. The cosine table
      27.1. It is cosine, and that was the whole question
      27.2. How it was found, since the obvious searches failed
      27.3. Two things transcribed rather than tidied
      27.4. What it explains elsewhere
   28. The frame loop
      28.1. The original targets 30, and busy-waits to get it
      28.2. Why ours was choppy, which was not the pacing
      28.3. What pacing actually looks like now
      28.4. Fixed simulation, variable render
      28.5. Input needs no thread, and audio is stubbed
   29. The 3x4 matrix helpers
      29.1. Matrix34_Copy is just a copy
      29.2. The element order, which matters more
      29.3. A correction worth recording
   30. The font family, and the .FNT format
      30.1. The vtable, confirmed
      30.2. The .FNT format, decoded
      30.3. What is honest rather than complete
      30.4. It draws
   31. The blitters, and a correction
      31.1. The colour LUT is the whole design
      31.2. Trame is a dissolve, not a fade
      31.3. A correction to section 30
      31.4. Two things it fixes on screen
   32. All eight blitters
      32.1. A miscount to own first
      32.2. Three of the eight are the same function
      32.3. CustomColor has no LUT, and that is the class
      32.4. Transp means it can reach zero ink
      32.5. Moto is the only hybrid
   33. menu.c, and where the game code goes
      33.1. It is a net file REMOVAL
      33.2. There is no giant switch, and there never will be
      33.3. Page 14, the title screen
      33.4. What page 14 does not do, and what gates it
      33.5. A field order worth knowing
   34. Polishing page 14, and a new evidence class
      34.1. [observed] - what a longplay is worth
      34.2. The strapline colour and font are both in the binary
      34.3. The sprite pointer table
      34.4. The demo3d letters are off by default
      34.5. What the recording gives the model instance struct
   35. MR_ModelInstance, and three hacks removed
      35.1. Rule 5a - never fake the retail look
      35.2. The three things it caught
      35.3. The struct
      35.4. What the recording did
      35.5. Still not drawable
   36. ModelInstance_Draw, and a cast that compiled
      36.1. It confirmed the struct rather than extending it
      36.2. The euler convention, from the disassembly
      36.3. A cast that compiled and was wrong
      36.4. What is still missing
   37. Page 14 animates, and rule 5b
      37.1. Rule 5b - discoveries flow BOTH ways
      37.2. The animation tables were invisible until their writer
            was found
      37.3. A discrepancy kept rather than resolved
      37.4. Two functions the port needed and did not have
      37.5. What it does now
   38. Two bugs the screen found
      38.1. 0x22 was a flags word, not a y coordinate
      38.2. The 3D layer was drawn outside the scene bracket
      38.3. What the phases actually are
   39. The view matrix was not identity
      39.1. What was wrong
      39.2. A retraction: there was no discrepancy
      39.3. The rotation was right all along
   40. The demo owned the pipeline
      40.1. What demo3d_init was quietly responsible for
      40.2. Three silent failures in a row
      40.3. The .map kind decode is not this
   41. primCount, and the diagnostics paying for themselves
      41.1. What it was
      41.2. The report found it immediately
      41.3. What the log confirmed on the way past
   42. The meshes were 96 times too small
      42.1. mesh_load_with_map(..., 0.0f)
      42.2. Five failures, one symptom
      42.3. A diagnostic that lied
   43. White squares: the bank rebase
      43.1. Mesh_RebaseFaceBanks
      43.2. A stale comment sent me the wrong way
      43.3. What the report will show now
   44. log.c
      44.1. Two calls, not a level system
      44.2. Taking the diagnostics out with the fix
      44.3. One per-frame line found and fixed
      44.4. get_log_file, and not copying the path logic
   45. Next step
   46. The virtual clock, and a primitive bisector
      46.1. The gotcha that decides the design
      46.2. Keeping the last frame visible turned out to be free
      46.3. Stepping
      46.4. g_queuePrimLimit
      46.5. Keys
   47. Two bugs in one chain: the euler matrix and the winding
      47.1. Matrix34_FromEuler was mis-transcribed
      47.2. Primitives were wound backwards
      47.3. Why they were hard to separate
   48. The transparency, and two switches with the same name
      48.1. What actually unblocked it
      48.2. Two implementations of one idea
         48.2.1. The other switch, finally read - and one divergence
                 removed
      48.3. The proxy that had been right until it wasn't
   49. Removing the proof-of-concept code
   50. Text buttons, and a clear that cleared too much
      50.1. Menu_ClearButtons does not clear the activation
      50.2. repeatOnHold, and the held mouse
      50.3. Buttons that say what is missing
      50.4. The volumes moved to audio.c, and they are not the mixer
      50.5. options.c, and one copy of every setting
      50.6. A button that lit up and could not be pressed
   51. C4013 IS AN ERROR, and why it has to be
      51.1. It has already happened once
      51.2. The one warning that is left
   52. Next step


1.  Building it

   Visual Studio 2017, toolset v141. Open src/mracer.sln, or:

       msbuild src\mracer.sln /p:Configuration=Release /p:Platform=Win32

   RELEASE|WIN32 IS THE ONLY CONFIGURATION THE AGENT BUILDS. CLAUDE.md
   rule 9 puts Debug and both x64 configurations off limits, and puts
   running any built executable off limits too. They are still
   configured and still in the solution - they are simply not what gets
   checked here, so treat a claim that they build as unverified unless
   you built them yourself.

   Release|Win32 rebuilds with 0 warnings and 0 errors at /W4.

   Output goes to build/<platform>/<configuration>/mracer.exe, which is
   outside src/ and already covered by .gitignore.


2.  What is here

   +---------------------+------------------------------------------+
   | File                | What it is                               |
   +=====================+==========================================+
   | mracer.sln          | solution, four configurations            |
   +---------------------+------------------------------------------+
   | mracer.vcxproj      | the project                              |
   +---------------------+------------------------------------------+
   | main.cpp            | console, window, message pump, and the   |
   |                     | window-placement save and restore        |
   +---------------------+------------------------------------------+
   | config.c / .h       | INI settings beside the executable       |
   +---------------------+------------------------------------------+
   | framebuffer.c / .h  | the 16 bpp back buffer and its present   |
   +---------------------+------------------------------------------+
   | lez.c / .h          | the .LEZ image format                    |
   +---------------------+------------------------------------------+
   | tga.c / .h          | the .TGA fallback loader                 |
   +---------------------+------------------------------------------+
   | sprite.c / .h       | MR_Sprite and the four blit modes        |
   +---------------------+------------------------------------------+
   | menures.c / .h      | the 209-entry menu resource manifest     |
   +---------------------+------------------------------------------+
   | page_loading.c / .h | menu page 1, the loading screen          |
   +---------------------+------------------------------------------+
   | mesh.c / .h         | the .3df geometry and .map formats       |
   +---------------------+------------------------------------------+
   | transform.c / .h    | object space to screen space             |
   +---------------------+------------------------------------------+

   main.cpp opens a debug console and a 640x480 window and runs menu
   page 1, the loading screen, every frame. If the data directory is
   not where the ini says, it falls back to a synthetic test pattern.
   Escape closes it. That is the whole program.

   640x480 is not arbitrary. It is the design resolution the menu art
   is authored against - Sprite_StartMove centres a sprite with
   (640 - width) / 2. See docs/structs/MR_Sprite.txt section 2.


3.  Project settings, and why

   +------------------------+---------------------------------------+
   | Setting                | Reason                                |
   +========================+=======================================+
   | no precompiled header  | asked for, and right: a reconstruction|
   |                        | wants every translation unit to state |
   |                        | its own dependencies                  |
   +------------------------+---------------------------------------+
   | CharacterSet MultiByte | the original is an ANSI program       |
   |                        | throughout - GetPrivateProfileStringA,|
   |                        | DirectDrawEnumerateA, its own WndProc |
   |                        | is the A variant. Building Unicode    |
   |                        | would mean rewriting string handling  |
   |                        | that is not ours to change            |
   +------------------------+---------------------------------------+
   | WIN32_LEAN_AND_MEAN    | keeps windows.h from pulling in the   |
   |                        | shell, RPC and OLE headers            |
   +------------------------+---------------------------------------+
   | /W4, ConformanceMode   | transcribed decompiler output is      |
   |                        | exactly the kind of code that hides   |
   |                        | sign and truncation bugs. The         |
   |                        | warnings are the point                |
   +------------------------+---------------------------------------+
   | winmm.lib              | the original paces animation off      |
   |                        | timeGetTime and reads the joystick    |
   |                        | through joyGetPosEx. Both are winmm   |
   +------------------------+---------------------------------------+
   | no MFC                 | the original links it, but only at    |
   |                        | its outermost shell. See              |
   |                        | docs/subsystems/app-init.txt 2.1.1.1  |
   +------------------------+---------------------------------------+

   WindowsTargetPlatformVersion is pinned to 10.0.17134.0. The
   floating "10.0" form is not accepted by this VS 2017 install - it
   fails with MSB8036 - so the version is written out. Installed SDKs
   here are 10.0.10240.0, 10.0.16299.0 and 10.0.17134.0; change the pin
   if you build elsewhere.


4.  Win32 or x64

   Both platforms are configured and both build. The mission targets
   64-bit Windows, so x64 is the destination.

   BUILD Win32 FIRST ANYWAY, and switch later. Everything arriving from
   the analysis assumes a 32-bit model: MR_Sprite is 0x74 bytes with
   4-byte pointers at 0x2c and 0x30, MR_Cd3d is 12 bytes and is three
   pointers, MR_Primitive is exactly 0x40. Those sizes are load-bearing
   - they are how file formats and ring buffers are indexed - and every
   one of them changes under a 64-bit pointer.

   Transcribe against the model the analysis was done in, get it
   running, and treat the 64-bit migration as its own deliberate pass
   with the struct sizes as the checklist. Starting at x64 means
   debugging the transcription and the pointer-width port at the same
   time.


5.  The debug console

   The project is /SUBSYSTEM:WINDOWS, so the process starts with no
   console and printf goes nowhere. console_open() in main.cpp
   allocates one and reopens stdout, stderr and stdin onto CONOUT$ and
   CONIN$, after which printf works normally.

   Three details worth knowing:

   o STDOUT IS UNBUFFERED. setvbuf(_IONBF) means a printf issued just
     before a crash still reaches the screen. That matters more than
     the throughput does while transcribing.

   o SCROLLBACK IS RAISED to 9999 lines. The default is a few hundred,
     which is not enough to hold the output of anything that runs per
     frame.

   o THE CLOSE BUTTON IS REMOVED from the console's system menu.
     Closing a console window terminates the whole process immediately,
     without ever delivering WM_CLOSE to the game window, which would
     silently lose the saved layout. The game window is the only way
     out.


6.  Window placement and mracer.ini

   Both windows save their position and size on exit and restore them
   on start. The file is mracer.ini, beside mracer.exe:

       [CONSOLE]
       x=60
       y=70
       w=720
       h=640
       [WINDOW]
       x=300
       y=250
       w=800
       h=600

   Rects are WINDOW rects in screen coordinates, not client rects.

6.1.  Why rcNormalPosition and not GetWindowRect

   The rect that gets saved comes from
   WINDOWPLACEMENT.rcNormalPosition, read by normal_rect_screen() in
   main.cpp - not from GetWindowRect.

   rcNormalPosition is the RESTORE rect, and it is meaningful in every
   window state: normal, arranged by a Win+Arrow snap, maximized, and
   minimized. GetWindowRect returns where the window is RIGHT NOW, so a
   window closed while snapped to the left half of the screen would
   reopen as a half-screen window forever, and one closed while
   minimized would save a rect somewhere off-screen.

   THE COORDINATE SPACE IS THE TRAP. rcNormalPosition is in workspace
   coordinates - relative to the work area of the PRIMARY monitor, not
   the monitor the window is on. The primary work origin is usually
   (0,0), but it is not when the taskbar is docked to the top or left
   edge, so the rect has to be offset by rcWork.left/top of the PRIMARY
   monitor to reach screen coordinates.

   Offsetting by the NEAREST monitor instead looks equivalent and is
   not: on a second monitor it adds that monitor's desktop origin a
   second time, so a window at x=1932 saves as x=3852 and marches
   further right on every run. The cdino version of this function
   carries a note about exactly that bug; this is the corrected form.

6.2.  Restoring safely

   clamp_win_rect_to_monitor() in config.c runs on every loaded rect
   before it is applied. A saved position can name a monitor that has
   since been unplugged, or sit under a taskbar that has moved, so the
   rect is snapped onto the nearest surviving monitor's work area and
   given a minimum size. Without it, a laptop undocked since the last
   run opens its window somewhere it cannot be reached.

6.3.  The window is resizable, deliberately

   The window uses WS_OVERLAPPEDWINDOW rather than a fixed-size style.
   A window without WS_THICKFRAME and WS_MAXIMIZEBOX cannot be snapped
   or maximized at all, which would make the rcNormalPosition handling
   above dead code.

   The back buffer stays 640x480 regardless; when the present exists it
   will be stretched to whatever the client area happens to be. If a
   fixed-size window is wanted instead, the style in CreateMainWindow
   is the only thing to change.


7.  The back buffer and the present

   framebuffer.c owns a 640x480 RGB565 buffer and gets it on screen.
   It is the host replacement for the DirectDraw back surface: the
   original locks a DDraw surface and hands the raw pointer and pitch
   to the software rasterizer, and those two values keep their original
   names here so transcribed code reads the same.

       extern unsigned short *g_frameBuffer;
       extern int g_frameStridePx;    /* PIXELS, not bytes */
       extern int g_frameWidth;
       extern int g_frameHeight;

   A row starts at g_frameBuffer + y * g_frameStridePx. The stride is
   not always the width: GDI wants DIB rows DWORD aligned, so at 16 bpp
   an odd width is padded by one pixel. 640 needs none.

7.1.  Why 565, and why we get to choose

   The original is ADAPTIVE. It asks DirectDraw what the surface format
   is and derives masks and shifts at run time into g_maskR, g_shlR,
   g_shrR and their siblings, so the same rasterizer runs against 565
   or 555 - see docs/subsystems/pixel-format.txt.

   We do not have to be. The buffer is ours, not the display's, so the
   format is a decision rather than a discovery: fixed RGB565, and GDI
   converts to whatever the desktop happens to be at present time. That
   removes a whole class of run-time variability from the port without
   losing anything, because nothing in the game ever reads the desktop
   format for its own sake - only to know how to pack.

   FB_RGB565(r, g, b) in framebuffer.h is the packing macro. It
   discards the low bits of each channel, which is the direction the
   original loses them in too.

7.2.  The present

   fb_blit() uses StretchDIBits with a BITMAPINFOHEADER carrying
   BI_BITFIELDS and the masks 0xf800 / 0x07e0 / 0x001f, and a NEGATIVE
   biHeight so the DIB is top-down and row 0 is the top. That matches
   the buffer's own indexing; a positive height would silently flip
   everything vertically.

   Two choices worth knowing:

   o THE IMAGE IS LETTERBOXED, not stretched to fill. The window is
     resizable, so the client area is rarely 4:3; the blit scales to
     the largest rect that fits with the aspect intact, centres it, and
     paints the margins black. Stretching to fill instead is a
     one-branch change in fb_blit.

   o COLORONCOLOR, not HALFTONE. Nearest neighbour is right for art
     authored at a fixed resolution, and HALFTONE additionally needs
     SetBrushOrgEx to stop its dither pattern crawling as the window
     moves.

   The present happens in two places: every pass of the main loop via
   fb_present(), and in WM_PAINT via fb_blit() on the BeginPaint DC so
   the update region is properly validated. WM_SIZE just invalidates -
   the blit already handles any client size.

7.3.  The test pattern

   draw_test_pattern() in main.cpp is scaffolding and should be deleted
   once a real sprite is on screen. It is built to fail loudly rather
   than plausibly:

   o Bands of pure red, green and blue, in that order top to bottom. If
     the BI_BITFIELDS masks were swapped the top band would come out
     blue - not a subtle difference.

   o A fourth grey band. Green carries six bits where red and blue
     carry five, so a grey ramp does not step evenly; the slight green
     tint on alternate steps is what distinguishes real 565 from 555.

   o A one-pixel white border, which proves the whole buffer reaches
     the screen and that the aspect-preserving blit is not cropping an
     edge.


8.  The data directory

   SEE CLAUDE.MD RULE 10 BEFORE EXTENDING ANY OF THIS. The deliverable
   is an executable someone drops into an existing MotoRacer install,
   so the reconstruction has to read assets out of the .BKF archives
   where a stock install keeps them. decomp/DATA/extracted/ is NOT
   shipped data - it came from an extraction tool and will not exist on
   anyone else's machine. Reading from it is a testing convenience with
   a due date, and every asset lookup should stay funnelled through one
   chokepoint so swapping the source is one change.

   THAT IS NOW DONE - see section 21. asset_load() in assets.h is the
   chokepoint, it reads the .BKF archives first, and the [PATHS]
   settings below are archive-relative prefixes rather than filesystem
   paths.

   The game's DATA tree is named in the ini rather than compiled in:

       [PATHS]
       datadir=.\DATA\

8.1.  Relative to the EXECUTABLE, never the working directory

   That default is the drop-in case: mracer.exe next to DATA inside
   somebody's MotoRacer install, working with no ini and no
   configuration at all. A relative value READ FROM the ini is
   resolved the same way, and an absolute one is left alone so a
   development ini can still point somewhere else entirely.

   The distinction matters more than it looks. A shortcut, a file
   manager and a debugger can each set a different working directory,
   and a game that only starts when launched from its own folder is a
   classic and entirely avoidable failure. get_exe_dir() in config.c is
   the one place that is decided.

   is_absolute_path() covers all three Windows cases, because getting
   this half right is worse than not doing it:

       C:\...          drive-qualified
       \\server\...    UNC
       \...            rooted on the current drive - left alone rather
                       than second-guessed by gluing a prefix in front

   o AN EXISTING mracer.ini IS NOT DISTURBED. The default is only
     written when the key is missing, and an ini that already holds an
     absolute datadir keeps using it. To pick up the new behaviour,
     delete the datadir line and let it be rewritten.

   get_data_dir() returns the result with a trailing backslash
   guaranteed, so callers can concatenate without thinking about it.

8.2.  data_path is for LOOSE files now

       char path[MAX_PATH];
       if (data_path(path, sizeof(path), "ILE01.BKF"))
           ...

   It refuses to overflow, and it strips any leading separator off the
   relative part so a stray backslash cannot silently turn the result
   into an absolute path.

   GAME ASSETS DO NOT GO THROUGH IT. They go through asset_load() in
   assets.h, which looks inside the .BKF archives first - section 21.
   data_path is what remains for the handful of things that really are
   loose files in DATA: the .BPT tables, the .trk files and the high
   scores.

   Nothing in src/ should contain an absolute path.


9.  The .LEZ decoder

   lez.c decodes the menu's image format: LZ77 over an 8-bit
   palettised image, magic 'LEZ1'. docs/formats/lez.txt has the layout,
   the bitstream, and the verification; the short version is that all
   143 shipped .LEZ files decompress to exactly their declared size,
   and ILE01/LOAD.LEZ comes out as the recognisable "Speed Bay"
   loading screen at 640x480.

       LEZ_IMAGE img;
       if (lez_load(path, &img)) {
           /* img.pixels is width*height RGB565, top-down */
           lez_free(&img);
       }

   TWO DELIBERATE DIVERGENCES from the original, both marked in the
   source:

   o EVERY READ IS BOUNDS-CHECKED. The original checks nothing. It can
     afford not to - it only opens files it shipped - but a decoder
     that walks three independent stream pointers off a header should
     not be the thing that turns a truncated file into a crash.

   o THE PALETTE IS ALWAYS BUILT OUT TO 256 ENTRIES, with the surplus
     black. The original converts only the entries the file declares,
     so a pixel indexing past the end samples pixel data as if it were
     a colour. Files with short palettes do ship - 201 and 160 entries
     are both present - so this is not hypothetical, though whether any
     of them actually index past the end has not been checked.

   The original splits loading in two: Lez_LoadFile reads the
   compressed file, and Lez_Decode decompresses it lazily on first
   draw. lez_load does both at once. Nothing here is expensive enough
   to be worth deferring, and the laziness only mattered because the
   original wanted to avoid decompressing pages the player never
   reaches.


10.  The sprite layer

   sprite.c is MR_Sprite: the 0x74-byte struct laid out to match the
   original field for field, a loader that picks between .LEZ and .TGA
   on the extension, the anchor logic, and the blitter.

   THE STRUCT IS EXACTLY 0x74 BYTES AND THE BUILD ENFORCES IT:

       typedef char mr_sprite_is_0x74[(sizeof(MR_Sprite) == 0x74) ? 1 : -1];

   Nothing depends on that yet. It is there because the sizes ARE
   load-bearing elsewhere in the engine - they are how file formats and
   ring buffers get indexed - and because a field identified later
   should land where docs/structs/MR_Sprite.txt says it does rather
   than wherever the compiler felt like putting it. It is also the
   first thing that will break under an x64 build, which is section 4's
   point made concrete.

10.1.  The four blit modes

       0  opaque copy
       1  colour-keyed: pixels equal to 0 are skipped
       2  stippled opaque: every other pixel, start column alternating
          per row
       3  stippled and keyed
       5  a sentinel meaning "use the sprite's own kind as the mode"

   MODES 2 AND 3 ARE A DITHER, NOT A BLEND. They never read the
   destination - they simply skip half the pixels in a checkerboard, so
   whatever is underneath shows through the gaps. Substituting alpha
   would look different, and it matters more than it sounds: an idle
   menu button with no alternate sprite is drawn in mode 3, so the
   stipple is on screen constantly. See
   docs/structs/MR_MenuButton.txt section 9.1.

   reveal and shrink are percentages. reveal skips rows off the TOP, so
   it wipes upward; shrink takes width off the right.

10.2.  The tweening system

   sprite_start_move and sprite_update_move are the whole of the
   menu's animation. Eight fields, a wall clock, and one of two curves.

   TWO THINGS THAT ARE EASY TO GET WRONG, and both were got wrong once
   in docs/structs/MR_Sprite.txt before the disassembly was checked:

   o THE DURATION IS IN SECONDS. The original takes a float and
     multiplies by 1000.0; its callers pass 0.7f and 1.0f. Passing
     milliseconds makes every animation finish in under a frame, which
     presents as nothing moving rather than as a units bug. The
     parameter here is named durationSec for that reason.

   o THE SINE EASE IS REMAPPED: (sin(u*PI - PI/2) + 1) * 0.5, not
     sin(u*PI - PI/2). The bare form runs -1 to +1 and would start the
     sprite a full delta before its origin.

   The clock is clamped rather than the interpolant, and completion is
   detected by comparing the clamped clock against endMs, so the final
   frame lands exactly on the target.

   One divergence: a zero duration divides by zero in the original and
   yields a NaN position. Here it is treated as already finished.


10.3.  One deliberate divergence

   The original folds the horizontal clip into an expression that is
   wrong when a sprite is BOTH off the left edge and wider than the
   clip rect - it subtracts the off-screen amount twice. No menu sprite
   is wider than the screen, so it cannot fire on shipped art. sprite.c
   clips correctly and says so in a comment, so a faithful
   transcription pass can decide to put the bug back.

   The other divergence is that loading decodes eagerly. The original
   stores the compressed file and decodes on first draw, which only
   ever mattered as a way of not decompressing pages the player never
   reaches.


11.  Where the code goes as it arrives

   The original tree is known - assert macros leaked thirteen paths
   under D:\Dsed\mr320\. See docs/original-layout.txt and CLAUDE.md.
   Mirror it, so a reconstructed file and the analysis note that
   describes it have the same name:

       src/
           main.cpp              the Win32 shell (ours, no original)
           menu/                 menu.cpp, bitmap.cpp, menuTools.cpp,
                                 pageMainMenu.cpp, pageProtocol.cpp
           screen3d/             screen3d.cpp, dd.cpp, bankw.Cpp
           Dsi_d3d.cpp
           frame3d.cpp, player.cpp, event.cpp, animDecor.cpp

   Those directories do not exist yet. Create each one when its first
   file arrives, not before.

   TWO OF THOSE ARE NOT TRANSCRIBED, THEY ARE REPLACED. screen3d/ and
   Dsi_d3d.cpp are the DirectDraw rasterizer and the Direct3D
   execute-buffer path; both are being rewritten rather than ported.
   See CLAUDE.md for what that
   costs.


11.1.  The gameplay file set is FIXED

   Five files hold all gameplay logic - physics.c, race.c, racer.c,
   bikes.c, grid.c - and a sixth is a decision to be raised, not made
   in passing. The table of what each owns, and the ORIGIN tag
   convention, are CLAUDE.md rule 13.

   The rule exists because it was broken: racetick.c and ridertick.c
   were added when race.c and racer.c already owned those concepts,
   and camera.c and a zero-byte agent.c the same way. All are folded
   back in and deleted.

   THE TELL WORTH KEEPING: a header nothing else includes is usually
   not a module. All three merged headers were included only by their
   own .c and by race.c. Check grep -l for the include before writing
   a new pair.

12.  Menu page 1, and the page that was not reachable

   page_loading.c is the first real screen: the track's load.lez as a
   background, the localised "Loading..." caption, the two cheat badges
   and the loading animation, each resolved by RESOURCE ID through the
   manifest in menures.c.

   That last part is the piece worth having. The game names its menu
   sprites by number, not by path; Sprite_Load takes an id, looks up a
   209-entry table, and builds the path from a per-entry localisation
   flag. All 209 entries are transcribed in menures.c, so every future
   page costs only its own layout.

12.1.  It was going to be the Delphine logo page

   Sections 9 and 11 of this note
   all named MenuPage25 - the Delphine Software logo, the first screen
   the game shows - as the target, on the grounds that it is a pure
   sprite composition.

   IT IS NOT. Its enter handler loads two raw textures and five 3df
   meshes from data\pages\titleDsi\ - camera, D, S, I and DsInt -
   scales them, initialises model instances and sets a camera; its tick
   animates six instances and calls Screen3D_DrawScene. The logo
   letters are geometry. It needs the whole renderer.

   Measuring instead of assuming: of the 26 live pages, 25 reach
   Screen3D_DrawScene, the cull and transform path, or 3df meshes. The
   26th, page 19, turns out to be a developer benchmark that allocates
   96000 bytes, profiles one call and draws nothing at all.

   Page 1 is the only page whose DRAWING half is sprite-only. Its tick
   does two jobs in sequence - paint the loading screen, then load the
   level and hand over to the game - and only the second half touches
   geometry. src/page_loading.c reimplements the first half.

   The lesson is the cheap one: the classification took a single
   closure walk over 28 handlers, and it would have been just as cheap
   before three documents committed to the wrong page.


13.  The mesh loader

   mesh.c reads .3df geometry and the paired .map that carries one
   texture record per face. Unlike everything else in src/ so far, the
   format was ALREADY fully decoded before the C was written -
   docs/formats/mesh-3df-map.txt, verified across all 128 .3df and 124
   .map files with every byte accounted for, and with a reference
   reader at tools/mesh3df.py.

   That makes this a transcription, not a reading, and it means the C
   is NOT independent corroboration of the format. tools/mesh3df.py is
   the oracle. mracer prints one mesh's counts and bounds at start-up
   so the two can be compared directly:

       python tools/mesh3df.py --info \
           decomp/DATA/extracted/MENU/MESHES/VITRE.3DF

   should agree with the console, which for that file means 59
   vertices, 99 faces, and x from -320.406 to 320.405.

13.1.  The two scale factors

   Writing the loader settled an open question in the format note. The
   two entry points scale vertices differently, and by a lot:

       Load3df        FMUL double ptr [0x004d0688]   2.8
       Load3dfAndMap  FMUL float  ptr [0x004d0690]   96.0

   A factor of about 34 for the same file. Load3df only applies its
   scale when its third argument is non-zero; Load3dfAndMap always
   applies its own. Both constants are exported from mesh.h so a caller
   has to pick one deliberately.

13.2.  What is deliberately missing

   o FACE NORMALS AND PLANE DISTANCES. The original computes both on
     load and carries a plane equation per face downstream. The winding
     and sign conventions live in Mesh_ComputeFaceNormal, which has not
     been read, and there is no consumer yet. Guessing a winding would
     be worse than leaving it out.

   o FLAT-COLOUR RESOLUTION. When a face's four UV pairs are all equal
     the original resolves one colour from the texture bank's page or
     palette instead of a texture. That needs the bank system and the
     .RAW loader.

   Both are marked in mesh.h so the gap is visible from the header
   rather than only from this note.


14.  The transform

   transform.c is the first piece of the RENDERER in src/, and it is
   the half that does not have to be rewritten: plain arithmetic, no
   DirectDraw, no Direct3D. From Screen3D_TransformVertsFlat
   (0x0049b2c0).

       camX = (x*m0 + y*m3 + z*m6 + m9 ) * g_projScaleX
       camY = (x*m1 + y*m4 + z*m7 + m10) * g_projScaleY
       camZ =  x*m2 + y*m5 + z*m8 + m11
       shade = intensity << 13
       invZ  = 1.0f / camZ
       if (camZ >= nearPlane) {
           screenX = round(centreX + invZ * camX)
           screenY = round(centreY - invZ * camY)
       }

   The matrix is 3x4, twelve floats, column major. MR_Vertex is 0x1c
   bytes and the build asserts it.

14.1.  Three things that are easy to get wrong

   o IT ROUNDS, IT DOES NOT TRUNCATE. The loop runs under a modified
     x87 control word that sets precision to single and masks two
     exceptions but LEAVES ROUNDING CONTROL ALONE, so FISTP rounds to
     nearest even. The truncation idiom people expect here sets bits
     10 and 11 with OR 0x0c00; this sets bits 2 and 3 with OR 0x000c.
     Same digits, different shift. transform.c uses lrintf; a cast
     would be wrong. See docs/structs/MR_Vertex.txt section 7.3.

   o THE VIEWPORT SCALES ARE FOLDED INTO THE MATRIX, once per call.
     camX and camY as stored are ALREADY scaled. That is the opposite
     of Screen3D_ProjectVertex, which keeps camX unscaled and applies
     the scale at projection. The two are alternative producers, not
     composable - feeding a vertex from one into the other applies the
     scale twice.

   o SCREEN COORDINATES ARE CONDITIONAL. They are written only when
     camZ passes the near plane, so a vertex behind the camera keeps
     whatever the previous user of that pool slot left there. Every
     consumer must test camZ first. transform.c reproduces this rather
     than tidying it, because the clipper is what makes the difference
     visible.

14.2.  Checking it

   mracer prints the first four vertices of VITRE.3DF transformed with
   an identity rotation, the mesh pushed 500 down +Z, and both scales
   at 1, so the arithmetic can be done by hand:

       camX = x, camY = y, camZ = z + 500
       screenX = round(320 + camX/camZ)
       screenY = round(240 - camY/camZ)

   For that mesh it should print shade 0x3e000 and the four corners at
   screen 321,240  319,240  321,240  319,240.

   One divergence: the pool has a capacity and refuses to overrun. The
   original has no bound check at all.

15.  Texture pages and banks

   rawtex.c reads a .RAW texture page; bank.c is the software half of
   the bank system that owns one. Both were transcribed rather than
   discovered - docs/formats/raw-texture.txt had already decoded the
   format across all 465 shipped files, and
   docs/subsystems/texture-banks.txt had already read bankw.Cpp end to
   end. tools/rawimg.py is the oracle for the first.

   The .RAW files are not a bespoke format. They are "mhwanh" images:
   a 32-byte BIG-ENDIAN header, magic, version 4, width, height, and a
   colour count where 0 means 24-bit truecolour. rawtex.c refuses any
   file whose header does not predict its size exactly

       0x20 + colours * 3 + w * h * (3 if colours == 0 else 1)

   because that formula reproducing all 465 file sizes is what pinned
   the layout in the first place. A file that fails it is not a file
   this code understands.

   A bank is one 256 x 256 16-bit page plus up to sixteen palette
   slots. bank.h carries only the software half: the decoded page, the
   palettes and the flags that say how to read them. The DirectDraw
   surfaces, the D3D texture slots and the reserved/published slot
   arrays that share the original record are all on the replace-list
   and mean nothing until a back end exists.

15.1.  Two behaviours reproduced on purpose

   o MODE 0 KEEPS THE PALETTE. The page holds one-byte indices and the
     palette resolves them. Mode non-zero expands the indices through
     the palette into 16-bit pixels and frees the palette. So "mode"
     is not a quality or a filtering hint; it chooses the texel
     format.

   o MODE 0 ON A 256x256 TRUECOLOUR FILE READS NO PIXELS AT ALL. The
     page is allocated, nothing is written into it, and the loader
     returns success. That was read from the disassembly rather than
     the decompiler, and the jump target was checked to be the
     ordinary success exit and not a bulk fast path - because a fast
     path was the comfortable explanation and it is not what is
     there. texture-banks.txt section 4.2.2 then found it FIRES ON
     SHIPPED DATA, for the Speed bikes.

     bank.c reproduces the skip and prints a line when it does. A
     caller that wants pixels out of a 256x256 truecolour page must
     not pass mode 0; MenuPage25 passes 1.

   The page is also always allocated the full 0x20000 whatever the
   file holds, so an indexed page writes 256 bytes into each 512-byte
   row and occupies half of what it reserves. A port that sizes the
   allocation from the texel format will differ from the original.

15.2.  Checking it

   The bank pages are now checked by being used: section 17's geometry
   is textured from bank 0, which is BNK00_00.RAW, the page that
   carries the Delphine logo letters. A wrong decode shows as a wrong
   picture on real geometry, which is a better test than the overlay
   that used to sit in the corner here, and the overlay has been
   removed.

   The mode-0 skip is still demonstrated deliberately: demo3d_init
   loads the same file a third time into bank 2 with MODE 0, nothing
   draws it, and its console line is the whole point.

       bank: 2 ...BNK00_00.RAW 256x256 truecolour mode 0 - page left
             empty, matching the original


16.  The D3D9 back end

   render.h is the seam and d3d9rt.cpp is the first thing behind it.

   The seam is not invented. docs/subsystems/renderer.txt section 3.4
   establishes that the hardware renderer is six geometry calls and six
   state calls, that everything above them only QUEUES primitives, and
   that Screen3D_DrawScene is a single if - one branch runs the
   software rasterizer, the other hands the same queued list to
   D3D_DrawScene and returns. render.h is those twelve calls with the
   execute-buffer encoding removed.

   What made this tractable rather than a rewrite is the vertex.
   D3D_WriteVertex fills a standard D3DTLVERTEX and advances by 0x20 -
   sx, sy, sz, rhw, colour, specular, tu, tv - and D3D9 still accepts
   exactly that as

       D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_SPECULAR | D3DFVF_TEX1

   The game already produces screen coordinates with 1/z in rhw and
   colours it computed itself through a shade table, so there are no
   matrices, no lighting and no vertex shader. R_Vertex in render.h is
   that struct, and the static assert on 0x20 is there to keep it that
   struct.

16.1.  No DirectX redistributable, and how that was checked

   The requirement is that a clean Windows 10 install runs this with
   nothing downloaded. That rules out more than it first appears to,
   and the traps are not where a modern reader expects:

   o d3d9.h, d3d9caps.h and d3d9types.h come from the WINDOWS SDK, in
     Include\<ver>\shared. Nothing here includes a header from the June
     2010 DirectX SDK.

   o NO D3DX. d3dx9.h and its d3dx9_43.dll ARE the redistributable,
     and they are the usual way a D3D9 program acquires that
     dependency without meaning to - D3DXMatrixPerspectiveFovLH,
     D3DXCreateTextureFromFile, ID3DXFont. None of them appear.
     Textures come from our own .RAW loader, there are no matrices at
     all because the vertices are pre-transformed, and there is no
     font.

   o NO SHADERS, so no d3dcompiler_47.dll. The fixed-function pipeline
     does exactly what the original asks for - one texture stage set
     to MODULATE - so this is a closer match than a shader, not a
     compromise.

   o NO D3DDEVTYPE_REF FALLBACK. The reference rasterizer only exists
     where the SDK debug runtime is installed, so asking for it on a
     clean machine fails. d3d9rt.cpp refuses to create a device rather
     than pretending to.

   o d3d9.dll is loaded BY NAME with LoadLibrary rather than linked,
     so the executable carries no import from it and d3d9.lib is not
     in the link line. The original does the same thing for
     DirectDraw - "Couldn't LoadLibrary DDraw" is a string in the
     binary.

   THE TRAP THAT ACTUALLY BIT was none of those. It was the C runtime.
   The first build linked /MD and imported VCRUNTIME140.dll, which is
   the Visual C++ redistributable and is NOT part of a clean Windows
   10 - the api-ms-win-crt-* stubs are, but VCRUNTIME140 is not. Both
   configurations now use the static CRT, which is also what the
   original does: MFC and the CRT are statically linked in MOTO.EXE.

   Checked, not assumed:

       dumpbin /DEPENDENTS build\Win32\Release\mracer.exe

           WINMM.dll
           KERNEL32.dll
           USER32.dll
           GDI32.dll

   Four OS DLLs, all present on a clean install. That is the whole
   dependency set.

   The one thing this cannot promise is a DRIVER. When CreateDevice
   fails, r_init prints why and returns 0, and main.cpp falls back to
   the GDI present of framebuffer.c, which needs nothing at all.

16.2.  What the back end reproduces, and what it drops

   Reproduced because the binary establishes it:

   o THE THREE BLEND MODES, and only three. r_blend_for_kind is the
     ladder at 0x004afaac transcribed - kinds 0x44 and 0x45 and 0x2c
     and 0x3e take the half blend, kind 0x0c takes ONE / SRCCOLOR, a
     colour-keyed 16-bit texture takes SRCALPHA / INVSRCALPHA, and
     everything else is opaque.

   o THE HALF BLEND SETS BOTH FACTORS TO INVSRCALPHA. That looks like
     a bug and is not: the alpha is stamped to 0x80 first, so
     1-srcAlpha is 0.5 twice and the result is a symmetric half blend.
     The comment in render.h says so, because the next person to read
     it will otherwise "fix" it.

   o CULLING IS OFF. Screen3D_CullAndQueueFaces has already back-face
     culled on the CPU, so a second cull here could only remove
     geometry the engine meant to keep.

   o TEXTUREMAPBLEND becomes a fixed MODULATE stage setup. The binary
     only ever sets it to MODULATE or DECAL, chosen from the device
     caps, and every modern part supports MODULATE.

   o ALPHATESTENABLE stays 0, because it is always 0 in the binary.
     Keyed textures are resolved with a real blend instead, which is
     what the original does.

   Dropped, deliberately:

   o STIPPLEDALPHA and its wCount 6 and 4 variants - the 1997 fallback
     for hardware that could not blend a flat alpha. A modern back end
     always has real blending.

   o THE EDGE-ANTIALIASING FLAGS. D3D_EmitTriangle's three winding
     cases differ in wFlags - 0x300, 0x1f, 0x700 - which named which
     edges to antialias. No D3D9 equivalent. The VERTEX ORDER is what
     survives and that is what r_emit_triangle takes.

   o THE EXECUTE BUFFER ITSELF. Batching by texture change is an
     optimisation, not semantics; the back end flushes on any state
     change instead, which produces identical output.

16.3.  Three things that are ours, not the game's

   o THE LETTERBOX. r_write_vertex maps design coordinates - 0..639 by
     0..479 - onto the client area with a scale and an offset. The
     original has no such transform: it owns the display mode and
     renders at whatever the mode is, so screen coordinates ARE device
     coordinates. This is the price of a resizable window.

   o THE HALF-PIXEL SHIFT. D3D9 samples a texel at its centre, so a
     pre-transformed vertex sits half a pixel back for a 1:1 blit to
     land on texel centres. That is a rule of this API, not of the
     game.

   o THE COLOUR CLEAR. The original clears Z and NOT colour, because
     the queue walk repaints every pixel - and it clears Z after the
     present rather than before the next frame. We clear both, because
     the letterbox margins are outside anything the game draws and
     nothing yet guarantees full coverage.

   Two implementation choices worth stating because they are what
   keeps the file short: textures are D3DPOOL_MANAGED and geometry
   goes out through DrawPrimitiveUP, so NOTHING lives in
   D3DPOOL_DEFAULT and a device Reset needs no resource teardown at
   all - only the render state re-applied. A dynamic vertex buffer
   would be faster and would drag a lost-device recreation path in
   behind it.


17.  The first geometry: the Delphine logo

   demo3d.c is scaffolding, and it is deliberately NOT MenuPage25. It
   draws the meshes that page draws - D, S and I from
   data\pages\titleDsi\ - because that is the shortest route to
   putting real game geometry through the seam. What would make it
   that page, and is absent, is everything that needs reading first:
   the camera, the DsInt and camera meshes, six model instances and
   the animation over them. The rotation here is invented.

   The chain it exercises is the whole of what src/ now has:

       mesh_load_with_map   .3df geometry, .map UVs and bank indices
       bank_load            the .RAW pages those indices name
       transform_verts_flat object space to screen space, MR_Vertex
       render.h             the seam, and D3D9 behind it

17.1.  Two things the data settled that guesses had wrong

   o THE LETTERS ARE SPREAD ALONG Z, NOT X [verified from the files].
     D occupies z -111.4 .. -27.6, S -30.3 .. 31.1, I 37.0 .. 68.0,
     and all three share x -14.5 .. 0.0 and y -53.9 .. 107.5. So each
     letter is an extrusion 14.5 units thick, the word reads along +Z,
     and the viewing axis is X.

     DO NOT CENTRE EACH MESH ON ITSELF and space them by hand along
     X. That throws the authored layout away and stacks the three
     letters on top of each other. They are already positioned
     relative to one another, so one matrix serves all three.

   o UVs ARE TEXEL UNITS, NOT NORMALISED. The .map stores them as
     INTEGERS and the shipped values here run 0 to 249 over a 256x256
     page. mesh-3df-map.txt section 6 states it from the other
     direction - the loader indexes the page as
     [ftol(v) * 0x100 + ftol(u)] - and the two agree. Every face in
     all three meshes references bank 0, and exactly one face per mesh
     is flat coloured.

17.2.  Checking it without running it

   Rule 8 says the game is never launched and rule 9 extends that to
   everything in build/, so there is no way to look at the output
   directly. What was done instead: demo3d.c and transform.c were
   MIRRORED IN PYTHON, including the projection, the perspective-
   correct interpolation and the 565 texel widening, and the frame
   rendered to a PNG.

   That predicted image shows the three extruded letters, textured
   from the bank page, at 201 triangles with no face dropped at the
   near plane. It is what the executable should show. A mirror is not
   a test of the executable - it shares the reasoning, so a
   misunderstanding of the format survives into both - but it does
   catch arithmetic and layout errors, and it caught the Z-versus-X
   one above.


18.  The queue

   queue.c, cull.c, detail.c and drawscene.c. This is the stage the
   back end was waiting for: everything between the frame's start and
   Screen3D_DrawScene only QUEUES, no pixels are touched by any of it,
   and then the whole frame is submitted in one call.

   The structure is a bucket sort with 8000 buckets and no comparisons.
   An array of linked-list HEADS is indexed by __ftol of the depth key,
   nodes come from one flat pool of 7499, and the walk runs the buckets
   in whichever direction the consumer needs. In 1997 that was the
   cheap way to depth sort a scene.

       queue_primitive       Screen3D_QueuePrimitive  0x0049c590
       queue_walk            the walk inside Screen3D_DrawScene
       cull_and_queue_faces  Screen3D_CullAndQueueFaces  0x0049c110
       detail_effective_kind the level-of-detail substitution
       drawscene_dispatch    D3D_DrawScene, into render.h

18.1.  Three things in it that look like defects and are not

   Each of these was transcribed rather than tidied, and each needed
   reading the disassembly rather than the decompiler to be sure of.

   o THE "SECOND LIST" IS BUCKET 7999. Screen3D_DrawScene walks a
     separate head at DAT_0065da34 after the bucket loop, in both
     directions. It is not separate: the array base is 0x00655d38 and
     0x00655d38 + 7999*4 is 0x0065da34 exactly. Ghidra had given it its
     own symbol because it is referenced directly.

     That makes the design legible. The depth path clamps at 7998, so
     nothing that sorts by depth can reach 7999; only bucketMode 2 puts
     anything there; it is drawn LAST either way; and its walk has no
     primitive-limit check. It is the "always on top, always drawn,
     never budgeted" slot - a HUD. bucketMode 1 is its mirror at bucket
     0. Written up in renderer.txt section 3.3.1 and corrected in the
     plate comment.

   o THE HIGH-WATER CLAMP DISCARDS ITS OWN TRACKING. "if the high mark
     is below 7990, set it to 7998" throws away the tracking on almost
     every frame. It is not a bug, because THE WALK CLEARS EACH BUCKET
     HEAD IT PASSES: forcing the high end is what guarantees the array
     is clean for the next frame, at the price of scanning empty
     buckets.

   o THE FACING TEST IS A BIT-PATTERN COMPARE.

         MOV EAX,dword ptr [ESP + 0x14]   ; the float's bits
         CMP EAX,0x3f800000               ; as a SIGNED int
         JLE reject

     Not a float-to-int conversion - that would reject everything,
     since a screen-space cross product on a 640x480 screen never
     exceeds about 307200 and 0x3f800000 as an integer is a billion.
     For positive IEEE floats the bit order matches the value order and
     a negative float compares as a negative integer, so it is exactly
     "facing > 1.0f". The constant is the same minimum-area value the
     emitters use, which means BACKFACE REJECTION AND DEGENERATE-AREA
     REJECTION ARE ONE COMPARISON.

18.2.  The guard band, which is the nicest thing in the stage

   Start a counter at 3. For each of the four corners add 1 when it is
   off the RIGHT edge and subtract 1 when it is off the LEFT. Then
   reject when the result READ AS UNSIGNED is 7 or more:

       all four off the right  ->  3 + 4 =  7          rejected
       all four off the left   ->  3 - 4 = -1          rejected,
                                   because -1 unsigned is enormous
       anything else           ->  0 .. 6              kept

   One comparison trivially rejects a face that is entirely off either
   side. The "off the right" test is itself unsigned, which folds
   "negative" into "very large" so a left-side vertex enters the same
   branch and a signed test then separates it - two unsigned compares
   doing the work of four signed ones.

   A triangle counts corner 0 twice, because MR_Primitive stores
   vertexIndex[3] = vertexIndex[0] for a triangle. That is what keeps
   the arithmetic at four terms so the endpoints still mean "all of
   them".

18.3.  The detail table is GENERATED, not transcribed

   Level of detail is one substitution and no branching:

       prim->effectiveKind = reverseIndex[stripped featureTable[kind]];

   so reducing detail swaps one kind for another and neither back end
   knows it happened. Both back ends read effectiveKind at +0x3a and
   never the authored kind at +0x00 - reading the wrong one would
   silently undo the whole system.

   Seventy kinds is too many to hand-transcribe reliably, so
   src/detail_table.inc is emitted by tools/gen_detail_table.py, which
   parses section 6 of detail-levels.txt, replays Screen3D_Open's
   construction loop, and CHECKS its own output against that section's
   reduction columns for all seventy before writing anything. The note
   is now load-bearing: an error in it would be an error in the build.

   That also reproduced the note's section 1.1 independently. The loop
   uses ZERO as its not-yet-assigned sentinel, and kind 0's natural
   pattern is also zero, so kind 0x16 steals the slot; a literal store
   after the loop puts it back. Withhold that store in the generator
   and it yields 0x16, exactly as predicted.

18.4.  What is NOT reproduced, and what it costs

   o THE NEAR-PLANE CLIP. A face with a corner behind the near plane
     arrives with hwFlags bit 0 set and has NO valid screen
     coordinates, because transform.c writes those only when camZ
     passes. The original splits it through Screen3D_ClipTriangleNear.
     Here it is DROPPED and counted. Nothing in the logo crosses the
     near plane so the count is zero, but any camera that gets close to
     geometry will make it visible as missing faces.

     The other half of that split - Screen3D_MakeTriangle for faces
     that only leave the VIEWPORT - is genuinely unnecessary, because
     D3D9 clips.

   o THE SHADE TABLE at DAT_005d43b0, which is how D3D_DrawScene gives
     each vertex its own colour from the intensity at MR_Vertex +0x0c.
     Without it every vertex of a primitive gets one colour and there
     is no Gouraud shading. This is the largest single thing missing
     from the hardware path.

   o THE PALETTED QUEUE. Screen3D_QueuePrimitivePaletted is chosen when
     a face's bank has palettes, and it applies its own kind-0x2a
     substitution. Every bank in use is truecolour, so there is one
     path here.

   o THE OTHER TWO SUBMITTERS. Ex owns the +1024.0 depth bias and
     Translucent is the first-person cockpit. Both of their depth
     tricks are SORT-KEY tricks that a real depth buffer cannot express
     as a bias; the faithful translation is to keep the ordering, in a
     separate pass.

18.5.  Two hazards the exercise exposed

   o THE BUCKET KEY IS IN WORLD UNITS. It is __ftol of the depth with
     no scaling, clamped at 7998, so the sort has resolution only while
     the scene's depths fall inside 0..7998 in whatever unit the world
     is authored in. mracer's own logo demo SATURATES it - the camera
     distance is derived from a bounding sphere over meshes scaled by
     Load3dfAndMap's 96.0, so every primitive lands in bucket 7998 and
     the sort does nothing. Only the depth buffer, which the original
     did not have, keeps the picture correct. queue.c reports the
     bucket range per frame for exactly this reason; watch for
     SATURATED on the console.

   o THE WALK ORDER IS NOT THE ORIGINAL'S DEFAULT. Screen3D_Open sets
     g_spanBufferEnabled to 1, so front-to-back is the normal path -
     but that exists for the span buffer, where whatever draws first
     claims its pixels. With a depth buffer it buys nothing and breaks
     blending, since a translucent primitive drawn before what is
     behind it has nothing to blend against. queue.c defaults to 0.
     This is the one divergence in the file that changes what is drawn.

18.6.  Checking it without running it

   The cull is the part that can be wrong in a way that still looks
   plausible, so it got the same treatment as the transform: mirrored
   in Python and rendered.

   Across the three logo meshes, 108 faces examined, 54 pass and 54 are
   back-faced - an exact half, which is what closed solids should give.
   The image is IDENTICAL to the pre-cull one, which is what a correct
   cull with a depth buffer must produce. Negating the facing test
   flips the split to the same 54/54 but renders the letters from the
   inside - the S reversed, the D hollow - so the test genuinely
   discriminates and the winding convention matches.


19.  The .map flags to kind decode

   prim.c. This was the first of the three gaps section 18.4 left, and
   it was the cheapest, and it turned out not to be a table at all.

   docs/formats/mesh-3df-map.txt section 5 had the seven .map flag bits
   and the nine kind constants they select - 1, 2, 8, 0xc, 0xd, 0x10,
   0x15, 0x26, 0x27 - and called decoding the mapping "a job of its own
   and has not been attempted". It is a BASE KIND PLUS THREE FEATURE
   BITS, and the nine constants are simply what those combinations are
   numbered:

       base                          kind 1     feature bits 0x01
       + map 0x010000                kind 2                  0x09
       + tier2 selector              kind 0x10               0x81
       + both                        kind 0x26               0x89
       + map 0x200000                kind 0x15               0x85
       + map 0x200000 and 0x010000   kind 0x27               0x8d

   Two more bits replace the kind outright rather than adding to it,
   and since both are unconditional stores the order in the function
   decides: 0x040000 gives kind 0x0d, then 0x080000 gives kind 8, so
   translucent beats exempt when both are set. Five shipped records set
   both.

19.1.  Why the feature-bit column is the whole point

   Those bit values are not from this function. They come from
   detail-levels.txt section 6, reconstructed from Screen3D_Open's
   construction loop - a different function, read in a different
   session, for a different purpose. Lining the two up, every column
   agrees:

   o map bit 0x010000 adds exactly DETAIL_NO_FILTER (0x08), in all
     three pairs 1->2, 0x10->0x26 and 0x15->0x27.

   o the tier2 selector adds exactly DETAIL_TIER2 (0x80).

   o map bit 0x200000 yields the full quality ladder, 0x85.

   Two independent decodes agreeing bit for bit is the strongest
   confirmation either of them has, and it is the reason this file can
   be trusted without running anything.

   It also explains the numbering, which looked arbitrary: the kind
   values are not a sequence, they are addresses in a feature space.

19.2.  The tier2 selector, and who asks for it

   It is Load3dfAndMap's fourth argument AND a non-zero face bank
   index, so it upgrades faces on the SECOND texture page and leaves
   the first alone. Read off the call sites:

       MenuTools_CreateSceneObject   0
       FUN_0047b600                  1
       maybe_Game_LoadBikeMeshes     0 at five sites, and a runtime
                                     SETZ on (DAT_00509398 == EBP) at
                                     two

   MenuTools_CreateSceneObject is what builds the 3D menu scene
   objects, so demo3d.c passes 0 - correct for menu geometry, and it
   would be wrong if this were loading a bike.

19.3.  Two things measured rather than assumed

   tools/map_kinds.py applies the decode to every shipped .map. Over
   10,330 real face records:

   o KIND 0x0C HAS NO PRODUCER. It is reachable only from the short
     path taken by a .map record whose vertex count is below three, and
     exactly ONE record in the whole game takes that path - with no
     flag bits set, so it comes out kind 0. Reproduced in prim.c
     anyway, since an authored .map could reach it.

   o MAP BIT 0x400000 IS NEVER SET, in any record of any file. That is
     the bit that becomes MR_Primitive flags bit 1, the +1024.0 depth
     bias whose only reader in the entire image is
     Screen3D_CullAndQueueFacesEx. The mechanism is real, documented at
     both ends, and unused by the game's own geometry.

   A METHODOLOGY NOTE, because it changed the numbers. 123 of the 124
   paired .map files carry an extra 128-byte block past the last face
   record, and the loader reads exactly 512 + faces * 128 and ignores
   it. Counting it as a face adds about 600 phantom records - kind 0x0d
   first came out as 179 rather than 119. An analysis script has to
   bound itself by the paired .3df's face count, not the file size.

19.4.  What it changes on screen

   Nothing, and that is the expected answer. The three logo meshes
   produce kinds 1 and 0x15 only - 84 faces and 27 - and both are
   opaque and filtered, so the picture is identical to before. What
   changed is that the detail system and the blend ladder now have real
   kinds to act on instead of the single placeholder kind 0 every face
   used to get.

   Two small functions were named in the DB while doing this:
   Prim_SetKind (0x00423150) and Prim_SetColorIndex (0x00423110). The
   first is bounds-checked against 0x46 - exactly the feature table's
   size - and SILENTLY IGNORES an out-of-range kind rather than
   clamping. The second turned out to be the 5-5-5 to 8-8-8 expansion
   of MR_Primitive.txt section 6.1 seen from the producing side, which
   confirms that note independently.


20.  The shade ramp

   shade.c. The second of section 18.4's three gaps, and the one that
   makes the hardware path lit.

   The table at 0x005d43b0 - now labelled g_d3dShadeRamp in the DB - is
   EMPTY IN THE IMAGE. All 8192 bytes are zero, and its only writer is a
   nine instruction loop at the end of Cdd_ctor2 that fills it at run
   time. The loop multiplies by repeated addition, which is why it took
   reading the assembly to see what it was; the whole table is

       table[level][channel] = channel * level / 32

   32 levels by 256 channel values. A plain linear ramp, no gamma. In
   1997 it bought a table lookup instead of a multiply and a shift per
   channel per vertex.

   D3D_DrawScene computes the row offset without ever forming the level:

       row = (vertex->shade & 0x3e000) >> 5;      /* level * 256 */

   and looks each of the primitive's three channel bytes up in that row.
   That is the whole of "the engine lights, Direct3D does not", and it
   is why D3D_EmitProcessVertices asks for TRANSFORM rather than
   TRANSFORMLIGHT.

20.1.  Three things worth knowing about it

   o FULL BRIGHTNESS IS 247, NOT 255. The largest entry is
     255 * 31 / 32 = 247, so a lit surface never quite reaches the
     primitive's own flat colour - it stops about 3 per cent short. A
     port that divides by 31 instead of 32 to "fix" that will be
     brighter than the original everywhere.

   o THE PRIMITIVE'S ALPHA BYTE SURVIVES the shading. shade_apply keeps
     it deliberately: it is what carries the 0x80 that makes the half
     blend come out at 50 per cent. Returning an opaque colour here
     would quietly break blending three files away.

   o THE SPECULAR STORE IS INERT, AND THAT IS A TRAP. D3D_DrawScene
     writes the same shaded RGB into the vertex's specular with alpha
     0xff. SPECULARENABLE is emitted from DAT_0053bb34, which Cd3d_Ctor
     zeroes in a run of stores right after XOR EAX,EAX - and the only
     two references to that address in the whole image are that write
     and the read. So the specular is computed every vertex and never
     used.

     d3d9rt.cpp now sets D3DRS_SPECULARENABLE to FALSE to match. It was
     TRUE, which was harmless only because we write specular 0; the
     moment someone writes the specular faithfully, TRUE would add the
     colour to itself and double every lit surface.

20.2.  Two tables with similar names, and only one of them is this one

   g_shadeLut at 0x004f62f0 is 32 x 32 entries of FOUR bytes, belongs to
   the SOFTWARE rasterizer, and still has no writer anywhere -
   rasterizer-pipeline.txt section 31.2 left that open and it stays
   open. g_d3dShadeRamp at 0x005d43b0 is 32 x 256 BYTES, belongs to the
   hardware path, and its writer is now found.

   Different sizes, different element widths, different subsystems.
   Merging them would be an easy and very confusing mistake.

20.3.  What is NOT reproduced, and why it is a real gap

   D3D_WriteVertex (0x004af420) has exactly one caller and it is
   D3D_DrawScene. So that function carries TWO colour routes: the
   inline shade-table computation transcribed here, and a call to
   D3D_WriteVertex, which branches on bits 31 and 30 of the shade word
   instead - a literal RGB colour, or full-bright white - and which is
   the route that reads the halve-intensity global.

   WHICH ROUTE SERVES WHICH PRIMITIVE WAS NOT TRACED. There are two
   untraced branches ahead of the table lookup, at 0x004af88d and
   0x004af895. Consequences, stated rather than papered over:

   o kinds 0x15, 0x27 and 0x20 are supposed to draw at half brightness,
     and 27 of the 111 logo faces are kind 0x15. They are drawn at full
     brightness here.

   o a vertex carrying a literal colour or the full-bright flag is
     shaded as though it carried an intensity.

   Neither shows on the logo, because nothing sets those bits. Both
   will show on real geometry.

20.4.  Checking it, and why Gouraud still is not visible

   transform_verts_flat gives every vertex of a mesh the SAME
   intensity, so nothing in src/ can produce a gradient across a face
   yet. Real per-vertex shading needs Screen3D_TransformVertsShaded
   (0x0049b490), and that function is NOT the simple variant of the
   flat one this note assumed when it was first named: it carries a
   second matrix, a per-vertex source switch and a morph path. It is a
   job of its own, and nothing in src/ produces per-vertex intensities
   to feed it anyway.

   So the ramp is demonstrated instead by giving the three letters
   three different flat levels - D 31, S 24, I 16 - which is
   scaffolding in demo3d.c and clearly marked as such. Mirrored in
   Python and rendered: the three letters come out at three visibly
   different brightnesses with the same geometry, which is the ramp
   working end to end.

   Note what the intensity is NOT. The window the hardware path reads
   is five bits, 0..31. demo3d.c used to pass 0xff, which masks down to
   15 - so "maximum brightness" would have rendered at just under half.
   That only went unnoticed because the shading was not applied yet.


21.  The asset layer: reading the game's own archives

   bkf.c and assets.c. This is the start of the path CLAUDE.md rule 10
   describes - an executable someone drops into an existing MotoRacer
   install, with nothing else alongside it.

   A stock install keeps almost everything in twenty-three .BKF
   archives in DATA. The format is a flat directory of 44-byte entries
   at the front of the file:

       u32          entry count
       entry[count] char name[36]; u32 offset; u32 size

   with the members tiling the rest of the file, no gaps and no
   overlap. All twenty-three pass that check.

   THE MAPPING IS NOT A CONVENTION WE INVENTED. Member names carry
   their subdirectory with a backslash - "TITLEDSI\BNK00_00.RAW",
   "BANK\BNK00_02.RAW" - so a member name is already the tail of the
   path an extracted tree mirrors. Split the first component off a
   logical name, use it as the archive stem, look the rest up:

       PAGES\TITLEDSI\D.3DF   ->  PAGES.BKF, member TITLEDSI\D.3DF
       ILE01\load.lez         ->  ILE01.BKF, member LOAD.LEZ

   Matching is case-insensitive and treats the two separators alike,
   which matters more than it sounds: the archives are upper case, the
   original's own strings are lower case, and page_loading.c builds
   "load.lez" exactly as the game does.

21.1.  One chokepoint, which is the whole point

   Every loader now resolves through asset_load(). Nothing in src/
   calls fopen on a game asset any more - lez.c, mesh.c and rawtex.c
   all changed, and in the first two the change was one function,
   because they already read the file whole and parsed from a buffer.
   rawtex.c needed restructuring from an incremental FILE * read, which
   also turned its size check into a comparison against a number
   already in hand.

   Resolution order, archive FIRST on purpose:

     1  the .BKF member                   what a stock install has
     2  <datadir>\<name>                  loose files - .BPT, .trk, or
                                          a player who unpacked their
                                          own game
     3  <datadir>\extracted\<name>        the pre-extracted tree, LAST

   Archive first means the stock path is exercised on every run, so it
   cannot quietly rot behind a fallback that always works. Step 3
   prints a line when it fires, and the exit summary says how many
   assets came from each route. A run reporting zero from the extracted
   tree is a run that would work on somebody else's machine.

   A leading "extracted\" on a logical name is stripped, so an
   mracer.ini written before this change keeps working without
   migration.

21.2.  Measured, not hoped for

   Simulating the resolution in Python against the real archives:

   o ALL 209 MENU MANIFEST ENTRIES resolve from an archive. Not the
     four the loading page happens to use - every entry in the table,
     which means every future menu page costs nothing here.

   o Every other asset the build asks for resolves from an archive too:
     the track background, the titleDsi meshes and bank pages, and
     VITRE.3DF.

   o THE EXTRACTED TREE IS NOW FULLY REDUNDANT for everything the code
     touches. 1507 files in it; 1507 members across the twenty-three
     archives. It is exactly the archives unpacked and nothing more,
     and 403 members across four archives compare byte-for-byte
     identical.

   The last point is worth stating plainly because it is the answer to
   "have we actually cut the dependency": not "we added a reader", but
   "nothing we load needs the tree any more".

21.3.  What this does not cover yet

   o THE LOOSE FILES. .BPT tables, ileNN\*.trk and bestscor.dat sit
     directly in DATA and route 2 handles them, but nothing loads one
     yet so that route is untested.

   o TRACK SELECTION. trackdir is a fixed "ILE01\" in the ini. The
     real game derives it from the track table indexed by the chosen
     circuit, and the naming is inconsistent - ile01.trk but
     Track03.trk and bercy.trk. data-files.txt section 5 says read the
     directory, do not assume the pattern.

   o THE DEFAULT DATADIR IS NOW ".\\DATA\\" relative to the exe, so a
     fresh install of mracer next to a real DATA folder needs no ini at
     all - see section 8.1. An existing mracer.ini with an absolute
     datadir keeps using it, which is why this changes nothing on the
     development machine until that line is deleted.

   o tga.c still opens a path directly. It reads no shipped asset - the
     game has no .tga - so it is a development convenience, but it is
     the one fopen left in src/ and it should not grow callers.


22.  The near-plane clipper

   clip.c. A face with a corner behind the near plane cannot be drawn
   as it stands - transform.c writes screenX and screenY only when camZ
   passes, so at least one corner has no valid screen position and
   nothing downstream can recover it. The cull reports the condition as
   hwFlags bit 0; queue.c used to drop those faces and now clips them.

22.1.  What is transcribed and what is not

   TRANSCRIBED: Screen3D_ClipEdgeNearHw (0x00497d60), 224 bytes, read
   instruction by instruction.

   NOT TRANSCRIBED: the case structure. Screen3D_ClipTriangleNear
   unrolls six cases across 5652 bytes and was read STRUCTURALLY -
   twelve edge clips in six pairs, ten output advances, fourteen
   vertex-pool writes - not line by line.

   So clip.c implements the same clip as ONE Sutherland-Hodgman loop
   rather than six unrolled cases. That is a divergence in SHAPE, not
   in result: walking a convex polygon's edges and keeping the inside
   points plus the crossings produces exactly the "one vertex in front
   gives a triangle, two give a quad" split the unrolled cases
   enumerate.

   The winding question that detail-levels.txt left open DISSOLVES
   rather than being answered. Sutherland-Hodgman emits in input order
   so winding is preserved anyway - but the cull has already run by the
   time a face reaches the queue, and the D3D9 back end draws with
   CULLMODE_NONE precisely because the engine culls on the CPU.
   Nothing downstream tests winding again.

22.2.  Three things the edge clip actually does

   The plate comment's sketch was right in outline and wrong in three
   details, all of which the decompiler hides.

   o THE DIVISION COMES FIRST. Each delta is divided by dz and then
     scaled, rather than forming t once and multiplying by it. Same
     value in exact arithmetic, different rounding in single
     precision.

   o NO PROJECTION SCALE APPEARS. That is a cross-check, not an
     omission: camX and camY arrive already viewport-scaled because
     transform_verts_flat folds the scales into the matrix once per
     call, so interpolating them keeps them scaled. Two stages read in
     different sessions agreeing on that is good evidence for both.

   o THE SHADE IS INTERPOLATED IN INTEGER ARITHMETIC, and that one is
     a defect:

         dzi = (int)(b->camZ - a->camZ);   /* truncates */
         ds  = b->shade - a->shade;
         if (dzi != 0) ds /= dzi;          /* INTEGER divide */
         out->shade = (int)(a->shade + (nearZ - a->camZ) * ds);

     A shade slope per WHOLE UNIT of z, truncating, then scaled by a
     float distance. WHEN |dz| < 1 THE DIVIDE IS SKIPPED ENTIRELY and
     the full shade difference is multiplied by the distance, so a
     short nearly z-parallel edge gets a wildly wrong intensity rather
     than a small error.

     Reproduced, not corrected. A "fixed" lerp would diverge from the
     original exactly where the original looks worst, which is the
     least useful place to differ.

22.3.  Where the pieces go

   The clipped vertices go into THE SAME per-frame bump pool the
   transform fills, via the new vertexpool_alloc. That is the
   original's arrangement, not a convenience - Screen3D_ClipTriangleNear
   writes the pool in fourteen places. A clip-heavy frame therefore
   consumes slots on top of the transformed ones, and nothing bounds
   either in the original; here the allocator returns NULL instead of
   running off the end.

   The polygon is then fanned from corner 0, and each triangle takes an
   MR_Primitive out of a split pool of 2500 - the size of the
   original's array at 0x0062ebf0 - because the mesh's own face array
   is shared and must not be written. Each split triangle is stored as
   a degenerate quad, and takes the source's EFFECTIVE kind into both
   kind fields: detail reduction has already happened, and a port must
   not "restore" the authored kind here.

22.4.  Making it visible, and a wrong turn worth recording

   The obvious way to exercise a near-plane clipper is to dolly the
   camera in. THAT DOES NOT WORK ON THIS SCENE. The logo is a thin
   plate spread very wide, so by the time anything reaches the plane
   the camera is inside the bounding sphere and the whole frame is off
   screen - every test frame came out black, with the counters
   cheerfully reporting seventy triangles drawn.

   Pushing the NEAR PLANE OUT instead does it properly, and it is
   better in every way: the framing never changes, so at 0 per cent the
   picture is exactly the intro, and raising it slices the letters
   where you can watch it happen.

       UP / DOWN     move the near plane in 10 per cent steps of the
                     camera distance; the clip starts biting at about
                     70 per cent

   Mirrored in Python at 80 per cent: with the clipper the frame draws
   102 triangles including 6 from the clip and drops nothing; without
   it, 96 triangles and two faces vanish. The visible difference is a
   wedge at the top right of the S that is present, and cleanly cut,
   only with the clipper on.

   ONE MORE WRONG TURN, because it nearly went in the write-up as a
   clipper bug: the first mirrored frames were black even with the
   clip disabled. The fault was in the mirror - it interpolated the
   shade perspective-correctly and forgot to divide by w, so every
   pixel resolved to intensity 0. A verification harness is code too,
   and a harness that reports black is not evidence about the thing
   being verified.


23.  The two colour routes

   D3D_DrawScene colours a vertex two different ways and picks between
   them per primitive. Section 20.3 left the selector open and named
   the two untraced branches at 0x004af88d and 0x004af895. The second
   turned out to be a guard - the vertex count is 2, 3 or 4 and never
   zero - and the first is the whole answer.

   IT IS TEXTURE PRESENCE [verified]:

       untextured = kind == 0x0c || kind == 5 || kind == 0
                 || bankIndex > 0x2d
                 || bank[bankIndex].texSlot[slot] == -1

       UNTEXTURED  ->  the inline SHADE-RAMP loop
       TEXTURED    ->  a call to D3D_WriteVertex

   The sense reads backwards for a moment and then makes sense. An
   untextured face has no texel to take its colour from, so its flat
   colour goes through the intensity ramp. A textured face wants a
   GREY modulator, so the MODULATE stage yields texel times light -
   and a grey is exactly what D3D_WriteVertex builds:

       g = (shade & 0x3fc00) >> 10;      /* EIGHT bits, not five */
       if (halved) g /= 2;
       diffuse = primAlpha | g<<16 | g<<8 | g;

23.1.  What that settles, and what it changed on screen

   o THE HALF-BRIGHTNESS KINDS ARE TEXTURED-ROUTE ONLY. DAT_0053bb38
     is set for kinds 0x15, 0x27 and 0x20 and D3D_WriteVertex is its
     only reader, so the ramp route never halves.

     THIS ONE IS VISIBLE. 26 of the logo's 54 drawn faces are kind
     0x15 - the letter faces, where the extrusion sides are kind 1 -
     so the front of each letter now renders at half the brightness of
     its sides. It is a striking change and it follows from a rule
     verified twice over, but note the absolute levels are not the
     game's: the per-mesh intensities in demo3d.c are invented
     scaffolding, so what the real MenuPage25 would look like is not
     something this scene can answer.

   o THE BIT-31 LITERAL COLOUR AND BIT-30 FULL BRIGHT are
     textured-route only as well. The ramp route reads bits 13..17 and
     honours neither.

   o THE EIGHT-BIT WINDOW vs THE RAMP'S FIVE barely matters in
     practice: intensity 31 gives 248 through D3D_WriteVertex against
     247 through the ramp. Same number at two precisions, as
     MR_Vertex.txt section 6.3 said.

23.2.  An inference this file was carrying, now corrected

   drawscene.c used to guess "textured" from whether a face's four UV
   pairs were all equal, flagged as INFERRED, with the note that if it
   turned out to come from the kind then every textured flat-coloured
   face would change appearance.

   IT COMES FROM THE KIND. D3D_DrawScene builds the state mask's bit 0
   as "kind is not 0, 5 or 0x0c" and never looks at the UVs. The guess
   is gone.

   And the mask's bit 0 is NOT the route selector, even though they
   overlap - the selector additionally requires a bound texture slot.
   A primitive can carry mask bit 0 and still take the untextured
   colour route. Two tests, two purposes; d3d-render-state.txt section
   3's "textured" is the mask one.

23.3.  The hardware path IS Gouraud shaded - a correction

   This section previously said the opposite, and the way it went wrong
   is more useful than the fact itself.

   D3D_WriteVertex takes the CORNER INDEX as its third argument and
   colours a corner only when that is 0, or when g_cfgGouraud
   (DAT_004dc1d8) is 1. The same global picks SHADEMODE:

       SHADEMODE = (g_cfgGouraud == 0) ? FLAT : 2 - (DAT_0053bdfc == 0)

   g_cfgGouraud IS INITIALISED TO 1 IN .data, and Cd3d_Ctor sets
   DAT_0053bdfc to 1, so the default is SHADEMODE 2, D3DSHADE_GOURAUD,
   and every corner gets its own colour [verified].

   The one write to that global in the whole image stores ZERO - and it
   is the HANDLER for "-NoGouraud", which the game's own flag
   documentation describes as "Disable gouraud shading".

   THE MISTAKE. The earlier pass searched for writers, found exactly
   one, saw it store 0, and took that for the initialisation. It even
   flagged the right residual risk - a table-driven parser writing the
   flag indirectly - and went and checked for it, which came back
   clean.

   The default was never a write. It was the initialised value in
   .data, and an instruction search cannot see that by construction.

   Either of two habits would have caught it, and both are cheap:

   o READ A GLOBAL'S INITIAL VALUE before concluding anything about its
     default. For a .data global that value IS the default; the writers
     are the exceptions to it.

   o TAKE A FLAG'S NAME SERIOUSLY. -NoGouraud, -NoD3D, -NoDither,
     -NoPal256 - this binary is full of negations, and a negation means
     the positive is the default. A "disable" handler storing 0 is the
     exception path.

   drawscene.c now defaults g_cfgGouraud to 1 and carries the same
   note, because the next person to search for writers will reach the
   same wrong answer by the same route.

23.4.  What is left of the shade work

   Between this and section 20, the whole colour path is now
   reconstructed and one gap remains: nothing in src/ produces
   PER-VERTEX intensities, because transform_verts_flat gives every
   vertex of a mesh the same one. That needs
   Screen3D_TransformVertsShaded, which is not the simple variant it
   was assumed to be - it carries a second matrix and a per-vertex
   source switch.

   Section 23.3 raises the priority of that rather than lowering it.
   Gouraud is the DEFAULT, so per-vertex intensity does reach the
   screen, and every corner of every primitive is being coloured. The
   logo demo cannot show it - one intensity per mesh means no gradient
   across a face whatever the shade mode - but real geometry will.


24.  C or C++: what the binary actually requires

   The reconstruction is C, with C++ only where it earns its place.
   That is a preference, but it is also supportable, and this section
   is the measurement behind it rather than the opinion.

24.1.  The binary is C with classes used as namespaces

   +--------------------------------------+---------------------------+
   | Measure                              | Value                     |
   +======================================+===========================+
   | game functions below 0x004b0000      | 2038, 688,974 code bytes  |
   +--------------------------------------+---------------------------+
   | of those, __thiscall                 | 81, about 4 per cent      |
   +--------------------------------------+---------------------------+
   | RTTI type descriptors in the image   | 36                        |
   +--------------------------------------+---------------------------+
   | of those, belonging to the GAME      | ONE - .?AVCdsiError@@     |
   +--------------------------------------+---------------------------+
   | vtable installs in the game region   | 24 distinct               |
   +--------------------------------------+---------------------------+

   THE RTTI LIST IS THE STRONGEST SINGLE PIECE OF EVIDENCE. Thirty-five
   of the thirty-six descriptors are MFC - CObject, CWinApp, CWnd,
   CDialog, CFile, CException and relatives. The game's own code
   contributes exactly one class with runtime type information, and it
   is an ERROR type: CdsiError.

   A codebase that used polymorphism as a design tool would have
   dozens. This one has a struct-and-functions design with the C++
   syntax on top, which is what the 4 per cent __thiscall figure says
   from the other direction: about eighty functions in two thousand
   take a this pointer, and every one of them is a plain
   "operate on this record" method.

24.2.  The 415 "virtual calls" are COM, not the game

   An indirect call through an offset - CALL dword ptr [reg + 0x50] -
   is how both a C++ virtual call and a COM method call disassemble,
   and the game region has 415 of them. Grouped by containing function
   they are overwhelmingly Cdd_*, Cd3d_*, D3D_*, DX_DetectVersion,
   Bankw_CreateSurfacePage and Csample_Load - DirectDraw, Direct3D and
   DirectSound.

   So they are not evidence of the game's own polymorphism. They are
   the API surface being replaced anyway, and they disappear with it.

24.3.  The one real hierarchy is the fonts, and it is small

   Six of the 24 vtables belong to one family: a base Font plus
   FontCustomColor, FontShaded, FontShadedTransp, and two more the
   constructors identify as Pal and Moto. The constructors chain -
   base installs its vtable, derived overwrites - so it is genuine
   single inheritance, up to three levels.

   Then the vtables themselves make it a non-problem [verified]:

   +------+--------------------------------+---------------------+
   | Slot | Across all six classes         | Distinct functions  |
   +======+================================+=====================+
   | 0    | same in five, differs in one   | 2                   |
   +------+--------------------------------+---------------------+
   | 1    | differs                        | 5                   |
   +------+--------------------------------+---------------------+
   | 2..5 | IDENTICAL IN ALL SIX           | 1 each              |
   +------+--------------------------------+---------------------+
   | 6    | differs                        | 6                   |
   +------+--------------------------------+---------------------+

   SEVEN SLOTS, THREE OF WHICH EVER VARY. Four of the seven are the
   same function in every class - they are virtual only because the
   base declared them so, and nothing overrides them.

   In C that is a struct with THREE function pointers and four ordinary
   functions. It is smaller than the sprite blit-mode switch already in
   sprite.c.

   For comparison, the Cd3d and Cdd vtables carry three entries each,
   and both classes are on the replace-list.

24.4.  The policy

   o C BY DEFAULT. Every engine subsystem - geometry, queue, physics,
     menus, tracks, fonts, audio mixing, file formats - is C. Nothing
     found so far needs more.

   o C++ ONLY FOR COM AND FOR THE WIN32 SHELL. d3d9rt.cpp is C++
     because IDirect3DDevice9 in C means lpVtbl-> on every call and an
     explicit this argument, which buries the logic in punctuation.
     main.cpp is C++ for no better reason than that it was, and it is
     the shell.

     Any future DirectSound or XAudio2 back end takes the same
     exemption. An input back end using raw Win32 does not need it.

   o EVERY C++ FILE PRESENTS A C HEADER. render.h is the model:
     extern "C", plain structs, no C++ in the interface. The C++ stays
     inside one translation unit and never spreads through a header.

   o NO MFC, EVER. Section 24.1 shows the game's MFC use is the
     CWinApp shell, CString::Format and AfxMessageBox in error paths,
     and CFile. All of it is replaceable with Win32 and printf, and
     main.cpp already replaces the shell.

   o NO EXCEPTIONS. CdsiError is the one game exception class; error
     codes and a printf are the reconstruction's equivalent, and that
     is already how every loader in src/ reports failure.

24.5.  What is left of the engine

   Code bytes per translation-unit region, which is the honest measure
   of remaining work - function counts flatter the small units:

   +--------------------------+---------+---------------------------------+
   | Region                   | Bytes   | State                           |
   +==========================+=========+=================================+
   | menu\pageMainMenu.cpp    | 100,534 | not started - the menu pages    |
   +--------------------------+---------+---------------------------------+
   | boot / bankw / low       |  95,508 | banks and .RAW done; app init,  |
   |                          |         | .bpt and protection left        |
   +--------------------------+---------+---------------------------------+
   | screen3d\screen3d.cpp    |  88,874 | transform, cull, queue done;    |
   |                          |         | the rest is the SOFTWARE        |
   |                          |         | rasterizer, which D3D9 replaces |
   +--------------------------+---------+---------------------------------+
   | player.cpp               |  68,097 | not started - bike physics      |
   +--------------------------+---------+---------------------------------+
   | frame3d.cpp              |  61,616 | mesh, prim and the kind decode  |
   |                          |         | done; object management left    |
   +--------------------------+---------+---------------------------------+
   | animDecor.cpp            |  59,057 | not started - includes the FONT |
   |                          |         | family at 0x00490700-0x00496da0 |
   +--------------------------+---------+---------------------------------+
   | menu\menuTools.cpp       |  55,704 | not started - 3D menu scenes    |
   +--------------------------+---------+---------------------------------+
   | menu\bitmap.cpp          |  43,286 | sprite layer done; the rest is  |
   |                          |         | menu bitmap management          |
   +--------------------------+---------+---------------------------------+
   | event.cpp                |  42,342 | not started - input and events  |
   +--------------------------+---------+---------------------------------+
   | Dsi_d3d.cpp              |  34,753 | REPLACED by d3d9rt.cpp          |
   +--------------------------+---------+---------------------------------+
   | screen3d\dd.cpp          |  23,404 | REPLACED                        |
   +--------------------------+---------+---------------------------------+
   | menu\pageProtocol.cpp    |  15,799 | copy protection - stub it       |
   +--------------------------+---------+---------------------------------+

   Two observations worth having before planning any of it.

   o THE SOFTWARE RASTERIZER MAY NEVER BE NEEDED. It is the single
     largest unported block, and rasterizer-pipeline.txt runs to six
     thousand lines describing it. With a D3D9 back end that draws
     every kind, it is redundant. Keeping the OPTION open costs
     nothing today, because the queue feeds both.

   o pageProtocol.cpp IS 15,799 BYTES WE SHOULD NOT REIMPLEMENT. The
     protection's only lasting effect on gameplay is subtracting 1.0
     from every bike's NbSpeed when it fails - see
     subsystems/copy-protection.txt. A reconstruction that simply
     always passes is both simpler and more correct than one that
     reproduces a CD check for a disc nobody has.


25.  The shaded transform, and Gouraud on screen

   transform_verts_shaded in transform.c - Screen3D_TransformVertsShaded
   (0x0049b490). Section 20.4 called this "not the simple variant of
   the flat one it was assumed to be", and having now read it, that was
   an understatement.

25.1.  Two surprises in the interface

   o THE Y COORDINATE COMES FROM A SEPARATE ARRAY. The position source
     is stride 3 and only elements [0] and [2] are ever read; y comes
     from a second pointer of stride 1. That y pointer is also what the
     function NULL-checks on entry, and the early return happens BEFORE
     the pool cursor advances, so a call without it reserves nothing.

     It is a heightfield-shaped interface - a base mesh giving x and z,
     a parallel stream giving y - which fits animDecor.cpp being the
     neighbourhood: deforming decorative geometry with per-vertex
     light.

     The convenient consequence: a mesh with interleaved xyz can be
     passed AS-IS for the xz source, because element [1] is never
     touched. demo3d.c does exactly that and supplies the y stream
     separately.

   o THE INTENSITY ARRAY IS OPTIONAL, AND WHEN NULL THE SHADE FIELD IS
     NOT WRITTEN. The pool slot keeps whatever the last user left in
     it. That is the stale-slot hazard MR_Vertex.txt already records
     for screenX and screenY, one field over, and it is reproduced
     rather than papered over.

   Both were read from the DISASSEMBLY. The decompiler renders the
   y-stream as an ordinary parameter and the override match as a float
   compare, and both readings are wrong.

25.2.  What is deliberately not reproduced

   The original takes two further arguments: an OBJECT whose matrix at
   +0x68 and negated translation at +0x98..+0xa0 combine with two
   globals into a SECOND matrix, and an OVERRIDE LIST of twelve-byte
   records

       { float x; int vertexIndex; float z; }

   where a matching vertex is transformed by that second matrix from
   the record's own x and z. The match is an INTEGER compare against
   the loop counter.

   It is a per-vertex position-and-matrix substitution for a feature
   nothing in src/ produces, needing an object type not yet
   reconstructed. The arithmetic is identical either way - only the
   matrix and the xz source change - so adding it later is local.

   o THE TWO GLOBALS ARE NOW IDENTIFIED, and they are not really
     globals. They are g_cosTable entries read at constant indices -
     0x005fcb80 is entry 0 and 0x00608b80 is entry 0x3000 - so the
     angle is a LITERAL and the second matrix carries a fixed -90
     degree rotation about one axis. See section 27.4. What remains
     unknown is the object, not the rotation.

25.3.  Gouraud is finally visible

   Nothing in src/ produces per-vertex lighting; that is the caller's
   job in the original and which caller supplies what has not been
   read. So demo3d.c invents one, and says so loudly: a VERTICAL
   GRADIENT, intensity 6 at the bottom of each letter rising to 31 at
   the top.

   That choice is scaffolding, but it is the RIGHT scaffolding, because
   the thing worth seeing is not the lighting - it is whether the
   gradient runs smoothly ACROSS a face or steps at face boundaries.
   Mirrored in Python both ways:

   o GOURAUD ON  - the letters shade smoothly from dark at the base to
     bright at the top, the gradient continuous across each tall side
     face.

   o GOURAUD OFF - every face is one flat colour taken from corner 0,
     so the same geometry looks blocky, with hard steps between
     adjacent faces and the I uniformly bright against a uniformly
     dark D.

   A principled alternative would be per-vertex normals averaged from
   face normals. It is deliberately not taken: mesh.c omits normals
   because the winding and sign conventions live in
   Mesh_ComputeFaceNormal, which has not been read. INVENTING A
   LIGHTING MODEL AND SAYING SO IS HONEST SCAFFOLDING; inventing a
   winding convention and letting it look like the game's would not be.

   The per-mesh flat intensities that used to demonstrate the shade
   ramp - D 31, S 24, I 16 - are gone, superseded by this.


26.  The viewport clipper, and a hedge removed

   FUN_00497080 is now Screen3D_ClipPolygonViewport. It was the largest
   unread function on the geometry path and the last thing clip.c was
   hedging about. No code was written for it, and that is the correct
   outcome.

26.1.  What it is

   THE SPLIT PATH IS THREE STAGES, NOT TWO. Screen3D_QueuePrimitive
   runs every triangle from Screen3D_MakeTriangle or
   Screen3D_ClipTriangleNear through this, and the fan loop turns ITS
   output into queued primitives:

       face -> Make or ClipNear -> up to 4 triangles
            -> ClipPolygonViewport (per triangle)
            -> up to 8 corners -> fanned -> queued

   It is Sutherland-Hodgman against the four screen edges, with an
   asymmetry worth noting: the right and bottom bounds are the maxima
   PLUS ONE while the left and top are the margins themselves, so the
   box is [left, maxX+1] x [top, maxY+1].

   Two scratch buffers of 0xe8 bytes are ping-ponged through the four
   passes - eight records of 0x1c plus the count at +0xe0. Eight is
   exactly the bound: a triangle clipped by four planes yields at most
   seven corners, plus one for the closing duplicate. And that
   duplicate is the same wrap-slot trick as MR_Primitive's fifth (u,v)
   pair - copy corner 0 to the end, then walk i against i+1 with no
   modulo and no last case.

26.2.  The hedge it removes

   clip.c said its UV interpolation was "the obvious choice and almost
   certainly what it does, but not verified". That hedge is gone, and
   the answer is better than a confirmation.

   THIS FUNCTION DOES SOMETHING DIFFERENT. It premultiplies u and v by
   1/z before clipping, carries u/z, v/z and 1/z through all four
   passes, and divides back out at the end:

       z = 1.0f / invZ;   u *= z;   v *= z;

   That is required because it clips in SCREEN space, where a plain
   lerp of u and v is wrong.

   AND THAT IS EXACTLY WHY clip.c NEEDS NONE OF IT. The near-plane
   clipper works in CAMERA space, before the projection divide, where
   the transform is still affine and u and v are linear in the same
   parameter as the position. A plain lerp there is not an
   approximation, it is exact - and adding a premultiply would make it
   wrong.

   Two clippers, two spaces, two different correct answers. Worth
   stating plainly because "the original divides by z, so we should
   too" is the natural and incorrect conclusion to draw from half of
   it.

26.3.  Why no code

   D3D9 clips to the viewport itself, perspective-correctly. The
   function is replaced by doing nothing, which queue.c already
   assumed when it decided that hwFlags bit 1 - "extends outside the
   viewport" - needed no split.

   That assumption is now checked rather than convenient.


27.  The cosine table

   trig.c and trig.h. g_cosTable at 0x005fcb80 is where every rotation
   in the game gets its sine and cosine - 0x4000 floats, one turn, 335
   reference sites - and the reconstruction now builds it the same way
   the original does.

27.1.  It is cosine, and that was the whole question

   The table is ALL ZEROS IN THE IMAGE, so reading the file settles
   nothing, and the builder had not been found across several sessions.
   MR_MenuCamera.txt had it flagged as open with a warning attached:
   many callers read entry 0 as a bare scalar, and an FMUL by f(0) "is
   a no-op if f is cosine and a zeroing if f is sine, which is a large
   difference to leave open".

   Trig_BuildCosTable (0x004901e0, was FUN_004901e0) settles it:

       for (i = 0; i < 0x4000; i++)
           g_cosTable[i] = (float)cos(i * 6.283185307178 / 16384.0);

   One caller, App_Init. So the pair every caller reads - index i and
   index (i - 0x1000) & 0x3fff - is (cos t, sin t), since
   cos(t - 90deg) = sin(t). Checked numerically against the built
   table, not inferred from the identity.

27.2.  How it was found, since the obvious searches failed

   Searching for FSIN found 26 sites in 15 functions, all easing and
   animation code. Searching for a 0x4000 loop bound found 90
   functions, because AND 0x3fff is the engine's angle wrap and drowns
   everything. Neither found it - FCOS is not FSIN, and the compiler
   had rotated the loop so the bound test does not sit where a counter
   search expects.

   ASKING WHICH INSTRUCTIONS WRITE INTO THE SPAN found it in one query:
   two writes out of 335 references, four instructions apart, both in
   the same function.

   o THAT QUERY SHOULD HAVE BEEN FIRST. It is a direct question with a
     small answer, and it does not depend on guessing how the builder
     is written. It even survives a sparse writer - only two of the
     0x4000 stores show up as references, the rest going through a
     walking index the analyser will not resolve, and two was plenty.

27.3.  Two things transcribed rather than tidied

   THE CONSTANT IS NOT 2*pi. The double at 0x004d1e38 is
   6.283185307178 - thirteen digits, short of true 2*pi by about 7150
   ulp. Somebody typed it. trig.h keeps the literal, because a port
   that wants the table bit-exact needs it and because "the constant
   looks slightly wrong" is exactly the sort of thing that gets
   silently corrected during transcription and costs an afternoon
   later.

   THE 14-BIT ANGLE CONVENTION IS KEPT. 0x4000 to the turn, wrapping by
   mask. Moving to radians would mean converting at every boundary with
   code that still speaks fixed point, and nothing about the table is a
   1997 compromise that needs undoing.

27.4.  What it explains elsewhere

   The 153 bare absolute reads of this table are not separate globals.
   They are CONSTANT-INDEX lookups - an inlined euler-to-matrix helper
   called with literal angles - and the compiler cannot fold a table
   lookup, so it emits the whole 3x3 composition at run time and
   multiplies by 1.0 and 0.0 for real. maybe_Racer_Draw shows the
   variable and constant forms four instructions apart.

   Screen3D_TransformVertsShaded builds its override matrix from
   entries 0 and 0x3000, which are (1, 0) and (0, -1): a fixed -90
   degree rotation about one axis. That is the first concrete answer to
   what that second matrix is FOR, and section 25.2's open question can
   be narrowed accordingly.

   The DB now types the table as float[0x4000], so all 335 accesses
   render as subscripts instead of raw addresses. Full write-up in
   docs/subsystems/trig-table.txt.


28.  The frame loop

   timing.c/.h and audio.c/.h. The old loop ended in Sleep(1) and drove
   the animation off timeGetTime; this replaces both.

28.1.  The original targets 30, and busy-waits to get it

   Frame_LimitAndMeasure (0x00459fd0, was FUN_00459fd0) is the whole of
   the original's pacing [verified]. g_frameRateMax (0x004dc200) holds
   30 in the image, -FrameRateMax<XX> overrides it, and the limiter
   SPINS on timeGetTime until the budget is up. There is no Sleep
   anywhere in MOTO.EXE, and no CreateThread either - the game is one
   thread with one clock.

   The same function keeps a 24-frame ring and returns the average fps.
   Our pacer keeps the same 24, because that is the original's number.

   Two small findings worth having: the help text claims the flag
   accepts 20 to 99, but the parser stores only when 20 < n < 99, so
   the real range is 21 to 98; and the game never calls timeBeginPeriod,
   so its own clock moves in 15.6 ms steps. That is very likely WHY 30
   was chosen - the clock is not good enough to pace a 16 ms frame.

28.2.  Why ours was choppy, which was not the pacing

   The loop already presented with D3DPRESENT_INTERVAL_ONE, so frames
   were arriving on the vblank. The stutter came from

       demo3d_draw((float)timeGetTime() * 0.001f)

   A 15.6 ms animation clock against a 16.67 ms display: the angle
   advanced one quantum most frames, two or none occasionally. Every
   frame on time, motion visibly uneven.

   o SMOOTH PRESENTATION AND SMOOTH MOTION ARE DIFFERENT PROPERTIES,
     and that is the reusable part. A correct frame rate with a coarse
     animation clock looks exactly like dropped frames, which sends you
     hunting in the wrong subsystem.

28.3.  What pacing actually looks like now

   There is no DirectX wait primitive beyond Present's vsync block, and
   a QPC busy-wait is exact but burns a core. The real answer is a
   WAITABLE TIMER created with CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
   which Windows 10 1803 added for this problem. tm_wait_until is a
   hybrid: sleep on that timer to within about a millisecond, then spin
   the remainder with YieldProcessor.

   THE SPIN IS BOUNDED BY THE MARGIN, NOT BY THE FRAME. Under a
   millisecond of burn instead of the whole remainder of the budget.
   Falls back to a plain timer, then to Sleep, and says which at
   startup. timeBeginPeriod(1) is raised and properly paired.

28.4.  Fixed simulation, variable render

   The simulation gets a constant step and the renderer runs as fast as
   the cap allows, with dt clamped at 0.25 s and the step count bounded
   at 8 per frame.

   That is a deliberate divergence, not a tidy-up. The original's
   physics step IS its frame, so running its loop at 144 Hz would
   quietly change the handling - and every tuned constant in the
   physics with it. simhz defaults to 30 for that reason. WHETHER THE
   GAMEPLAY IS ACTUALLY RATE-INDEPENDENT IS UNKNOWN and cannot be
   settled by reading, so do not raise it on the assumption.

28.5.  Input needs no thread, and audio is stubbed

   The original polls winmm and GetKeyState inline, and nothing modern
   changes that: raw input is messages the loop already pumps, XInput
   and DirectInput reads do not block. The one real caution is that
   XInputGetState on an EMPTY slot is slow, so disconnected slots get
   rescanned on a timer rather than every frame - a thread would hide
   that cost, not remove it.

   audio.h is the full interface, audio.c is silent. Music stays
   addressed BY CD TRACK NUMBER as the original addresses it.

   o AND THERE IS A REAL LIMIT OF RULE 10 HERE. The original's music is
     Red Book CD audio via mciSendCommandA - it was never installed,
     it was on the disc. A drop-in EXE finds every sound effect in the
     archives and cannot produce the soundtrack from a stock DATA
     folder. Music will need files the user supplies.

   Full treatment in docs/subsystems/frame-loop.txt.


29.  The 3x4 matrix helpers

   No code written. Three functions read, one open question closed, and
   the element order of every matrix in the game settled.

29.1.  Matrix34_Copy is just a copy

   MR_MenuCamera.txt had FUN_004039c0 down as "a normalise, a copy, or
   a further multiply - unread", applied to the assembled view matrix
   before it is stored. IT IS A COPY: twelve dwords, dst = src, return
   dst, 83 bytes, nothing else [verified]. __thiscall, so a C++
   operator= on a 3x4, with 49 call sites in 28 functions.

   The call site materialises a temporary and then copies out of it
   again dword by dword, which is what an unoptimised operator=
   returning a reference produces.

   o SO THE RIGID-BODY INVERSE IS THE WHOLE OF THAT FUNCTION'S MATHS.
     Nothing happens to it between assembly and storage, and a port
     builds it and writes it out.

   There is a byte-identical twin at 0x00403a20 (Matrix34_Copy2), 24
   sites in 17 functions - two classes with the same layout, each with
   its own operator=. Which is which is open.

29.2.  The element order, which matters more

   Matrix34_Multiply (0x00403a80) settles what a 3x4 IS here:

       out[0..8]   the 3x3 product
       out[9..11]  a's translation through b's 3x3, plus b's

   THE 3x3 IS [0..8] AND THE TRANSLATION IS [9..11]. That is why an
   identity has 1.0 at dwords 0, 4 and 8 - the pattern MenuCamera_Init
   writes, and the thing that let the camera layout be read off it in
   the first place. transform.c's convention agrees.

   Every load in it happens before every store, so out may alias a or
   b. WHETHER ANY CALLER RELIES ON THAT IS NOT CHECKED - 35 sites, none
   read - so a port should keep the ordering rather than assume it is
   free to reorder, and should not claim the aliasing is deliberate
   either.

29.3.  A correction worth recording

   The first version of the Matrix34_Multiply plate asserted that
   "several callers pass the same matrix as out and as a". That was
   invented - the load-before-store ordering was verified, the caller
   behaviour was not, and the two got written down in one breath.
   Corrected in the DB.

   o THE TELL IS THE SHAPE OF THE SENTENCE. A verified mechanism
     followed by an unverified motive for it, joined by "so". The
     mechanism earns [verified]; the motive needs its own evidence and
     usually has none.


30.  The font family, and the .FNT format

   font.c, font.h and tools/fnt.py. Section 24.3 sized this family long
   before any of it was written - "seven vtable slots, three of which
   vary, so three function pointers in C". That estimate was right, and
   this is it cashed in.

30.1.  The vtable, confirmed

   All eight vtables read - six classes plus the two Trame variants
   Font_LoadAll makes by swapping a vtable over a plain object
   [verified]. Slots 0, 1 and 6 vary; slots 2 to 5 are the same
   function in all eight.

   Two things the earlier count did not have:

   o SLOT 5 IS A STUB THAT RETURNS ZERO in every class - five bytes,
     XOR EAX,EAX / RET 0x10. It is the loader for a second face
     extension, and since all 33 shipped faces are .FNT, nothing ever
     called it. Slot 4 wraps it and is equally dead. A second face
     format was designed and never shipped.

   o SLOT 0 PINS DOWN TWO UNKNOWN FIELDS. It compares three bytes at
     +0x5494 against the incoming fore colour and three at +0x5497
     against the back colour, copying each only if it differs. Those
     are foreColor[3] and backColor[3] - the FontForeColors and
     FontBackColors keys - and they were part of a packed region
     MR_Font.txt had as unresolved.

30.2.  The .FNT format, decoded

   THIS WAS THE REAL BLOCKER and it was not on the list. The family
   structure was known; what nothing could do was load a face, because
   .FNT had never been decoded. It is now, byte-exact:

       u32 firstChar, charCount, glyphHeight, paletteEntries
       u8  palette[entries][3]      only when entries != 0
       u32 width[charCount]
       u8  pixels[sum(width) * glyphHeight]     row major

   THE PALETTE COMES BEFORE THE WIDTH TABLE, which is the one part
   that is not guessable - the natural reading works for the 13 faces
   without a palette and fails for all 20 with one. Both orders were
   tested against every file; one fits 33 of 33 and the other fits 13.

   o A DERIVED LENGTH THAT LANDS EXACTLY ON 33 FILES IS THE WHOLE
     PROOF. Four header fields feed a sum over a variable-length
     table, so a wrong model landing on the byte every time would take
     remarkable luck. That is why formats/fnt.txt is tagged verified
     without the loader having been read.

   Row-major was settled by rendering rather than by argument: glyph 0
   of HIRES\CHRONO.FNT reads as a '0' one way and as noise the other.

30.3.  What is honest rather than complete

   The original has EIGHT slot-6 glyph blitters, 898 to 1363 bytes
   each, and NONE has been read. font.c stands in with TWO, chosen by
   whether the face carries a palette rather than by which class asked.

   That split is not invented - it falls out of the format, and it
   matches the declared class exactly: every face of a Pal font is
   palettised, every Direct and AntiAlias8 face is not. What it does
   not reproduce is the per-class part: the Trame halftone, the
   AntiAlias8 intensity ramps (which is what the extra 0x1c00 bytes in
   those objects hold), and whatever CustomColor does differently. Each
   entry in the class table carries the address of its real slot-6
   function so the next pass has somewhere to start.

   The atlas is not reproduced either, and that is a replace-list
   decision rather than a gap: the original packs glyphs into a
   DirectDraw surface and records normalised texture coordinates, and
   our renderer takes a framebuffer.

30.4.  It draws

   main.cpp loads FONTES\DELPHINE\DEBUG.FNT - font id 8, "InfosDebug",
   type Direct, 94 characters from 0x20 - and draws the frame rate over
   the 2D layer. That is the font the developers printed diagnostics
   with, so using it for ours is the same job.

   It goes into the framebuffer, which is where the original puts text
   too: Font_DrawTextInternal writes 16-bit pixels straight into the
   locked surface rather than blitting through DirectDraw, through a
   block with the same shape as ours - a bits pointer, a stride in
   pixels, two bytes each.


31.  The blitters, and a correction

   Three of the eight slot-6 glyph blitters read - the base, Trame and
   AntiAlias8 - and what they show reframes section 30.

31.1.  The colour LUT is the whole design

   MR_Font+0x418 is 0x200 bytes that MR_Font.txt had as "never touched
   by anything read so far", with a guess that it was a third
   per-character table.

   IT IS A 256-ENTRY RGB565 TABLE INDEXED BY THE GLYPH'S PIXEL BYTE
   [verified from two blitters independently]. Font_BuildColorLut -
   slot 1, and formerly misnamed Font_BuildCharRange - fills it with a
   32-step backColor-to-foreColor gradient repeated eight times:
   lut[i] = ramp32[i & 0x1f].

   So the pixel byte splits in two:

       bits 0..4   which of 32 gradient steps      b & 0x1f
       bits 5..7   the blend level, 0 = opaque     b >> 5

   ONE BYTE CARRYING BOTH COLOUR AND ALPHA is what makes AntiAlias8
   work, and the shipped data says so out loud: HIRES\MSG.FNT uses
   exactly 0, 31, 63, 95 ... 255, which is 31 | (level << 5) - always
   full foreground, varying only in alpha.

   o SLOT 1'S ARGUMENT IS A LUT INDEX RANGE, NOT A CHARACTER RANGE.
     Slot 0 passes (1, 0xff) and the obvious reading is "every
     character"; nothing about it is per character. The old name
     encoded the misreading, which is why it is gone.

31.2.  Trame is a dissolve, not a fade

   FontDirectTrame_DrawGlyph is byte for byte the base blitter with ONE
   extra test around the store [verified]:

       level * 2 < (((row & 15)*(row & 15) +
                     (col & 15)*(col & 15)) & 0x7f)

   A 16x16 cell of squared distances masked to seven bits. Level 0
   draws everything; higher levels erode the glyph through a fixed
   pattern. "Trame" is French for a halftone screen, which was a guess
   from the name and is now confirmed - and it explains why the family
   has both Trame and AntiAlias8 variants: one dissolves, the other
   blends, and on a 1997 display those are different tools.

   The row and column counters run DOWNWARD in the original, so the
   pattern anchors to the bottom right of the glyph. Reproduced rather
   than tidied - anchoring it top left would shift the pattern by the
   glyph size and make dissolving text ripple differently.

31.3.  A correction to section 30

   Section 30.3 said the two-blitter split "falls straight out of the
   format" and "matches the declared class exactly". THE FIRST HALF WAS
   WRONG.

   paletteEntries == 0 covers TWO different meanings. A Direct face and
   an AntiAlias8 face both have no palette and their bytes do not mean
   the same thing - DEBUG.FNT holds only 29 and 128, MSG.FNT holds a
   clean eight-level ramp. Choosing a blitter by "does the face have a
   palette" conflates them, which is precisely why the original has
   eight blitters and not two.

   o THE TELL, AGAIN, WAS A CORRELATION READ AS A MECHANISM. Pal fonts
     really do have palettised faces, so the rule looked verified from
     the data; what it could not see was the distinction it was
     collapsing. The same shape as the Matrix34 aliasing claim in
     section 29.3, one level up.

   font.c now has three blitters keyed to the class, and the class
   table marks which four rows point at a blitter that was NOT read.

31.4.  Two things it fixes on screen

   COLOUR. DEBUG.FNT holds only the values 29 and 128. Through the LUT
   those are ramp[29], near the foreground, and ramp[0], the
   background - white glyph, black box. Read as an intensity, which is
   what section 30 did, they are near-black and mid-grey: black text on
   a grey box, which is what it looked like.

   TABULAR DIGITS, which are ours and not the original's. The shipped
   faces are proportional - DEBUG.FNT's digits are 4, 6 or 7 pixels
   wide, with '1' the narrow one and '4' and '7' the wide ones - so a
   counter changing value moves everything after it. font_set_tabular_
   digits advances every digit by the widest and centres it, so only
   the digits change and the text after them holds still.

   The periodic console fps line is gone; the overlay replaced it.


32.  All eight blitters

   The remaining five slot-6 functions read, plus the two slot-1
   builders that go with them. Nothing in the font family is unread
   now, and font.c has no stand-ins left.

32.1.  A miscount to own first

   Section 31.3 said "the class table marks which four rows point at a
   blitter that was NOT read". THE TABLE MARKED FIVE. Three of eight
   were read, so five remained, and both the prose here and the open
   question in MR_Font.txt said four - each having silently dropped
   AntiAlias8Transp.

   o THE TABLE WAS RIGHT AND THE SENTENCE WAS WRONG, which is the
     useful direction: the machine-checkable artifact disagreed with
     the prose about it and the prose lost. Worth a habit - when a
     count appears in both a table and a sentence, the sentence is the
     one to distrust.

32.2.  Three of the eight are the same function

   Direct, Pal and Moto are all 898 bytes and functionally IDENTICAL -
   113 and 251 bytes differ from the base respectively, and every
   difference is register allocation [verified]. The inner loop is the
   same opaque store in all three.

   PAL AND MOTO SPECIALISE ENTIRELY IN SLOT 1. Their blitters had
   nothing to change because the colour indirection already lives in
   the LUT. That is the payoff of the design section 31.1 uncovered,
   and it is why byte-comparing two of these functions before
   decompiling either was worth the one query it cost.

32.3.  CustomColor has no LUT, and that is the class

   It computes (fore*step + back*(31-step))/31 per pixel from
   b & 0x1f, inline, and never touches the table.

   o SO ITS BARE-RETURN SLOT 0 AND SLOT 1 ARE THE POINT, NOT STUBS.
     With no table to maintain, a colour change needs no rebuild and
     takes effect on the very next glyph - which is what a class
     called CustomColor would want. Section 31 recorded those
     three-byte RET 0x8 overrides as "ignores colour changes", which
     was the right observation with the wrong sign on it.

   CustomColorTrame is that with the same halftone test. The family is
   regular: Direct/DirectTrame and CustomColor/CustomColorTrame differ
   by exactly one screen-door comparison.

32.4.  Transp means it can reach zero ink

   FontShadedTransp_DrawGlyph does the same blend as AntiAlias8 but
   with explicit arithmetic rather than the ramp tables:

       alpha = bias + (b > 0x1f ? (b & 0xe0) : 0);
       if (alpha < 0xe1) {
           alpha = (alpha << 8) / 0xe0;
           out   = src - ((src - dst) * alpha >> 8);
       }

   ALPHA IS level/7, NOT level/8. Level 0 is opaque and level 7 leaves
   the destination untouched. AntiAlias8's (i*n)/8 tables can never
   reach zero ink; this can. That is a real behavioural difference and
   it justifies two classes where one looked sufficient.

   o IT ALSO SETTLES HALF OF AN OPEN QUESTION. Section 31 flagged that
     the tabulated version's indexing had an off-by-one and that only
     the DIRECTION was certain. This function blends with no tables to
     misindex, and it blends the same way round - independent evidence
     for the half that was certain, and no help with the half that was
     not.

32.5.  Moto is the only hybrid

   FontMoto_BuildColorLut fills lut[1..14] with a FOURTEEN-step
   gradient and lut[32..62] from the face palette, leaving the rest
   alone. It also ignores its range arguments - the zones are
   hardcoded.

   Fourteen matches the data: the MOTO faces hold pixel values 0..15.

   And FontPal_BuildColorLut resolved the last unknown fields:
   MR_Font+0x549a is a pointer to the face's RGB triples and +0x549e
   the entry count, used as a MODULO so a palette of any size tiles
   across the 256 entries. The packed tail of MR_Font is now fully
   mapped.


33.  menu.c, and where the game code goes

   The menu driver, the page table and the page handlers, in one file
   named after the original's own: the leaked assert paths put menu.cpp
   under D:\Dsed\mr320\menu\.

33.1.  It is a net file REMOVAL

   page_loading.c and page_loading.h are gone - page 1 moved into
   menu.c unchanged in behaviour. Adding page 14 as a second per-page
   file would have started a pattern that ends at 28 of them.

   THE ORIGINAL DID SPLIT PER PAGE - pageMainMenu.cpp and
   pageProtocol.cpp are two more leaked paths, alongside menu.cpp,
   bitmap.cpp and menuTools.cpp. So one file per page is what Delphine
   did, and it is still the wrong choice here: their 28 pages were
   written by a team over a year, ours are transcribed one at a time
   and share every helper.

   WHEN TO SPLIT, so it is a decision rather than a drift: when the
   page handlers dominate menu.c, move them to menu_pages.c and leave
   the driver behind. That mirrors menu.cpp against pageXxx.cpp without
   going to one file per page.

33.2.  There is no giant switch, and there never will be

   The question was whether main.cpp grows a mode switch as the game
   fills in. It does not, because the original has none [verified].
   Three levels and not one of them switches on a mode enum:

   +-------+---------------------------+---------------------------+
   | Level | Mechanism                 | Where                     |
   +=======+===========================+===========================+
   | game  | a nine-slot C++ vtable on | 0x004d04c8, one instance  |
   |       | one global object         | see game-object.txt       |
   +-------+---------------------------+---------------------------+
   | mode  | a BOOLEAN - in a race or  | FUN_004126a0 returns 0 or |
   |       | not                       | non-zero, two branches    |
   +-------+---------------------------+---------------------------+
   | page  | a 28-entry TABLE indexed  | g_menuPageTable, 16-byte  |
   |       | by page number            | records                   |
   +-------+---------------------------+---------------------------+

   o SO THE FRAME LOOP DOES NOT GROW. It gains one call - game_tick() -
     and everything below it is table dispatch. The loop in main.cpp
     is close to its final shape already; what fills in is what
     game_tick does, not the loop around it.

   That is a better answer than a switch would have been, and it was
   worth checking rather than assuming: a 28-way switch would have
   argued for splitting the pages across files, and a table argues for
   keeping them together.

33.3.  Page 14, the title screen

   Enter loads four title LAYERS - ids 188..191, or 192..195 for the
   Grand Prix branding - and draws all four in one frame. The tick then
   draws only as many as the reveal has reached, and once the title is
   fully up it blinks "FULL D3D VERSION" in font id 31. Enter goes to
   page 15.

   THE BLINK IS REPRODUCED AS A TIME, NOT A FRAME COUNT. The original
   counts frames: a counter increments per tick, the text draws above
   10 and resets past 20. Twenty frames at the 30 fps the code was
   written for is 0.667 s; at 144 it would be a flicker. This is
   exactly the hazard frame-loop.txt section 4 describes, met for the
   first time in real code.

   It also exercises font.c's AntiAlias8Transp path for real -
   MENULINK.FNT is the face the menus use, so the /7 blend read in
   section 32 is doing the work rather than sitting in a table.

33.4.  What page 14 does not do, and what gates it

   THE THREE 3D MESHES. Enter loads ellipse, moto and racer .3df/.map
   pairs from data\pages\titleMr\, scales each with
   branding-specific factors, and initialises three model instances at
   0x00540500, 0x00540684 and 0x00540808 - stride 0x184, which the tick
   walks as 0x61 floats and animates along per-object speed curves.

   The geometry path would run them today. What is missing is ONE
   STRUCT: the 0x184-byte model instance, initialised by
   maybe_ModelInstance_Init, unread. Inventing a layout to get letters
   moving would be the guess rule 4 exists to stop, so the meshes wait.

   o THAT MAKES THE MODEL INSTANCE THE HIGHEST-VALUE STRUCT LEFT. It
     gates page 14's letters, page 25's Delphine logo, and every other
     3D menu page - which is 25 of the 26 live ones.

   THE MENU WIDGET LIST. Enter registers a sprite button for VK_RETURN
   and the tick calls Menu_Draw, Menu_GetActivated and
   Menu_UpdateTransition. Those belong to MR_Menu, the 0xd4c object
   App_Init constructs, not reconstructed. menu_on_key stands in.

33.5.  A field order worth knowing

   The page record is {initialised, fnTick, fnEnter, fnLeave} - TICK
   BEFORE ENTER, which is not the order they run in. menu-pages.txt
   section 2 lists its address columns in run order instead, so the
   two disagree by design; section 1.1.1 now says so.

   Caught by reading the raw record rather than trusting either.


34.  Polishing page 14, and a new evidence class

   Three fixes from a retail comparison, one structural find, and a
   tag.

34.1.  [observed] - what a longplay is worth

   Rule 8 says never run the game, so runtime behaviour has no evidence
   path from inside this project. A comparison against a retail
   longplay recording supplies one, and it earns its own tag rather
   than being folded into [verified]:

       [observed]   seen in a recording of a retail build

   IT IS WEAKER THAN [verified] IN TWO SPECIFIC WAYS. The recording may
   be of 3.20 rather than our 3.22 - docs/releases.txt has three builds
   with the same thirteen translation units - and it is somebody else's
   capture rather than our binary. It is STRONGER than [inferred] for
   anything visual, because no amount of static reading tells you which
   way a logo slides.

   Used properly it also CHECKS the static work rather than just adding
   to it, which is the point of the next two sections.

34.2.  The strapline colour and font are both in the binary

   THE COLOUR IS IN THE BINARY. The tick calls Font_Get(0x1f) and then
   that font's vtable slot 0 - Font_SetColors - before drawing. The two
   arguments are 0x004f0de8 and 0x004f0dd8, and reading them gives
   (255,255,0) on black [verified]. Yellow.

   o IT IS EASY TO READ PAST. It renders as
     (**(code **)*puVar9)(&DAT_004f0de8,&DAT_004f0dd8) - an indirect
     call through slot 0 with two data pointers - and looks like
     because it had no name on it. The font work two sections earlier
     had already established that slot 0 is Font_SetColors and takes
     two colour triples, so the pieces were both in hand.

   THE POSITION IS ARGUABLE FROM THE BINARY. The draw call carries 0x3c
   and 0x22 - 60 and 34 - among its arguments, and 34 is a top-of-screen
   y. Tagged [inferred], corroborated by the recording, because the
   argument order of that 26-argument function is still unworked.

34.3.  The sprite pointer table

   Menu_LoadResources fills an array of MR_Sprite* at 0x00504880
   INDEXED BY RESOURCE ID [verified]. Every page reads its sprites from
   it, and the base is loaded as a register in Menu_LoadResources,
   Menu_Tick, Menu_GotoPage, Menu_UpdateHover and a dozen handlers.

   o IT MAKES EVERY DAT_00504xxx GLOBAL IN ANY PAGE HANDLER READABLE -
     subtract, divide by four, look the id up in menures.c. Twenty-six
     page handlers are written against this array and not one of them
     names a resource.

   It fell out of asking what the START button the recording described
   actually was. 0x005049b0 resolves to id 76, start.lez, and 0x005049d4
   to id 85, Pointeur.lez - a button bound to VK_RETURN and a mouse
   pointer gated on fullscreen, which is exactly what those two names
   predict. Two independent confirmations from one question.

   The button is loaded and drawn now. ITS HOVER IS NOT REPRODUCED -
   retail keeps it transparent until the pointer is over it, which is
   Menu_UpdateHover's job and needs MR_Menu.

34.4.  The demo3d letters were off by default, and are now gone

   HISTORICAL - demo3d.c has since been DELETED; see section 49.

   They were the geometry path's test harness and they drew over the
   menu, so [DEBUG] demo3d=1 was needed to bring them back. They were
   kept for a while because nothing else exercised transform, cull,
   clip, the queue and drawscene end to end. Pages 14 and 25 do now.

34.5.  What the recording gives the model instance struct

   The three meshes have names - ellipse, moto, racer - and now they
   have BEHAVIOUR [observed]:

       ellipse   the centre oval; zooms toward the camera while
                 rotating about the vertical axis, then stops
       moto      the upper logo; slides in from the RIGHT
       racer     the lower logo; slides in from the LEFT

   o THAT TURNS THE STRUCT FROM GUESSWORK INTO SOMETHING CHECKABLE. A
     layout that does not produce a decelerating zoom-and-spin plus two
     opposite lateral slides is the wrong layout. Before this, any
     self-consistent reading of 0x184 bytes would have looked as good
     as any other.

   The tick already agrees: only object 0 takes the branch that writes
   a rotation pair - -PI/2 and PI below a threshold, a different pair
   above - and objects 1 and 2 take only the position lerp. One
   rotator, two sliders.


35.  MR_ModelInstance, and three hacks removed

   The struct that gates 25 of the 26 live menu pages, plus a rule and
   the corrections it forced.

35.1.  Rule 5a - never fake the retail look

   CLAUDE.md rule 5a. Match the original's CODE, not screenshots of
   it: a visual hack is indistinguishable from a correct
   implementation until the real code lands, and then it silently
   fights it.

35.2.  The three things it caught

   THE REVEAL TIMER. Page 14's phase was being advanced on a 400 ms
   timer so the later title art would appear. The original's phase is
   the COUNT OF FINISHED MODEL ANIMATIONS - with no instances nothing
   finishes and the phase stays 0. It stays 0 now, and only the first
   title sprite draws. That is correct for what is reconstructed.

   THE SPRITE STACK. The tick draws ONE title sprite, selected by
   phase [verified]. The ENTER HANDLER is what layers all four, in a
   single establishing frame - do not read its behaviour as the
   tick's.

   THE FONT. The strapline used MENULINK and AntiAlias8Transp because
   the menus use them. The tick asks Font_Get for id 0x1f, and FONTS.BPT
   id 31 is "DataDiskMenu", type Direct - so it is DATADISK.FNT. The
   id is in the decompilation and the id table is in MR_Font.txt. The
   retail font is [observed] "shorter, thicker, italic", a different
   face, and
   they were right.

   o ALL THREE WERE RECOVERABLE FROM THE BINARY WITHOUT THE
     RECORDING. That is the pattern worth noticing: the recording did
     not supply answers, it pointed at questions I had not asked.

   The START button was a fourth. Enter calls Sprite_SetPosition with a
   third argument of 1 - SPRITE_ANCHOR_CENTRE_X - and the shared
   load_res helper had no anchor parameter, so it passed 0 and the
   button sat bottom-left instead of bottom-centre. A helper that
   quietly drops an argument the original considered important.

35.3.  The struct

   ModelInstance_Init(this, position, rotation, mesh), 0x184 bytes:

       +0x00  float[3]  rotation, Euler, RADIANS
       +0x0c  float[3]  position
       +0x34  void*     the mesh
       +0x94  float     1.0f, probably a scale, no reader found

   THE INITIALISER ALONE DOES NOT SETTLE THE ARGUMENT ROLES, and an
   earlier plate correctly warned against building on them. What
   settles them is the CALLER: MenuPage14_Title_Tick walks the three
   instances at a stride of 0x61 floats - which is 0x184 - and treats
   [0..2] as a rotation and [3..5] as a position. Those are offsets
   0x00 and 0x0c. The page-14 call passes PI/2 as 0x3fc90fdb in the
   vector that lands at +0x00, and a quarter turn belongs in an
   orientation.

   o IT IS NOT A MENU TYPE. Racer_Create embeds THREE in every racer,
     at +0x24, +0x1a8 and +0x32c, filled from a body mesh and
     g_meshRoueArriere and g_meshRoueAvant - two wheels. The wheels are
     separate placed meshes rather than part of the body.

35.4.  What the recording did

   It CHECKED the layout rather than supplying it. Page 14's three
   instances are the oval, the "moto" logo and the "racer" logo; the
   oval rotates as it zooms and the two logos only slide [observed].

   In the tick, only object 0 takes the branch writing a rotation
   triple - objects 1 and 2 get the position lerp alone [verified]. One
   rotator, two sliders. A reading with +0x00 as the position would
   have predicted two rotators and one slider, which is not what the
   screen does.

   That is the whole value of the tag: before it, any self-consistent
   reading of 0x184 bytes looked as good as any other.

35.5.  Still not drawable

   Nothing has been read that consumes an instance and emits geometry,
   so the type can be built and animated but not rendered. FUN_0048c030
   - same address region, called three times by the page-14 tick - is
   the obvious candidate and is unread. Until it is, page 14's oval and
   logos stay off screen.

   Two other known gaps, neither guessed at: the START button's
   un-hovered stipple is Menu_Draw's choice and Menu_Draw is unread, so
   the mode is not invented here; and our binary's strapline BLINKS
   where the recording does not, which is kept because the code is the
   authority - likeliest explanation is that the recording is 3.20 and
   ours is 3.22.


36.  ModelInstance_Draw, and a cast that compiled

   model.c and model.h. The draw path read, transcribed, and one real
   bug caught by not trusting the build.

36.1.  It confirmed the struct rather than extending it

   ModelInstance_Draw reads position at +0x0c, rotation at +0x00 and
   the mesh at +0x34 - all three settled last session from the
   initialiser and the page-14 tick, BEFORE this function was read. It
   adds exactly one field, detailLevel at +0x14c, whose SIGN picks
   between the two cull variants.

   o A PREDICTION THAT SURVIVES ITS TEST IS WORTH MORE THAN ONE MADE
     AFTERWARDS. Had the draw disagreed - position at +0x00, say - the
     whole reading would have been wrong and the recording's
     one-rotator-two-sliders check would have been a coincidence.

36.2.  The euler convention, from the disassembly

   The decompiler loses most of the FPU stack in the rotation block, so
   this came from the instructions [verified]:

       index = (int)(radians * 2607.5945876183)
       cos   = g_cosTable[index & 0x3fff]
       sin   = g_cosTable[(index - 0x1000) & 0x3fff]

   THE CONSTANT IS 0x4000 / 2pi USING THE SAME SHORT 2pi trig.c
   already reproduces - 16384 / 6.283185307178 matches the stored
   double, 16384 / the true 2pi does not. So the typo'd literal is used
   twice, and that is a consistency check on both readings rather than
   a curiosity: a table built from one and indexed by the other would
   drift by half an entry at the far end.

   The nine matrix products were transcribed and then CHECKED - each
   row squares to 1 identically in c0,s0,c1,s1,c2,s2, which a
   mis-transcribed sign would break. The same nine appear in
   MenuTools_BuildViewMatrix, so this is the game's one euler
   convention rather than this function's.

36.3.  A cast that compiled and was wrong

   The first version of model_draw did what the original does:

       cull_and_queue_faces(verts, (MR_Primitive *)m->mesh->faces,
                            m->mesh->faceCount);

   IT BUILT CLEANLY AND IT WAS GARBAGE. In the original a mesh's faces
   ARE MR_Primitives - Load3dfAndMap builds them that way - but our
   mesh.c predates MR_Primitive being in src/ and emits its own simpler
   MESH_FACE. The two are different shapes, and demo3d.c has carried a
   converter between them all along with a comment saying it should not
   exist.

   o THE CAST WAS A LIE THE COMPILER COULD NOT CATCH, because both
     sides are pointers. What caught it was reading the two type
     definitions - and the only reason to go looking was that the
     original's line looked too easy.

   o IT IS ALSO EXACTLY THE FAILURE MODE RULE 5a DESCRIBES, one level
     down. Faithfully copying the original's SHAPE while the types
     underneath disagree produces something that looks correct and is
     not.

   model_draw now takes the primitives explicitly, with the real fix
   named: when mesh.c emits MR_Primitive directly, the extra fields go
   away and mesh->faces is used as the original uses it.

36.4.  What is still missing

   detailLevel has nowhere to go - cull_and_queue_faces takes no level
   argument, so the sign test cannot be reproduced. Recorded, not
   faked.

   And page 14 still does not animate, because nothing yet builds its
   three instances: that needs mesh.c's converter output wired to
   model_set_prims and the page-14 tick's animation curves transcribed.
   The pieces are all present now - this is assembly work rather than
   discovery.


37.  Page 14 animates, and rule 5b

   The three instances built, driven and drawn. Plus a rule about
   keeping the two halves of the project honest with each other.

37.1.  Rule 5b - discoveries flow BOTH ways

   CLAUDE.md rule 5b. A fact learned on either side - the Ghidra DB
   or src/ - is recorded on both. A rename or retype that exists only
   in src/ is a divergence, not a discovery.

   It does NOT mean src/ must mirror the DB's layouts;
   MR_ModelInstance is deliberately simpler. What is banned is the
   two drifting apart by accident.

37.2.  The animation tables were invisible until their writer was found

   g_p14AnimStart, g_p14AnimEnd and g_p14AnimDuration are ALL ZERO IN
   THE IMAGE, so they read as dead data. MenuPage14_Title_ResetAnim
   (0x00446460, called at the end of enter) fills them [verified]:

       ellipse  (60, 42, -40000) -> (60, 42, -800)      1.0 s
       moto     (-12000, 44, -798) -> (42, 44, -798)    1.2 s
       racer    (12000, 25, -798) -> (40, 25, -798)     1.4 s

   THE ELLIPSE COVERS 39200 UNITS IN ONE SECOND, straight at the
   camera. That is the oval rushing at the viewer, and it was sitting
   in a table that looked empty.

   o THE LAST TWO VECTORS GO THROUGH A __thiscall COPY whose
     destination is in ECX, which the decompiler renders as a call
     with one argument and no visible target. Reading the MOV ECX
     before each call gives 0x540010 and 0x5401b8 - the third object's
     start and end. A decompiler blank is not an absence.

37.3.  A discrepancy kept rather than resolved

   The retail footage has "moto" arriving from the RIGHT and "racer"
   from the LEFT. The table has moto starting at x = -12000 and racer
   at +12000 - the other way round. The mesh load order is
   unambiguous, so this is not a mix-up in the reading.

   IMPLEMENTED AS THE CODE HAS IT, per rule 5a, and flagged in three
   places. It will be obvious on screen, which is the point of not
   papering over it.

37.4.  Two functions the port needed and did not have

   mesh_scale, from Mesh_Scale (0x004204b0) - a PER-AXIS vertex
   multiply, where mesh_load only had a uniform load-time scale. Page
   14 needs it: its meshes scale by (0.04, 0.03875, 0.04) and friends,
   deliberately unequal. The original also rebuilds face normals
   afterwards; our MESH carries none, which is a gap and is recorded
   as one.

   prim_build_from_mesh moved out of demo3d.c into prim.c. Two callers
   is where a private helper becomes a shared one, and prim.c already
   owns prim_kind_from_map. demo3d.c is now a three-line wrapper.

   o NOT DUPLICATED. Copying the converter into menu.c would have been
     the fast move and would have left two things to fix when mesh.c
     finally emits MR_Primitive directly.

37.5.  What it does now

   Enter loads bank 3 and the three .3df/.map pairs, scales each,
   builds primitives and initialises three instances at their start
   positions. The tick advances each by dt / duration, lerps the
   position, rotates object 0 through a full turn, counts the finished
   ones into the phase, and draws all three through model_draw into
   the queue.

   THE PHASE IS NOW EARNED. Section 35 left it stuck at 0 because
   nothing could finish; it advances off real animation completions
   now, so the title sprite progression and the strapline appear
   because the models arrived - which is what the original ties them
   to.


38.  Two bugs the screen found

   Both reported from looking at the running build, both real, and both
   traceable to a specific wrong assumption rather than a typo.

38.1.  0x22 was a flags word, not a y coordinate

   The strapline was drawn at (60, 34), from the two small integers in
   the draw call - 0x3c and 0x22. It looked left-of-centre on screen
   and the user expected it centred. It is centred, and finding that
   took two more functions.

   MENU_DRAWTEXT PASSES ITS x AND y BY ADDRESS. arg4 and arg5 go to
   Text_ApplyAlignment (0x00417900) as int*, and are read back
   afterwards as the final position. They are in/out.

   TEXT_APPLYALIGNMENT IS THE ANCHOR LOGIC, and its fifth argument -
   the 0x22 - is a BITFIELD [verified]:

       if ((flags & 6) && !(flags & 0x18) && !(flags & 1))
           x += (mainWidth - textWidth) / 2;

   0x22 has bit 1 set and bits 3 and 4 clear, so it takes exactly that
   branch, with x passed as 0. Centred. The y is arg5 = 60.

   o SO BOTH NUMBERS WERE MISREAD, in different ways. 0x3c was the y
     and I called it x; 0x22 was an alignment mode and I called it y.
     Two small integers side by side in an argument list look exactly
     like a coordinate pair, and in a 26-argument call that is a guess
     wearing a disguise.

   The full flag table is on the Ghidra plate. It covers left, centre,
   right, half-width, top, middle, bottom and half-height - so any page
   that positions text is now readable without re-deriving it.

38.2.  The 3D layer was drawn outside the scene bracket

   Page 14's three models were queued and walked from inside the page
   tick, which the frame loop calls in its 2D phase - BEFORE
   r_begin_scene. Sprites survive that because they are written into
   the framebuffer in software. Geometry does not.

   Split into menu_draw_3d, called from inside the bracket next to
   demo3d_draw.

   o THE ORIGINAL HAS THE SAME SPLIT AND NEVER HAS TO SAY SO. Its page
     ticks call Screen3D_BeginFrame and EndFrame THEMSELVES, so the
     whole tick is already inside a bracket. Ours is called from the
     loop, so the constraint moved and nothing enforced it.

   o AND IT WOULD HAVE LOOKED LIKE A MESH BUG. Nothing failed, nothing
     logged, the meshes were loading correctly the whole time. The
     first instinct on "the model does not appear" is to doubt the
     loader, and the loader was fine.

38.3.  What the phases actually are

   Asked whether enter is the animation phase, and whether the glass
   shatter between pages is a leave handler. The table settles both.

   ENTER RUNS ONCE, on the frame the page becomes current. Page 14's
   loads meshes and sprites, sets up the camera, and calls
   MenuPage14_Title_ResetAnim to zero the animation parameters. The
   ANIMATION ITSELF IS IN THE TICK - it advances t every frame.

   LEAVE CANNOT BE AN ANIMATION, and this is structural rather than a
   guess. Menu_GotoPage calls the leave handler and then immediately
   calls Menu_SetPage, in the same call [verified] - so leave returns
   and the page has already changed. There is no frame in which a
   leave handler is still running. MenuPage14_Title_Leave is 50 bytes,
   which is a teardown and nothing else.

   SO THE SHATTER IS A TRANSITION, NOT A LEAVE. Menu_UpdateTransition
   (0x0040c500, 86 bytes) is called from every page tick, every frame,
   which is the only shape that can play an effect out over time. That
   is where to look, and it is small.


39.  The view matrix was not identity

   Page 14 still showed no oval and no slide. The animation was
   correct; nothing was ever in front of the camera.

39.1.  What was wrong

   model_draw was handed an IDENTITY view matrix, because
   MenuPage14_Title_Enter appeared to set the camera up with a zero
   vector and I read that as "no rotation".

   FUN_0046e140 IS A LOOK-AT, not a rotation setter. Renamed
   Camera_LookAt [verified]:

       d    = normalise(target - camera->position)
       hyp  = sqrt(d.x^2 + d.z^2)
       A    = yaw about the vertical axis, from d.z/hyp and d.x/hyp
       B    = pitch, from hyp and d.y - a cosine/sine pair, since d is
              a unit vector
       view = A * B, translation zero

   Page 14 puts the camera at (0, 0, 4500) and looks at the ORIGIN, so
   d is (0,0,-1) and the view is a 180 DEGREE YAW:

       [-1 0 0][0 1 0][0 0 -1]

   With identity the ellipse's camZ came out -44500. Behind the
   camera, clipped, every frame. With the real matrix:

       ellipse  camZ  44500 -> 5300      the zoom
       moto     camX  12000 -> -42
       racer    camX -12000 -> -40

   o IT FAILED COMPLETELY SILENTLY. No error, no log, correct meshes,
     correct animation - and the symptom pointed at the loader or the
     transform, which were both fine. A camera that faces the wrong
     way produces exactly "nothing draws".

39.2.  A retraction: there was no discrepancy

   Section 37.3 recorded that the code contradicted the retail footage
   on which logo enters from which side, and implemented the code
   "per rule 5a". THAT WAS MY ERROR, in three places, now corrected.

   THE YAW NEGATES x. Moto's world x of -12000 is a CAMERA x of
   +12000 - the right of the screen. It enters from the right and
   racer from the left, exactly as the footage shows.

   o I COMPARED A WORLD COORDINATE AGAINST A SCREEN DIRECTION with the
     transform between them left out. A coordinate is meaningless
     without its space, and "the code says left" was a claim about
     world x wearing the clothes of a claim about the screen.

   o AND RULE 5a WAS INVOKED TO PROTECT THE MISTAKE. The rule is
     right - follow the code, not the footage - but it is not a
     licence to stop checking when the two disagree. A contradiction
     between the binary and a recording of the binary is far more
     likely to be a reading error than a real difference, and should
     be treated as a prompt to look harder.

39.3.  The rotation was right all along

   Confirmed independently: slow-motion footage shows the oval rotate
   about the HORIZONTAL axis until it faces the viewer, then about the
   VERTICAL axis until it stops. The transcribed curve is

       u < 0.5   rot = (u*2pi, -pi/2, pi)      X sweeps 0 to 180 deg
       u >= 0.5  rot = (0, u*2pi + pi/2, 0)    Y takes over

   which is that, in that order. Two independent confirmations of the
   rotation branch now: this, and the one-rotator-two-sliders shape.


40.  The demo owned the pipeline

   Page 14 drew nothing for a third time. Not the meshes, not the
   camera, not the animation - the geometry path was configured to
   discard everything, and had been since demo3d went behind a flag.

40.1.  What demo3d_init was quietly responsible for

   Section 34.4 put the spinning-letters demo behind [DEBUG] demo3d
   and defaulted it off, because it drew over the menu. demo3d_init
   also created and configured the ENTIRE geometry pipeline:

   +-----------------------+--------------------------------------+
   | Owned by the demo     | What zero does                       |
   +=======================+======================================+
   | queue_create          | queue_primitive returns immediately  |
   |                       | when its buckets are NULL            |
   +-----------------------+--------------------------------------+
   | vertexpool_create     | no slice to transform into           |
   +-----------------------+--------------------------------------+
   | g_projScaleX/Y        | every vertex projects to one point   |
   +-----------------------+--------------------------------------+
   | g_nearPlaneZ          | the near test degenerates            |
   +-----------------------+--------------------------------------+
   | g_viewMaxX/Y          | the cull rejects everything as being |
   |                       | off screen                           |
   +-----------------------+--------------------------------------+

   Five separate ways to draw nothing, all of them silent, all of them
   introduced by turning off something unrelated.

   o A TEST HARNESS THAT INITIALISES THE THING IT TESTS HIDES THE
     INITIALISATION. It only shows up when the harness is switched
     off - and by then the suspicion is on whatever is new. That is
     the whole lesson, and it cost three sessions of looking in the
     wrong place.

   main.cpp owns all of it now, next to fb_create. demo3d still sets
   the view bounds, with a comment saying the authority moved.

40.2.  Three silent failures in a row

   Page 14's models were, in order: queued outside the scene bracket,
   transformed by an identity view matrix that put them behind the
   camera, and finally dropped by a pipeline that had never been
   configured. THE MESHES AND THE ANIMATION WERE CORRECT THROUGHOUT.

   Every one of them presented identically - "the model does not
   appear" - and every one of them sent me to doubt the newest code.

   So menu_draw_3d now prints its state once: the view matrix, the
   pool capacity, the projection scale, the cull bounds, each model's
   vertex and primitive counts and position, and the cull's
   examined/submitted counts. A stage that drops everything now says
   which stage it was.

   o THIS IS THE FIX FOR THE CLASS, not the instance. Guessing worked
     twice and failed three times; the pipeline can report itself for
     the cost of a dozen printfs.

40.3.  The .map kind decode is not this

   It was the next thing on the list and it would not have helped. The
   kind decode selects blend and texture modes - it is about how a face
   LOOKS once drawn. Nothing in it decides whether a face reaches the
   queue.

   Still worth doing, and still the largest known gap in the geometry
   path: every face is currently kind 0, opaque flat. But it is an
   appearance job, and appearance was not the problem.


41.  primCount, and the diagnostics paying for themselves

   The fourth silent failure, found in one run instead of one session.

41.1.  What it was

   Page 14's enter did this:

       prim_build_from_mesh(&mesh[i], &prims[i], &inst[i].primCount);
       model_init(&inst[i], ...);                 // memsets the instance
       model_set_prims(&inst[i], prims[i], inst[i].primCount);

   WRITE INTO A STRUCT, CLEAR THE STRUCT, READ IT BACK. The count went
   straight into the instance, model_init zeroed the whole instance,
   and model_set_prims then read back the zero. Every model got
   primCount 0, model_draw returned at its guard, and nothing drew.

   o EACH OF THE THREE LINES IS CORRECT. Only the order is wrong,
     which is the kind of defect that survives re-reading them
     individually - and there is no compiler warning for it.

   The count goes in a local now.

41.2.  The report found it immediately

   Section 40 added a one-shot dump because three failures in a row had
   all presented as "the model does not appear". The very next run:

       page14: model 0  verts 4  prims 0  pos 60 42 -39976
       page14: cull examined 0 submitted 0

   Three facts in two lines. THE MESH LOADED - four verts, a quad. THE
   ANIMATION WAS RUNNING - z had already moved from -40000 to -39976.
   AND THE PRIMITIVES WERE ZERO, which is the only thing between them
   and the screen.

   o NO GUESSING WAS REQUIRED, and the previous three took a session
     each. That is the entire argument for instrumenting a pipeline
     that can discard silently: the cost is a dozen printfs and the
     saving is measured in sessions.

   model_draw's guard now says so out loud too, once, so the next
   instance of this does not even need the dump.

41.3.  What the log confirmed on the way past

   Reading the rest of it, several things are working that had not been
   directly observed before: 23 archives open, DEBUG.FNT resolving, the
   D3D9 device up at 640x480 RGB565, all four title layers and
   start.lez loading at their right sizes and positions - 120x40 at
   260,430, which is the centred START button - bank 3 loading
   PAGES\TITLEMR\BNK00_00.RAW as 256x256 truecolour, and all three
   title meshes at 4 verts and 2 faces each.

   TWO TRIANGLES PER MODEL. The oval and both logo halves are single
   textured quads, as expected from their 78-byte files.


42.  The meshes were 96 times too small

   Not a timing problem, and nothing was blocking the view: the models
   were being drawn, correctly, at a size below one pixel.

42.1.  mesh_load_with_map(..., 0.0f)

   mesh.h has recorded since it was written that the two loader entry
   points scale differently [verified]:

       Load3df        applies its argument ONLY when non-zero    2.8
       Load3dfAndMap  ALWAYS applies its own                    96.0

   Page 14 called mesh_load_with_map with a scale of 0. The argument
   exists only because both entry points share mesh_load underneath;
   for the with-map path the original has no such choice and always
   scales by 96.

   SO EVERY TITLE MESH LOADED 96 TIMES TOO SMALL. It was transformed,
   culled and queued perfectly and came out sub-pixel - which on screen
   is indistinguishable from never having been drawn.

   o demo3d.c PASSES THE CONSTANT AND ALWAYS HAS. The working example
     was one file away, and the fact that the demo's letters were
     visible while the menu's were not should have pointed straight at
     the difference between the two call sites.

42.2.  Five failures, one symptom

   Page 14's models have now failed to appear five times, for five
   unrelated reasons:

   +---+----------------------------------+------------------------+
   | 1 | queued outside the scene bracket | wrong phase of the loop|
   +---+----------------------------------+------------------------+
   | 2 | identity view matrix             | models behind the      |
   |   |                                  | camera                 |
   +---+----------------------------------+------------------------+
   | 3 | no queue, zeroed projection      | the demo owned the     |
   |   |                                  | pipeline               |
   +---+----------------------------------+------------------------+
   | 4 | primCount zeroed by model_init   | write, clear, read back|
   +---+----------------------------------+------------------------+
   | 5 | mesh scale 0 instead of 96       | sub-pixel geometry     |
   +---+----------------------------------+------------------------+

   EVERY ONE PRESENTED AS "the model does not appear", and not one of
   them produced an error, a log line or a failed return.

   o A PIPELINE WITH FIVE INDEPENDENT SILENT DROP POINTS IS THE REAL
     DEFECT. The individual bugs are ordinary; what made them expensive
     is that the geometry path answers "did anything draw?" only by
     showing something. Sections 40 and 41 added counters, and 41 found
     bug 4 in one run - but the counters could not have found bug 5,
     because everything they measure succeeded.

   So the report now prints the SCREEN COORDINATES of each model's
   corners after the draw. A quad at (320,240) to (320,240) is
   sub-pixel; a quad off the edges is mispositioned; and the difference
   between those and "nothing drew" is now visible without running a
   debugger.

42.3.  A diagnostic that lied

   The first version of the one-shot report printed BEFORE the draw
   loop, so its cull counts came from the previous frame - zero, on
   frame one, which reads exactly like "the cull rejected everything".
   It sent me looking at the cull twice.

   o A DIAGNOSTIC THAT REPORTS THE WRONG FRAME IS WORSE THAN NONE,
     because it is trusted. Moved after the draw, and the counts now
     describe the frame they are printed in.


43.  White squares: the bank rebase

   The animation runs. The quads drew white because their faces
   pointed at a texture bank nobody had loaded.

43.1.  Mesh_RebaseFaceBanks

   MenuPage14_Title_Enter calls FUN_00422250(3) three times, once per
   mesh, straight after loading them. It is eleven instructions and it
   ADDS to every face's bank index [verified]:

       for (i = 0; i < faceCount; i++)
           faces[i].bankIndex += delta;

   A .map numbers its banks RELATIVE TO ITS OWN PAGE, typically from
   zero; the caller shifts them to wherever the texture was actually
   loaded. Page 14 loads its raw into bank 3, so the delta is 3.

   o WITHOUT IT, EVERY FACE POINTED AT BANK 0. drawscene's
     prim_is_textured found no loaded page there and returned 0, so
     the quad fell back to its flatColor - and prim_build_from_mesh
     sets that to 0xffffffff for any face whose UVs differ, i.e. any
     TEXTURED face. White.

   o SO "UNTEXTURED" RENDERS AS WHITE BY DESIGN, which is why it looks
     like an unimplemented placeholder rather than a broken lookup.

   demo3d.c never needed this: it loads banks 0 and 1 and its .maps
   already say 0 and 1, so its delta would be zero. Another case of
   the working example not exercising the thing that was missing.

43.2.  A stale comment sent me the wrong way

   I told the user the .map kind decode was still outstanding and that
   it was why the quads looked wrong. THAT WAS WRONG. prim.c has
   decoded the kind since it was written - prim_kind_from_map is that
   transcription.

   The claim came from a comment sitting directly above the converter
   saying "WHAT IT CANNOT DO YET: THE KIND ... every face here is given
   kind 0". It was written in demo3d.c BEFORE prim.c existed and rode
   along when the function moved here in section 37.4.

   o A STALE COMMENT OUTRANKS THE CODE IT SITS NEXT TO, in the reader,
     every time. I read the comment and reported it without checking
     the line below it, which calls the decoder.

   o AND MOVING CODE MOVES ITS COMMENTS' ASSUMPTIONS. The comment was
     true where it was written and false where it landed; nothing in
     the move flagged that. Worth a look whenever a block changes
     files.

   Corrected in place.

43.3.  What the report will show now

   The one-shot dump prints each model's first primitive as bank, kind
   and flatColor. bank 3 with a non-white flatColor means the lookup
   found the page; bank 0 or ffffffff means it did not. That
   distinction was invisible until this session and is what the whole
   problem turned on.


44.  log.c

   Everything the reconstruction prints now goes through mrlog, which
   writes to the console and to <exe_stem>.log beside the executable -
   the same rule config.c uses for the ini. Default on; it will become
   a [LOG] key when there is a reason to turn it off.

44.1.  Two calls, not a level system

       mrlog      an ordinary line
       mrlog_err  the same, prefixed "ERROR: "

   115 printf calls converted, 31 of them to mrlog_err. The split was
   made on what each MESSAGE SAYS rather than where it is - "cannot",
   "truncated", "failed", "did not load" - and then read back one by
   one. Every one is a genuine failure.

   o A SEVERITY ENUM WOULD HAVE BEEN WORSE. The only question anyone
     asks of this file is "did something break", and one greppable
     prefix answers it. Levels invite deciding between INFO and DEBUG
     on every call, which is a decision with no consequence.

   THE FILE IS TRUNCATED PER RUN, not appended. The question it answers
   is "what happened on the run I just did", and appending makes you
   find the last session before you can start reading.

   FLUSHED EVERY LINE, because a log that is still buffered when the
   process dies is missing exactly the part that mattered.

44.2.  Taking the diagnostics out with the fix

   log.h states the policy and this section is it being applied: the
   page-14 one-shot dump, the per-model load lines and the per-resource
   sprite lines are GONE. They were added to chase five specific bugs,
   they found the last three, and keeping them would have left the
   startup log twice as long for no further benefit.

   o THE HARD PART OF A LOGGING POLICY IS DELETION. Adding a dump to
     chase a problem is obviously right; removing it once the problem
     is dead feels like losing something, and that is how a log turns
     into a wall nobody reads.

   What survives is the startup spine - archives opened, device up,
   font loaded, pipeline configured - plus every failure path.

44.3.  One per-frame line found and fixed

   transform_verts_flat logged whenever the vertex pool was full. A
   full pool is full again next frame and every frame after, so that
   line would have written sixty a second the first time it fired.
   One-shot now, and promoted to mrlog_err.

   Two others were checked and were already safe: page14's idle notice
   self-throttles, and model_draw's no-primitives warning was already
   one-shot.

   o WORTH DOING AS A SWEEP, not per call site. "Is this inside
     something that runs every frame" is a question about the call
     graph, and the answer is not visible from the line itself.

44.4.  get_log_file, and not copying the path logic

   config.c grew exe_path_with_ext and get_ini_file now uses it too.
   The ".exe"-stripping is fiddly enough - last dot AFTER the last
   separator - that a second copy would have drifted from the first.


45.  The lens was six times too short

     Section 42 found the meshes 96 times too small and fixed the mesh
     scale. This is the same class of bug one stage further down the
     pipeline, and it had been hiding underneath that one: the
     PROJECTION scale was a guess, and the guess was wrong twice over.

     transform.c had

         g_projScaleX = g_projScaleY = 320

     with a comment calling 320 "a focal length in pixels, which for a
     640 wide image is about a 90 degree horizontal field". That is a
     perfectly reasonable thing to assume and it was never checked.

45.1.  What Screen3D_Open actually computes

     The scales are not free variables. They are DERIVED - base times
     zoom - and Screen3D_Open (0x004995a0) sets the bases from the
     resolution using two doubles:

         g_projBaseX = g_projScaleX = 307200.0 / width
         g_projBaseY = g_projScaleY = 230400.0 / height

     307200 is 640*480 and 230400 is 480*480, and all five call sites
     pass 640 and 480, so both bases are 480. Not 320. A 67 degree
     field, not 90.

     Read the FXCH at 0x004995ef or the axes come out transposed. The
     two FILDs push width then height; the FXCH swaps them. At 640x480
     both answers are 480 so the mistake is invisible - which is
     exactly why it is worth writing down.

45.2.  But the menu does not use it

     And this is the part that mattered. Menu_SetPage (0x0040a000)
     REINSTATES ITS OWN PROJECTION ON EVERY PAGE CHANGE:

         Screen3D_SetProjBaseX(1911.0)
         Screen3D_SetProjBaseY(1911.0)
         [0x00625774] = 256.0        the near plane, not 1.0
         [0x006d1f70] = 40000.0      was 16000 at open

     1911. A 19 degree horizontal field - a long lens, which is why
     menu models read as near-orthographic, and what lets page 14 park
     a model at z = -40000 and still have it be a shape rather than a
     dot. The far distance going to exactly 40000, page 14's own start
     z, is not likely to be a coincidence.

     So the title screen was running at 320 against a correct 1911.
     Everything the geometry pipeline drew on a menu page was SIX
     TIMES too small. That is the "way too small during the animation"
     report, and it was never specific to the animation - the landed
     frame was equally wrong, just less obviously so.

45.3.  The lesson, which is the same one as section 41

     A UNIFORM SCALE ERROR DOES NOT LOOK LIKE A BUG. It is folded into
     the matrix once, upstream of every vertex, so nothing is wrong
     relative to anything else. No geometry is misplaced, no face is
     inverted, no diagnostic fires. It presents as "the art is the
     wrong size", which sends the search to the mesh loader and the
     bank scale - where section 42 had in fact already found a real
     96x error, which made the story more convincing and the remaining
     6x easier to write off.

     The general form: when a value in the reconstruction is a
     PLAUSIBLE GUESS rather than a reading, say so at the point of
     use, and treat any size or position complaint as a reason to go
     and read it. The comment in transform.c did say the camera was
     "ours" - it just said it about the camera position and not about
     the 320 sitting on the line above.

45.4.  What changed

     transform.c now holds all five globals and the five accessors
     (screen3d_open_projection and the base/zoom setters and getter),
     main.cpp calls screen3d_open_projection(640, 480), menu_set_page
     does what Menu_SetPage does, and page14_enter calls
     screen3d_set_proj_zoom(1.0f) as MenuPage14_Title_Enter does.

     THE RULE FOR CALLERS: set a base or set the zoom, never assign a
     scale. The scales are derived and the next setter call would
     silently overwrite the assignment.

     Full working in docs/subsystems/projection.txt, including the one
     thing still unread: Screen3D_DrawScene halves g_projScaleY and
     g_projBaseY, per frame, under a condition nobody has looked at.

46.  The virtual clock, and a primitive bisector

     Two debug facilities. Neither is game behaviour and neither
     changes anything when it is not being driven.

46.1.  The gotcha that decides the design

     "Pause the game" looks like a flag and an early return. It is
     not, and the reason is specific:

         page14_tick:  now = tm_now();  dtMs = now - s_p14_last_frame;

     which is what MenuPage14_Title_Tick does with timeGetTime. GAME
     CODE READS THE CLOCK ITSELF. So skipping the tick for two seconds
     pauses nothing - the next call sees two seconds and advances the
     animation by two seconds in one frame. Skipping is deferring, and
     the deferred time is all paid back at once.

     A flag would have to be honoured by every reader of the clock,
     forever, and the first one that forgot would resume with a jump.

     SO THE CLOCK PAUSES, NOT THE CALLER. tm_now became virtual;
     tm_real_now is the real one. frame_begin is the only thing that
     advances the virtual clock, so every subsystem freezes for free
     and none of them knows a pause exists.

46.2.  Keeping the last frame visible turned out to be free

     This was the part that looked hard, and it evaporated. Drawing
     and presenting keep running every frame - only time stops. So the
     game redraws the identical image from a clock that is not moving,
     and the frame stays up because it is being regenerated, not
     because it is being preserved. No "hold the last framebuffer"
     path exists to get wrong, and input and WM_PAINT need no special
     case.

     The pacer keeps its deadlines in REAL time, so a paused frame
     still waits out the budget instead of spinning.

     A free diagnostic falls out: anything that keeps moving while
     paused is reading the wrong clock.

46.3.  Stepping

     tm_step_frames pauses first, so one key does both jobs. Requests
     accumulate and are consumed one per frame, which is what makes
     holding the key down slow motion at the keyboard repeat rate
     rather than one jump. A step is a fixed amount - the cap period,
     or the simulation step when uncapped - because a step that is a
     different size every time is not reproducible enough to debug
     with.

46.4.  g_queuePrimLimit

     The queue is the only place where every primitive of a frame
     exists at once, in draw order, before any of it is rasterized.
     Capping dispatch there and walking the cap up draws the frame one
     primitive at a time, which turns "there is a stray vertical line"
     into "primitive 47 is the stray vertical line".

     It caps DISPATCH, not the walk. queue_walk clears bucket heads as
     it goes and that is how the queue empties; breaking out early
     would leave last frame's nodes linked and the next frame would
     draw both. Exactly the reason the original keeps walking past its
     own limit rather than returning.

     It also caps bucket 7999, which the game exempts. The exemption
     is right for a HUD and wrong for a bisector - a limit that cannot
     reach a primitive cannot isolate it. The dispatched statistic is
     still taken before the top bucket so its meaning is unchanged.

46.5.  Keys

     SPACE pause, A step (hold for slow motion), [ ] the primitive
     limit, backslash clears it. Handled ahead of menu_on_key so a
     page cannot swallow them. Full detail in
     docs/subsystems/frame-loop.txt section 8.

47.  Two bugs in one chain: the euler matrix and the winding

     Page 14's title animation was wrong in a way that took the whole
     chain to unpick, and the two faults masked each other.

47.1.  Matrix34_FromEuler was mis-transcribed

     The nine products had rot[0] and rot[1] driving each other's
     terms. Corrected in model.c; the working is in
     docs/subsystems/menu-pages.txt section 2.2.5.

     THE LESSON IS ABOUT THE CHECKS, not the maths. Two independent
     verifications passed on the wrong matrix:

     o ORTHONORMALITY PROVES NOTHING. The wrong form is also
       orthonormal with determinant +1 for every input - a rotation
       matrix, just not this one. The comment in the code said "each
       row squares to 1, so a mis-transcribed sign would break it".
       That was false, and it had been sitting there being reassuring.

     o THE DECOMPILER AGREES WITH WHATEVER YOU BRING IT. Ghidra does
       not model the x87 stack, so extraout_ST0 and friends are
       placeholders. Mapping them onto cos/sin pairs by eye reproduces
       the reading you already had - twice, in my case. Simulating the
       instruction sequence symbolically is the honest way, and it
       took about thirty lines.

     What settled it was a PHYSICAL constraint: every flat billboard
     in the game is planar in X, so the rest rotation (0, pi/2, 0)
     must be the quarter turn that faces one at a camera on +Z. The
     old form returned the identity there.

47.2.  Primitives were wound backwards

     Load3df pushes a face's indices index[0] first, and x86 pushes
     right to left, so index[0] lands in the LAST parameter and the
     constructor stores { idx3, idx2, idx1, idx0 } - the file order
     REVERSED. prim.c copied the file order.

     o IT DOES NOT PRESENT AS A WINDING BUG. Geometry, transform,
       projection and animation all stay correct; the only thing that
       changes is WHICH face of a double-sided surface passes the
       facing test. The title billboards are two coincident quads with
       opposite winding and different textures, so the symptom was
       "the logos read mirrored and the light and dark sides are
       swapped" - which sounds like an art or a texture problem.

     o THE UVs REVERSE WITH THE INDICES. A .map record's u[k]/v[k]
       belongs to face vertex k. Reversing the indices alone re-pairs
       every corner with another corner's texture coordinate, which
       mirrors the art a second time and cancels half the fix - it
       would have looked like the reversal made no difference.

47.3.  Why they were hard to separate

     The euler fault put the billboards edge-on, which hid the winding
     fault completely - you cannot tell which face of an invisible
     quad is being drawn. Fixing the matrix made the animation correct
     and revealed the second fault underneath it, unchanged and
     previously unobservable.

     The general shape: when two faults sit in series on one pipeline,
     the upstream one determines what the downstream one is allowed to
     look like. Fixing the first does not "reveal a new bug" - it
     reveals an old one, and the change in symptoms is evidence the
     first fix was right, not evidence it broke something.

48.  The transparency, and two switches with the same name

     The title billboards drew on an opaque black square. Both ends of
     the path were missing and they met in the middle: drawscene.c
     passed the blend ladder's keyed16 argument as a hardcoded 0, so
     its one conditional row was unreachable, and r_set_bank_colour_key
     existed in d3d9rt.cpp - complete with the upload path that turns
     key texels into alpha 0 - with NO CALLER.

     Now: bank.h carries MR_Bank.flags, bank_alloc_texture_slot sets
     them the way D3D_AllocTextureSlot does and applies the key,
     page14_enter calls it, and drawscene.c reads keyed16 off the
     bank the primitive textures from.

48.1.  What actually unblocked it

     THE COLOUR KEY IS A CALLER'S ARGUMENT, not a property of the data.
     D3D_AllocTextureSlot takes wantKey, and page 14 passes 1 with
     rgbBits 16, so bank 3 carries 0x10 | 0x02 - "a colour-keyed 16-bit
     texture" - which is exactly what the ladder tests.

     An earlier session went looking for this and read
     maybe_D3D_RegisterTexture's keyBlack argument instead. Page 14
     passes ZERO there, so the conclusion was "the title bank is not
     keyed" and the whole line of enquiry stopped.

     +-------------------------------+------------------------------+
     | wantKey, D3D_AllocTextureSlot | flags 0x10; drives the blend |
     +-------------------------------+------------------------------+
     | keyBlack, ..RegisterTexture   | flags 0x400; drives the raw  |
     |                               | loader                       |
     +-------------------------------+------------------------------+

     TWO DIFFERENT SWITCHES, both called "the colour key" in different
     notes. When a search for a mechanism comes back "this case does
     not use it", check that the thing you read is the thing the
     consumer actually tests - here the consumer tests 0x10, and only
     one of the two switches sets it.

48.2.  Two implementations of one idea

     When wantKey is set the original ALSO calls SetColorKey with a
     DDCOLORKEY of {0, 0} - black, and not configurable - but only if
     the texture is paletted or -Alpha is off. With -Alpha on it skips
     the key entirely, because the transparency then travels as
     per-texel alpha in a 4444 texture.

     Our back end always uploads A8R8G8B8 with alpha, so it is
     permanently on the second path. The FLAG is what both paths leave
     behind for the blend to read, which is why the flag and not the
     SetColorKey call is the thing to reproduce.

48.2.1.  The other switch, finally read - and one divergence removed

     Section 48.1 left keyBlack as "drives the raw loader", which is
     true and explains nothing, and a divergence grew in the gap: page
     14 was keyed by an explicit bank_set_colour_key call and page 25
     was explicitly NOT, with a comment saying the camera would go
     hollow otherwise. Both pages pass the same arguments to both
     functions. Reading maybe_D3D_LoadRawToTexture through settles it.

     BLACK IS TRANSPARENT WITH OR WITHOUT keyBlack. For a truecolour
     .raw the loader sets alpha 0 whenever the texel quantises to
     black, and never looks at keyBlack. What keyBlack does is run
     first and PROTECT: it maps pure green onto black so green keys
     out, and lifts near-black up to the OpaqueBlackLevel floor so it
     stays opaque. keyBlack = 1 keys LESS black. The name reads
     backwards, which is most of why it was misread twice.

     So wantKey is the only switch, and it was already being recorded
     as flag 0x10. bank_texture now reads that flag; r_set_bank_colour_key
     and bank_set_colour_key are gone, and no page calls anything to
     turn keying on or off. Bank 2, the glass capture, passes wantKey 0
     at 0x0040ab03 and comes out unkeyed for free - which is right,
     because a screenshot's blacks are picture.

     THE DIVERGENCE RESTED ON A NUMBER MEASURED OVER THE WRONG AREA.
     "60 per cent of the texels the camera's own faces sample are
     exactly black" was the whole 256x256 page (63 per cent). The
     camera's 166 faces sample x[19..241] y[14..223] and are 1.7 per
     cent black, with 158 of them at 0.0. The flat billboards are the
     ones with a black background - DSINT 50.8 per cent, and page 14's
     three at 21, 29 and 32 - and they are what the key is for.

     o THE HOLLOW CAMERA WAS THE MISSING MIRROR, fixed later in
       mesh.c. This was collateral from the same wrong reading and it
       outlived the fix, because a divergence with a confident comment
       on it does not get re-tested. Section 47 records the same shape
       of mistake with "carved pan = depth ordering".

     WHO DOES PASS keyBlack = 1, since the question was left open:
     exactly one caller, maybe_Cdd_LoadRawToSurface at 0x00478844.
     All seven call sites were read; the other six pass 0 and
     D3D_InitDevice passes bank.flags & 0x400, which only replays the
     bit. That loader serves the HORIZON BACKDROP - banks 4/5/6,
     Bank\HorizLow|Med|Hi.raw - plus App_Init and Cdd_Init, and a
     full-screen sky-and-ground image is exactly what must not have
     its dark half keyed away.

     The bike effects atlas, guessed here as the likely caller because
     its shadow art sits at (0,0,16) - the OpaqueBlackLevel floor for
     level 1 at 5 bits - passes 0. Its dust and shadow alpha comes
     from the hardcoded path strcmp instead, which is gated on
     alphaBits == 4 and not on keyBlack. Blue 8 would already have
     survived the unconditional black test, so 16 is the artists
     following a convention the code does not enforce for them.

     o MEASURE THE AREA THE CODE ACTUALLY READS. A per-face statistic
       over the .map's own UV bounds took one script and refutes the
       page-wide one outright. It was available the whole time.

48.3.  The proxy that had been right until it wasn't

     Turning the keyed row on made the billboards lose their black
     background AND become 50 per cent ghosts. The second half was a
     line in drawscene.c that had been correct for as long as it had
     existed:

         if (blend != R_BLEND_NONE)
             alpha = 0x80;

     The original does not test "is it blended". It tests a KIND LIST
     [verified, d3d-render-state.txt section 3.2]:

         prim[0x3f] = 0xff;
         if (kind == 0x2c || kind == 0x3e || kind == 0x0c ||
             (kind == 0x44 && devDoesAlphaFlatStippled))
             alpha = 0x80;

     While the only reachable blends were the half blend and the
     additive one, the two conditions selected the same set, so the
     proxy was indistinguishable from the real test. Making SRCALPHA
     reachable added a blend that is NOT in the list, and the proxy
     extended the flat 50 per cent to it. With ALPHAOP set to
     MODULATE(TEXTURE, DIFFUSE) the final alpha is texel times vertex,
     so 0x80 halved the art while the key still zeroed the background:
     a ghost billboard.

     o A PROXY FOR A CONDITION IS A BUG WITH A DELAY. It holds while
       the two agree and breaks the moment somebody widens either one -
       which is precisely when nobody is reading that line, because
       they are looking at the thing they just changed. The tell was
       available: r_blend_for_kind's own comment already said the keyed
       row exists BECAUSE a keyed texture must not take the half blend.
       Two parts of the same file disagreed and only one of them was
       transcribed from the binary.

     Transcribing the real test costs nothing and cannot rot. Where a
     condition is cheap to copy exactly, copy it exactly, even when a
     shorter equivalent is sitting right there.

49.  Removing the proof-of-concept code

     Four things went, all of them scaffolding rather than game:

     +----------------------+---------------------------------------+
     | demo3d.c / demo3d.h  | the spinning DSI letters; the geometry|
     |                      | path's end-to-end test harness        |
     | draw_test_pattern    | four ramps and a border, to prove the |
     |                      | pixel format and the blit             |
     | mesh_smoke           | load one .3df and print what it holds |
     | transform_smoke      | run one mesh through the transform    |
     +----------------------+---------------------------------------+

     with their call sites, the [DEBUG] demo3d ini flag, the UP/DOWN
     near-plane nudge keys, and the entries in both project files.

     WHY NOW AND NOT EARLIER. The harness earned its place while it was
     the ONLY thing that ran the geometry path end to end - transform,
     cull, clip, queue, drawscene. Pages 14 and 25 do that now, with
     real game data and against a known-correct reference, which is
     strictly better: a harness can only tell you the pipeline runs, a
     reconstructed page tells you it runs CORRECTLY.

     o THE ONE THING TO CHECK WHEN DELETING SCAFFOLDING is what it
       quietly owned. demo3d_init used to create the queue and the
       vertex pool and set the projection and viewport, and when it
       was first put behind an ini flag the geometry path silently
       stopped working - section 41. That ownership had already been
       moved to main.cpp, which is why this removal was uneventful.
       The general rule: before deleting a test harness, grep for what
       it INITIALISES, not just for what calls it.

     Some comments elsewhere still refer to demo3d.c as the place a
     function came from. Those are provenance and were reworded rather
     than dropped - the history of why prim_build_from_mesh lives in
     prim.c is worth keeping even though the file it left is gone.

50.  Text buttons, and a clear that cleared too much

     Page 11 is the first page built out of KIND 1 buttons, and it
     needed two things the sprite-only widget did not have.

     A TEXT BUTTON HAS NO AUTHORED SIZE. Menu_AddTextButton
     (0x0040bd60) stores a string, a font and a position, and
     FUN_00492a90 measures the text to get the hit rect:

         rect = (x, y, x + width - 1, y + height - 1)

     which is why the original can change a value from OFF to
     FILTERED without touching any geometry. Its colour IS the hover
     state - yellow 0x004f0de8 on, white 0x004f0dd4 off - where a
     kind 2 button uses a second sprite or the stipple.

50.1.  Menu_ClearButtons does not clear the activation

     menu_clear_buttons had been clearing the hover index and the
     activated index along with the count. The original clears
     NEITHER: 0x0040bd40 writes buttonCount at +0x40 and needsRedraw
     at +0x30 and stops [verified].

     That difference was invisible until page 11, because every page
     before it built its buttons once in Enter. Page 11 rebuilds the
     whole list every frame - it has to, the buttons are text whose
     content changes - and a click lands in the window procedure
     BETWEEN two ticks. Clearing the activation would have thrown
     every click away one tick after it happened, and the page would
     have been silently dead to the mouse.

     o THE RESET STILL HAS TO HAPPEN SOMEWHERE. It moved to
       menu_set_page, which is the one funnel every page change goes
       through, so a stale hover or activation cannot cross a page
       boundary.

     o AND THIS IS THE SHAPE OF THE WHOLE CLASS OF BUG: a helper
       that does slightly more than the function it stands for, in a
       way that is harmless under every caller that exists yet.

50.2.  repeatOnHold, and the held mouse

     Eight of page 11 buttons are steppers - three volumes and the
     session refresh rate - and a volume runs 0 to 32 in steps of 1.
     Thirty-two clicks is not what the original asks for: Menu_Tick
     re-activates the hovered button on EVERY FRAME the mouse bit is
     set in both this frame and the last, with no initial delay
     [verified, 0x00409c00].

     So the shell now reports the held state as well as the click.
     WM_LBUTTONDOWN and WM_LBUTTONUP set it, and WM_KILLFOCUS clears
     it - a release that arrives while another window has the mouse
     never reaches us, and a latched button would keep stepping.

     THE RATE, THOUGH, IS OURS, AND IT HAD TO BE. Transcribed exactly,
     a single click lasts five or six frames and moves the volume a
     fifth of its range: about six reachable settings out of
     thirty-three. That is not the original behaving differently, it
     is the original behaving the same and the machine being sixty
     times faster.

     Two globals in that block look like the rate limit that would
     have fixed it - 0x00504878 takes timeGetTime and 0x00504e7c is
     zeroed right beside it - and BOTH ARE WRITE-ONLY across all
     239,438 instructions. Nothing reads either one. So there is no
     rate to recover; the feature was started and never finished.

     MENU_REPEAT_DELAY is 0.40s and MENU_REPEAT_RATE 0.09s, both in
     REAL seconds off tm_real_now so the debug pause and the speed
     control do not touch them. A click is exactly one step and a
     hold crosses 0..32 in about three seconds - the same on any
     machine, which is the one thing the original cannot claim.

     o THE TEST FOR THIS KIND OF DIVERGENCE is whether the original's
       own behaviour is well defined. A rate expressed in frames is
       not: it meant one thing on a Pentium and means another here,
       so there is no "correct" version to copy. Compare the volume
       range, which IS well defined at 0..32 and is copied exactly.

50.3.  Buttons that say what is missing

     BtConfig, btinfo and DATADISK lead to pages 12, 13 and 23, none
     of which are built, and the D3D row OFF arm is the software
     rasterizer we do not have. Each logs a line naming what is
     missing rather than doing nothing silently.

     Transitioning to an unbuilt page would have been worse than
     doing nothing: menu_tick would find no handlers and the screen
     would go black with no way back.

     SCREEN MODE AND WAVE OUTPUT WERE IN THAT LIST AND ARE NOT NOW.
     They were left inert on the argument that a value which changes
     and does nothing looks exactly like a working setting - the
     rule 5a hack, indistinguishable from a correct implementation
     until the real code lands and starts fighting it.

     That argument was half right. The line is:

       STORING a setting nothing reads yet is a GAP. The resolution
       work and the audio back end will each read one of these, and
       the ini is where they will read it from.

       DRAWING a value the code does not produce is a FAKE. That is
       what rule 5a forbids, and it is why the D3D row is still
       fixed at ON: there is no second renderer for it to select,
       so an OFF it could show would be a lie about the port.

     Both rows log what is still missing when they are changed, so
     the gap is visible from the console rather than only from this
     document.

50.4.  The volumes moved to audio.c, and they are not the mixer

     The original has no volume of its own. Mixer_GetVolume,
     Mixer_VolumeUp and Mixer_VolumeDown drive a WINDOWS MIXER LINE
     through mixerGetControlDetailsA and mixerSetControlDetails; the
     page only ever sees a 0..32 integer, the range Mixer_Init writes
     into all three max fields [verified].

     audio.c holds the three values instead. Moving the user Windows
     volume from inside a game is 1997 behaviour, and the CD volume
     has nothing to drive now that the tracks are local files. It is
     the only deliberate divergence on the page, and the channel
     numbering is kept as the binary has it - master 0, WAVE 2, CD 1,
     which is NOT row order.

50.5.  options.c, and one copy of every setting

     The analogue of Settings_LoadAll (0x0042ae30), which reads 36
     named integers out of CONFIG.BPT and distributes them across the
     config globals. Ours reads the same settings out of mracer.ini
     under one [OPTIONS] section, keeps the original key names where
     one exists, and clamps every value into range on the way in.

     EVERY VALUE IS AN INTEGER, and where the original stored
     something else we store an INDEX - the wave format is one index
     over eight combinations where the original keeps Wave16Bits,
     Stereo and ReplayRate separately. That keeps the file hand-
     editable and parsing to atoi, and it means a typo can be clamped
     rather than rejected. An unclamped index would read off the end
     of a table, which is the one failure mode a text file this open
     to editing should not have.

     o ONE COPY OF EACH SETTING, WHICH IS THE POINT OF THE MODULE.
       The eleven page settings live in g_options and the three
       volumes live in audio.c; options.c owns neither, it only
       reads and writes them. The page had been holding its own
       statics, which would have become a second copy the moment
       anything else wanted to know the detail level.

     o AND IT LOADS BEFORE audio_init. Nothing plays yet, but the
       volumes have to be in audio.c before anything asks, and the
       ordering is cheaper to get right now than to debug later.

50.6.  A button that lit up and could not be pressed

     Page 14's START had its own hit test and its own VK_RETURN test,
     written before the button widget existed. The hover worked, the
     stipple worked, the key worked - and the mouse did nothing,
     because the hover was a local in page14_tick and menu_on_click
     had no branch for the title page to route a click into.

     THAT IS THE FAILURE MODE OF A HAND-ROLLED WIDGET: it reproduces
     the visible half. The half you can see was three lines and got
     written; the half you cannot see was a dispatch entry and did
     not.

     The original never had two mechanisms. Enter registers START at
     0x00446a43 with Menu_AddSpriteButton and passes 0x0d - VK_RETURN
     - as the button's own key code, so the key and the click land in
     one activation slot and the tick reads Menu_GetActivated without
     caring which happened. Routing the page through the widget list
     removed the hand-rolled hover, the hand-rolled cue edge, the
     hand-rolled stipple and the special-cased key.

     o THE MID-TICK PAGE CHANGE IS SAFE, and it is worth saying why
       rather than rediscovering it. Activating now calls
       menu_goto_page from inside the tick, which frees page 14's
       meshes while the frame is still going. Nothing walks them: the
       3D layer runs later, in the scene bracket, and
       menu_draw_page_3d already refuses to draw a page whose Enter
       has not run - a guard added when page 15 learned to switch to
       itself mid-tick.

51.  C4013 IS AN ERROR, and why it has to be

   The project promotes C4013 - "undefined; assuming extern returning
   int" - to an error in both configurations. That is not tidiness.

   IN C, AN UNDECLARED FUNCTION IS ASSUMED TO RETURN int. On x86 that
   means the caller reads the return value out of EAX. A function that
   actually returns float or double returns it on the FPU stack, so
   the caller takes whatever integer happened to be in EAX and uses it
   as the answer - and the program compiles, links and runs.

   THIS CODEBASE IS THE WORST POSSIBLE PLACE FOR THAT. Almost every
   function worth calling returns a float: engine_torque_at,
   physics_slope, trk_ground_height_at, racer_lean_response,
   racer_ai_blend. A missing include would not produce a crash or a
   link error. It would produce a bike that accelerates wrongly, and
   the search would start in physics.c - where the arithmetic is
   correct - rather than at the top of the file that forgot an
   include.

   The cost of catching it is one line per file. The cost of not
   catching it is a physics bug that is not a physics bug.

51.1.  It has already happened once

   Moving script_asset_name out of bikes.c into assets.c as
   asset_name_from_script left bikes.c calling it with no declaration.
   The build SUCCEEDED - the function returns int, so the wrong
   assumption happened to be the right one, and nothing was visibly
   broken.

   That is the whole argument for the flag. The one time it was
   harmless is the time it got committed; the next one would not have
   been, and would have been found somewhere else entirely.

   Fixed by including assets.h. A full /t:Rebuild afterwards confirms
   no other translation unit has an implicit declaration.

51.2.  The one warning that is left

   racer.c's "conversion from int to float" at the progress
   assignment is DELIBERATE - racer+0x31b1 is a float that holds a
   segment index, which MR_Racer.txt records as a surprise of the
   original rather than a mistake of ours. It is left visible rather
   than cast away, so it keeps saying so.


52.  The debug agent

   agent.c opens a named pipe, \.\pipe\mracer_agent, that an
   external harness drives while the game runs. It is ours - nothing
   in it is transcribed - and it is modelled on the cdino_pc agent.

   THE PROTOCOL LIVES IN agent.h and is not restated here. Only
   GET_MEM and STATUS are implemented; hooks come later.

   tools/mr_agent.py is the client:

       python tools/mr_agent.py STATUS
       python tools/mr_agent.py GET_MEM name=s_race_drive len=64
       python tools/mr_agent.py GET_MEM name=s_race_racers
           offset=0x2e1d len=4 -f f

52.1.  It needs the linker map, and the map has a hole

   The symbol table is parsed out of mracer.map beside the .exe at
   startup, and Debug|Win32 is the only configuration that links one.
   THE AGENT IS THEREFORE A DEBUG TOOL. It still starts under
   Release, finds no map, and answers STATUS with symbols=0 - that is
   the symptom to recognise. Nothing else changes: the game runs
   identically either way.

   Addresses in the map are Rva+Base against the PREFERRED base, so
   they are rebased onto the module's actual base before use. That
   makes the agent correct under ASLR rather than confidently wrong,
   which is what the reference implementation settled for.

   RELEASE ALSO LINKS WITH LTCG, which would hole the map even if one
   were asked for: a function inlined into every caller leaves no
   COMDAT and no symbol. Measured on a Release map -
   racer_wheel_contacts and racer_lean_delta absent, race_tick_physics
   and racer_build_pose present, every file static present. Debug does
   not enable LTCG (it is set only on the two Release configurations),
   so the map the agent actually reads does not have that hole.

52.2.  Duplicate names are real

   44 names in the first map are duplicated, all of them CRT statics
   folded across objects - memcpy_s appears three times at one
   address, One three times at three different ones. The symbol table
   therefore keeps duplicates rather than dropping them, and obj=
   selects between them. Without obj= the first in map order wins.

52.3.  Function hooks, and why they run on the game thread

   SET_HOOK overwrites the first five bytes of a function with a jmp to
   one of TEN per-slot trampolines. The protocol is in agent.h and is
   not repeated here. What matters about the shape:

   THE PIPE THREAD NEVER PATCHES CODE. Hook commands are posted to a
   mailbox and executed by agent_service(), called once per frame from
   the loop in main.cpp. If the game thread were executing inside the
   five bytes being overwritten the process would die somewhere
   unrelated, and nothing would point back here. GET_TRACE crosses the
   same way, because draining the ring rewrites the head and count that
   the trampoline writes. STATUS and GET_MEM stay on the pipe thread:
   they only read, and routing them through a frame would stall them.

   IT DEPENDS ON THE /hotpatch PREAMBLE, five bytes exactly:

       66 90        xchg ax,ax     the hotpatch pad
       55           push ebp
       8b ec        mov ebp,esp

   Debug|Win32 sets CreateHotpatchableImage and the X86Image linker
   flag, so this is what our functions start with - checked, not
   assumed, against six functions in build/Win32/Debug/mracer.exe.
   Anything else at the entry is refused rather than patched, which is
   also what makes a Release build fail safely instead of corrupting
   itself.

   THE WHOLE PREAMBLE IS REPLACED, so the target's own push ebp / mov
   ebp,esp never runs. The trampoline's prologue rebuilds that frame
   identically, which is what puts the arguments where a cdecl callee
   expects them and makes _ReturnAddress() the real call site.

   C4740 is suppressed around the slot macro alone. The warning is
   correct - the trampoline jmps out instead of returning - and ten
   slots would emit it ten times a build.

52.4.  aNf, and why it was added

   cdino's target is integer. Ours passes floats by value everywhere,
   so a raw argument word is a float bit pattern and printing it as a
   pointer is useless. log=a0f reads that word AS A FLOAT; log=a0f+OFF
   dereferences, like the b/w/l forms. The '+' is what separates them.

       SET_HOOK add name=racer_lean_delta nargs=4 cap=20 tag=lean
                log=a0f,a1f,a2f,ra

   reads the two yaws and the speed directly.

52.5.  SEND_INPUT, and the two input paths

   The protocol is in agent.h. What matters about the shape is that
   this program reads input TWO ways, and a harness needs both:

   +--------+---------------------------+--------------------------+
   | Path   | Read by                   | SEND_INPUT covers it as  |
   +========+===========================+==========================+
   | held   | race_poll_input, once a   | keys= - overrides the    |
   |        | frame, GetAsyncKeyState   | driving word for a run   |
   +--------+---------------------------+--------------------------+
   | edge   | WndProc: the menu, and    | tap= - posts a real key  |
   |        | F1 / SPACE / F5           | message to the window    |
   +--------+---------------------------+--------------------------+

   TAPS ARE REAL KEY MESSAGES, not a reimplementation. agent_service
   runs on the thread that owns the window, so SendMessage lands
   straight in the WndProc and a tap does exactly what the key does -
   menu navigation, page restart, pause, single step - with no logic
   duplicated and nothing to keep in sync.

   THE HELD BITS ARE THE GAME'S OWN, 1 accelerate, 2 brake, 4 left,
   8 right, from racer+0x31e9. So a mask assembled from keys= is the
   same mask moto_send_input takes, and one description can drive both
   sides of a differential test. The names are a readable spelling of
   the bits, not a separate scheme.

   A RUN ALWAYS OWNS THE KEYBOARD for its length, even when keys= is
   absent - it then holds nothing. Letting the real keyboard through
   would mean a scripted run recorded whatever the user happened to be
   leaning on, which is the non-determinism the command exists to
   remove.

   IT ENDS PAUSED. The run unpauses, holds for frames= frames, then
   pauses, so whatever is read afterwards is a still frame rather than
   a moving target. That is the same contract the MOTO.EXE side
   offers.

   The command is the only one that BLOCKS for its whole length, so
   both the mailbox wait and the bridge budget scale with frames= at
   45 ms each - headroom over the 30 fps target.

53.  Next step

     1  A REAL PAGE OR A TRACK. The geometry, colour and shading paths
        are all reconstructed, all verified against the binary, and all
        running on the game's own archives. Nothing on the renderer
        spine is unread. Page 14 (title) and page 3 (main menu) are the
        menu answer; .trk is the track answer and its reader is already
        documented.

     2  THE FONT FAMILY, once a page needs text. Section 24.3 sized it:
        seven vtable slots, three of which vary, so three function
        pointers in C.

     3  THE MENUTOOLS SCENE OBJECT, the struct MenuPage25 and the other
        3D menu pages are built from. An earlier coverage note
        calls it the next real struct to discover, and section 25.2
        wants it too - it is the object the shaded transform's second
        matrix comes from.
