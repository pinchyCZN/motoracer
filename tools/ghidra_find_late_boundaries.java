// ghidra_find_late_boundaries.java
//
// Finds functions whose recorded entry point is LATER than the real one.
//
// A function reached only through a pointer - a window procedure, a
// virtual method in a vtable, an entry in a dispatch table - has no CALL
// reference. Ghidra therefore never disassembles its prologue, and if
// some other pass defines the tail as a function, the result is a
// function that starts several bytes into itself. The decompiler then
// invents phantom inputs and loses the calling convention.
//
// This is a DIFFERENT defect from the three in docs/ghidra-db-repairs.txt:
// the tail here is already a well-formed function, so it does not look
// like orphaned code and the existing repair scripts do not flag it.
//
// The detector: a DATA reference into .text whose target is not inside
// any function, is not disassembled, and whose bytes look like an x86
// prologue. Requiring the prologue shape is what removes the noise -
// without it the results are dominated by MSVC switch jump tables.
//
// Reports only; it does not modify the program. Repair by hand:
// remove the late function, disassemble at the real entry, recreate,
// then restore the name, calling convention and comment.
//
// Known finds (all repaired):
//   0x00432980  the window procedure, recorded at 0x004329a5  (-0x25)
//   0x00414080  CWinApp::InitInstance,  recorded at 0x004140a2 (-0x22)
//   0x004a31d0  a screen3d span routine, recorded at 0x004a31db (-11)
//
// @category MotoRacer

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;

import java.util.HashSet;
import java.util.Set;

public class ghidra_find_late_boundaries extends GhidraScript {

    private static final long TEXT_LO = 0x00401000L;
    private static final long TEXT_HI = 0x004cf5ffL;

    private boolean looksLikePrologue(Memory m, Address a) {
        try {
            int b0 = m.getByte(a) & 0xff;
            int b1 = m.getByte(a.add(1)) & 0xff;
            int b2 = m.getByte(a.add(2)) & 0xff;
            // push ebp; mov ebp,esp
            if (b0 == 0x55 && b1 == 0x8b && b2 == 0xec) return true;
            // mov eax,fs:[0]
            if (b0 == 0x64 && b1 == 0xa1) return true;
            // sub esp,imm8 / imm32
            if (b0 == 0x83 && b1 == 0xec) return true;
            if (b0 == 0x81 && b1 == 0xec) return true;
            // push -1
            if (b0 == 0x6a && b1 == 0xff) return true;
            // push ebx,esi,edi
            if (b0 == 0x53 && b1 == 0x56 && b2 == 0x57) return true;
        } catch (Exception e) {
            return false;
        }
        return false;
    }

    public void run() throws Exception {
        Memory mem = currentProgram.getMemory();
        ReferenceIterator it = currentProgram.getReferenceManager()
                .getReferenceIterator(toAddr(TEXT_LO));
        Set<Long> seen = new HashSet<>();
        int hits = 0;

        println("Late-boundary suspects: data-referenced,");
        println("undisassembled, prologue-shaped.");
        println("");

        while (it.hasNext()) {
            Reference r = it.next();
            Address to = r.getToAddress();
            long t = to.getOffset();
            if (t < TEXT_LO || t > TEXT_HI) continue;
            if (!r.getReferenceType().isData()) continue;
            if (getFunctionAt(to) != null) continue;
            if (getFunctionContaining(to) != null) continue;
            if (currentProgram.getListing()
                    .getInstructionAt(to) != null) continue;
            if (!seen.add(t)) continue;
            if (!looksLikePrologue(mem, to)) continue;

            // find the function that starts shortly after, if any
            Function late = null;
            for (long p = t + 1; p < t + 64 && p <= TEXT_HI; p++) {
                Function cand = getFunctionAt(toAddr(p));
                if (cand != null) { late = cand; break; }
            }

            StringBuilder by = new StringBuilder();
            for (int i = 0; i < 8; i++) {
                by.append(String.format("%02x ", mem.getByte(to.add(i))));
            }
            Function from = getFunctionContaining(r.getFromAddress());

            println("  real entry " + to + "   " + by);
            println("      referenced from " + r.getFromAddress()
                    + (from == null ? " (not in a function)"
                                    : " in " + from.getName()));
            if (late != null) {
                println("      RECORDED AS " + late.getName() + " @"
                        + late.getEntryPoint() + "  ("
                        + (late.getEntryPoint().getOffset() - t)
                        + " bytes late)");
            } else {
                println("      no function nearby - may be a stub");
            }
            println("");
            hits++;
        }

        println("total: " + hits);
        if (hits == 0) {
            println("clean - no late boundaries of this shape remain.");
        }
    }
}
