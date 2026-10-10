// ghidra_restore_original_bytes.java
//
// The MOTO.EXE this project analyses is modified at seven sites: five
// defeat the CD copy protection, two relax a 16-bit colour-depth gate.
// See docs/binary-modifications.txt for the byte-diff that established
// them against decomp/ORIGINAL_MOTO.EXE.
//
// This script writes the ORIGINAL bytes back into the Ghidra database so
// the decompiler shows the game's real logic instead of the patched
// logic. It does NOT touch MOTO.EXE on disk, so tools/diff_original.py
// keeps working.
//
// Run it after any reimport of MOTO.EXE, alongside the other repair
// scripts listed in docs/ghidra-db-repairs.txt.
//
// It is idempotent: sites already holding the original bytes are
// reported as "already original" and left alone.
//
// @category MotoRacer

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.mem.Memory;

public class ghidra_restore_original_bytes extends GhidraScript {

    // patchAddr, originalBytes, crackedBytes, insnStart, clearEnd, label
    private static final Object[][] SITES = {
        {0x00414331L, new byte[]{0x75, 0x0c},
         new byte[]{(byte) 0x90, (byte) 0x90},
         0x00414331L, 0x00414333L, "1  PId message box"},
        {0x0047e8dbL, new byte[]{0x0f, (byte) 0x84, (byte) 0x99, 0, 0, 0},
         new byte[]{(byte) 0xe9, (byte) 0x9a, 0, 0, 0, (byte) 0x90},
         0x0047e8dbL, 0x0047e97aL, "2  ILLEGAL VERSION overlay"},
        {0x00442f44L, new byte[]{0x74, (byte) 0x91},
         new byte[]{(byte) 0x90, (byte) 0x90},
         0x00442f44L, 0x00442f46L, "3  CD-present local"},
        {0x00444e73L, new byte[]{0x74, 0x5a},
         new byte[]{(byte) 0x90, (byte) 0x90},
         0x00444e73L, 0x00444e75L, "4  CD-audio playback"},
        {0x00442785L, new byte[]{0x33, (byte) 0xc0},
         new byte[]{(byte) 0xeb, (byte) 0xf4},
         0x00442785L, 0x0044278dL, "5  disc-signature verdict"},
        {0x0045a7caL, new byte[]{(byte) 0x84}, new byte[]{(byte) 0x8d},
         0x0045a7c9L, 0x0045a7cfL, "6  colour depth, bpp == 16"},
        {0x0047642dL, new byte[]{0x74}, new byte[]{0x7d},
         0x0047642dL, 0x0047642fL, "7  colour depth, bpp == 16"},
    };

    private boolean matches(Memory mem, long addr, byte[] want)
            throws Exception {
        byte[] got = new byte[want.length];
        mem.getBytes(toAddr(addr), got);
        for (int i = 0; i < want.length; i++) {
            if (got[i] != want[i]) return false;
        }
        return true;
    }

    public void run() throws Exception {
        Memory mem = currentProgram.getMemory();
        int restored = 0, already = 0, unknown = 0;

        for (Object[] s : SITES) {
            long pa = (Long) s[0];
            byte[] orig = (byte[]) s[1];
            byte[] crak = (byte[]) s[2];
            long ins = (Long) s[3];
            long end = (Long) s[4];
            String tag = (String) s[5];

            if (matches(mem, pa, orig)) {
                println("  site " + tag + " : already original");
                already++;
                continue;
            }
            if (!matches(mem, pa, crak)) {
                println("  site " + tag + " : UNEXPECTED BYTES - skipped");
                unknown++;
                continue;
            }

            clearListing(toAddr(ins), toAddr(end));
            mem.setBytes(toAddr(pa), orig);
            disassemble(toAddr(ins));

            Instruction i = currentProgram.getListing()
                    .getInstructionAt(toAddr(ins));
            println("  site " + tag + " : restored -> "
                    + (i == null ? "<disassembly failed>" : i.toString()));
            restored++;
        }

        // Site 2 uncovers 153 bytes that belong to FUN_0047e3d0 but are
        // orphaned while the jump is unconditional.
        if (currentProgram.getListing()
                .getInstructionAt(toAddr(0x0047e8e1L)) == null) {
            disassemble(toAddr(0x0047e8e1L));
        }
        Function f = getFunctionContaining(toAddr(0x0047e8dbL));
        if (f != null && !f.getBody().contains(toAddr(0x0047e8e1L))) {
            AddressSet body = new AddressSet(f.getBody());
            body.add(toAddr(0x0047e8e1L), toAddr(0x0047e979L));
            f.setBody(body);
            println("  extended " + f.getName()
                    + " to cover the overlay block");
        }

        println("");
        println("  restored " + restored + ", already original " + already
                + ", unexpected " + unknown);
        println("  MOTO.EXE on disk is untouched;");
        println("  diff_original.py still applies.");
    }
}
