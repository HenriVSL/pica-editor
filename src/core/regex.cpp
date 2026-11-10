// regex.cpp - regex parser and program compiler (DESIGN 6.14, 11.1). Owner: T03.
// Pattern (code points) -> AST (node pool) -> instruction program (RxProg, regex_int.h). rxvm.cpp runs programs
// with a Pike VM (no backreferences / lookahead), rxbt.cpp with a budgeted backtracker, rxpre.cpp holds the
// literal prefilter. Also here: character classes, assertions, the literal prefix and the first-character set.
#include "regex_int.h"
#include "estr.h"
#include <string.h>

RxProg::RxProg()
  : flags(0), ngroups(0), ncap(2), nloops(0), first_hi(1), first_ok(0), use_bt(0), anchored(0), prefix_icase(0),
    pure(0), line_local(0), req_icase(0), bol(0)
{
  memset(first, 0, sizeof(first));
  pad2[0] = pad2[1] = pad2[2] = 0;
}

// ---------------------------------------------------------------------------------------------------------------
// Character classes and assertions

static int is_digit_c(ecp c) { return c >= '0' && c <= '9'; }

static int is_alpha_c(ecp c)
{
  if (c < 128) return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
  if (ECP_IS_BAD(c)) return 0;
  return c >= 0xAA && uc_is_word(c) && c != 0xB2 && c != 0xB3 && c != 0xB9 && c != 0xBC && c != 0xBD && c != 0xBE;
}

static int is_cntrl_c(ecp c) { return c < 0x20 || (c >= 0x7F && c <= 0x9F); }

static int is_punct_c(ecp c)
{
  if (c < 128) return (c >= 0x21 && c <= 0x2F) || (c >= 0x3A && c <= 0x40) || (c >= 0x5B && c <= 0x60) ||
                      (c >= 0x7B && c <= 0x7E);
  if (ECP_IS_BAD(c)) return 0;
  return (c >= 0xA1 && c <= 0xBF && c != 0xAA && c != 0xB5 && c != 0xBA) || c == 0xD7 || c == 0xF7 ||
         (c >= 0x2010 && c <= 0x2027) || (c >= 0x2030 && c <= 0x205E) || (c >= 0x3001 && c <= 0x303F);
}

static int is_space_c(ecp c)
{
  if (c < 128) return c == ' ' || (c >= 9 && c <= 13);
  if (ECP_IS_BAD(c)) return 0;
  return uc_is_space(c);
}

