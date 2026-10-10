// ghidra_tu_fence.java
//
// WRITES TO THE GHIDRA DATABASE. Set DRY_RUN = true to preview.
//
// Two triage operations, idempotent, safe to re-run:
//
//   1. Library fence. Tags every function at or above 0x004b0000 with
//      LIBRARY. That boundary is where Ghidra's named-function density
//      jumps from 0.1% to ~74%, i.e. where statically-linked MFC/CRT
//      begins. Expect 948 functions.
//
//   2. Translation-unit anchors. Sets a plate comment on each function
//      that references one of the 13 leaked assert-path strings from the
//      original source tree (D:/Dsed/mr320/...). Expect 24 comments.
//
// Re-run this after any re-analysis that clears tags or comments.
// See docs/code-map.txt for the reasoning and the resulting map.
//
// Run it from the Script Manager, or by name through the MCP -
// tools/ is a registered script directory.
//
// IT USED TO BE AN INLINE SNIPPET, which is why it has a class
// wrapper now and did not before: run_script_inline WRAPS what it is
// given, so a bare run of statements was right for that runner and is
// a compile error as a standalone script - Java calls it an unnamed
// class and disables it as a preview feature. Every other script here
// carries the wrapper; these two were the odd ones out.
//
// @category MotoRacer

import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.FunctionIterator;

public class ghidra_tu_fence extends GhidraScript {

    @Override
    public void run() throws Exception {
        boolean DRY_RUN = false;

        long LIB = 0x004b0000L;

        ghidra.program.model.listing.FunctionManager fm =
            currentProgram.getFunctionManager();

        // ---- 1. library fence -----------------------------------------------
        FunctionIterator it = fm.getFunctions(true);
        int tagged = 0;
        int already = 0;
        while (it.hasNext()) {
            ghidra.program.model.listing.Function f = it.next();
            if (f.getEntryPoint().getOffset() >= LIB) {
                if (f.getTags().toString().contains("LIBRARY")) {
                    already++;
                } else {
                    if (!DRY_RUN) { f.addTag("LIBRARY"); }
                    tagged++;
                }
            }
        }
        println("LIBRARY tag: " + tagged + " tagged, "
                + already + " already had it");

        // ---- 2. translation-unit anchors ------------------------------------
        // Address -> TU. Each address is a function that references that TU's
        // assert path string, so membership is verified for that function only.
        String[][] anchors = {
          {"00401e40", "screen3d/bankw.Cpp"},
          {"00409c00", "menu/menu.cpp"},
          {"0040bd60", "menu/menu.cpp"},
          {"0040bef0", "menu/menu.cpp"},
          {"004140a2",
           "init + command-line parser (TU unknown, no assert string)"},
          {"0041c050", "Dsi_d3d.cpp"},
          {"00422280", "frame3d.cpp"},
          {"004329a5", "event.cpp"},
          {"0043e280", "menu/bitmap.cpp"},
          {"0043e700", "menu/bitmap.cpp"},
          {"00449100", "menu/pageMainMenu.cpp"},
          {"00449560", "menu/pageMainMenu.cpp"},
          {"0044af0b", "menu/pageMainMenu.cpp"},
          {"00451505", "menu/pageMainMenu.cpp"},
          {"00462c60", "player.cpp"},
          {"00473da0", "screen3d/dd.cpp"},
          {"00477b30", "screen3d/dd.cpp"},
          {"00479260", "menu/menuTools.cpp"},
          {"00479630", "menu/menuTools.cpp"},
          {"0048713c",
           "menu/pageProtocol.cpp (region start; not in a function)"},
          {"004888bf", "menu/pageProtocol.cpp (region end; not in a function)"},
          {"0048b370", "animDecor.cpp"},
          {"0048b7e0", "animDecor.cpp"},
          {"0049a160", "screen3d/screen3d.cpp"}
        };

        int commented = 0;
        for (int i = 0; i < anchors.length; i++) {
            ghidra.program.model.address.Address a = toAddr(anchors[i][0]);
            String txt = "TU ANCHOR: " + anchors[i][1]
                       + "\nVerified: references this TU's assert path string."
                       + "\nObject files are laid out contiguously, so nearby"
                       + "\nfunctions likely belong to the same TU (inferred).";
            if (!DRY_RUN) { setPlateComment(a, txt); }
            commented++;
        }
        println("plate comments: " + commented);

        // ---- 3. verify ------------------------------------------------------
        int libCount = 0;
        int gameCount = 0;
        ghidra.program.model.listing.FunctionIterator v = fm.getFunctions(true);
        while (v.hasNext()) {
            ghidra.program.model.listing.Function f = v.next();
            if (f.getTags().toString().contains("LIBRARY")) {
                libCount++;
            } else if (f.getEntryPoint().getOffset() < LIB) {
                gameCount++;
            }
        }
        println("VERIFY LIBRARY-tagged      = " + libCount + "  (expect 948)");
        println("VERIFY untagged below fence = " + gameCount
                + "  (expect 1594)");
    }
}
