// lexgen.cpp - generic data-driven lexer and the .lng parser (ARCH 7.2, DESIGN 6.13). Owner: T04.
// The .lng format is documented at the top of src/langs/cpp.lng and in docs/HANDOFF.md (T04).
//
// Generic state word (10 bits, GEN_STATE_MASK):
//   bits 0-2 mode: 0 normal, 1 block comment, 2 string continued by an escape at EOL, 3 multi-line string,
//                  4 line comment continued by '\' at EOL (C)
//   bit  3   inside a preprocessor line (continued by '\' at EOL, or by a block comment opened in it)
//   bits 4-5 index: block comment pair or string kind
//   bits 6-9 block comment nesting depth (1..15)
#include "lexer_int.h"
#include "ucd.h"

enum { M_NORMAL = 0, M_BLOCK = 1, M_STRCONT = 2, M_MLSTR = 3, M_LCCONT = 4 };

static u16 gpack(u32 mode, u32 pp, u32 idx, u32 depth)
{
  return (u16)(mode | (pp << 3) | (idx << 4) | (depth << 6));
}

int lex_word_char(ecp c)
{
  if (c < 128) return lex_is_alnum(c) || c == '_';
  if (c >= 0x110000u) return 0;
  return uc_is_word(c);
}

// ---------------------------------------------------------------------------------------------------------------
// KwTable

static u32 kw_hash(const u8* s, u32 n)
{
  u32 h = 2166136261u, i;
  for (i = 0; i < n; i++) { h ^= s[i]; h *= 16777619u; }
  return h;
}

KwTable::KwTable() : count_(0), fold_(false) {}
KwTable::~KwTable() {}

void KwTable::clear() { pool_.release(); slots_.release(); count_ = 0; }

// pool entry at off: [class][mask][len][bytes...]
int KwTable::rehash_(u32 nslots)
{
  Vec<u32> ns;
  u32 i, mask = nslots - 1;
  if (ns.resize(nslots) != E_OK) return E_NOMEM;
  for (i = 0; i < slots_.size(); i++) {
    u32 off = slots_[i], h;
    if (!off) continue;
    h = kw_hash(pool_.data() + off + 3, pool_[off + 2]) & mask;
    while (ns[h]) h = (h + 1) & mask;
    ns[h] = off;
  }
  slots_.swap(ns);
  return E_OK;
}

static u32 kw_lookup(const Vec<u8>& pool, const Vec<u32>& slots, const u8* s, u32 n)
{
  u32 mask = slots.size() - 1, h = kw_hash(s, n) & mask;
  for (;;) {
    u32 off = slots[h];
    if (!off) return 0;
    if (pool[off + 2] == n && memcmp(pool.data() + off + 3, s, n) == 0) return off;
    h = (h + 1) & mask;
  }
}

int KwTable::add(const char* w, u32 n, u8 cls)
{
  u8 buf[KW_MAX_LEN + 1];
  u32 i, h, mask, off;
  if (n == 0 || n > KW_MAX_LEN || cls < 1 || cls > 8) return E_RANGE;
  for (i = 0; i < n; i++) {
    u8 c = (u8)w[i];
    buf[i] = (fold_ && c >= 'A' && c <= 'Z') ? (u8)(c + 32) : c;
  }
  if (count_) {
    off = kw_lookup(pool_, slots_, buf, n);
    if (off) { pool_[off + 1] |= (u8)(1u << (cls - 1)); return E_OK; }
  }
  if (pool_.empty() && pool_.push(0) != E_OK) return E_NOMEM;     // offset 0 = empty slot
  if ((count_ + 1) * 2 > slots_.size()) {
    u32 ns = slots_.size() ? slots_.size() * 2 : 64;
    while ((count_ + 1) * 2 > ns) ns *= 2;
    if (rehash_(ns) != E_OK) return E_NOMEM;
  }
  off = pool_.size();
  if (pool_.reserve(off + n + 3) != E_OK) return E_NOMEM;
  pool_.push(cls); pool_.push((u8)(1u << (cls - 1))); pool_.push((u8)n);
  for (i = 0; i < n; i++) pool_.push(buf[i]);
  mask = slots_.size() - 1;
  h = kw_hash(buf, n) & mask;
  while (slots_[h]) h = (h + 1) & mask;
  slots_[h] = off;
  count_++;
  return E_OK;
}

u8 KwTable::find_ascii(const char* s, u32 n, u8* mask) const
{
  u32 off;
  if (mask) *mask = 0;
  if (!count_ || n == 0 || n > KW_MAX_LEN) return 0;
  off = kw_lookup(pool_, slots_, (const u8*)s, n);
  if (!off) return 0;
  if (mask) *mask = pool_[off + 1];
  return pool_[off];
}

static int kw_fold_ecp(const ecp* s, u32 n, bool fold, char* buf)
{
  u32 i;
  if (n == 0 || n > KW_MAX_LEN) return 0;
  for (i = 0; i < n; i++) {
    ecp c = s[i];
    if (c >= 128 || c == 0) return 0;
    if (fold && c >= 'A' && c <= 'Z') c += 32;
    buf[i] = (char)c;
  }
  return 1;
}

u8 KwTable::find(const ecp* s, u32 n) const
{
  char buf[KW_MAX_LEN + 1];
  if (!count_ || !kw_fold_ecp(s, n, fold_, buf)) return 0;
  return find_ascii(buf, n, 0);
}

u8 KwTable::find_mask(const ecp* s, u32 n) const
{
  char buf[KW_MAX_LEN + 1];
  u8 m;
  if (!count_ || !kw_fold_ecp(s, n, fold_, buf)) return 0;
  find_ascii(buf, n, &m);
  return m;
}

// ---------------------------------------------------------------------------------------------------------------
// GenLexer

GenLexer::GenLexer()
  : nlc(0), nbc(0), nested(0), nstr(0), nline(0), ncword(0), pp_char(0), deco_char(0), number(GN_C), vars(GV_NONE),
    label(GL_NONE), regex(0), ini_section(0), ini_key(0), make_rules(0), lc_cont(0), case_sens(1)
{
  memset(cls, 0, sizeof(cls));
  memset(lc, 0, sizeof(lc)); memset(bo, 0, sizeof(bo)); memset(bc, 0, sizeof(bc));
  memset(str, 0, sizeof(str)); memset(line, 0, sizeof(line)); memset(cword, 0, sizeof(cword));
}
GenLexer::~GenLexer() {}

