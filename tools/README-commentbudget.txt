MotoRacer PC Reverse Engineering                           Tool Note


                        THE COMMENT BUDGET


Abstract

   CLAUDE.md rule 16 says what a src/ comment has to earn.  It does
   not say HOW MUCH, and that turns out to be the half that gets
   broken: a file already dense with comment reads as licence to add
   more, so each change arrives with a larger comment-to-code ratio
   than the last.  race.c reached 55 per cent comment that way.

   This measures the one thing rule 16 leaves unmeasured.

Table of Contents

   1. Running it
   2. What it counts
   3. The one-to-one rule
   4. The reduction gate
   5. What it cannot see

1.  Running it

       python tools/commentbudget.py            the working tree
       python tools/commentbudget.py --reduce   a reduction pass
       python tools/commentbudget.py --blocks   oversized blocks

   The first form reads `git diff -U0 -- src` and reports, per file
   and in total, the NET comment and code lines - added minus
   removed.  It exits non-zero when net comment exceeds net code.

   NET, NOT ADDED, and the difference matters: replacing a fourteen
   line comment with a six line one ADDS six, which read as a
   regression under the first version of this tool and punished
   exactly the edit it is meant to encourage.  A change that deletes
   more comment than it writes is a reduction and reports as one.

   The second form walks src/ and lists every run of comment lines
   longer than twelve.  It is ADVISORY and always exits zero: the
   tree is full of them already and failing on that would make the
   tool useless on day one.

2.  What it counts

   A line is comment if it starts with "/*", "*" or "//", or ends
   with "*/".  Blank lines count as neither.  That misreads a string
   containing "//" and a continuation line inside a block that
   happens to start with a letter, both rare enough not to matter.

   Only .c, .h and .cpp under src/ are counted.  docs/ and tools/
   are not - they are supposed to be prose.

3.  The one-to-one rule

   Net comment may not exceed net code.

   It is arbitrary and that is the point: it is a number, so it
   cannot be argued with the way "is this comment earning its place"
   can.  Rule 16 decides WHICH comments; this decides how many.

   The escape is the one rule 16 already names - a long explanation
   goes to docs/ and leaves a one-line pointer.  Hitting the budget
   is the signal to do that, not a reason to delete something true.

   o ONE EXEMPTION, a judgement call to state out loud in the change
     rather than to encode here: a new file's header, which is
     prose by nature.

   o "A PURE COMMENT SWEEP ADDS NONE" WAS ALSO LISTED HERE AND IT IS
     NOT TRUE. A sweep corrects comments, and a correction is usually
     longer than the wrong line it replaces - five swept files grew
     under this gate. Section 4 is the gate for a sweep.

4.  The reduction gate

   --reduce requires the net comment to be NEGATIVE and ignores the
   code delta entirely.

   THE DEFAULT GATE PERMITS GROWTH ON PURPOSE - net comment may not
   exceed net CODE, so a change that adds code may explain it. That
   is the wrong test for a pass whose only purpose is to shrink the
   comment, and it is why menu.c came out 103 lines longer, 
   racerdraw.c 71 and physics.c 70 while all three were being swept.

   A FILE WHOSE COMMENTS THE DIFF DOES NOT TOUCH IS NEUTRAL, so a
   reduction pass may still carry an unrelated code fix. Only a file
   with comment churn and a net of zero or more is flagged GREW.

   Use it for the second pass over a file section 3 already let
   through, and the ordinary gate for everything else.


5.  What it cannot see

   o A COMMENT THAT IS SIMPLY WRONG.  The budget is blind to truth;
     a stale comment passes it perfectly.  That is what the sweep
     and docs/comment-defects.txt are for.

   o THE SAME FACT IN TWO FILES.  Rule 14.1's one-fact-one-home is
     not a count and nothing here checks it.  Four files restating
     one fact can each sit under budget.

   o WHETHER THE CODE EARNED ITS LINES EITHER.  A change that pads
     the code passes by padding.

   o A PURE DELETION OF CODE.  Removing fifty lines of code and
     their twenty of comment nets -20 against -50 and passes, which
     is right, but so does removing fifty lines of code and adding a
     comment about having done so.
