// ptree_int.h - private test hooks of the piece tree (T16). Tests only.
#ifndef PICA_PTREE_INT_H
#define PICA_PTREE_INT_H

#include "etypes.h"

void pt_line_cache_enable(int on);   // 0 = every line query scans its piece (timing comparisons); default 1

#endif
