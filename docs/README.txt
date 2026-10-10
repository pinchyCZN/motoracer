MotoRacer PC Reverse Engineering                           Analysis Note


                                 DOCS/


Abstract

   Everything we discover about MotoRacer lives here. Baseline facts
   about the executable (segments, counts, imports, the recovered
   original source tree) are in the root CLAUDE.md - this directory
   holds the detail that grows as work proceeds.

Table of Contents

   1. Layout
   2. Rules


1.  Layout

   +--------------+-------------------------------------------------------------------------+
   | Path         | Holds                                                                   |
   +==============+=========================================================================+
   | structs/     | One file per reconstructed struct. Copy structs/_TEMPLATE.txt to start. |
   +--------------+-------------------------------------------------------------------------+
   | subsystems/  | One file per subsystem: renderer, audio, input, network, menu, physics. |
   +--------------+-------------------------------------------------------------------------+
   | formats/     | On-disk file formats once we start pulling apart game data.             |
   +--------------+-------------------------------------------------------------------------+
   | glossary.txt | Naming decisions, abbreviations, and what the original prefixes mean.   |
   +--------------+-------------------------------------------------------------------------+
   | releases.txt | Which build we analyse, and how our copy differs from retail.           |
   +--------------+-------------------------------------------------------------------------+

2.  Rules

   Tag every claim. [verified] (read out of the binary or observed
   running), [observed] (seen in a recording of a retail build - see
   below), [inferred] (reasoned from stated evidence), [guess]
   (untested hypothesis).

   Addresses are absolute and written 0x004xxxxx, matching the Ghidra
   image base of 0x00400000. Always give the address alongside a symbol
   name - names change, addresses don't.

   Every document ends with an Open questions section. A recorded gap is
   worth more than a confident guess, and it is where the next session
   starts.

   Write it the session you find it. A discovery that only exists in a
   chat transcript is gone.
