// ghidra_commit_signatures.java
//
// The bulk signature pass. Commits the decompiler's own inferred
// prototype to the database for every NAMED function in the game range
// that still declares an undefined return type.
//
// See docs/type-status.txt section 8 for the reasoning and the results.
// It took the game range from 45 typed to 361 of 435 in two runs.
//
// WHY THIS IS SAFE TO DO IN BULK
//
//   - Everything is written with SourceType.ANALYSIS, so Ghidra records
//     these as derived by analysis, not asserted by a person. That is
//     the honest provenance for them and it satisfies rule 4 of
//     CLAUDE.md at a scale hand-work cannot reach. A later verified
//     signature overwrites one of these without argument.
//
//   - Any function with a USER_DEFINED parameter is skipped outright,
//     so hand-built signatures - the applied MR_ struct pointers - are
//     never replaced by a guess. Those functions get their RETURN TYPE
//     only, via setReturnType, leaving the parameter list alone.
//
//   - It is idempotent. Re-run it after any batch of new names.
//
// API NOTE. The overload set is version-specific. This Ghidra has only
//     commitParamsToDatabase(HighFunction, boolean, ReturnCommitOption,
//                            SourceType)
// There is no commitReturnToDatabase, and the three-argument
// commitParamsToDatabase does not exist. A wrong overload fails as an
// opaque ClassNotFoundException from the script loader, not as a type
// error, so check by reflection before editing this.
//
// Usage: Ghidra Script Manager, or headless analyzeHeadless -postScript.
//
//@category MotoRacer

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.data.DataType;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.listing.Parameter;
import ghidra.program.model.pcode.HighFunctionDBUtil;
import ghidra.program.model.pcode.HighFunctionDBUtil.ReturnCommitOption;
import ghidra.program.model.symbol.SourceType;
import ghidra.app.script.GhidraScript;

public class ghidra_commit_signatures extends GhidraScript {

    private static final long GAME_CODE_END = 0x004b0000L;
    private static final int  TIMEOUT_SECS  = 60;

    private boolean isNamed(Function f) {
        String n = f.getName();
        return !n.startsWith("FUN_") && !n.startsWith("caseD_");
    }

    private boolean hasHandBuiltParams(Function f) {
        for (Parameter p : f.getParameters())
            if (p.getSource() == SourceType.USER_DEFINED) return true;
        return false;
    }

    @Override
    public void run() throws Exception {
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        FunctionManager fm = currentProgram.getFunctionManager();

        int committed = 0, returnOnly = 0, failed = 0;

        for (Function f : fm.getFunctions(true)) {
            if (f.getEntryPoint().getOffset() >= GAME_CODE_END) continue;
            if (!isNamed(f)) continue;
            if (!f.getReturnType().getName().startsWith("undefined"))
                continue;

            DecompileResults dr =
                    di.decompileFunction(f, TIMEOUT_SECS, monitor);
            if (dr == null || dr.getHighFunction() == null) {
                failed++;
                continue;
            }

            if (hasHandBuiltParams(f)) {
                // Return type only - do not touch the hand-built parameters.
                DataType rt = dr.getHighFunction()
                        .getFunctionPrototype().getReturnType();
                if (rt == null || rt.getName().startsWith("undefined"))
                    continue;
                try {
                    f.setReturnType(rt, SourceType.ANALYSIS);
                    println("  return only: " + f.getName()
                            + " -> " + rt.getName());
                    returnOnly++;
                } catch (Exception e) { failed++; }
                continue;
            }

            try {
                HighFunctionDBUtil.commitParamsToDatabase(
                        dr.getHighFunction(), true, ReturnCommitOption.COMMIT,
                        SourceType.ANALYSIS);
                committed++;
            } catch (Exception e) {
                println("  FAILED " + f.getName() + ": " + e);
                failed++;
            }
        }
        di.dispose();

        int typed = 0, undef = 0;
        for (Function f : fm.getFunctions(true)) {
            if (f.getEntryPoint().getOffset() >= GAME_CODE_END) continue;
            if (!isNamed(f)) continue;
            if (f.getReturnType().getName().startsWith("undefined"))
                undef++;
            else
                typed++;
        }

        println("full signatures committed = " + committed);
        println("return type only          = " + returnOnly);
        println("failed                    = " + failed);
        println("named game functions: typed=" + typed
                + " still undefined=" + undef);
        println("The remainder are functions where the decompiler cannot get");
        println("past undefined4. Do not 'fix' those by declaring int - see");
        println("docs/type-status.txt section 8.4.");
    }
}
