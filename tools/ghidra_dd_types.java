// ghidra_dd_types.java
//
// WRITES TO THE GHIDRA DATABASE. Set DRY_RUN = true to preview.
//
// Three operations, idempotent, safe to re-run:
//
//   1. Creates IDirectDrawVtbl and IDirectDraw data types. Ghidra ships
//      no ddraw.h, so the vtable is declared here from the documented
//      IDirectDraw v1 layout. Offsets 0x4c/0x50/0x54 are the ones the
//      game actually calls and they match observed argument counts.
//
//   2. Applies IDirectDraw * to the global DirectDraw object pointer.
//
//   3. Renames functions and globals whose identity is established in
//      docs/subsystems/dd-chain.txt, dd-init.txt and config-globals.txt.
//      Every name here is evidence-backed; see those documents.
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
import ghidra.program.model.data.DataTypeConflictHandler;
import ghidra.program.model.data.StructureDataType;

public class ghidra_dd_types extends GhidraScript {

    @Override
    public void run() throws Exception {
        boolean DRY_RUN = false;

        ghidra.program.model.data.DataTypeManager dtm =
            currentProgram.getDataTypeManager();
        ghidra.program.model.symbol.SourceType US =
            ghidra.program.model.symbol.SourceType.USER_DEFINED;

        // ---- 1. IDirectDraw v1 vtable ---------------------------------------
        String[] methods = {
            "QueryInterface", "AddRef", "Release",
            "Compact", "CreateClipper", "CreatePalette", "CreateSurface",
            "DuplicateSurface", "EnumDisplayModes", "EnumSurfaces",
            "FlipToGDISurface", "GetCaps", "GetDisplayMode", "GetFourCCCodes",
            "GetGDISurface", "GetMonitorFrequency", "GetScanLine",
            "GetVerticalBlankStatus", "Initialize", "RestoreDisplayMode",
            "SetCooperativeLevel", "SetDisplayMode", "WaitForVerticalBlank"
        };

        ghidra.program.model.data.DataType iddPtr = null;
        if (!DRY_RUN) {
            ghidra.program.model.data.StructureDataType vt =
                new StructureDataType("IDirectDrawVtbl", 0);
            for (int i = 0; i < methods.length; i++) {
                vt.add(new ghidra.program.model.data.PointerDataType(), 4,
                       methods[i], "vtable +0x" + Integer.toHexString(i * 4));
            }
            ghidra.program.model.data.DataType vtR = dtm.addDataType(vt,
                DataTypeConflictHandler.REPLACE_HANDLER);

            ghidra.program.model.data.StructureDataType idd =
                new StructureDataType("IDirectDraw", 0);
            idd.add(new ghidra.program.model.data.PointerDataType(vtR), 4,
                    "lpVtbl", "COM vtable pointer");
            ghidra.program.model.data.DataType iddR = dtm.addDataType(idd,
                DataTypeConflictHandler.REPLACE_HANDLER);
            iddPtr = new ghidra.program.model.data.PointerDataType(iddR);
        }
        println("types: IDirectDrawVtbl (" + methods.length
                + " methods), IDirectDraw");

        // ---- 2. apply the type to the object pointer ------------------------
        ghidra.program.model.address.Address ddAddr = toAddr("005d922c");
        if (!DRY_RUN) {
            try {
                clearListing(ddAddr, ddAddr.add(3));
                createData(ddAddr, iddPtr);
                println("typed 005d922c as IDirectDraw *");
            } catch (Exception e) {
                println("FAILED to type 005d922c: " + e.getMessage());
            }
        }

        // ---- 3. renames -----------------------------------------------------
        // Functions. Names taken from the binary's own assert strings where the
        // string exists; otherwise from observed behaviour (marked below).
        // 004752d0 name is verbatim from the binary's own assert string.
        // 00476100 from "Fatal error in DirectDraw init"; the rest from
        // observed behaviour. See docs/subsystems/dd-chain.txt.
        String[][] funcs = {
            {"004752d0", "Cdd_SetCooperativeLevelAndDisplayMode"},
            {"00473900", "Cdd_ctor"},
            {"00473950", "Cdd_ctor2"},
            {"00476100", "Cdd_Init"},
            {"004767f0", "Cdd_ReportError"},
            {"0045adb0", "DX_DetectVersion"},
            {"0045acd0", "Str_CaseInsensitiveFind"},
            {"004140a2", "Main_InitAndParseCommandLine"}
        };

        int fn = 0;
        for (int i = 0; i < funcs.length; i++) {
            try {
                ghidra.program.model.listing.Function f =
                    getFunctionAt(toAddr(funcs[i][0]));
                if (f == null) {
                    println("no function at " + funcs[i][0]);
                    continue;
                }
                if (!DRY_RUN) { f.setName(funcs[i][1], US); }
                fn++;
            } catch (Exception e) {
                println("FAILED " + funcs[i][0] + ": " + e.getMessage());
            }
        }
        println("functions renamed: " + fn + "/" + funcs.length);

        // Globals.
        String[][] globals = {
            // DirectDraw object and window state
            {"005d922c", "g_pDirectDraw"},
            {"005d43a8", "g_ddFullscreen"},
            {"005d63b0", "g_hwndMain"},
            {"004ee9dc", "g_ddInitComplete"},
            // resolved display mode
            {"005d93bc", "g_displayWidth"},
            {"005d9c20", "g_displayHeight"},
            {"005d8944", "g_displayBpp"},
            {"005d8968", "g_displayModeTable"},
            {"004ee984", "g_displayModeCount"},
            // command-line configuration
            {"004dc158", "g_cfgMinTextureWidth"},
            {"004dc15c", "g_cfgMaxTextureWidth"},
            {"004dc160", "g_cfgMinTextureHeight_NEVER_WRITTEN"},
            {"004dc164", "g_cfgMaxTextureHeight"},
            {"004dc1f0", "g_cfgFullscreen"},
            {"004dc200", "g_cfgFrameRateMax"},
            {"004dc194", "g_cfgUseD3D"},
            {"004dc198", "g_cfgNoD3D"},
            {"004dc19c", "g_cfgUse3dfx"},
            {"004dc1cc", "g_cfgUsePowerVr"},
            {"004dc1a8", "g_cfgNoSound"},
            {"004dbf7c", "g_cfgJoystickThreshold"},
            {"004ee968", "g_cfgClampTexture256"},
            {"004e4f48", "g_cfgOpaqueBlackLevel"},
            // resolved texture bounds
            {"004ee9f4", "g_texMinWidth"},
            {"004ee9f8", "g_texMaxWidth"},
            {"004ee9fc", "g_texMinHeight"},
            {"004eea00", "g_texMaxHeight"}
        };

        int gn = 0;
        for (int i = 0; i < globals.length; i++) {
            try {
                if (!DRY_RUN) {
                    createLabel(toAddr(globals[i][0]), globals[i][1], true);
                }
                gn++;
            } catch (Exception e) {
                println("FAILED " + globals[i][0] + ": " + e.getMessage());
            }
        }
        println("globals labelled: " + gn + "/" + globals.length);

        // ---- 4. verify ------------------------------------------------------
        println("");
        println("VERIFY:");
        ghidra.program.model.listing.Function v =
            getFunctionAt(toAddr("004752d0"));
        println("  004752d0 -> " + (v == null ? "null" : v.getName()));
        ghidra.program.model.listing.Data dv = getDataAt(ddAddr);
        println("  005d922c -> " + (dv == null ? "no data"
                : dv.getDataType().getName() + " " + dv.getLabel()));
        println("  IDirectDraw in dtm: "
                + (dtm.getDataType("/IDirectDraw") != null));
    }
}
