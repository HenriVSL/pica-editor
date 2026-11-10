// fif_int.h - private to fif.cpp and its tests (T11).
#ifndef PICA_FIF_INT_H
#define PICA_FIF_INT_H

#include "etypes.h"

enum { FIF_MAX_HITS_DEFAULT = 50000, FIF_TEXT_CHARS = 256, FIF_BINARY_SAMPLE = 4096 };
// Tests: files above mem_limit bytes are searched through a block cache of 4 x block bytes instead of being read
// whole (0 = defaults: 16 MB, 256 KB).
void fif_test_limits(u32 mem_limit, u32 block);
// "!\name" / "!/name" tokens in the masks exclude folders (DOS wildcards, case-insensitive).
int  fif_dir_excluded(const char* masks, const char* name);

#endif
