// ghidra_assert_map.java
//
// Extracts the original SOURCE MAP that the assert macros leaked.
//
// Thirteen paths under D:\Dsed\mr320\ survive in .rdata, and each is
// referenced from one or more assert sites. Every site pushes a LINE
// NUMBER next to the path, so each reference yields a triple:
//
//     original file  +  line number  +  the function it sits in
//
// That is worth more than the paths alone. It attributes functions to
// translation units, and the line numbers ORDER them within a file, so
// the reconstruction can lay a file out in something close to its
// original sequence instead of by address.
//
// The call shape is cdecl sprintf with arguments pushed right to left:
//
//     PUSH line          <- the line number, immediately before
//     PUSH path          <- the reference this script finds
//     PUSH message
//     PUSH format
//     PUSH buffer
//     CALL sprintf
//
// So the line is the nearest preceding PUSH of a scalar and the message
// is the nearest following PUSH of a string pointer. Both are searched
// in a small window; a site that does not match the shape reports its
// line as -1 rather than guessing.
//
// Reports only; it does not modify the program. Output is sorted by
// file then line, which is the order the original source had.
//
// Re-run after any reimport - the results feed docs/source-map.txt.
//
// @category MotoRacer

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.DataIterator;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.scalar.Scalar;
import ghidra.program.model.symbol.Reference;

import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

public class ghidra_assert_map extends GhidraScript {

    private static final String ROOT = "D:\\Dsed\\mr320\\";

    private String stringAt(Address a) {
        Data d = getDataAt(a);
        if (d != null && d.getValue() instanceof String) {
            return (String) d.getValue();
        }
        return null;
    }

    public void run() throws Exception {
        Map<Address, String> paths = new HashMap<>();
        DataIterator di = currentProgram.getListing().getDefinedData(true);
        while (di.hasNext()) {
            Data d = di.next();
            Object v = d.getValue();
            if (v instanceof String && ((String) v).contains("Dsed")) {
                paths.put(d.getAddress(), (String) v);
            }
        }

        List<String> rows = new ArrayList<>();
        int unmatched = 0;

        for (Map.Entry<Address, String> e : paths.entrySet()) {
            for (Reference r : getReferencesTo(e.getKey())) {
                Instruction i = getInstructionAt(r.getFromAddress());
                if (i == null) continue;
                Function f = getFunctionContaining(r.getFromAddress());

                long line = -1;
                Instruction p = i.getPrevious();
                for (int k = 0; k < 4 && p != null; k++, p = p.getPrevious()) {
                    if (!p.getMnemonicString().equals("PUSH")) continue;
                    Object[] o = p.getOpObjects(0);
                    if (o.length == 1 && o[0] instanceof Scalar) {
                        line = ((Scalar) o[0]).getUnsignedValue();
                        break;
                    }
                }
                if (line < 0) unmatched++;

                String msg = "";
                Instruction n = i.getNext();
                for (int k = 0; k < 5 && n != null; k++, n = n.getNext()) {
                    if (!n.getMnemonicString().equals("PUSH")) continue;
                    Object[] o = n.getOpObjects(0);
                    if (o.length == 1 && o[0] instanceof Scalar) {
                        String s = stringAt(
                                toAddr(((Scalar) o[0]).getUnsignedValue()));
                        if (s != null) { msg = s; break; }
                    }
                }

                rows.add(String.format("%-30s|%6d|%-32s|%s|%s",
                        e.getValue().replace(ROOT, ""), line,
                        f == null ? "?" : f.getName(),
                        r.getFromAddress(), msg.replace("\n", "")));
            }
        }

        Collections.sort(rows);
        for (String s : rows) println(s);
        println("");
        println("sites: " + rows.size() + "   files: " + paths.size()
                + "   line not recovered: " + unmatched);
    }
}