static int props_match(u32 pr, ecp c)
{
  int bad = ECP_IS_BAD(c) ? 1 : 0;
  if ((pr & RXP_WORD) && rx_is_word(c)) return 1;
  if ((pr & RXP_NWORD) && !rx_is_word(c)) return 1;
  if ((pr & RXP_DIGIT) && is_digit_c(c)) return 1;
  if ((pr & RXP_NDIGIT) && !is_digit_c(c)) return 1;
  if ((pr & RXP_SPACE) && is_space_c(c)) return 1;
  if ((pr & RXP_NSPACE) && !is_space_c(c)) return 1;
  if ((pr & RXP_ALPHA) && is_alpha_c(c)) return 1;
  if ((pr & RXP_ALNUM) && (is_alpha_c(c) || is_digit_c(c))) return 1;
  if ((pr & RXP_UPPER) && !bad && (c < 128 ? (c >= 'A' && c <= 'Z') : uc_lower(c) != c)) return 1;
  if ((pr & RXP_LOWER) && !bad && (c < 128 ? (c >= 'a' && c <= 'z') : uc_upper(c) != c)) return 1;
  if ((pr & RXP_PUNCT) && is_punct_c(c)) return 1;
  if ((pr & RXP_XDIGIT) && (is_digit_c(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return 1;
  if ((pr & RXP_BLANK) && !bad && (c == ' ' || c == 9 || (c >= 128 && is_space_c(c) && c != 0x85 && c != 0x2028 &&
                                                          c != 0x2029))) return 1;
  if ((pr & RXP_CNTRL) && is_cntrl_c(c)) return 1;
  if ((pr & RXP_PRINT) && !bad && !is_cntrl_c(c)) return 1;
  if ((pr & RXP_GRAPH) && !bad && !is_cntrl_c(c) && !is_space_c(c)) return 1;
  return 0;
}

static int class_raw(const RxProg* p, const RxClass* k, ecp c)
{
  if (k->rcount) {
    const u32* r = p->ranges.data() + k->rstart;
    u32 a = 0, b = k->rcount;        // binary search over pairs
    while (a < b) {
      u32 mid = (a + b) / 2;
      if (c < r[mid * 2]) b = mid;
      else if (c > r[mid * 2 + 1]) a = mid + 1;
      else return 1;
    }
  }
  return k->props ? props_match(k->props, c) : 0;
}

int rx_class_slow(const RxProg* p, const RxClass* k, ecp c)
{
  int r = class_raw(p, k, c);
  if (!r && k->icase && !ECP_IS_BAD(c)) {
    ecp f = uc_fold(c), u = uc_upper(c), l = uc_lower(c);
    r = (f != c && class_raw(p, k, f)) || (u != c && class_raw(p, k, u)) || (l != c && class_raw(p, k, l));
  }
  return k->negate ? !r : r;
}

int rx_accepts(const RxProg* p, const RxInst* in, ecp c, ecp fc)
{
  switch (in->op) {
  case RXO_CHAR:  return c == in->x;
  case RXO_CHARI: return fc == in->x;
  case RXO_ANY:   return c != 10 && c != 13;
  case RXO_ANYNL: return 1;
  case RXO_CLASS: return rx_class_has(p, in->x, c);
  default:        return 0;
  }
}

int rx_assert(u32 op, const RxAt* a)
{
  int wp, wn;
  switch (op) {
  case RXO_BOL: return !a->has_prev || a->prev == 10 || (a->prev == 13 && !(a->has_next && a->next == 10));
  case RXO_EOL: return !a->has_next || a->next == 13 || (a->next == 10 && !(a->has_prev && a->prev == 13));
  case RXO_BOT: return !a->has_prev;
  case RXO_EOT: return !a->has_next;
  default: break;
  }
  wp = a->has_prev && rx_is_word(a->prev);
  wn = a->has_next && rx_is_word(a->next);
  switch (op) {
  case RXO_WORDB:  return wp != wn;
  case RXO_NWORDB: return wp == wn;
  case RXO_WBEG:   return !wp && wn;
  case RXO_WEND:   return wp && !wn;
  case RXO_NWPREV: return !wp;
  case RXO_NWNEXT: return !wn;
  default:         return 0;
  }
}

void rx_fill_match(const RxProg* p, const epos* caps, RxMatch* m)
{
  u32 k;
  m->pos = caps[0];
  m->len = caps[1] - caps[0];
  for (k = 0; k < RX_GROUPS; k++) {
    if (k <= p->ngroups && caps[2 * k] != EPOS_NONE && caps[2 * k + 1] != EPOS_NONE && caps[2 * k + 1] >= caps[2 * k]) {
      m->gpos[k] = caps[2 * k];
      m->glen[k] = caps[2 * k + 1] - caps[2 * k];
    } else {
      m->gpos[k] = EPOS_NONE;
      m->glen[k] = 0;
    }
  }
}

// ---------------------------------------------------------------------------------------------------------------
// Parser

enum { NT_EMPTY = 0, NT_CHAR, NT_ANY, NT_CLASS, NT_CAT, NT_ALT, NT_GROUP, NT_REPEAT, NT_ASSERT, NT_BACKREF, NT_LOOK };
enum { RX_SKIP = 0xFFFFFFFEu };

// CHAR a=char; ANY a=dotall; CLASS a=class; GROUP a=capture index (0 = none); REPEAT a=min b=max greedy;
// ASSERT a=op; BACKREF a=group; LOOK a=negative. CAT/ALT: children linked by next.
struct RxNode { u8 type; u8 icase; u8 greedy; u8 pad; u32 a; u32 b; u32 child; u32 next; u32 pos; };

struct RxParser {
  const ecp* p; u32 n; u32 i;
  u32 flags;            // current RXF_ICASE / RXF_DOTALL (scoped by groups)
  Vec<RxNode> nodes;
  RxProg* prog;
  u32 caps_seen;        // capturing groups opened so far (also those beyond 9, which don't capture)
  u32 depth;
  int in_quote;
  int has_bt;
  u32 max_backref, backref_pos;
  int err; u32 errpos; char msg[96];
  Vec<u32> cr;          // ranges of the class being built
};

static u32 fail(RxParser* ps, int code, u32 pos, const char* msg)
{
  if (!ps->err) {
    ps->err = code;
    ps->errpos = pos;
    es_copy(ps->msg, sizeof(ps->msg), msg);
  }
  return RX_NONE;
}

static u32 fail_ch(RxParser* ps, int code, u32 pos, const char* fmt, ecp c)
{
  char buf[16];
  char msg[96];
  if (c >= 0x21 && c < 0x7F) es_fmt(buf, sizeof(buf), "%c", (int)c);
  else es_fmt(buf, sizeof(buf), "\\x{%X}", (u32)c);
  es_fmt(msg, sizeof(msg), fmt, buf);
  return fail(ps, code, pos, msg);
}

static u32 mk(RxParser* ps, u8 type, u32 pos)
{
  RxNode nd;
  memset(&nd, 0, sizeof(nd));
  nd.type = type;
  nd.child = RX_NONE;
  nd.next = RX_NONE;
  nd.pos = pos;
  nd.icase = (ps->flags & RXF_ICASE) ? 1 : 0;
  if (ps->nodes.push(nd) != E_OK) return fail(ps, E_NOMEM, pos, "Out of memory");
  return ps->nodes.size() - 1;
}

static u32 mk_char(RxParser* ps, ecp c, u32 pos)
{
  u32 k = mk(ps, NT_CHAR, pos);
  if (k != RX_NONE) ps->nodes[k].a = c;
  return k;
}

static int hexval(ecp c)
{
  if (c >= '0' && c <= '9') return (int)(c - '0');
  if (c >= 'a' && c <= 'f') return (int)(c - 'a' + 10);
  if (c >= 'A' && c <= 'F') return (int)(c - 'A' + 10);
  return -1;
}

// Reads exactly `count` hex digits at ps->i. -1 if they aren't there.
static i32 read_hex(RxParser* ps, u32 count)
{
  u32 v = 0, k;
  if (ps->i + count > ps->n) return -1;
  for (k = 0; k < count; k++) {
    int h = hexval(ps->p[ps->i + k]);
    if (h < 0) return -1;
    v = v * 16 + (u32)h;
  }
  ps->i += count;
  return (i32)v;
}

// ---- class ranges ----

static int cr_add(RxParser* ps, u32 lo, u32 hi)
{
  if (ps->cr.push(lo) != E_OK || ps->cr.push(hi) != E_OK) { fail(ps, E_NOMEM, ps->i, "Out of memory"); return 0; }
  return 1;
}

static void cr_sort_merge(Vec<u32>* v)
{
  u32 np = v->size() / 2, gap, i, j, w;
  u32* d = v->data();
  // shell sort of (lo, hi) pairs by lo
  for (gap = np / 2; gap > 0; gap /= 2) {
    for (i = gap; i < np; i++) {
      u32 lo = d[i * 2], hi = d[i * 2 + 1];
      j = i;
      while (j >= gap && d[(j - gap) * 2] > lo) {
        d[j * 2] = d[(j - gap) * 2];
        d[j * 2 + 1] = d[(j - gap) * 2 + 1];
        j -= gap;
      }
      d[j * 2] = lo;
      d[j * 2 + 1] = hi;
    }
  }
  w = 0;
  for (i = 0; i < np; i++) {
    if (w > 0 && d[i * 2] <= d[(w - 1) * 2 + 1] + 1) {
      if (d[i * 2 + 1] > d[(w - 1) * 2 + 1]) d[(w - 1) * 2 + 1] = d[i * 2 + 1];
    } else {
      d[w * 2] = d[i * 2];
      d[w * 2 + 1] = d[i * 2 + 1];
      w++;
    }
  }
  v->resize(w * 2);
}

static u32 finish_class(RxParser* ps, u32 props, int negate, u32 pos)
{
  RxClass k;
  RxProg* p = ps->prog;
  int icase = (ps->flags & RXF_ICASE) ? 1 : 0;
  u32 c, node;
  cr_sort_merge(&ps->cr);
  if (icase) {   // add the case variants of small ranges (Kelvin sign for k etc.); big ranges use runtime checks
    u32 np = ps->cr.size() / 2, i, budget = 4096;
    for (i = 0; i < np && !ps->err; i++) {
      u32 lo = ps->cr[i * 2], hi = ps->cr[i * 2 + 1], x;
      if (hi - lo >= 512 || hi - lo + 1 > budget) continue;
      budget -= hi - lo + 1;
      for (x = lo; x <= hi && !ps->err; x++) {
        ecp f = uc_fold(x), u = uc_upper(x), l = uc_lower(x);
        if (f != x) cr_add(ps, f, f);
        if (u != x) cr_add(ps, u, u);
        if (l != x) cr_add(ps, l, l);
      }
    }
    if (ps->err) return RX_NONE;
    cr_sort_merge(&ps->cr);
  }
  memset(&k, 0, sizeof(k));
  k.rstart = p->ranges.size();
  k.rcount = ps->cr.size() / 2;
  k.props = props;
  k.negate = (u8)(negate ? 1 : 0);
  k.icase = (u8)icase;
  if (ps->cr.size() && p->ranges.insert(p->ranges.size(), ps->cr.data(), ps->cr.size()) != E_OK)
    return fail(ps, E_NOMEM, pos, "Out of memory");
  for (c = 0; c < 256; c++)
    if (rx_class_slow(p, &k, c)) k.bits[c >> 5] |= 1u << (c & 31);
  if (p->classes.push(k) != E_OK) return fail(ps, E_NOMEM, pos, "Out of memory");
  node = mk(ps, NT_CLASS, pos);
  if (node != RX_NONE) ps->nodes[node].a = p->classes.size() - 1;
  return node;
}

static u32 shorthand_class(RxParser* ps, u32 props, u32 pos)
{
  ps->cr.clear();
  return finish_class(ps, props, 0, pos);
}

// ---- escapes ----
enum { ESC_NONE = 0, ESC_CHAR = 1, ESC_PROPS = 2 };

// ps->i is just after the backslash. Handles the escapes valid both inside and outside classes. Returns ESC_CHAR
// (*cp), ESC_PROPS (*props), ESC_NONE (ps->i back on the escape letter; caller decides) or RX_NONE on error.
static u32 esc_common(RxParser* ps, int in_class, ecp* cp, u32* props)
{
  u32 start = ps->i - 1;
  ecp c;
  if (ps->i >= ps->n) return fail(ps, E_SYNTAX, start, "Pattern ends with a backslash");
  c = ps->p[ps->i++];
  switch (c) {
  case 't': *cp = 9; return ESC_CHAR;
  case 'n': *cp = 10; return ESC_CHAR;
  case 'r': *cp = 13; return ESC_CHAR;
  case 'f': *cp = 12; return ESC_CHAR;
  case 'v': *cp = 11; return ESC_CHAR;
  case 'e': *cp = 27; return ESC_CHAR;
  case 'a': *cp = 7; return ESC_CHAR;
  case '0': *cp = 0; return ESC_CHAR;
  case 'x':
    if (ps->i < ps->n && ps->p[ps->i] == '{') {
      u32 v = 0, digits = 0;
      ps->i++;
      while (ps->i < ps->n && hexval(ps->p[ps->i]) >= 0 && digits < 8) {
        v = v * 16 + (u32)hexval(ps->p[ps->i]);
        ps->i++;
        digits++;
      }
      if (digits == 0 || ps->i >= ps->n || ps->p[ps->i] != '}')
        return fail(ps, E_SYNTAX, start, "\\x{...} needs hex digits and a closing }");
      ps->i++;
      if (v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) return fail(ps, E_SYNTAX, start, "Invalid character code");
      *cp = v;
      return ESC_CHAR;
    } else {
      i32 v = read_hex(ps, 2);
      if (v < 0) return fail(ps, E_SYNTAX, start, "\\x needs two hex digits (or \\x{...})");
      *cp = (ecp)v;
      return ESC_CHAR;
    }
  case 'u': {
    i32 v = read_hex(ps, 4);
    if (v < 0) return fail(ps, E_SYNTAX, start, "\\u needs four hex digits");
    if (v >= 0xD800 && v <= 0xDBFF && ps->i + 6 <= ps->n && ps->p[ps->i] == '\\' && ps->p[ps->i + 1] == 'u') {
      u32 save = ps->i;
      i32 lo;
      ps->i += 2;
      lo = read_hex(ps, 4);
      if (lo >= 0xDC00 && lo <= 0xDFFF) {
        *cp = 0x10000 + (((u32)v - 0xD800) << 10) + ((u32)lo - 0xDC00);
        return ESC_CHAR;
      }
      ps->i = save;
    }
    if (v >= 0xD800 && v <= 0xDFFF) return fail(ps, E_SYNTAX, start, "Invalid character code (lone surrogate)");
    *cp = (ecp)v;
    return ESC_CHAR;
  }
  case 'c':
    if (ps->i < ps->n && ((ps->p[ps->i] >= 'a' && ps->p[ps->i] <= 'z') || (ps->p[ps->i] >= 'A' && ps->p[ps->i] <= 'Z'))) {
      *cp = ps->p[ps->i++] & 31;
      return ESC_CHAR;
    }
    return fail(ps, E_SYNTAX, start, "\\c needs a letter");
  case 'd': *props = RXP_DIGIT; return ESC_PROPS;
  case 'D': *props = RXP_NDIGIT; return ESC_PROPS;
  case 'w': *props = RXP_WORD; return ESC_PROPS;
  case 'W': *props = RXP_NWORD; return ESC_PROPS;
  case 's': *props = RXP_SPACE; return ESC_PROPS;
  case 'S': *props = RXP_NSPACE; return ESC_PROPS;
  case 'b':
    if (in_class) { *cp = 8; return ESC_CHAR; }
    break;
  case 'p': case 'P':
    return fail(ps, E_UNSUPPORTED, start, "Unicode properties (\\p) are not supported");
  case '<': case '>':
    if (!in_class) break;
    *cp = c;
    return ESC_CHAR;
  default:
    if (c >= 128 || !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) {
      *cp = c;                       // escaped punctuation or non-ASCII: literal
      return ESC_CHAR;
    }
    break;
  }
  ps->i--;
  return ESC_NONE;
}

// ---- classes ----

static const struct { const char* name; u32 props; } k_posix[] = {
  { "alpha", RXP_ALPHA }, { "digit", RXP_DIGIT }, { "space", RXP_SPACE }, { "upper", RXP_UPPER },
  { "lower", RXP_LOWER }, { "alnum", RXP_ALNUM }, { "punct", RXP_PUNCT }, { "xdigit", RXP_XDIGIT },
  { "blank", RXP_BLANK }, { "cntrl", RXP_CNTRL }, { "print", RXP_PRINT }, { "graph", RXP_GRAPH },
  { "word", RXP_WORD }, { 0, 0 }
};

// At "[:" inside a class: parses "[:name:]". Returns 1 (props set), 0 (not a POSIX class: '[' is literal),
// RX_NONE on error.
static u32 parse_posix(RxParser* ps, u32* props)
{
  u32 j = ps->i + 2, start = ps->i, k;
  char name[16];
  u32 len = 0;
  while (j < ps->n && ps->p[j] >= 'a' && ps->p[j] <= 'z' && len < 15) name[len++] = (char)ps->p[j++];
  name[len] = 0;
  if (j + 1 >= ps->n || ps->p[j] != ':' || ps->p[j + 1] != ']') {
    if (j < ps->n && ps->p[j] == '^') return fail(ps, E_UNSUPPORTED, start, "Negated POSIX classes are not supported");
    return 0;
  }
  for (k = 0; k_posix[k].name; k++) {
    if (es_icmp(k_posix[k].name, name) == 0 && es_len(name) == es_len(k_posix[k].name)) {
      *props |= k_posix[k].props;
      ps->i = j + 2;
      return 1;
    }
  }
  return fail(ps, E_SYNTAX, start, "Unknown POSIX class [:name:]");
}

static u32 parse_class(RxParser* ps)
{
  u32 start = ps->i, props = 0;
  int negate = 0, first = 1;
  ps->i++;
  ps->cr.clear();
  if (ps->i < ps->n && ps->p[ps->i] == '^') { negate = 1; ps->i++; }
  for (;;) {
    ecp lo, hi;
    u32 pp = 0, r;
    int lo_is_char = 1;
    if (ps->i >= ps->n) return fail(ps, E_SYNTAX, start, "Missing ] at the end of the class");
    lo = ps->p[ps->i];
    if (lo == ']' && !first) { ps->i++; break; }
    first = 0;
    if (lo == '[' && ps->i + 1 < ps->n && ps->p[ps->i + 1] == ':') {
      r = parse_posix(ps, &props);
      if (r == RX_NONE) return RX_NONE;
      if (r == 1) continue;
      ps->i++;
    } else if (lo == '\\') {
      u32 escpos = ps->i;
      ps->i++;
      r = esc_common(ps, 1, &lo, &pp);
      if (r == RX_NONE) return RX_NONE;
      if (r == ESC_NONE) {
        ecp e = ps->p[ps->i];
        if (e >= '1' && e <= '9') return fail(ps, E_SYNTAX, escpos, "Backreferences can't be used in a class");
        return fail_ch(ps, E_SYNTAX, escpos, "Unknown escape \\%s in a class", e);
      }
      if (r == ESC_PROPS) { props |= pp; lo_is_char = 0; }
    } else {
      ps->i++;
    }
    if (!lo_is_char) continue;
    // range?
    if (ps->i + 1 < ps->n && ps->p[ps->i] == '-' && ps->p[ps->i + 1] != ']') {
      u32 save = ps->i, dashpos = ps->i;
      ps->i++;
      hi = ps->p[ps->i];
      if (hi == '\\') {
        ps->i++;
        r = esc_common(ps, 1, &hi, &pp);
        if (r == RX_NONE) return RX_NONE;
        if (r == ESC_NONE) return fail_ch(ps, E_SYNTAX, ps->i - 1, "Unknown escape \\%s in a class", ps->p[ps->i]);
        if (r == ESC_PROPS) {        // [a-\d]: '-' is literal
          if (!cr_add(ps, lo, lo) || !cr_add(ps, '-', '-')) return RX_NONE;
          props |= pp;
          continue;
        }
      } else if (hi == '[' && ps->i + 1 < ps->n && ps->p[ps->i + 1] == ':') {
        ps->i = save;                 // [a-[:digit:]]: '-' literal, POSIX class parsed next round
        if (!cr_add(ps, lo, lo) || !cr_add(ps, '-', '-')) return RX_NONE;
        ps->i++;
        continue;
      } else {
        ps->i++;
      }
      if (hi < lo) return fail(ps, E_SYNTAX, dashpos, "Range out of order in class");
      if (!cr_add(ps, lo, hi)) return RX_NONE;
      continue;
    }
    if (!cr_add(ps, lo, lo)) return RX_NONE;
  }
  return finish_class(ps, props, negate, start);
}

// ---- quantifiers ----

// At ps->i: 1 = quantifier parsed (ps->i after it), 0 = none here, RX_NONE = error.
static u32 parse_quant(RxParser* ps, u32* mn, u32* mx)
{
  ecp c;
  if (ps->i >= ps->n) return 0;
  c = ps->p[ps->i];
  if (c == '*') { *mn = 0; *mx = RX_INF; ps->i++; return 1; }
  if (c == '+') { *mn = 1; *mx = RX_INF; ps->i++; return 1; }
  if (c == '?') { *mn = 0; *mx = 1; ps->i++; return 1; }
  if (c == '{') {
    u32 j = ps->i + 1, a = 0, b, nd = 0;
    while (j < ps->n && ps->p[j] >= '0' && ps->p[j] <= '9') {
      if (a < 100000) a = a * 10 + (ps->p[j] - '0');
      j++; nd++;
    }
    if (nd == 0) return 0;
    if (j < ps->n && ps->p[j] == '}') {
      b = a;
    } else if (j < ps->n && ps->p[j] == ',') {
      u32 nd2 = 0;
      j++;
      b = 0;
      while (j < ps->n && ps->p[j] >= '0' && ps->p[j] <= '9') {
        if (b < 100000) b = b * 10 + (ps->p[j] - '0');
        j++; nd2++;
      }
      if (nd2 == 0) b = RX_INF;
      if (j >= ps->n || ps->p[j] != '}') return 0;
    } else {
      return 0;
    }
    if (a > RX_MAX_REPEAT || (b != RX_INF && b > RX_MAX_REPEAT))
      return fail(ps, E_TOOBIG, ps->i, "Repeat count too large (max 1000)");
    if (b < a) return fail(ps, E_SYNTAX, ps->i, "Invalid repeat {n,m}: m is smaller than n");
    *mn = a; *mx = b;
    ps->i = j + 1;
    return 1;
  }
  return 0;
}

static u32 parse_alt(RxParser* ps);

static u32 parse_group(RxParser* ps)
{
  u32 open = ps->i, saved = ps->flags, child, node, capidx = 0;
  int kind = 0;                      // 0 capture, 1 non-capturing, 2 lookahead, 3 negative lookahead
  ps->i++;
  if (++ps->depth > RX_MAX_DEPTH) return fail(ps, E_TOOBIG, open, "Too many nested groups (max 100)");
  if (ps->i < ps->n && ps->p[ps->i] == '?') {
    ecp c;
    ps->i++;
    if (ps->i >= ps->n) return fail(ps, E_SYNTAX, open, "Missing )");
    c = ps->p[ps->i];
    if (c == ':') { kind = 1; ps->i++; }
    else if (c == '=') { kind = 2; ps->i++; }
    else if (c == '!') { kind = 3; ps->i++; }
    else if (c == '<') {
      if (ps->i + 1 < ps->n && (ps->p[ps->i + 1] == '=' || ps->p[ps->i + 1] == '!'))
        return fail(ps, E_UNSUPPORTED, open, "Lookbehind (?<= (?<! is not supported");
      return fail(ps, E_UNSUPPORTED, open, "Named groups are not supported");
    }
    else if (c == 'P' || c == '\'') return fail(ps, E_UNSUPPORTED, open, "Named groups are not supported");
    else if (c == '>') return fail(ps, E_UNSUPPORTED, open, "Atomic groups (?> are not supported");
    else if (c == '|') return fail(ps, E_UNSUPPORTED, open, "Branch reset groups (?| are not supported");
    else if (c == 'R' || c == '&' || c == '+' || (c >= '0' && c <= '9') ||
             (c == '-' && ps->i + 1 < ps->n && ps->p[ps->i + 1] >= '0' && ps->p[ps->i + 1] <= '9'))
      return fail(ps, E_UNSUPPORTED, open, "Recursion and subroutine calls are not supported");
    else if (c == '#') {
      while (ps->i < ps->n && ps->p[ps->i] != ')') ps->i++;
      if (ps->i >= ps->n) return fail(ps, E_SYNTAX, open, "Missing ) after the comment");
      ps->i++;
      ps->depth--;
      return RX_SKIP;
    } else {
      u32 nf = ps->flags;
      int on = 1, any = 0;
      for (;;) {
        if (ps->i >= ps->n) return fail(ps, E_SYNTAX, open, "Missing )");
        c = ps->p[ps->i];
        if (c == ')' || c == ':') break;
        if (c == '-' && on) on = 0;
        else if (c == 'i') { if (on) nf |= RXF_ICASE; else nf &= ~(u32)RXF_ICASE; }
        else if (c == 's') { if (on) nf |= RXF_DOTALL; else nf &= ~(u32)RXF_DOTALL; }
        else if (c == 'm') { /* ^ and $ are always line-wise */ }
        else if (c == 'x') return fail(ps, E_UNSUPPORTED, ps->i, "The x (extended) flag is not supported");
        else return fail_ch(ps, E_SYNTAX, ps->i, "Unknown group type or inline flag (?%s", c);
        any = 1;
        ps->i++;
      }
      if (!any && c == ')') return fail(ps, E_SYNTAX, open, "Empty inline flag group (?)");
      ps->i++;
      if (c == ')') {                // (?i): applies to the rest of the enclosing group
        ps->flags = nf;
        ps->depth--;
        return RX_SKIP;
      }
      ps->flags = nf;                // (?i:...)
      kind = 1;
    }
  } else {
    ps->caps_seen++;
    if (ps->caps_seen < RX_GROUPS) capidx = ps->caps_seen;
  }
  if (kind >= 2) ps->has_bt = 1;
  child = parse_alt(ps);
  if (child == RX_NONE) return RX_NONE;
  if (ps->i >= ps->n || ps->p[ps->i] != ')') return fail(ps, E_SYNTAX, open, "Missing )");
  ps->i++;
  ps->flags = saved;
  ps->depth--;
  if (kind == 1) return child;
  node = mk(ps, kind == 0 ? NT_GROUP : NT_LOOK, open);
  if (node == RX_NONE) return RX_NONE;
  ps->nodes[node].child = child;
  ps->nodes[node].a = kind == 0 ? capidx : (kind == 3 ? 1u : 0u);
  return node;
}

static u32 mk_assert(RxParser* ps, u32 op, u32 pos)
{
  u32 k = mk(ps, NT_ASSERT, pos);
  if (k != RX_NONE) ps->nodes[k].a = op;
  return k;
}

// \R = (?:\r\n|\n|\r)
static u32 mk_linebreak(RxParser* ps, u32 pos)
{
  u32 alt = mk(ps, NT_ALT, pos), cat, a, b, c1, c2, c3;
  cat = mk(ps, NT_CAT, pos);
  a = mk_char(ps, 13, pos);
  b = mk_char(ps, 10, pos);
  c2 = mk_char(ps, 10, pos);
  c3 = mk_char(ps, 13, pos);
  if (ps->err) return RX_NONE;
  ps->nodes[a].icase = ps->nodes[b].icase = ps->nodes[c2].icase = ps->nodes[c3].icase = 0;
  ps->nodes[a].next = b;
  ps->nodes[cat].child = a;
  c1 = cat;
  ps->nodes[c1].next = c2;
  ps->nodes[c2].next = c3;
  ps->nodes[alt].child = c1;
  return alt;
}

static u32 parse_atom(RxParser* ps)
{
  u32 pos = ps->i, r;
  ecp c = ps->p[ps->i];
  if (ps->in_quote) {
    ps->i++;
    return mk_char(ps, c, pos);
  }
  switch (c) {
  case '(':
    return parse_group(ps);
  case '[':
    return parse_class(ps);
  case '.': {
    u32 k = mk(ps, NT_ANY, pos);
    ps->i++;
    if (k != RX_NONE) ps->nodes[k].a = (ps->flags & RXF_DOTALL) ? 1u : 0u;
    return k;
  }
  case '^': ps->i++; return mk_assert(ps, RXO_BOL, pos);
  case '$': ps->i++; return mk_assert(ps, RXO_EOL, pos);
  case '*': case '+': case '?':
    return fail(ps, E_SYNTAX, pos, "Nothing to repeat before the quantifier");
  case '{': {
    u32 mn, mx;
    r = parse_quant(ps, &mn, &mx);
    if (r == RX_NONE) return RX_NONE;
    if (r == 1) { ps->i = pos; return fail(ps, E_SYNTAX, pos, "Nothing to repeat before the quantifier"); }
    ps->i++;
    return mk_char(ps, c, pos);
  }
  case '\\': {
    ecp cp = 0;
    u32 props = 0;
    ps->i++;
    r = esc_common(ps, 0, &cp, &props);
    if (r == RX_NONE) return RX_NONE;
    if (r == ESC_CHAR) return mk_char(ps, cp, pos);
    if (r == ESC_PROPS) return shorthand_class(ps, props, pos);
    c = ps->p[ps->i++];
    switch (c) {
    case 'b': return mk_assert(ps, RXO_WORDB, pos);
    case 'B': return mk_assert(ps, RXO_NWORDB, pos);
    case 'A': return mk_assert(ps, RXO_BOT, pos);
    case 'z': return mk_assert(ps, RXO_EOT, pos);
    case '<': return mk_assert(ps, RXO_WBEG, pos);
    case '>': return mk_assert(ps, RXO_WEND, pos);
    case 'R': return mk_linebreak(ps, pos);
    case 'Q':
      ps->in_quote = 1;
      return RX_SKIP;
    case 'E':
      return RX_SKIP;                // \E without \Q: ignored (PCRE)
    case 'Z': return fail(ps, E_UNSUPPORTED, pos, "\\Z is not supported (use \\z or $)");
    case 'G': return fail(ps, E_UNSUPPORTED, pos, "\\G is not supported");
    case 'k': case 'g': return fail(ps, E_UNSUPPORTED, pos, "Named or relative backreferences are not supported");
    case 'K': return fail(ps, E_UNSUPPORTED, pos, "\\K is not supported");
    case 'X': case 'C': case 'N': case 'h': case 'H': case 'V':
      return fail_ch(ps, E_UNSUPPORTED, pos, "\\%s is not supported", c);
    default:
      if (c >= '1' && c <= '9') {
        u32 k = mk(ps, NT_BACKREF, pos);
        if (k == RX_NONE) return RX_NONE;
        ps->nodes[k].a = c - '0';
        ps->has_bt = 1;
        if (c - '0' > ps->max_backref) { ps->max_backref = c - '0'; ps->backref_pos = pos; }
        return k;
      }
      return fail_ch(ps, E_SYNTAX, pos, "Unknown escape \\%s", c);
    }
  }
  default:
    ps->i++;
    return mk_char(ps, c, pos);
  }
}

static u32 parse_repeat(RxParser* ps)
{
  u32 atom = parse_atom(ps), mn = 1, mx = 1, qpos, r, node;
  int greedy = 1;
  u8 t;
  if (atom == RX_NONE || atom == RX_SKIP) return atom;
  if (ps->in_quote) {
    if (ps->i + 1 < ps->n && ps->p[ps->i] == '\\' && ps->p[ps->i + 1] == 'E') { ps->i += 2; ps->in_quote = 0; }
    else return atom;
  }
  qpos = ps->i;
  r = parse_quant(ps, &mn, &mx);
  if (r == RX_NONE) return RX_NONE;
  if (r == 0) return atom;
  t = ps->nodes[atom].type;
  if (t == NT_ASSERT) return fail(ps, E_SYNTAX, qpos, "Nothing to repeat: an anchor can't be repeated");
  if (t == NT_LOOK) return fail(ps, E_SYNTAX, qpos, "Nothing to repeat: a lookahead can't be repeated");
  if (ps->i < ps->n && ps->p[ps->i] == '?') { greedy = 0; ps->i++; }
  else if (ps->i < ps->n && ps->p[ps->i] == '+')
    return fail(ps, E_UNSUPPORTED, ps->i, "Possessive quantifiers (*+ ++ ?+) are not supported");
  {
    u32 save = ps->i, m2, x2;
    r = parse_quant(ps, &m2, &x2);
    if (r == RX_NONE) return RX_NONE;
    if (r == 1) return fail(ps, E_SYNTAX, save, "Nothing to repeat: two quantifiers in a row");
    ps->i = save;
  }
  if (mn == 1 && mx == 1) return atom;
  node = mk(ps, NT_REPEAT, qpos);
  if (node == RX_NONE) return RX_NONE;
  ps->nodes[node].a = mn;
  ps->nodes[node].b = mx;
  ps->nodes[node].greedy = (u8)greedy;
  ps->nodes[node].child = atom;
  return node;
}

static u32 parse_cat(RxParser* ps)
{
  u32 first = RX_NONE, last = RX_NONE, count = 0, pos = ps->i, node;
  while (ps->i < ps->n && (ps->in_quote || (ps->p[ps->i] != '|' && ps->p[ps->i] != ')'))) {
    u32 r;
    if (ps->in_quote && ps->i + 1 < ps->n && ps->p[ps->i] == '\\' && ps->p[ps->i + 1] == 'E') {
      ps->i += 2;
      ps->in_quote = 0;
      continue;
    }
    r = parse_repeat(ps);
    if (r == RX_NONE) return RX_NONE;
    if (r == RX_SKIP) continue;
    if (first == RX_NONE) first = r;
    else ps->nodes[last].next = r;
    last = r;
    count++;
  }
  if (count == 1) return first;
  node = mk(ps, count == 0 ? NT_EMPTY : NT_CAT, pos);
  if (node != RX_NONE && count) ps->nodes[node].child = first;
  return node;
}

static u32 parse_alt(RxParser* ps)
{
  u32 first = parse_cat(ps), last, alt;
  if (first == RX_NONE) return RX_NONE;
  if (ps->i >= ps->n || ps->p[ps->i] != '|') return first;
  alt = mk(ps, NT_ALT, ps->i);
  if (alt == RX_NONE) return RX_NONE;
  last = first;
  while (ps->i < ps->n && ps->p[ps->i] == '|') {
    u32 c;
    ps->i++;
    c = parse_cat(ps);
    if (c == RX_NONE) return RX_NONE;
    ps->nodes[last].next = c;
    last = c;
  }
  ps->nodes[alt].child = first;
  return alt;
}

// ---------------------------------------------------------------------------------------------------------------
// Compiler

struct RxComp { RxParser* ps; RxProg* prog; u32 rep_pos; };   // rep_pos: outermost quantifier being emitted

static u32 emit(RxComp* cc, u8 op, u8 arg, u32 x, u32 y, u32 pos)
{
  RxInst in;
  if (cc->ps->err) return RX_NONE;
  if (cc->prog->code.size() >= RX_MAX_PROG)
    return fail(cc->ps, E_TOOBIG, cc->rep_pos != RX_NONE ? cc->rep_pos : pos,
                "Pattern too complex (more than 4000 instructions)");
  in.op = op; in.arg = arg; in.pad = 0; in.x = x; in.y = y;
  if (cc->prog->code.push(in) != E_OK) return fail(cc->ps, E_NOMEM, pos, "Out of memory");
  return cc->prog->code.size() - 1;
}

static u32 pc_now(RxComp* cc) { return cc->prog->code.size(); }

static int nullable(const Vec<RxNode>& nd, u32 k)
{
  const RxNode& n = nd[k];
  u32 c;
  switch (n.type) {
  case NT_CHAR: case NT_ANY: case NT_CLASS: return 0;
  case NT_CAT:
    for (c = n.child; c != RX_NONE; c = nd[c].next) if (!nullable(nd, c)) return 0;
    return 1;
  case NT_ALT:
    for (c = n.child; c != RX_NONE; c = nd[c].next) if (nullable(nd, c)) return 1;
    return 0;
  case NT_GROUP: return nullable(nd, n.child);
  case NT_REPEAT: return n.a == 0 || nullable(nd, n.child);
  default: return 1;                 // EMPTY, ASSERT, LOOK, BACKREF
  }
}

static void emit_node(RxComp* cc, u32 k)
{
  RxParser* ps = cc->ps;
  RxNode n;
  u32 c;
  if (ps->err) return;
  n = ps->nodes[k];
  switch (n.type) {
  case NT_EMPTY:
    break;
  case NT_CHAR:
    if (n.icase) emit(cc, RXO_CHARI, 0, rx_fold(n.a), 0, n.pos);
    else emit(cc, RXO_CHAR, 0, n.a, 0, n.pos);
    break;
  case NT_ANY:
    emit(cc, n.a ? RXO_ANYNL : RXO_ANY, 0, 0, 0, n.pos);
    break;
  case NT_CLASS:
    emit(cc, RXO_CLASS, 0, n.a, 0, n.pos);
    break;
  case NT_ASSERT:
    emit(cc, (u8)n.a, 0, 0, 0, n.pos);
    break;
  case NT_BACKREF:
    emit(cc, RXO_BACKREF, n.icase, n.a, 0, n.pos);
    break;
  case NT_CAT:
    for (c = n.child; c != RX_NONE && !ps->err; c = ps->nodes[c].next) emit_node(cc, c);
    break;
  case NT_ALT: {
    u32 jumps = RX_NONE;             // chain of JMPs to patch through their x fields
    for (c = n.child; c != RX_NONE && !ps->err; c = ps->nodes[c].next) {
      if (ps->nodes[c].next != RX_NONE) {
        u32 sp = emit(cc, RXO_SPLIT, 0, 0, 0, n.pos), j;
        if (sp == RX_NONE) return;
        cc->prog->code[sp].x = sp + 1;
        emit_node(cc, c);
        j = emit(cc, RXO_JMP, 0, jumps, 0, n.pos);
        if (j == RX_NONE) return;
        jumps = j;
        cc->prog->code[sp].y = pc_now(cc);
      } else {
        emit_node(cc, c);
      }
    }
    while (jumps != RX_NONE && !ps->err) {
      u32 nx = cc->prog->code[jumps].x;
      cc->prog->code[jumps].x = pc_now(cc);
      jumps = nx;
    }
    break;
  }
  case NT_GROUP:
    if (n.a) emit(cc, RXO_SAVE, 0, 2 * n.a, 0, n.pos);
    emit_node(cc, n.child);
    if (n.a) emit(cc, RXO_SAVE, 0, 2 * n.a + 1, 0, n.pos);
    break;
  case NT_LOOK: {
    u32 lk = emit(cc, RXO_LOOK, (u8)n.a, 0, 0, n.pos);
    if (lk == RX_NONE) return;
    emit_node(cc, n.child);
    emit(cc, RXO_LOOKEND, 0, 0, 0, n.pos);
    if (!ps->err) cc->prog->code[lk].x = pc_now(cc);
    break;
  }
  case NT_REPEAT: {
    u32 mn = n.a, mx = n.b, r, outer = cc->rep_pos;
    int nul = nullable(ps->nodes, n.child), greedy = n.greedy;
    if (outer == RX_NONE) cc->rep_pos = n.pos;
    if (mx == RX_INF && mn >= 1 && !nul) {
      u32 top;
      for (r = 0; r + 1 < mn && !ps->err; r++) emit_node(cc, n.child);
      top = pc_now(cc);
      emit_node(cc, n.child);
      if (greedy) emit(cc, RXO_SPLIT, 0, top, pc_now(cc) + 1, n.pos);
      else emit(cc, RXO_SPLIT, 0, pc_now(cc) + 1, top, n.pos);
      cc->rep_pos = outer;
      break;
    }
    for (r = 0; r < mn && !ps->err; r++) emit_node(cc, n.child);
    if (mx == RX_INF) {
      u32 top = pc_now(cc), sp, loop = 0;
      sp = emit(cc, RXO_SPLIT, 0, 0, 0, n.pos);
      if (sp == RX_NONE) return;
      if (nul) {
        loop = cc->prog->nloops++;
        emit(cc, RXO_MARK, 0, loop, 0, n.pos);
      }
      emit_node(cc, n.child);
      if (nul) emit(cc, RXO_CHKPROG, 0, loop, 0, n.pos);
      emit(cc, RXO_JMP, 0, top, 0, n.pos);
      if (ps->err) return;
      if (greedy) { cc->prog->code[sp].x = sp + 1; cc->prog->code[sp].y = pc_now(cc); }
      else { cc->prog->code[sp].x = pc_now(cc); cc->prog->code[sp].y = sp + 1; }
    } else {
      u32 chain = RX_NONE;           // SPLITs to patch (linked through the field that gets the exit)
      for (r = mn; r < mx && !ps->err; r++) {
        u32 sp = emit(cc, RXO_SPLIT, 0, 0, 0, n.pos);
        if (sp == RX_NONE) return;
        if (greedy) { cc->prog->code[sp].x = sp + 1; cc->prog->code[sp].y = chain; }
        else { cc->prog->code[sp].y = sp + 1; cc->prog->code[sp].x = chain; }
        chain = sp;
        emit_node(cc, n.child);
      }
      while (chain != RX_NONE && !ps->err) {
        RxInst& in = cc->prog->code[chain];
        u32 nx;
        if (greedy) { nx = in.y; in.y = pc_now(cc); }
        else { nx = in.x; in.x = pc_now(cc); }
        chain = nx;
      }
    }
    cc->rep_pos = outer;
    break;
  }
  default:
    break;
  }
}

// ---- literal prefix, anchoring, first-character set ----

struct RxPrefixWalk { RxProg* p; int mode; int pure; };   // mode -1 none yet, 0 exact, 1 icase

static int prefix_walk(const Vec<RxNode>& nd, u32 k, RxPrefixWalk* w)
{
  const RxNode& n = nd[k];
  u32 c, r;
  switch (n.type) {
  case NT_EMPTY:
    return 1;
  case NT_CHAR: {
    int m = n.icase ? 1 : 0;
    if (w->p->prefix.size() >= RX_PREFIX_MAX) { w->pure = 0; return 0; }
    if (w->mode < 0) w->mode = m;
    else if (w->mode != m) {
      w->pure = 0;
      if (w->mode == 0) {            // switch to case-insensitive comparison (a superset filter)
        u32 i;
        for (i = 0; i < w->p->prefix.size(); i++) w->p->prefix[i] = rx_fold(w->p->prefix[i]);
        w->mode = 1;
      }
    }
    if (w->p->prefix.push(w->mode ? rx_fold(n.a) : n.a) != E_OK) { w->pure = 0; return 0; }
    return 1;
  }
  case NT_ASSERT:
    w->pure = 0;
    return 1;
  case NT_GROUP:
    if (n.a) w->pure = 0;
    return prefix_walk(nd, n.child, w);
  case NT_CAT:
    for (c = n.child; c != RX_NONE; c = nd[c].next) if (!prefix_walk(nd, c, w)) return 0;
    return 1;
  case NT_REPEAT:
    if (n.a == 0) { w->pure = 0; return 0; }
    for (r = 0; r < n.a; r++) if (!prefix_walk(nd, n.child, w)) return 0;
    if (n.a != n.b) { w->pure = 0; return 0; }
    return 1;
  default:
    w->pure = 0;
    return 0;
  }
}

// Does every match start with \A (bol_ok = 0) or with ^ or \A (bol_ok = 1)?
static int starts_bot(const Vec<RxNode>& nd, u32 k, int bol_ok)
{
  const RxNode& n = nd[k];
  u32 c;
  switch (n.type) {
  case NT_ASSERT: return n.a == RXO_BOT || (bol_ok && n.a == RXO_BOL);
  case NT_GROUP: return starts_bot(nd, n.child, bol_ok);
  case NT_CAT: return n.child != RX_NONE && starts_bot(nd, n.child, bol_ok);
  case NT_ALT:
    for (c = n.child; c != RX_NONE; c = nd[c].next) if (!starts_bot(nd, c, bol_ok)) return 0;
    return 1;
  default: return 0;
  }
}

static int inst_may_hi(const RxProg* p, const RxInst* in)
{
  switch (in->op) {
  case RXO_CHAR: return in->x >= 256;
  case RXO_CLASS: {
    const RxClass* k = &p->classes[in->x];
    if (k->negate || k->props || k->icase) return 1;
    return k->rcount && p->ranges[k->rstart + 2 * k->rcount - 1] >= 256;
  }
  default: return 1;
  }
}

static void compute_first(RxProg* p)
{
  Vec<u8> seen;
  Vec<u32> stack;
  u32 ninst = p->code.size(), c;
  int ok = 1;
  p->first_ok = 0;
  p->first_hi = 1;
  memset(p->first, 0, sizeof(p->first));
  if (seen.resize(ninst) != E_OK || stack.push(0) != E_OK) return;
  p->first_hi = 0;
  while (!stack.empty() && ok) {
    u32 pc = stack.back();
    const RxInst* in;
    stack.pop();
    if (pc >= ninst || seen[pc]) continue;
    seen[pc] = 1;
    in = &p->code[pc];
    switch (in->op) {
    case RXO_JMP:
      if (stack.push(in->x) != E_OK) ok = 0;
      break;
    case RXO_SPLIT:
      if (stack.push(in->y) != E_OK || stack.push(in->x) != E_OK) ok = 0;
      break;
    case RXO_CHAR: case RXO_CHARI: case RXO_ANY: case RXO_ANYNL: case RXO_CLASS:
      for (c = 0; c < 256; c++)
        if (rx_accepts(p, in, c, rx_fold(c))) p->first[c >> 5] |= 1u << (c & 31);
      if (inst_may_hi(p, in)) p->first_hi = 1;
      break;
    case RXO_MATCH: case RXO_LOOK: case RXO_LOOKEND: case RXO_BACKREF:
      ok = 0;
      break;
    default:                           // SAVE, assertions, MARK, CHKPROG
      if (stack.push(pc + 1) != E_OK) ok = 0;
      break;
    }
  }
  if (!ok) { p->first_hi = 1; memset(p->first, 0, sizeof(p->first)); return; }
  p->first_ok = 1;
}

// Longest run of literal characters in the top-level concatenation (every match contains it), used when there is
// no prefix; and whether a match can never contain CR or LF (then the run's line bounds the match start).
static void compute_req(RxProg* p, const Vec<RxNode>& nd, u32 root)
{
  u32 c, k, cur_start = RX_NONE, cur_len = 0, best_start = RX_NONE, best_len = 0;
  int cur_ic = 0, best_ic = 0, more = 1;
  for (k = 0; k < p->code.size(); k++) {
    const RxInst* in = &p->code[k];
    if (in->op <= RXO_CLASS && (rx_accepts(p, in, 10, 10) || rx_accepts(p, in, 13, 13))) break;
  }
  p->line_local = (u8)(k == p->code.size() ? 1 : 0);
  if (p->prefix.size() || nd[root].type != NT_CAT) return;
  c = nd[root].child;
  while (more) {
    int is_char = c != RX_NONE && nd[c].type == NT_CHAR;
    if (is_char && cur_len && (int)nd[c].icase == cur_ic && cur_len < RX_PREFIX_MAX) {
      cur_len++;
    } else {
      if (cur_len > best_len) { best_len = cur_len; best_start = cur_start; best_ic = cur_ic; }
      cur_len = 0;
      if (is_char) { cur_start = c; cur_len = 1; cur_ic = nd[c].icase; }
    }
    if (c == RX_NONE) more = 0;
    else c = nd[c].next;
  }
  if (best_len < 1) return;
  for (c = best_start, k = 0; k < best_len; k++, c = nd[c].next)
    if (p->req.push(best_ic ? rx_fold(nd[c].a) : nd[c].a) != E_OK) { p->req.clear(); return; }
  p->req_icase = (u8)best_ic;
}

// ---------------------------------------------------------------------------------------------------------------
// API

int rx_compile(const ecp* pat, u32 n, u32 flags, RxProg** out, RxError* err)
{
  RxParser ps;
  RxComp cc;
  RxProg* p;
  u32 root = RX_NONE;
  if (out) *out = 0;
  if (err) { err->code = E_OK; err->pos = 0; err->msg[0] = 0; }
  if (!out || (!pat && n)) return E_RANGE;
  if (n > RX_MAX_PATTERN) {
    if (err) {
      err->code = E_TOOBIG; err->pos = RX_MAX_PATTERN;
      es_copy(err->msg, sizeof(err->msg), "Pattern too long (max 1000 characters)");
    }
    return E_TOOBIG;
  }
  p = new RxProg;
  if (!p) {
    if (err) { err->code = E_NOMEM; es_copy(err->msg, sizeof(err->msg), "Out of memory"); }
    return E_NOMEM;
  }
  p->flags = flags;
  ps.p = pat; ps.n = n; ps.i = 0;
  ps.flags = flags & (RXF_ICASE | RXF_DOTALL);
  ps.prog = p;
  ps.caps_seen = 0; ps.depth = 0; ps.in_quote = 0; ps.has_bt = 0;
  ps.max_backref = 0; ps.backref_pos = 0;
  ps.err = 0; ps.errpos = 0; ps.msg[0] = 0;

  if (flags & RXF_LITERAL_) {
    u32 i, last = RX_NONE, first = RX_NONE;
    for (i = 0; i < n && !ps.err; i++) {
      u32 k = mk_char(&ps, pat[i], i);
      if (k == RX_NONE) break;
      if (first == RX_NONE) first = k; else ps.nodes[last].next = k;
      last = k;
    }
    if (!ps.err) {
      root = mk(&ps, n ? NT_CAT : NT_EMPTY, 0);
      if (root != RX_NONE) ps.nodes[root].child = first;
    }
  } else {
    root = parse_alt(&ps);
    if (!ps.err && ps.i < n) fail(&ps, E_SYNTAX, ps.i, "Unmatched )");
    if (!ps.err && ps.max_backref > ps.caps_seen)
      fail(&ps, E_SYNTAX, ps.backref_pos, "Backreference to a group that doesn't exist");
  }

  if (!ps.err) {
    RxPrefixWalk w;
    int complete;
    cc.ps = &ps;
    cc.prog = p;
    cc.rep_pos = RX_NONE;
    p->ngroups = ps.caps_seen < RX_GROUPS ? ps.caps_seen : RX_GROUPS - 1;
    p->ncap = 2 * (p->ngroups + 1);
    p->use_bt = (u8)((ps.has_bt || (flags & RXF_FORCEBT_)) ? 1 : 0);
    emit(&cc, RXO_SAVE, 0, 0, 0, 0);
    if (flags & RXF_WORD) emit(&cc, RXO_NWPREV, 0, 0, 0, 0);
    emit_node(&cc, root);
    if (flags & RXF_WORD) emit(&cc, RXO_NWNEXT, 0, 0, 0, n);
    emit(&cc, RXO_SAVE, 0, 1, 0, n);
    emit(&cc, RXO_MATCH, 0, 0, 0, n);
    if (!ps.err) {
      w.p = p; w.mode = -1; w.pure = 1;
      complete = prefix_walk(ps.nodes, root, &w);
      p->prefix_icase = (u8)(w.mode == 1 ? 1 : 0);
      p->pure = (u8)((complete && w.pure && p->prefix.size() > 0 && !(flags & RXF_WORD) && p->ngroups == 0) ? 1 : 0);
      p->anchored = (u8)starts_bot(ps.nodes, root, 0);
      p->bol = (u8)starts_bot(ps.nodes, root, 1);
      compute_first(p);
      compute_req(p, ps.nodes, root);
    }
  }
  if (ps.err) {
    if (err) { err->code = ps.err; err->pos = ps.errpos; es_copy(err->msg, sizeof(err->msg), ps.msg); }
    delete p;
    return ps.err;
  }
  *out = p;
  return E_OK;
}

void rx_free(RxProg* p)
{
  delete p;
}

u32 rx_group_count(const RxProg* p)
{
  return p ? p->ngroups : 0;
}

int rx_uses_backtracker(const RxProg* p)
{
  return p ? p->use_bt : 0;
}

int rx_literal_prefix(const RxProg* p, Vec<ecp>* lit, int* icase)
{
  if (lit) lit->clear();
  if (icase) *icase = 0;
  if (!p) return E_STATE;
  if (icase) *icase = p->prefix_icase;
  if (lit && p->prefix.size() && lit->insert(0, p->prefix.data(), p->prefix.size()) != E_OK) return E_NOMEM;
  return E_OK;
}
