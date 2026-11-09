// regex.h - regex compiler and matcher (DESIGN 6.14). Owner after Wave 0: T03.
#ifndef PICA_REGEX_H
#define PICA_REGEX_H

#include "etypes.h"
#include "emem.h"
#include "esource.h"
#include "codec.h"

enum RxFlag { RXF_ICASE = 1, RXF_DOTALL = 2, RXF_WORD = 4 };
enum { RX_MAX_PATTERN = 1000, RX_MAX_PROG = 4000, RX_MAX_REPEAT = 1000, RX_GROUPS = 10 };
struct RxError { int code; u32 pos; char msg[96]; };
struct RxMatch { epos pos; epos len; epos gpos[RX_GROUPS]; epos glen[RX_GROUPS]; };   // gpos EPOS_NONE = unset
class RxProg;
int  rx_compile(const ecp* pat, u32 n, u32 flags, RxProg** out, RxError* err);
void rx_free(RxProg* p);
u32  rx_group_count(const RxProg* p);
int  rx_uses_backtracker(const RxProg* p);
int  rx_literal_prefix(const RxProg* p, Vec<ecp>* lit, int* icase);   // required literal at the start (may be empty)
class RxMatcher {                      // per-thread matching state
public:
  RxMatcher();  ~RxMatcher();
  int  init(const RxProg* p);
  void set_budget(u32 steps);         // backtracker step limit per search call (default 10,000,000)
  // Leftmost-first match starting in [from, hi], ending <= hi. lo <= from is the context floor for ^ \b \<.
  // Returns 1 found, 0 none, E_BUDGET, E_CANCEL, E_IO, E_NOMEM.
  int  search(ByteSource* s, const Codec* c, epos lo, epos from, epos hi, RxMatch* m, ProgressCb* cb);
private:                               // private state: T03 may change freely
  const RxProg* prog_; u32 budget_;
  RxMatcher(const RxMatcher&); RxMatcher& operator=(const RxMatcher&);
};

#endif