void GenLexer::finish()
{
  u32 k, j;
  for (k = 0; k < nlc; k++) if (lc[k].len && lc[k].t[0] < 128) cls[lc[k].t[0]] |= CC_SPECIAL;
  for (k = 0; k < nbc; k++) if (bo[k].len && bo[k].t[0] < 128) cls[bo[k].t[0]] |= CC_SPECIAL;
  for (k = 0; k < nstr; k++) if (str[k].open.len && str[k].open.t[0] < 128) cls[str[k].open.t[0]] |= CC_SPECIAL;
  if (vars == GV_MAKE) cls['$'] |= CC_SPECIAL;
  if (vars == GV_BAT || vars == GV_PERCENT) cls['%'] |= CC_SPECIAL;
  if (regex) cls['/'] |= CC_SPECIAL;
  if (number == GN_BASIC) cls['&'] |= CC_SPECIAL;
  if (number == GN_PASCAL) cls['$'] |= CC_SPECIAL;
  // longer string openers first (""" before ")
  for (k = 1; k < nstr; k++) {
    for (j = k; j > 0 && str[j].open.len > str[j - 1].open.len; j--) {
      GenStr t = str[j]; str[j] = str[j - 1]; str[j - 1] = t;
    }
  }
  for (k = 0; k < 128; k++) if (cls[k] & CC_IDS) cls[k] |= CC_IDP;
}

int GenLexer::match_tok(const ecp* s, u32 n, u32 i, const GenTok& t) const
{
  u32 k;
  if (!t.len || t.len > n - i) return 0;
  if (case_sens) {
    for (k = 0; k < t.len; k++) if (s[i + k] != t.t[k]) return 0;
  } else {
    for (k = 0; k < t.len; k++) if (lex_lower(s[i + k]) != lex_lower(t.t[k])) return 0;
  }
  return 1;
}

u32 GenLexer::scan_block(const ecp* s, u32 n, u32 i, u32 idx, u32* depth, u8* st) const
{
  u32 start = i;
  while (i < n) {
    if (nested && match_tok(s, n, i, bo[idx])) {
      if (*depth < 15) (*depth)++;
      i += bo[idx].len;
      continue;
    }
    if (match_tok(s, n, i, bc[idx])) {
      i += bc[idx].len;
      if (--(*depth) == 0) { lex_fill(st, start, i, ST_COMMENT); return i; }
      continue;
    }
    i++;
  }
  lex_fill(st, start, n, ST_COMMENT);
  return n;
}

// i = just after the opening token. *open_state: 0 closed (or unterminated single-line), else M_STRCONT / M_MLSTR
u32 GenLexer::scan_string(const ecp* s, u32 n, u32 i, u32 idx, u8* st, int* open_state) const
{
  const GenStr& d = str[idx];
  u32 start = i;
  *open_state = 0;
  while (i < n) {
    if (d.esc && s[i] == d.esc) {
      if (i + 1 < n) { i += 2; continue; }
      i = n;
      *open_state = d.multiline ? M_MLSTR : M_STRCONT;     // escape at EOL continues the string
      lex_fill(st, start, n, d.style);
      return n;
    }
    if (match_tok(s, n, i, d.close)) {
      i += d.close.len;
      lex_fill(st, start, i, d.style);
      return i;
    }
    i++;
  }
  if (d.multiline) *open_state = M_MLSTR;
  lex_fill(st, start, n, d.style);
  return n;
}

u32 GenLexer::scan_number(const ecp* s, u32 n, u32 i) const
{
  u32 j = i;
  int hex = 0;
  if ((number == GN_BASIC && s[i] == '&') || (number == GN_PASCAL && s[i] == '$')) {
    j = i + 1;
    if (number == GN_BASIC) j++;                           // the H / O / B letter
    while (j < n && (lex_is_alnum(s[j]) || s[j] == '_')) j++;
    return j;
  }
  if (s[j] == '0' && j + 1 < n && (s[j + 1] == 'x' || s[j + 1] == 'X')) hex = 1;
  while (j < n) {
    ecp c = s[j];
    if (lex_is_alnum(c) || c == '_') {
      if (!hex && (c == 'e' || c == 'E') && j + 2 < n && (s[j + 1] == '+' || s[j + 1] == '-') && lex_is_digit(s[j + 2])) {
        j += 3;
        continue;
      }
      j++;
      continue;
    }
    if (c == '.' && !(j + 1 < n && s[j + 1] == '.')) { j++; continue; }
    break;
  }
  return j;
}

static u32 match_bracket(const ecp* s, u32 n, u32 i)      // s[i] is ( or {; returns index after the match or n
{
  ecp o = s[i], c = (o == '(') ? ')' : '}';
  u32 d = 0;
  while (i < n) {
    if (s[i] == o) d++;
    else if (s[i] == c && --d == 0) return i + 1;
    i++;
  }
  return n;
}

// returns the end of a variable starting at i (0 = none); *style receives ST_VARIABLE or ST_DEFAULT (escapes)
static u32 scan_var(u32 kind, const ecp* s, u32 n, u32 i, u8* style)
{
  u32 j;
  *style = ST_VARIABLE;
  if (kind == GV_MAKE) {
    if (s[i] != '$' || i + 1 >= n) return 0;
    if (s[i + 1] == '(' || s[i + 1] == '{') return match_bracket(s, n, i + 1);
    if (s[i + 1] == ' ' || s[i + 1] == '\t') return 0;
    if (s[i + 1] == '*' && i + 2 < n && s[i + 2] == '*') return i + 3;
    return i + 2;
  }
  if (s[i] != '%' || i + 1 >= n) return 0;
  if (kind == GV_BAT) {
    ecp c = s[i + 1];
    if (c == '%') {                                        // %%i, %%~nxi, or a literal %%
      if (i + 2 < n && s[i + 2] == '~') {
        j = i + 3;
        while (j < n && (lex_is_alnum(s[j]) || s[j] == '$' || s[j] == ':')) j++;
        return j;
      }
      if (i + 2 < n && lex_is_alpha(s[i + 2])) return i + 3;
      *style = ST_DEFAULT;
      return i + 2;
    }
    if (c == '~') {
      j = i + 2;
      while (j < n && (lex_is_alnum(s[j]) || s[j] == '$' || s[j] == ':')) j++;
      return j;
    }
    if (lex_is_digit(c) || c == '*') return i + 2;
  }
  // %NAME% (no spaces inside)
  for (j = i + 1; j < n; j++) {
    if (s[j] == '%') return j > i + 1 ? j + 1 : 0;
    if (s[j] == ' ' || s[j] == '\t') return 0;
  }
  return 0;
}

