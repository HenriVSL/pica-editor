// regex.cpp - regex parser and program compiler (DESIGN 6.14, 11.1). STUB from T00. Owner after Wave 0: T03.
// rxvm.cpp holds RxMatcher (Pike VM), rxbt.cpp the backtracker, rxpre.cpp the literal prefilter; they share
// declarations through a private header src/core/regex_*.h.
#include "regex.h"
#include "estr.h"

int rx_compile(const ecp* pat, u32 n, u32 flags, RxProg** out, RxError* err)
{
  (void)pat; (void)n; (void)flags;
  if (out) *out = 0;
  if (err) { err->code = E_NOTIMPL; err->pos = 0; es_copy(err->msg, sizeof(err->msg), "Regular expressions are not implemented yet"); }
  return E_NOTIMPL;
}
void rx_free(RxProg* p) { (void)p; }
u32 rx_group_count(const RxProg* p) { (void)p; return 0; }
int rx_uses_backtracker(const RxProg* p) { (void)p; return 0; }
int rx_literal_prefix(const RxProg* p, Vec<ecp>* lit, int* icase) { (void)p; if (lit) lit->clear(); if (icase) *icase = 0; return E_NOTIMPL; }
