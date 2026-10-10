// ghidra_check_pointer_tables.java
//
// Finds code that is reachable ONLY through a function pointer stored in
// data, and which Ghidra therefore never disassembled.
//
// This is the third defect class in docs/ghidra-db-repairs.txt, and the
// other two tools cannot see it:
//
//   ghidra_fix_split_functions.java  compares two DEFINED functions
//   ghidra_define_orphan_code.java   walks the INSTRUCTION LISTING
//
// Bytes that were never disassembled are in neither. Ghidra finds code
// by following flow from entry points, so a dispatch table full of
// pointers is invisible to it unless something says otherwise. Any table
// of function pointers is therefore also a list of places to check.
//
// Method: walk every aligned dword in the data sections, treat values
// inside the game code range as candidate pointers, and classify the
// target:
//
//   entry           already a function; fine
//   mid-function    inside another function; usually a jump table or a
//                   coincidence, left alone
//   disassembled    code but in no function; the orphan sweep handles it
//   NOT DISASSEMBLED  the interesting case
//
// On MOTO.EXE this found 13 targets in .data forming two tables: a menu
// page handler table of 16-byte records holding three function pointers
// each, and two more span functions just past the rasterizer walker
// table. One of the eleven menu entries was a 4884-byte function that
// nothing in the database had ever seen.
//
// .rdata is a different story. It holds about 470 pointers into
// undisassembled code, and those are MSVC C++ exception-handling tables
// pointing at unwind funclets. They are compiler-generated cleanup code,
// not game logic, and defining them would add hundreds of tiny functions
// for no analytical gain. This script reports them separately and
// DOES NOT define them by default.
//
// IMPORTANT: after defining anything, run
//   tools/ghidra_define_orphan_code.java   then
//   tools/ghidra_fix_split_functions.java
// Defining a function whose body was already defined creates a split,
// and disassembling fresh code can expose new orphan runs. On MOTO.EXE
// this produced four splits and six orphan runs that needed a repair
// cycle to settle.
//
// Usage: via the ghidra MCP run_script_inline.
//   DRY_RUN = true    report only
//   DRY_RUN = false   define the .data targets
//   INCLUDE_RDATA     also define the EH funclets; normally false

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.listing.Listing;
import ghidra.program.model.mem.Memory;

import java.util.ArrayList;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Set;

public class ghidra_check_pointer_tables extends GhidraScript {

    static final boolean DRY_RUN       = true;
    static final boolean INCLUDE_RDATA = false;

    static final long CODE_LO = 0x00401000L;
    static final long CODE_HI = 0x004b0000L;

    // section, lo, hi, treatAsGameLogic
    static final long[][] SECTIONS = {
        {0x004d0000L, 0x004dae00L, 0},   // .rdata, EH tables
        {0x004db000L, 0x0070c568L, 1},   // .data, real dispatch tables
    };

    Set<Long> sweep(long lo, long hi, String name, List<Long> ptrSites) {
        FunctionManager fm = currentProgram.getFunctionManager();
        Listing listing = currentProgram.getListing();
        Memory mem = currentProgram.getMemory();
        Set<Long> targets = new LinkedHashSet<Long>();
        int entry = 0, mid = 0, dis = 0;

        for (long a = lo; a + 4 <= hi; a += 4) {
            int p;
            try { p = mem.getInt(toAddr(a)); }
            catch (Exception e) { continue; }
            long pv = p & 0xffffffffL;
            if (pv < CODE_LO || pv >= CODE_HI) continue;
            Address t = toAddr(pv);
            if (fm.getFunctionAt(t) != null) { entry++; continue; }
            if (fm.getFunctionContaining(t) != null) { mid++; continue; }
            if (listing.getInstructionAt(t) != null) { dis++; continue; }
            targets.add(pv);
            if (ptrSites != null) ptrSites.add(a);
        }
        println(String.format(
            "%s: entry %4d, mid-function %3d, disassembled-orphan %3d, "
            + "UNDISASSEMBLED %3d", name, entry, mid, dis, targets.size()));
        return targets;
    }

    public void run() throws Exception {
        FunctionManager fm = currentProgram.getFunctionManager();
        Listing listing = currentProgram.getListing();

        List<Long> sites = new ArrayList<Long>();
        Set<Long> rdata = sweep(SECTIONS[0][0], SECTIONS[0][1],
                                ".rdata", null);
        Set<Long> data = sweep(SECTIONS[1][0], SECTIONS[1][1],
                               ".data ", sites);

        println("");
        println(".data targets and the pointer that names them:");
        int i = 0;
        for (Long t : data) {
            String at = (i < sites.size())
                ? toAddr(sites.get(i)).toString() : "?";
            println("   ptr at " + at + " -> " + toAddr(t));
            i++;
        }

        if (DRY_RUN) {
            println("");
            println("DRY_RUN - nothing changed.");
            return;
        }

        Set<Long> todo = new LinkedHashSet<Long>(data);
        if (INCLUDE_RDATA) todo.addAll(rdata);

        int made = 0, fail = 0;
        for (Long pv : todo) {
            Address a = toAddr(pv);
            if (fm.getFunctionAt(a) != null) continue;
            try {
                if (listing.getInstructionAt(a) == null) disassemble(a);
                Function f = createFunction(a, null);
                if (f == null) { println("   FAIL " + a); fail++; }
                else {
                    println("   defined " + a + "  body "
                            + f.getBody().getNumAddresses());
                    made++;
                }
            } catch (Exception e) {
                println("   FAIL " + a + ": " + e.getMessage());
                fail++;
            }
        }
        println("");
        println("defined " + made + ", failed " + fail);
        println("");
        println("NOW RUN ghidra_define_orphan_code.java, then");
        println("ghidra_fix_split_functions.java, and repeat until both");
        println("report nothing. Defining here creates work for both.");
    }
}