u32 GenLexer::scan_regex(const ecp* s, u32 n, u32 i) const
{
  u32 j = i + 1;
  int in_class = 0;
  if (j >= n || s[j] == '/' || s[j] == '*') return 0;
  while (j < n) {
    ecp c = s[j];
    if (c == '\\') { j += 2; continue; }
    if (in_class) { if (c == ']') in_class = 0; j++; continue; }
    if (c == '[') { in_class = 1; j++; continue; }
    if (c == '/') {
      j++;
      while (j < n && lex_is_alpha(s[j])) j++;
      return j;
    }
    j++;
  }
  return 0;                                                // unterminated: a division after all
}

static int is_idp_c(const GenLexer* g, ecp c)
{
  if (c < 128) return (g->cls[c] & CC_IDP) != 0;
  return lex_word_char(c);
}

u32 GenLexer::make_rule(const ecp* s, u32 n, u8* st) const
{
  u32 e = 0, q, k, t, oplen = 0;
  if (s[0] == '#') return 0;
  while (e < n && (lex_is_alnum(s[e]) || s[e] == '_' || s[e] == '.')) e++;
  if (e > 0) {                                             // NAME = value, NAME := value, ...
    q = e;
    while (q < n && lex_is_space(s[q])) q++;
    if (q < n) {
      if (s[q] == '=') oplen = 1;
      else if (q + 1 < n && s[q + 1] == '=' && (s[q] == ':' || s[q] == '+' || s[q] == '?' || s[q] == '!')) oplen = 2;
      else if (q + 2 < n && s[q] == ':' && s[q + 1] == ':' && s[q + 2] == '=') oplen = 3;
    }
    if (oplen) {
      lex_fill(st, 0, e, ST_VARIABLE);
      lex_fill(st, e, q, ST_DEFAULT);
      lex_fill(st, q, q + oplen, ST_OPERATOR);
      return q + oplen;
    }
  }
  for (k = 0; k < n; k++) {                                // target: text before the first ':' (not ':=')
    ecp c = s[k];
    if (c == '$' && k + 1 < n && (s[k + 1] == '(' || s[k + 1] == '{')) { k = match_bracket(s, n, k + 1) - 1; continue; }
    if (c == '=' || c == '#') return 0;
    if (c == ':') {
      if (k + 1 < n && (s[k + 1] == '\\' || s[k + 1] == '/') && k >= 1 && lex_is_alpha(s[k - 1]) &&
          (k == 1 || lex_is_space(s[k - 2]))) continue;   // drive letter c:\path
      if (k + 1 < n && s[k + 1] == '=') return 0;
      break;
    }
  }
  if (k >= n || k == 0) return 0;
  t = 0;
  while (t < k) {
    if (vars == GV_MAKE && s[t] == '$') {
      u8 vs;
      u32 ve = scan_var(GV_MAKE, s, k, t, &vs);
      if (ve > t) { lex_fill(st, t, ve, vs); t = ve; continue; }
    }
    if (st) st[t] = lex_is_space(s[t]) ? ST_DEFAULT : ST_LABEL;
    t++;
  }
  e = (k + 1 < n && s[k + 1] == ':') ? k + 2 : k + 1;
  lex_fill(st, k, e, ST_OPERATOR);
  return e;
}

// line-start rules; returns where the main loop continues; *done = 1 when the whole line was styled
u32 GenLexer::line_rules(const ecp* s, u32 n, u8* st, int* pp, int* done) const
{
  u32 j = 0, k, e;
  *done = 0;
  for (k = 0; k < nline; k++) {
    if (match_tok(s, n, 0, line[k].prefix)) { lex_fill(st, 0, n, line[k].style); *done = 1; return n; }
  }
  while (j < n && lex_is_space(s[j])) j++;
  lex_fill(st, 0, j, ST_DEFAULT);
  if (j >= n) { *done = 1; return n; }
  if (pp_char && s[j] == pp_char) { *pp = 1; return j; }
  for (k = 0; k < nlc; k++) {
    if (lc[k].start_only && match_tok(s, n, j, lc[k])) { lex_fill(st, j, n, ST_COMMENT); *done = 1; return n; }
  }
  if (ncword) {
    u32 w = j;
    if (s[w] == '@') w++;
    e = w;
    while (e < n && e - w < 15 && (lex_is_alnum(s[e]) || s[e] == '_')) e++;
    if (e > w && (e >= n || !lex_word_char(s[e]))) {
      for (k = 0; k < ncword; k++) {
        u32 len = es_len(cword[k]), m;
        if (len != e - w) continue;
        for (m = 0; m < len; m++) {
          ecp a = s[w + m], b = (u8)cword[k][m];
          if (!case_sens) { a = lex_lower(a); b = lex_lower(b); }
          if (a != b) break;
        }
        if (m == len) { lex_fill(st, j, n, ST_COMMENT); *done = 1; return n; }
      }
    }
  }
  if (label == GL_COLON_PREFIX && s[j] == ':') { lex_fill(st, j, n, ST_LABEL); *done = 1; return n; }
  if (ini_section && s[j] == '[') {
    e = j + 1;
    while (e < n && s[e] != ']') e++;
    if (e < n) e++;
    lex_fill(st, j, e, ST_SECTION);
    return e;
  }
  if (ini_key) {
    for (e = j; e < n && s[e] != '='; e++) {}
    if (e < n && e > j) {
      k = e;
      while (k > j && lex_is_space(s[k - 1])) k--;
      lex_fill(st, j, k, ST_KEY);
      lex_fill(st, k, e, ST_DEFAULT);
      lex_fill(st, e, e + 1, ST_OPERATOR);
      return e + 1;
    }
  }
  if (make_rules && j == 0) {
    e = make_rule(s, n, st);
    if (e) return e;
  }
  if ((label == GL_ASM || label == GL_COLON_SUFFIX) && s[j] < 128 && (cls[s[j]] & CC_IDS)) {
    e = j + 1;
    while (e < n && is_idp_c(this, s[e])) e++;
    if (e < n && s[e] == ':') {
      e++;
      if (e < n && s[e] == ':') e++;
      lex_fill(st, j, e, ST_LABEL);
      return e;
    }
    if (label == GL_ASM && j == 0 && !kw.find(s, e)) { lex_fill(st, 0, e, ST_LABEL); return e; }
  }
  if (deco_char && s[j] == deco_char) {
    e = j + 1;
    while (e < n && (is_idp_c(this, s[e]) || s[e] == '.')) e++;
    lex_fill(st, j, e, ST_PREPROC);
    return e;
  }
  return j;
}

