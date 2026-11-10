// regex_int.h - private declarations shared by regex.cpp (parser, compiler), rxvm.cpp (Pike VM), rxbt.cpp
// (backtracker), rxpre.cpp (prefilter) and search.cpp. Owner: T03. Not for use outside the regex/search modules.
#ifndef PICA_REGEX_INT_H
#define PICA_REGEX_INT_H

#include "regex.h"
#include "ucd.h"

// Internal rx_compile flags (never set by callers outside regex/search and their tests).
enum {
  RXF_LITERAL_ = 0x100,      // the pattern is a literal string (Normal / Extended search modes)
  RXF_FORCEBT_ = 0x200       // always use the backtracker (tests compare it with the Pike VM)
};

enum { RX_NONE = 0xFFFFFFFFu, RX_INF = 0xFFFFFFFFu, RX_PREFIX_MAX = 64, RX_MAX_DEPTH = 100 };

enum RxOp {
  // consuming
  RXO_CHAR = 0, RXO_CHARI, RXO_ANY, RXO_ANYNL, RXO_CLASS,
  // control
  RXO_MATCH, RXO_JMP, RXO_SPLIT, RXO_SAVE,
  // zero-width assertions (rx_assert)
  RXO_BOL, RXO_EOL, RXO_BOT, RXO_EOT, RXO_WORDB, RXO_NWORDB, RXO_WBEG, RXO_WEND, RXO_NWPREV, RXO_NWNEXT,
  // backtracker only (the Pike VM never sees BACKREF/LOOK; MARK/CHKPROG are no-ops there)
  RXO_BACKREF, RXO_LOOK, RXO_LOOKEND, RXO_MARK, RXO_CHKPROG
};

// CHAR x=char; CHARI x=folded char; CLASS x=class index; JMP x; SPLIT x (preferred) y; SAVE x=slot;
// BACKREF x=group arg=icase; LOOK arg=negative x=continuation (body at pc+1 ends with LOOKEND);
// MARK/CHKPROG x=loop slot (empty-iteration check for nullable loop bodies).
struct RxInst { u8 op; u8 arg; u16 pad; u32 x; u32 y; };

enum RxProp {
  RXP_WORD = 1, RXP_NWORD = 2, RXP_DIGIT = 4, RXP_NDIGIT = 8, RXP_SPACE = 16, RXP_NSPACE = 32,
  RXP_ALPHA = 64, RXP_UPPER = 128, RXP_LOWER = 256, RXP_ALNUM = 512, RXP_PUNCT = 1024, RXP_XDIGIT = 2048,
  RXP_BLANK = 4096, RXP_CNTRL = 8192, RXP_PRINT = 16384, RXP_GRAPH = 32768
};

// bits: final membership (negation and case folding applied) for chars < 256. Chars >= 256 (and bad markers)
// go through rx_class_slow: ranges (pairs in RxProg::ranges) + props, case variants, then negate.
struct RxClass { u32 bits[8]; u32 rstart; u32 rcount; u32 props; u8 negate; u8 icase; u8 pad[2]; };

class RxProg {
public:
  RxProg();
  Vec<RxInst> code;
  Vec<RxClass> classes;
  Vec<u32> ranges;           // lo, hi pairs (inclusive), sorted and merged per class
  Vec<ecp> prefix;           // required literal at the start of every match (folded when prefix_icase)
  u32 flags;                 // RXF_* given to rx_compile
  u32 ngroups;               // capturing groups 1..ngroups (<= 9)
  u32 ncap;                  // capture slots: 2 * (ngroups + 1)
  u32 nloops;                // MARK/CHKPROG slots
  u32 first[8];              // chars < 256 that can start a match (valid when first_ok)
  u8  first_hi;              // a char >= 256 (or a bad marker) may start a match
  u8  first_ok;              // the first-char set is usable (pattern not nullable, no lookahead/backref first)
  u8  use_bt;                // backreferences or lookahead: backtracker
  u8  anchored;              // every match starts at the context floor (\A)
  u8  prefix_icase;          // prefix compares case-insensitively
  u8  pure;                  // the whole pattern is exactly the prefix (no groups, no assertions)
  u8  line_local;            // no match contains CR or LF
  u8  req_icase;
  u8  bol;                   // every match starts at a line start (^ or \A first on every path)
  u8  pad2[3];
  Vec<ecp> req;              // literal inside every match (when there is no prefix), folded if req_icase
};

// ---- character helpers ----
inline ecp rx_fold(ecp c)
{
  if (c < 128) return (c >= 'A' && c <= 'Z') ? c + 32 : c;
  if (ECP_IS_BAD(c)) return c;
  return uc_fold(c);
}
inline int rx_is_word(ecp c)
{
  if (c < 128) return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
  if (ECP_IS_BAD(c)) return 0;
  return uc_is_word(c);
}
int rx_class_slow(const RxProg* p, const RxClass* k, ecp c);
inline int rx_class_has(const RxProg* p, u32 idx, ecp c)
{
  const RxClass* k = &p->classes[idx];
  if (c < 256) return (int)((k->bits[c >> 5] >> (c & 31)) & 1u);
  return rx_class_slow(p, k, c);
}
inline int rx_first_has(const RxProg* p, ecp c)
{
  if (c < 256) return (int)((p->first[c >> 5] >> (c & 31)) & 1u);
  return p->first_hi;
}
// Does a consuming instruction accept c? (fc = rx_fold(c))
int rx_accepts(const RxProg* p, const RxInst* in, ecp c, ecp fc);

