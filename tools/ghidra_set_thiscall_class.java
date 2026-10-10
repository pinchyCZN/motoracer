// ghidra_set_thiscall_class.java
//
// Give a __thiscall function's hidden `this` parameter a real type.
//
// THE PROBLEM
//
//   Setting a prototype on a __thiscall function types the STACK
//   parameters but leaves `this` as void*, so field accesses decompile
//   as *(int *)((int)this + 4) instead of this->segmentCount. Both the
//   MCP set_function_prototype and set_parameter_type refuse it, with:
//
//       Cannot modify auto-parameter: this
//
//   That error is the whole explanation. Under __thiscall, `this` is an
//   AUTO-PARAMETER - Ghidra synthesises it from the calling convention
//   rather than storing it - so there is no variable to retype. It is
//   not an MCP limitation and no MCP call can fix it.
//
// THE FIX
//
//   An auto-`this` takes its type from the function's CLASS NAMESPACE.
//   Put the function in a GhidraClass whose name matches an existing
//   structure and Ghidra types `this` as a pointer to it. That is why
//   the MR_Racer functions always decompiled correctly - somebody had
//   already put them in an MR_Racer class - and why the Trk ones did
//   not.
//
//   In the GUI this is: right-click the function in the Symbol Tree or
//   Listing -> Edit Function..., or drag the function onto a class in
//   the Symbol Tree; create the class first with right-click ->
//   New -> Class on the Classes folder. The decompiler updates on the
//   next refresh.
//
// USAGE
//
//   Edit CLASS_NAME and ADDRS below, then run via the MCP
//   run_script_inline, or drop this in ghidra_scripts and run it from
//   the Script Manager.
//
//   The class name must match a structure that already exists in the
//   data type manager, or `this` stays void*.
//
// NOTE ON run_script_inline
//
//   Its output is prefixed by ~100 lines of compile errors naming
//   nine McpInline_*.java files, plus a bundle-activation stack
//   trace. None of it is yours, and THE FILES DO NOT EXIST - it is
//   stale in-memory state in Ghidra's GhidraSourceBundle, which
//   never purges build errors for deleted files. Read past it to
//   --- SCRIPT OUTPUT ---; a run ending "SCRIPT COMPLETED
//   SUCCESSFULLY" worked. Script Manager -> Refresh clears it.
//   See docs/ghidra-db-repairs.txt 11.2.

import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Namespace;
import ghidra.program.model.symbol.SourceType;
import ghidra.program.model.symbol.SymbolTable;

public class ghidra_set_thiscall_class extends GhidraScript {

    // The class to create or reuse. Must match an existing struct.
    private static final String CLASS_NAME = "MR_Trk";

    // The __thiscall functions to move into it.
    private static final long[] ADDRS = {
        0x0040c7e0L,        // Trk_FindSegmentForPoint
        0x0040ca80L,        // maybe_Trk_ClipMoveAgainstWalls
    };

    @Override
    public void run() throws Exception {
        SymbolTable st = currentProgram.getSymbolTable();
        Namespace global = currentProgram.getGlobalNamespace();

        Namespace cls = st.getNamespace(CLASS_NAME, global);
        if (cls == null) {
            cls = st.createClass(global, CLASS_NAME, SourceType.USER_DEFINED);
            println("created class " + CLASS_NAME);
        } else {
            println("class " + CLASS_NAME + " already existed");
        }

        for (long a : ADDRS) {
            Function f = currentProgram.getFunctionManager()
                                       .getFunctionAt(toAddr(a));
            if (f == null) {
                println("no function at " + Long.toHexString(a));
                continue;
            }
            f.setParentNamespace(cls);
            println("moved " + f.getName() + " -> "
                    + f.getParentNamespace().getName() + " ; now "
                    + f.getSignature().getPrototypeString());
        }
    }
}
