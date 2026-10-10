// ghidra_grep_comments.java
//
// Search every comment in the database for a substring: function plate
// comments, and the pre / EOL / post / plate comments on any address.
//
// The Ghidra comments carry findings that never reached docs/, so this
// is the companion to grepping docs/ before assigning a name. A global's
// identity is often already written in the plate comment of a function
// nobody has read yet - maybe_g_spanShade was renamed three times, once
// in the wrong direction, while the answer sat in
// maybe_Screen3D_SpanShadedTex's comment the whole time.
//
// Run it with the search term as the script argument, or edit TERM
// below. The match is case-insensitive.
//
//   tools/ghidra_grep_comments.java   006d1f80
//   tools/ghidra_grep_comments.java   "shade"
//
// @category MotoRacer

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressIterator;
import ghidra.program.model.listing.CodeUnit;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.listing.Listing;

public class ghidra_grep_comments extends GhidraScript {

    // Used when no argument is supplied.
    private static final String TERM = "";

    private String term;

    private boolean hit(String s) {
        return s != null && s.toLowerCase().contains(term);
    }

    private void show(String where, String what, String body) {
        // Print only the matching lines, with a little context.
        String[] lines = body.split("\n");
        for (int i = 0; i < lines.length; i++) {
            if (!hit(lines[i])) continue;
            println("  " + where + "  " + what);
            if (i > 0) println("        " + lines[i - 1].trim());
            println("     >  " + lines[i].trim());
            if (i + 1 < lines.length) println("        " + lines[i + 1].trim());
            println("");
            return;   // one hit per comment is enough to find it
        }
    }

    public void run() throws Exception {
        String[] args = getScriptArgs();
        term = (args.length > 0 ? args[0] : TERM).toLowerCase();
        if (term.isEmpty()) {
            println("give a search term as the script argument");
            return;
        }
        println("searching all comments for \"" + term + "\"");
        println("");

        int hits = 0;
        Listing l = currentProgram.getListing();

        FunctionIterator fi = l.getFunctions(true);
        while (fi.hasNext()) {
            Function f = fi.next();
            String c = f.getComment();
            if (hit(c)) {
                show(f.getEntryPoint().toString(), f.getName(), c);
                hits++;
            }
        }

        int[] types = {CodeUnit.PLATE_COMMENT, CodeUnit.PRE_COMMENT,
                       CodeUnit.EOL_COMMENT, CodeUnit.POST_COMMENT};
        String[] names = {"plate", "pre", "eol", "post"};
        for (int t = 0; t < types.length; t++) {
            AddressIterator ai =
                    l.getCommentAddressIterator(types[t],
                            currentProgram.getMemory(), true);
            while (ai.hasNext()) {
                Address a = ai.next();
                String c = l.getComment(types[t], a);
                if (!hit(c)) continue;
                Function f = getFunctionContaining(a);
                show(a.toString(),
                     names[t] + (f == null ? "" : " in " + f.getName()), c);
                hits++;
            }
        }

        println("total: " + hits + " comment(s) mention it");
    }
}
