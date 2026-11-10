// doc_int.h - private test hooks of the Document (T16). Tests only.
#ifndef PICA_DOC_INT_H
#define PICA_DOC_INT_H

#include "codec.h"

// EOL counts over [a, b) of s. mode 0: the memchr paths (known_b0 = 0x0A/0x0D with known_n = the number of those
// units in [a, b) skips that pass; 0 = none), mode 1: the reference per-unit loop.
int doc_count_eol_test(ByteSource* s, epos a, epos b, u32 unit, int be, int mode, u8 known_b0, u32 known_n, EolCounts* c);

#endif