u16 GenLexer::lex_line(const ecp* s, u32 n, u16 state, u8* styles) const
{
  if (n > LEX_LINE_MAX) { lex_fill(styles, LEX_LINE_MAX, n, ST_DEFAULT); n = LEX_LINE_MAX; }
  return lex_range(s, n, state, styles, 1);
}

u16 GenLexer::lex_range(const ecp* s, u32 n, u16 state, u8* st, int line_start) const
{
  u32 mode = state & 7, pp = (state >> 3) & 1, idx = (state >> 4) & 3, depth = (state >> 6) & 15;
  u32 i = 0, e, k;
  int rx_ok = 1, os;
  u8 base;

  if (mode == M_BLOCK && idx < nbc) {
    if (depth == 0) depth = 1;
    i = scan_block(s, n, 0, idx, &depth, st);
    if (depth) return gpack(M_BLOCK, pp, idx, depth);
  } else if ((mode == M_STRCONT || mode == M_MLSTR) && idx < nstr) {
    i = scan_string(s, n, 0, idx, st, &os);
    if (os) return gpack((u32)os, pp, idx, 0);
    rx_ok = 0;
  } else if (mode == M_LCCONT) {
    lex_fill(st, 0, n, ST_COMMENT);
    if (lc_cont && n && s[n - 1] == '\\') return gpack(M_LCCONT, pp, 0, 0);
    return 0;
  } else if (line_start && !pp) {
    int done, ppi = 0;
    i = line_rules(s, n, st, &ppi, &done);
    if (done) return 0;
    pp = (u32)ppi;
  }
  base = pp ? (u8)ST_PREPROC : (u8)ST_DEFAULT;

  while (i < n) {
    ecp c = s[i];
    u32 f = c < 128 ? cls[c] : (lex_word_char(c) ? (u32)(CC_IDS | CC_IDP) : 0u);
    if (f & CC_SPECIAL) {
      int handled = 0;
      for (k = 0; k < nbc && !handled; k++) {
        if (match_tok(s, n, i, bo[k])) {
          lex_fill(st, i, i + bo[k].len, ST_COMMENT);
          depth = 1;
          e = scan_block(s, n, i + bo[k].len, k, &depth, st);
          if (depth) return gpack(M_BLOCK, pp, k, depth);
          i = e; handled = 1;
        }
      }
      if (handled) continue;
      for (k = 0; k < nlc; k++) {
        if (!lc[k].start_only && match_tok(s, n, i, lc[k])) {
          lex_fill(st, i, n, ST_COMMENT);
          if (lc_cont && n && s[n - 1] == '\\') return gpack(M_LCCONT, pp, 0, 0);
          return 0;
        }
      }
      for (k = 0; k < nstr && !handled; k++) {
        if (match_tok(s, n, i, str[k].open)) {
          lex_fill(st, i, i + str[k].open.len, str[k].style);
          e = scan_string(s, n, i + str[k].open.len, k, st, &os);
          if (os) return gpack((u32)os, pp, k, 0);
          i = e; handled = 1; rx_ok = 0;
        }
      }
      if (handled) continue;
      if (vars && !pp) {
        u8 vs;
        e = scan_var(vars, s, n, i, &vs);
        if (e > i) { lex_fill(st, i, e, vs); i = e; rx_ok = 0; continue; }
      }
      if (regex && !pp && c == '/' && rx_ok) {
        e = scan_regex(s, n, i);
        if (e) { lex_fill(st, i, e, ST_STRING); i = e; rx_ok = 0; continue; }
      }
      if ((number == GN_BASIC && c == '&' && i + 2 < n && (s[i + 1] == 'H' || s[i + 1] == 'h' || s[i + 1] == 'O' ||
           s[i + 1] == 'o') && lex_is_alnum(s[i + 2])) ||
          (number == GN_PASCAL && c == '$' && i + 1 < n && lex_is_alnum(s[i + 1]))) {
        e = scan_number(s, n, i);
        lex_fill(st, i, e, pp ? ST_PREPROC : ST_NUMBER);
        i = e; rx_ok = 0;
        continue;
      }
    }
    if (number != GN_NONE && (lex_is_digit(c) || (c == '.' && !(f & CC_IDS) && i + 1 < n && lex_is_digit(s[i + 1])))) {
      e = scan_number(s, n, i);
      lex_fill(st, i, e, pp ? ST_PREPROC : ST_NUMBER);
      i = e; rx_ok = 0;
      continue;
    }
    if (f & CC_IDS) {
      u8 kc;
      e = i + 1;
      while (e < n && is_idp_c(this, s[e])) e++;
      if (prefixes.count() && e < n && prefixes.find(s + i, e - i)) {
        int handled = 0;
        for (k = 0; k < nstr && !handled; k++) {
          if (match_tok(s, n, e, str[k].open)) {
            lex_fill(st, i, e + str[k].open.len, str[k].style);
            e = scan_string(s, n, e + str[k].open.len, k, st, &os);
            if (os) return gpack((u32)os, pp, k, 0);
            handled = 1;
          }
        }
        if (handled) { i = e; rx_ok = 0; continue; }
      }
      kc = kw.find(s + i, e - i);
      lex_fill(st, i, e, pp ? ST_PREPROC : (kc ? (u8)(ST_KEYWORD1 + kc - 1) : base));
      rx_ok = (kc == 1);
      i = e;
      continue;
    }
    if (f & CC_OP) {
      if (st) st[i] = pp ? (u8)ST_PREPROC : (u8)ST_OPERATOR;
      rx_ok = !(c == ')' || c == ']');
      i++;
      continue;
    }
    if (st) st[i] = base;
    i++;
  }
  if (pp) {
    k = n;
    while (k > 0 && lex_is_space(s[k - 1])) k--;
    if (k > 0 && s[k - 1] == '\\') return gpack(M_NORMAL, 1, 0, 0);
  }
  return 0;
}