// Assertion context at a position: the character before (if any) and the character at it (if any).
struct RxAt { ecp prev; ecp next; u8 has_prev; u8 has_next; u8 pad[2]; };
int rx_assert(u32 op, const RxAt* a);
inline int rx_is_assert(u32 op) { return op >= RXO_BOL && op <= RXO_NWNEXT; }

// ---- reading characters from a ByteSource ----
struct RxReader {
  ByteSource* s; const Codec* c; epos size;
  const u8* chunk; epos cbeg, cend;   // cached chunk from s->get(); invalidated by rxr_reset
  int err;                            // first I/O error
  u8 fast;                            // codec is ASCII compatible: bytes < 0x80 decode to themselves
  u8 unit;
  u8 pad[2];
};
void rxr_init(RxReader* r, ByteSource* s, const Codec* c);
inline void rxr_reset(RxReader* r) { r->chunk = 0; r->cbeg = r->cend = 0; }
// Decodes the character at pos: returns its byte length (>= 1), or 0 at the end of the source or on an I/O error
// (r->err set).
u32 rxr_decode(RxReader* r, epos pos, ecp* cp);
// The character before pos (pos is a character boundary, lo a known boundary <= pos): 1 found, 0 none (pos <= lo),
// < 0 error. *start = its position.
int rxr_prev(RxReader* r, epos lo, epos pos, ecp* cp, epos* start);
// A character boundary at or before b (>= lo) for codecs that can't resynchronise (DBCS): just after a CR/LF byte.
epos rxr_snap(RxReader* r, epos lo, epos b);
// Start of the line containing q, not before floor (unit-1 ASCII-compatible codecs and UTF-16).
epos rxr_line_start(RxReader* r, epos floor, epos q);

// ---- progress ----
struct RxProgress { ProgressCb* cb; epos base; epos total; epos next; };
void rxp_init(RxProgress* g, ProgressCb* cb, epos base, epos total);
inline int rxp_tick(RxProgress* g, epos pos)   // E_CANCEL when the callback asks to stop
{
  if (!g->cb || pos < g->next) return E_OK;
  g->next = pos + 65536;
  if (g->next < pos) g->next = EPOS_NONE;
  return g->cb->progress(pos - g->base, g->total) ? E_CANCEL : E_OK;
}

// ---- byte prefilter (Horspool over raw spans) ----
enum { RXPRE_MAX = 512 };
struct RxPre {
  u32 m;               // literal length in bytes (0 = unusable)
  u8  ok;              // usable
  u8  never;           // the literal can't occur in this encoding (no match possible)
  u8  keyed;           // compare key[byte] with lk[] (case folding), else bytes with lit[]
  u8  full;            // the literal covers the whole prefix exactly (no truncation)
  u8  align;           // candidates at (q - base) % align == 0
  u8  pad[3];
  u8  lit[RXPRE_MAX];
  u32 lk[RXPRE_MAX];
  u32 key[256];
  u32 shift[256];
};
// Builds the prefilter for a literal (a program's prefix or required literal) in codec c. Never fails (ok = 0 when
// not applicable; never = 1 when the literal can't occur in this encoding).
void rxpre_build(RxPre* pf, const ecp* lit, u32 plen, int icase, const Codec* c);
// Plain byte finder (hex search, tests).
void rxpre_bytes(RxPre* pf, const u8* bytes, u32 m);
// First candidate q with from <= q <= start_max and q + m <= end, aligned to base. 1 found, 0 none, < 0 error.
int  rxpre_find(RxPre* pf, ByteSource* s, epos from, epos start_max, epos end, epos base, epos* q, RxProgress* g);
// Letters whose case-insensitive class includes a non-ASCII character encodable in c ('k' Kelvin sign, 's' long s,
// 'i' dotted capital I ...): an ASCII-folding byte filter can't be exact for them. Sets bits in mask[0..3] (by char).
void rxpre_unsafe_letters(const Codec* c, u32 mask[4]);
// Can some character case-equivalent to x (x already folded when icase) be encoded in c?
int  rxpre_encodable(const Codec* c, ecp x, int icase);

// ---- matcher state (rxvm.cpp) and the backtracker (rxbt.cpp) ----
struct RxList { u32* sparse; u32* pcs; epos* caps; u32 n; };
struct RxStk { u32 kind; u32 a; epos v; };
struct RxBtFrame { u32 pc; u32 i; u32 trail; };
struct RxTrail { u32 slot; u32 old; };
struct RxMState {
  const RxProg* prog;
  u32 ninst, ncap;
  RxList l[2];
  epos* tmp;           // working captures in addthread
  epos* best;          // captures of the best match
  epos* none;          // all EPOS_NONE (captures of a new thread)
  RxStk* stk;          // addthread stack
  const Codec* pre_codec; u32 pre_key; int pre_built;
  RxPre pre;           // prefix prefilter
  RxPre req;           // required-literal filter (line-local patterns without a prefix)
  // backtracker
  Vec<ecp> wc;         // window characters
  Vec<epos> wp;        // their positions (wp[n] = end of the last one)
  Vec<u32> sl;         // capture slots (char indices) + loop slots
  Vec<RxBtFrame> bstk;
  Vec<RxTrail> trail;
};
int  rxbt_search(RxMState* st, RxReader* rd, epos lo, epos from, epos hi, epos ctx_end, u32 budget, RxMatch* m,
                 RxProgress* g);
void rx_fill_match(const RxProg* p, const epos* caps, RxMatch* m);

#endif
