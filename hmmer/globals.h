/************************************************************
 * @LICENSE@
 ************************************************************/

/* globals.h
 * Mon Nov 18 13:05:03 1996
 * 
 * Global variable definitions. 
 * This file may only be included in a main() .c file.
 * 
 * Alphabet[] is usually treated as an array, but may be treated as
 * strings w/ strchr() calls to find index of a particular character,
 * so they must be null terminated.  Hence the +1.
 * [bug #h25. xref STL7 p121] 
 */

char  hmmer_Alphabet[MAXCODE+1]; /* ACGT, for instance                    */ 
int   Alphabet_type;       /* hmmNUCLEIC or hmmAMINO                */
int   Alphabet_size;       /* uniq alphabet size: 4 or 20           */
int   Alphabet_iupac;      /* total size of alphabet + IUPAC degen. */
char  hmmer_Degenerate[MAXCODE][HMMERMAXABET]; /* 1/0 arrays, for whether IUPAC code includes a residue */
int   hmmer_DegenCount[MAXCODE];