// ---------------------------------------------------------------------------------------------------------------
// .lng parser

static const char* const g_style_names[ST_COUNT] = {
  "default", "comment", "string", "char", "number", "keyword1", "keyword2", "keyword3", "keyword4", "preproc",
  "operator", "label", "variable", "section", "key", "tag", "attr", "entity", "heading", "emph", "code", "diff_add",
  "diff_del", "diff_hunk", "diff_header", "invalid"
};

const char* lng_style_name(u32 style) { return style < ST_COUNT ? g_style_names[style] : "?"; }

int lng_style_from_name(const char* s)
{
  u32 i;
  for (i = 0; i < ST_COUNT; i++) if (es_icmp(s, g_style_names[i]) == 0) return (int)i;
  return -1;
}

// next whitespace-separated token with escapes \s (space), \t (tab), \\ (backslash); returns length, -1 if too long
static int next_token(const char** p, char* out, u32 cap)
{
  const char* q = *p;
  u32 n = 0;
  while (*q == ' ' || *q == '\t') q++;
  while (*q && *q != ' ' && *q != '\t') {
    char c = *q++;
    if (c == '\\' && (*q == 's' || *q == 't' || *q == '\\')) {
      c = (*q == 's') ? ' ' : (*q == 't') ? '\t' : '\\';
      q++;
    }
    if (n + 1 >= cap) return -1;
    out[n++] = c;
  }
  out[n] = 0;
  *p = q;
  return (int)n;
}

static int set_tok(GenTok* t, const char* s, u32 maxlen)
{
  u32 n = es_len(s), k;
  if (n == 0 || n > maxlen || n > GEN_TOK_MAX) return 0;
  for (k = 0; k < n; k++) t->t[k] = (u8)s[k];
  t->len = (u8)n;
  return 1;
}

static int parse_bool(const char* v, u8* out)
{
  if (es_icmp(v, "1") == 0 || es_icmp(v, "yes") == 0 || es_icmp(v, "true") == 0) { *out = 1; return 1; }
  if (es_icmp(v, "0") == 0 || es_icmp(v, "no") == 0 || es_icmp(v, "false") == 0) { *out = 0; return 1; }
  return 0;
}

static int parse_charset(GenLexer* g, const char* v, u8 bits)
{
  u32 n = es_len(v), i;
  for (i = 0; i < n; i++) {
    u8 a = (u8)v[i];
    if (a == ' ' || a == '\t') continue;
    if (a < 33 || a > 126) return 0;
    if (i + 2 < n && v[i + 1] == '-' && (u8)v[i + 2] > a) {
      u8 b = (u8)v[i + 2], c;
      if (b > 126) return 0;
      for (c = a; c <= b; c++) g->cls[c] |= bits;
      i += 2;
      continue;
    }
    g->cls[a] |= bits;
  }
  return 1;
}

