// ghidra_fix_case_functions.java
//
// Repairs the fourth Ghidra function-boundary defect: switch CASE BODIES
// promoted to functions. See docs/ghidra-db-repairs.txt section 10.
//
// Ghidra labels computed-jump targets caseD_NN. When it also makes them
// functions, the case bodies are amputated from the function that owns
// them, which then decompiles truncated. MenuPage12_ControllerConfig_Tick
// was showing 468 bytes of a 2230-byte function this way.
//
// Detection needs no heuristic. A block is a case body, not a function,
// when all three hold:
//     - the name starts with "caseD_"
//     - no reference to it is a CALL
//     - the COMPUTED_JUMP that reaches it comes from inside another
//       function, which is the parent it belongs to
//
// Repair: delete the case functions, then set the parent's body to the
// union of its own body and theirs.
//
// Run it after ghidra_define_orphan_code.java and
// ghidra_fix_split_functions.java, and repeat all three until quiet.
// Then re-run ghidra_commit_signatures.java, because the repaired
// parents need their prototypes recomputed from the full body.
//
// Usage: Ghidra Script Manager, or headless analyzeHeadless -postScript.
//
//@category MotoRacer

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import ghidra.program.model.symbol.ReferenceManager;

import java.util.ArrayList;
import java.util.HashMap;

public class ghidra_fix_case_functions extends GhidraScript {

    // Game code only. At and above this the image is MFC/CRT, where
    // switch-heavy library code produces caseD_ labels legitimately.
    private static final long GAME_CODE_END = 0x004b0000L;

    @Override
    public void run() throws Exception {
        FunctionManager fm = currentProgram.getFunctionManager();
        ReferenceManager rm = currentProgram.getReferenceManager();

        ArrayList<Function> candidates = new ArrayList<>();
        for (Function f : fm.getFunctions(true)) {
            if (f.getEntryPoint().getOffset() >= GAME_CODE_END) continue;
            if (f.getName().startsWith("caseD_")) candidates.add(f);
        }
        println("caseD_ functions in game code: " + candidates.size());

        HashMap<Address, ArrayList<Function>> byParent = new HashMap<>();
        int skipped = 0;

        for (Function f : candidates) {
            boolean called = false;
            Function parent = null;
            ReferenceIterator ri = rm.getReferencesTo(f.getEntryPoint());
            while (ri.hasNext()) {
                Reference r = ri.next();
                if (r.getReferenceType().isCall()) called = true;
                if (r.getReferenceType().isJump())
                    parent = fm.getFunctionContaining(r.getFromAddress());
            }
            if (called || parent == null) {
                println("  SKIP " + f.getEntryPoint() + (called
                        ? " - has a CALL reference"
                        : " - no containing parent"));
                skipped++;
                continue;
            }
            byParent.computeIfAbsent(parent.getEntryPoint(),
                    k -> new ArrayList<>()).add(f);
        }

        int absorbed = 0;
        for (Address pa : byParent.keySet()) {
            Function parent = fm.getFunctionAt(pa);
            ArrayList<Function> kids = byParent.get(pa);
            long before = parent.getBody().getNumAddresses();

            AddressSet union = new AddressSet(parent.getBody());
            for (Function k : kids) union.add(k.getBody());
            for (Function k : kids) fm.removeFunction(k.getEntryPoint());
            parent.setBody(union);

            println("  " + parent.getName() + " @" + pa + ": " + before
                    + " -> " + parent.getBody().getNumAddresses()
                    + " bytes, absorbed " + kids.size() + " case blocks");
            absorbed += kids.size();
        }

        // A case body can come back if an analyzer reads its jump-table
        // entry as a function pointer. caseD_200 at 0x00433637 does; see
        // docs/ghidra-db-repairs.txt section 10.3. Report, do not loop.
        int remaining = 0;
        for (Function f : fm.getFunctions(true)) {
            if (f.getEntryPoint().getOffset() >= GAME_CODE_END) continue;
            if (f.getName().startsWith("caseD_")) {
                println("  REGENERATED: " + f.getEntryPoint() + " "
                        + f.getName() + " - parent stays split, see"
                        + " ghidra-db-repairs.txt 10.3");
                remaining++;
            }
        }

        println("absorbed=" + absorbed + " skipped=" + skipped
                + " remaining=" + remaining);
    }
}
