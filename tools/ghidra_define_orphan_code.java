// ghidra_define_orphan_code.java
//
// Finds and defines code that Ghidra disassembled but assigned to no
// function.
//
// This is a DIFFERENT defect from the split functions handled by
// tools/ghidra_fix_split_functions.java. There the stub and the orphan
// were both defined functions with a fall-through between them. Here
// the instructions belong to no function at all, so they are invisible
// to the decompiler, to call graphs, and to every function-scoped
// search. The split-function detector cannot see them.
//
// Detection: walk every instruction in game code and group maximal runs
// whose containing function is null.
//
// The runs fall into two populations:
//
//   1. MSVC SEH unwind thunks. Ten bytes, "MOV EAX,<handler table in
//      .rdata>" followed by a jump. Compiler-generated, ~160 of them.
//      Harmless either way, but defining them keeps the invariant
//      "no code outside a function" simple.
//
//   2. Real game code. On MOTO.EXE this was 125 runs totalling 68420
//      bytes, including the D3D device init at 0x0041e8f0 and four
//      rasterizer span setups around 0x0049e510.
//
// Fix: createFunction at each run start and iterate. Ghidra bounds the
// body by following flow, so a run that begins with a thunk yields a
// small function and the remainder shows up as a new run on the next
// pass. Iterating to a fixpoint sorts that out.
//
// IMPORTANT: run tools/ghidra_fix_split_functions.java afterwards.
// Defining a function at an SEH prologue whose body was also orphaned
// recreates the split-function defect; on MOTO.EXE this happened three
// times.
//
// Usage: via the ghidra MCP run_script_inline.
//   DRY_RUN = true    report and classify only
//   DRY_RUN = false   define

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.listing.Listing;

import java.util.ArrayList;
import java.util.List;

public class ghidra_define_orphan_code extends GhidraScript {

    static final boolean DRY_RUN  = true;
    static final long    LO       = 0x00401000L;
    static final long    HI       = 0x004b0000L;
    static final int     MAX_PASS = 12;

    /** Maximal runs of instructions belonging to no function. */
    List<Address> scan(List<Long> sizesOut, List<String> firstOut) {
        FunctionManager fm = currentProgram.getFunctionManager();
        Listing listing = currentProgram.getListing();
        List<Address> starts = new ArrayList<Address>();
        Address runStart = null, runEnd = null;
        String first = null;

        InstructionIterator it = listing.getInstructions(true);
        while (it.hasNext()) {
            Instruction ins = it.next();
            Address a = ins.getMinAddress();
            long off = a.getOffset();
            if (off < LO || off >= HI) continue;
            if (fm.getFunctionContaining(a) == null) {
                if (runStart == null) { runStart = a; first = ins.toString(); }
                runEnd = ins.getMaxAddress();
            } else if (runStart != null) {
                starts.add(runStart);
                if (sizesOut != null)
                    sizesOut.add(runEnd.getOffset() - runStart.getOffset() + 1);
                if (firstOut != null) firstOut.add(first);
                runStart = null;
            }
        }
        if (runStart != null) {
            starts.add(runStart);
            if (sizesOut != null)
                sizesOut.add(runEnd.getOffset() - runStart.getOffset() + 1);
            if (firstOut != null) firstOut.add(first);
        }
        return starts;
    }

    public void run() throws Exception {
        if (DRY_RUN) {
            List<Long> sizes = new ArrayList<Long>();
            List<String> firsts = new ArrayList<String>();
            List<Address> starts = scan(sizes, firsts);

            int thunks = 0, small = 0, real = 0;
            long thunkBytes = 0, realBytes = 0;
            StringBuilder sb = new StringBuilder();

            for (int i = 0; i < starts.size(); i++) {
                long sz = sizes.get(i);
                String f = firsts.get(i);
                boolean isThunk = (sz <= 16) && f.startsWith("MOV EAX,0x4d");
                if (isThunk) { thunks++; thunkBytes += sz; continue; }
                if (sz < 32) { small++; continue; }
                real++; realBytes += sz;
                sb.append(String.format("  %s  %6d  %s%n",
                                        starts.get(i), sz, f));
            }

            println("orphan runs               : " + starts.size());
            println("  SEH unwind thunks       : " + thunks
                    + " (" + thunkBytes + " bytes)");
            println("  other under 32 bytes    : " + small);
            println("  SUBSTANTIAL 32+ bytes   : " + real
                    + " (" + realBytes + " bytes)");
            println("");
            println("  start       bytes  first instruction");
            println("  ----------  -----  -----------------");
            println(sb.toString());
            println("DRY_RUN - nothing changed.");
            return;
        }

        int total = 0;
        for (int pass = 1; pass <= MAX_PASS; pass++) {
            List<Address> starts = scan(null, null);
            if (starts.isEmpty()) {
                println("pass " + pass + ": no orphan code left");
                break;
            }
            int made = 0;
            for (Address a : starts) {
                try {
                    if (createFunction(a, null) != null) made++;
                } catch (Exception e) {
                    // a run Ghidra cannot bound by flow; reported below
                }
            }
            println("pass " + pass + ": runs " + starts.size()
                    + ", defined " + made);
            total += made;
            if (made == 0) {
                println("stalled; remaining runs:");
                for (Address a : starts) println("   " + a);
                break;
            }
        }

        println("");
        println("TOTAL defined: " + total);
        println("orphan runs remaining: " + scan(null, null).size());
        println("");
        println("NOW RUN tools/ghidra_fix_split_functions.java - defining");
        println("a function at an SEH prologue can orphan its body.");
    }
}