// appends ';'-joined lowercase items from v to dst (cap bytes); strips "*." / "." from extensions
static int add_list(char* dst, u32 cap, const char* v, int ext)
{
  char item[64];
  const char* p = v;
  for (;;) {
    u32 n = 0, cur;
    while (*p == ' ' || *p == '\t' || *p == ';' || *p == ',') p++;
    if (!*p) return 1;
    while (*p && *p != ' ' && *p != '\t' && *p != ';' && *p != ',') {
      char c = *p++;
      if (n + 1 >= sizeof(item)) return 0;
      item[n++] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    item[n] = 0;
    if (ext) {
      const char* s = item;
      if (s[0] == '*' && s[1] == '.') s += 2;
      else if (s[0] == '.') s += 1;
      if (!*s) continue;
      if (s != item) memmove(item, s, es_len(s) + 1);
    }
    cur = es_len(dst);
    if (cur + (cur ? 1 : 0) + es_len(item) + 1 > cap) return 0;
    if (cur) es_cat(dst, cap, ";");
    es_cat(dst, cap, item);
  }
}

static int add_words(KwTable* t, const char* v, u8 cls)
{
  char w[KW_MAX_LEN + 2];
  const char* p = v;
  for (;;) {
    int n = next_token(&p, w, sizeof(w));
    if (n < 0) return E_RANGE;
    if (n == 0) return E_OK;
    if (t->add(w, (u32)n, cls) != E_OK) return E_NOMEM;
  }
}

struct LngSeen { u8 braces, ident_start, ident_part, comment, comment_block, id; };

// keywords are collected as "<class> words\n" lines and added after the whole file is read (case= may come last)
static int lng_key(const char* key, char* val, LangInfo* out, LngExtra* ex, GenLexer* g, LngSeen* seen, EStr* words,
                   EStr* msg)
{
  char t1[64], t2[64], t3[64];
  const char* p = val;
  int n;
  u8 b;
  u32 i;

  if (es_icmp(key, "id") == 0) {
    u32 len = es_len(val);
    if (len == 0 || len > 15) { msg->append("id must be 1-15 characters"); return E_SYNTAX; }
    for (i = 0; i < len; i++) {
      char c = val[i];
      if (!(lex_is_alnum((u8)c) || c == '_' || c == '-')) { msg->append("id may contain only letters, digits, _ and -"); return E_SYNTAX; }
      out->id[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    out->id[len] = 0;
    seen->id = 1;
    return E_OK;
  }
  if (es_icmp(key, "name") == 0) {
    if (!*val || es_len(val) > 31) { msg->append("name must be 1-31 characters"); return E_SYNTAX; }
    es_copy(out->name, sizeof(out->name), val);
    return E_OK;
  }
  if (es_icmp(key, "lexer") == 0) {
    if (es_icmp(val, "generic") == 0) ex->kind = LK_GENERIC;
    else if (es_icmp(val, "html") == 0) ex->kind = LK_HTML;
    else if (es_icmp(val, "xml") == 0) ex->kind = LK_XML;
    else if (es_icmp(val, "markdown") == 0 || es_icmp(val, "md") == 0) ex->kind = LK_MD;
    else { msg->append("lexer must be generic, html, xml or markdown"); return E_SYNTAX; }
    return E_OK;
  }
  if (es_icmp(key, "extensions") == 0) {
    if (!add_list(out->exts, sizeof(out->exts), val, 1)) { msg->append("extensions list too long"); return E_SYNTAX; }
    return E_OK;
  }
  if (es_icmp(key, "names") == 0) {
    if (!add_list(out->names, sizeof(out->names), val, 0)) { msg->append("names list too long"); return E_SYNTAX; }
    return E_OK;
  }
  if (es_icmp(key, "case") == 0) {
    if (!parse_bool(val, &b)) { msg->append("case must be 0 or 1"); return E_SYNTAX; }
    out->case_sensitive = b; g->case_sens = b;
    return E_OK;
  }
  if (es_icmp(key, "tab_width") == 0) {
    u32 v;
    if (es_parse_u32(val, &v, 10) != E_OK || v > 16) { msg->append("tab_width must be 0-16"); return E_SYNTAX; }
    out->tab_width = (u8)v;
    return E_OK;
  }
  if (es_icmp(key, "use_tabs") == 0) {
    u32 v;
    if (es_parse_u32(val, &v, 10) != E_OK || v > 2) { msg->append("use_tabs must be 0, 1 or 2"); return E_SYNTAX; }
    out->use_tabs = (u8)v;
    return E_OK;
  }
  if (es_icmp(key, "braces") == 0) {
    u32 len = es_len(val);
    if (len % 2 || len > 6) { msg->append("braces must be up to 3 pairs, e.g. ()[]{}"); return E_SYNTAX; }
    es_copy(out->braces, sizeof(out->braces), val);
    seen->braces = 1;
    return E_OK;
  }
  if (es_icmp(key, "comment") == 0) {
    n = next_token(&p, t1, sizeof(t1));
    if (n <= 0 || n > 8) { msg->append("comment must be a token of 1-8 characters"); return E_SYNTAX; }
    for (i = 0; i < (u32)n; i++) out->line_comment[i] = (u8)t1[i];
    out->line_comment_len = (u8)n;
    seen->comment = 1;
    return E_OK;
  }
  if (es_icmp(key, "comment_block") == 0) {
    int n2;
    n = next_token(&p, t1, sizeof(t1));
    n2 = next_token(&p, t2, sizeof(t2));
    if (n <= 0 || n > 4 || n2 <= 0 || n2 > 4) { msg->append("comment_block needs two tokens of 1-4 characters"); return E_SYNTAX; }
    for (i = 0; i < (u32)n; i++) out->block_open[i] = (u8)t1[i];
    for (i = 0; i < (u32)n2; i++) out->block_close[i] = (u8)t2[i];
    out->block_open_len = (u8)n; out->block_close_len = (u8)n2;
    seen->comment_block = 1;
    return E_OK;
  }
  if (es_icmp(key, "line_comment") == 0) {
    GenTok* t;
    if (g->nlc >= GEN_MAX_LC) { msg->append("too many line_comment entries (max 4)"); return E_SYNTAX; }
    t = &g->lc[g->nlc];
    n = next_token(&p, t1, sizeof(t1));
    if (n <= 0 || !set_tok(t, t1, GEN_TOK_MAX)) { msg->append("line_comment needs a token of 1-8 characters"); return E_SYNTAX; }
    n = next_token(&p, t2, sizeof(t2));
    if (n > 0) {
      if (es_icmp(t2, "start") != 0) { msg->append("line_comment flag must be 'start'"); return E_SYNTAX; }
      t->start_only = 1;
    }
    g->nlc++;
    return E_OK;
  }
  if (es_icmp(key, "block_comment") == 0) {
    if (g->nbc >= GEN_MAX_BC) { msg->append("too many block_comment entries (max 2)"); return E_SYNTAX; }
    if (next_token(&p, t1, sizeof(t1)) <= 0 || next_token(&p, t2, sizeof(t2)) <= 0 ||
        !set_tok(&g->bo[g->nbc], t1, GEN_TOK_MAX) || !set_tok(&g->bc[g->nbc], t2, GEN_TOK_MAX)) {
      msg->append("block_comment needs open and close tokens of 1-8 characters");
      return E_SYNTAX;
    }
    g->nbc++;
    return E_OK;
  }
  if (es_icmp(key, "nested") == 0) {
    if (!parse_bool(val, &g->nested)) { msg->append("nested must be 0 or 1"); return E_SYNTAX; }
    return E_OK;
  }
  if (es_icmp(key, "comment_continuation") == 0) {
    if (!parse_bool(val, &g->lc_cont)) { msg->append("comment_continuation must be 0 or 1"); return E_SYNTAX; }
    return E_OK;
  }
  if (es_icmp(key, "comment_word") == 0) {
    for (;;) {
      n = next_token(&p, t1, sizeof(t1));
      if (n == 0) return E_OK;
      if (n < 0 || n > 15 || g->ncword >= GEN_MAX_CWORD) { msg->append("comment_word: at most 4 words of 1-15 characters"); return E_SYNTAX; }
      es_copy(g->cword[g->ncword++], 16, t1);
    }
  }
  if (es_icmp(key, "string") == 0) {
    GenStr* s;
    if (g->nstr >= GEN_MAX_STR) { msg->append("too many string entries (max 4)"); return E_SYNTAX; }
    s = &g->str[g->nstr];
    s->style = ST_STRING;
    if (next_token(&p, t1, sizeof(t1)) <= 0 || next_token(&p, t2, sizeof(t2)) <= 0 ||
        !set_tok(&s->open, t1, GEN_TOK_MAX) || !set_tok(&s->close, t2, GEN_TOK_MAX)) {
      msg->append("string needs open and close tokens of 1-8 characters");
      return E_SYNTAX;
    }
    for (;;) {
      n = next_token(&p, t3, sizeof(t3));
      if (n == 0) break;
      if (n < 0) { msg->append("string: token too long"); return E_SYNTAX; }
      if (n == 1) s->esc = (u8)t3[0];
      else if (es_icmp(t3, "none") == 0) s->esc = 0;
      else if (es_icmp(t3, "char") == 0) s->style = ST_CHAR;
      else if (es_icmp(t3, "multiline") == 0) s->multiline = 1;
      else { msg->append("string: unknown flag (use an escape character, none, char or multiline)"); return E_SYNTAX; }
    }
    g->nstr++;
    return E_OK;
  }
  if (es_icmp(key, "string_prefixes") == 0) {
    char w[KW_MAX_LEN + 2];
    while ((n = next_token(&p, w, sizeof(w))) > 0) {}
    if (n < 0) { msg->append("string prefix longer than 47 characters"); return E_SYNTAX; }
    if (words->append("p ") != E_OK || words->append(val) != E_OK || words->append("\n") != E_OK) return E_NOMEM;
    return E_OK;
  }
  if (es_icmp(key, "preprocessor") == 0 || es_icmp(key, "decorator") == 0) {
    if (es_len(val) != 1 || (u8)val[0] < 33 || (u8)val[0] > 126) { msg->append("preprocessor/decorator must be one character"); return E_SYNTAX; }
    if (key[0] == 'p' || key[0] == 'P') g->pp_char = (u8)val[0]; else g->deco_char = (u8)val[0];
    return E_OK;
  }
  if (es_icmp(key, "number") == 0) {
    if (es_icmp(val, "c") == 0 || es_icmp(val, "asm") == 0) g->number = GN_C;
    else if (es_icmp(val, "basic") == 0) g->number = GN_BASIC;
    else if (es_icmp(val, "pascal") == 0) g->number = GN_PASCAL;
    else if (es_icmp(val, "none") == 0) g->number = GN_NONE;
    else { msg->append("number must be c, asm, basic, pascal or none"); return E_SYNTAX; }
    return E_OK;
  }
  if (es_icmp(key, "variables") == 0) {
    if (es_icmp(val, "make") == 0) g->vars = GV_MAKE;
    else if (es_icmp(val, "bat") == 0) g->vars = GV_BAT;
    else if (es_icmp(val, "percent") == 0) g->vars = GV_PERCENT;
    else if (es_icmp(val, "none") == 0) g->vars = GV_NONE;
    else { msg->append("variables must be make, bat, percent or none"); return E_SYNTAX; }
    return E_OK;
  }
  if (es_icmp(key, "label") == 0) {
    if (es_icmp(val, "colon_prefix") == 0) g->label = GL_COLON_PREFIX;
    else if (es_icmp(val, "colon_suffix") == 0) g->label = GL_COLON_SUFFIX;
    else if (es_icmp(val, "asm") == 0) g->label = GL_ASM;
    else if (es_icmp(val, "none") == 0) g->label = GL_NONE;
    else { msg->append("label must be colon_prefix, colon_suffix, asm or none"); return E_SYNTAX; }
    return E_OK;
  }
  if (es_icmp(key, "regex") == 0) { if (!parse_bool(val, &g->regex)) { msg->append("regex must be 0 or 1"); return E_SYNTAX; } return E_OK; }
  if (es_icmp(key, "ini_section") == 0) { if (!parse_bool(val, &g->ini_section)) { msg->append("ini_section must be 0 or 1"); return E_SYNTAX; } return E_OK; }
  if (es_icmp(key, "ini_key") == 0) { if (!parse_bool(val, &g->ini_key)) { msg->append("ini_key must be 0 or 1"); return E_SYNTAX; } return E_OK; }
  if (es_icmp(key, "make_rules") == 0) { if (!parse_bool(val, &g->make_rules)) { msg->append("make_rules must be 0 or 1"); return E_SYNTAX; } return E_OK; }
  if (es_icmp(key, "ident_start") == 0 || es_icmp(key, "ident_part") == 0) {
    int start = (key[6] == 's' || key[6] == 'S');
    if (start) { if (!seen->ident_start) { for (i = 0; i < 128; i++) g->cls[i] &= (u8)~CC_IDS; } seen->ident_start = 1; }
    else { if (!seen->ident_part) { for (i = 0; i < 128; i++) g->cls[i] &= (u8)~CC_IDP; } seen->ident_part = 1; }
    if (!parse_charset(g, val, start ? (u8)(CC_IDS | CC_IDP) : (u8)CC_IDP)) { msg->append("bad character set"); return E_SYNTAX; }
    return E_OK;
  }
  if (es_icmp(key, "operators") == 0) {
    for (i = 0; val[i]; i++) {
      u8 c = (u8)val[i];
      if (c == ' ' || c == '\t') continue;
      if (c < 33 || c > 126) { msg->append("operators must be printable ASCII"); return E_SYNTAX; }
      g->cls[c] |= CC_OP;
    }
    return E_OK;
  }
  if (es_nicmp(key, "keywords", 8) == 0 && key[8] >= '1' && key[8] <= '4' && key[9] == 0) {
    char w[KW_MAX_LEN + 2];
    char cls[3];
    while ((n = next_token(&p, w, sizeof(w))) > 0) {}
    if (n < 0) { msg->append("keyword longer than 47 characters"); return E_SYNTAX; }
    cls[0] = key[8]; cls[1] = ' '; cls[2] = 0;
    if (words->append(cls) != E_OK || words->append(val) != E_OK || words->append("\n") != E_OK) return E_NOMEM;
    return E_OK;
  }
  if (es_icmp(key, "line") == 0) {
    int sty;
    GenLine* l;
    if (g->nline >= GEN_MAX_LINE) { msg->append("too many line rules (max 16)"); return E_SYNTAX; }
    l = &g->line[g->nline];
    if (next_token(&p, t1, sizeof(t1)) <= 0 || !set_tok(&l->prefix, t1, GEN_TOK_MAX) ||
        next_token(&p, t2, sizeof(t2)) <= 0 || (sty = lng_style_from_name(t2)) < 0) {
      msg->append("line needs a prefix (1-8 characters) and a style name");
      return E_SYNTAX;
    }
    l->style = (u8)sty;
    g->nline++;
    return E_OK;
  }
  if (es_icmp(key, "script") == 0 || es_icmp(key, "style") == 0) {
    char* dst = (key[1] == 'c' || key[1] == 'C') ? ex->script : ex->style;
    if (es_len(val) > 15) { msg->append("script/style must name a language id"); return E_SYNTAX; }
    es_copy(dst, 16, val);
    for (i = 0; dst[i]; i++) if (dst[i] >= 'A' && dst[i] <= 'Z') dst[i] = (char)(dst[i] + 32);
    return E_OK;
  }
  msg->appendf("unknown key '%.40s'", key);
  return E_SYNTAX;
}

int lng_parse(const char* text, u32 len, LangInfo* out, LngExtra* ex, Lexer** lex, EStr* err)
{
  GenLexer* g;
  LngSeen seen;
  EStr msg, words;
  char line[1024];
  u32 pos = 0, lineno = 0, i;
  int r = E_OK;

  memset(out, 0, sizeof(*out));
  memset(ex, 0, sizeof(*ex));
  memset(&seen, 0, sizeof(seen));
  *lex = 0;
  if (!text) len = 0;
  for (i = 0; i < len; i++) if (text[i] == 0) { len = i; break; }   // RCDATA may be NUL-padded
  if (len >= 3 && (u8)text[0] == 0xEF && (u8)text[1] == 0xBB && (u8)text[2] == 0xBF) pos = 3;
  g = new GenLexer;
  if (!g) return E_NOMEM;
  out->case_sensitive = 1;
  out->use_tabs = 2;
  g->cls['_'] = CC_IDS | CC_IDP;
  for (i = 'A'; i <= 'Z'; i++) g->cls[i] = g->cls[i + 32] = CC_IDS | CC_IDP;
  for (i = '0'; i <= '9'; i++) g->cls[i] = CC_IDP;

  while (pos < len && r == E_OK) {
    u32 e = pos, n = 0;
    char* key;
    char* val;
    lineno++;
    while (e < len && text[e] != '\n') e++;
    if (e - pos >= sizeof(line)) { msg.appendf("line %u: line too long (max 1023 characters)", lineno); r = E_SYNTAX; break; }
    for (i = pos; i < e; i++) {
      u8 c = (u8)text[i];
      if (c == '\r') continue;
      if (c == '\t') c = ' ';
      if (c < 32 || c > 126) { msg.appendf("line %u: non-ASCII or control character (byte 0x%02X)", lineno, (u32)c); r = E_SYNTAX; break; }
      line[n++] = (char)c;
    }
    pos = e + 1;
    if (r != E_OK) break;
    line[n] = 0;
    key = es_trim(line);
    if (!*key || key[0] == ';' || key[0] == '#') continue;
    if (key[0] == '[') {
      if (es_icmp(key, "[language]") != 0) { msg.appendf("line %u: unknown section %.40s", lineno, key); r = E_SYNTAX; }
      continue;
    }
    if (!es_kv(key, &key, &val)) { msg.appendf("line %u: expected key=value", lineno); r = E_SYNTAX; break; }
    {
      EStr m2;
      r = lng_key(key, val, out, ex, g, &seen, &words, &m2);
      if (r == E_NOMEM) { msg.append("out of memory"); break; }
      if (r != E_OK) { msg.appendf("line %u: %s", lineno, m2.c_str()); break; }
    }
  }
  if (r == E_OK && !out->name[0]) { msg.append("missing name="); r = E_SYNTAX; }
  if (r == E_OK) {                                         // keyword lists, now that case= is known
    const char* w = words.c_str();
    g->kw.set_fold(!g->case_sens);
    g->prefixes.set_fold(!g->case_sens);
    while (*w && r == E_OK) {
      const char* e = w;
      char buf[1100];
      while (*e && *e != '\n') e++;
      if ((u32)(e - w) < sizeof(buf)) {
        memcpy(buf, w + 2, (u32)(e - w) - 2);
        buf[(e - w) - 2] = 0;
        r = add_words(w[0] == 'p' ? &g->prefixes : &g->kw, buf, w[0] == 'p' ? (u8)1 : (u8)(w[0] - '0'));
        if (r != E_OK) { msg.append(r == E_NOMEM ? "out of memory" : "bad keyword"); if (r != E_NOMEM) r = E_SYNTAX; }
      }
      w = *e ? e + 1 : e;
    }
  }
  if (r != E_OK) {
    delete g;
    if (err) err->append(msg.c_str());
    return r;
  }
  if (!seen.braces) es_copy(out->braces, sizeof(out->braces), ex->kind == LK_GENERIC ? "()[]{}" : "");
  if (!seen.comment && g->nlc && g->lc[0].len) {
    for (i = 0; i < g->lc[0].len; i++) out->line_comment[i] = g->lc[0].t[i];
    out->line_comment_len = g->lc[0].len;
  }
  if (!seen.comment_block && g->nbc && g->bo[0].len <= 4 && g->bc[0].len <= 4) {
    for (i = 0; i < g->bo[0].len; i++) out->block_open[i] = g->bo[0].t[i];
    for (i = 0; i < g->bc[0].len; i++) out->block_close[i] = g->bc[0].t[i];
    out->block_open_len = g->bo[0].len; out->block_close_len = g->bc[0].len;
  }
  if (ex->kind == LK_HTML && !ex->script[0]) es_copy(ex->script, 16, "js");
  if (ex->kind == LK_HTML && !ex->style[0]) es_copy(ex->style, 16, "css");
  g->finish();
  if (ex->kind == LK_GENERIC) {
    *lex = g;
  } else if (ex->kind == LK_HTML || ex->kind == LK_XML) {
    HtmlLexer* h = new HtmlLexer(ex->kind == LK_XML, g);
    if (!h) { delete g; return E_NOMEM; }
    *lex = h;
  } else {
    MdLexer* m = new MdLexer;
    delete g;
    if (!m) return E_NOMEM;
    *lex = m;
  }
  return E_OK;
}
