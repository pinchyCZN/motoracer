// ghidra_fix_split_functions.java
//
// Repairs functions that Ghidra split into a stub plus an orphaned body.
//
// Two MSVC prologue shapes cause it, and both end in a NOP that Ghidra
// treats as a function boundary.
//
// 1. Flag scheduling, gaps of 7 to 17 bytes. The compiler hoists a
//    flag-setting instruction above the callee-saved pushes and leaves
//    the branch below them, because pushes do not disturb the flags:
//
//        004a1770  SUB ESP,0x8
//        004a1773  MOV EAX,[0x0062e444]
//        004a1778  TEST EAX,EAX          <- sets flags
//        004a177a  NOP
//        004a177b  PUSH EBX/ESI/EDI/EBP  <- pushes do not touch flags
//        004a177f  JZ  ...               <- consumes flags from 004a1778
//
//    Ghidra's function-start analyzer pattern-matches the PUSH sequence
//    as a prologue and creates a function there, truncating the real one.
//
// 2. SEH prologue, gaps of 31 to 41 bytes. Same split at the trailing
//    NOP. See docs/subsystems/dd-primary-surface.txt.
//
// The symptom in the decompiler is phantom inputs on the orphaned body:
// in_ZF, in_AL, in_EAX, in_stack_00000010. Any analysis of such a
// function is unreliable, which is why this matters. At least one real
// colour-conversion site was missed because of it; see
// docs/ghidra-db-repairs.txt.
//
// Detection is exact and needs no heuristic: a function whose last
// instruction falls through into the entry point of another function is
// always wrong. Real functions do not fall out of their own body.
//
// Fix: delete the orphan, delete the stub, recreate at the stub's entry
// so Ghidra re-walks the flow and produces one whole function.
//
// Run after any reimport or reanalysis. Via the ghidra MCP
// run_script_inline, or from the Ghidra script manager.
//
//   DRY_RUN = true    report only, change nothing
//   DRY_RUN = false   apply
//   FIX_BELOW         address ceiling; library code is out of scope

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.Listing;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.SourceType;

import java.util.ArrayList;
import java.util.List;

public class ghidra_fix_split_functions extends GhidraScript {

    static final boolean DRY_RUN   = true;
    static final long    FIX_BELOW = 0x004b0000L;
    static final int     MAX_PASS  = 5;

    public void run() throws Exception {
        if (DRY_RUN) {
            report();
            println("");
            println("DRY_RUN - nothing changed.");
            return;
        }

        int total = 0;
        // Merging can expose a further fall-through, so iterate rather
        // than assuming one pass is enough.
        for (int pass = 1; pass <= MAX_PASS; pass++) {
            int fixed = onePass(pass);
            total += fixed;
            if (fixed == 0) break;
        }
        println("");
        println("TOTAL repaired " + total);
        report();
    }

    /** List every split, in scope or not. Changes nothing. */
    void report() throws Exception {
        FunctionManager fm = currentProgram.getFunctionManager();
        Listing listing = currentProgram.getListing();
        int game = 0, lib = 0;

        println("");
        println("  stub        orphan      gap  refs  scope");
        println("  ----------  ----------  ---  ----  -----");

        for (Function f : fm.getFunctions(true)) {
            Address max = f.getBody().getMaxAddress();
            Instruction last = listing.getInstructionContaining(max);
            if (last == null) continue;
            Address ft = last.getFallThrough();
            if (ft == null) continue;
            Function next = fm.getFunctionAt(ft);
            if (next == null || next.equals(f)) continue;

            int n = 0;
            for (Reference r : getReferencesTo(next.getEntryPoint())) n++;
            boolean inScope =
                f.getEntryPoint().getOffset() < FIX_BELOW;
            if (inScope) game++; else lib++;

            println(String.format("  %s  %s  %3d  %4d  %s%s",
                f.getEntryPoint(), next.getEntryPoint(),
                ft.getOffset() - f.getEntryPoint().getOffset(), n,
                inScope ? "game" : "lib ",
                next.getName().startsWith("FUN_")
                    ? "" : ("  ORPHAN NAMED: " + next.getName())));
        }
        println("");
        println("splits in game code: " + game + ", in library: " + lib);
    }

    /** One repair pass. Returns how many were merged. */
    int onePass(int pass) throws Exception {
        FunctionManager fm = currentProgram.getFunctionManager();
        Listing listing = currentProgram.getListing();
        List<Address> stubs = new ArrayList<Address>();

        for (Function f : fm.getFunctions(true)) {
            if (f.getEntryPoint().getOffset() >= FIX_BELOW) continue;
            Address max = f.getBody().getMaxAddress();
            Instruction last = listing.getInstructionContaining(max);
            if (last == null) continue;
            Address ft = last.getFallThrough();
            if (ft == null) continue;
            Function next = fm.getFunctionAt(ft);
            if (next == null || next.equals(f)) continue;

            // An orphan carrying a real name would lose it. Skip and
            // report rather than silently discarding someone's work.
            if (!next.getName().startsWith("FUN_")) {
                println("SKIP " + f.getEntryPoint() + ": orphan named '"
                        + next.getName() + "' - repair by hand, moving"
                        + " the name to the stub's entry point");
                continue;
            }
            stubs.add(f.getEntryPoint());
        }

        int fixed = 0, failed = 0;
        for (Address stubAt : stubs) {
            try {
                Function stub = fm.getFunctionAt(stubAt);
                if (stub == null) continue;
                Address max = stub.getBody().getMaxAddress();
                Instruction last = listing.getInstructionContaining(max);
                if (last == null || last.getFallThrough() == null) continue;
                Function orph = fm.getFunctionAt(last.getFallThrough());
                if (orph == null) continue;

                String stubName = stub.getName();
                boolean keep = !stubName.startsWith("FUN_");
                long want = stub.getBody().getNumAddresses()
                          + orph.getBody().getNumAddresses();

                removeFunction(orph);
                removeFunction(stub);
                Function merged = createFunction(stubAt, null);
                if (merged == null) {
                    println("FAIL " + stubAt +
                            ": createFunction returned null");
                    failed++;
                    continue;
                }
                if (keep) merged.setName(stubName, SourceType.USER_DEFINED);

                long got = merged.getBody().getNumAddresses();
                if (got < want) {
                    println("WARN " + stubAt + ": body " + got +
                            " < expected " + want);
                }
                fixed++;
            } catch (Exception e) {
                println("FAIL " + stubAt + ": " + e.getMessage());
                failed++;
            }
        }
        println("pass " + pass + ": repaired " + fixed +
                ", failed " + failed);
        return fixed;
    }
}
