// search.cpp - Searcher and Replace All (DESIGN 6.15, 11). Owner: T03.
// Normal and Extended modes compile to literal regex programs (the matcher then runs the Horspool prefilter, and
// pure literals never enter the VM); Regex mode compiles the pattern; Hex mode searches raw bytes with ? wildcard
// digits (hexfmt.cpp parses the bytes).
// Matching semantics: [lo, hi] is the scope. Forward: the leftmost match starting in [from, hi] and ending <= hi;
// assertions see the text in [lo, hi] only (lo and hi act as the text ends). Backward: the last match of the forward
// iteration that starts before `from` and ends <= from (assertions see up to hi). Iterations (count, find_all,
// replace_all) continue at the end of each match; after an empty match one character later.
#include "search.h"
#include "regex_int.h"
#include "hexfmt.h"
#include "estr.h"
#include <string.h>

static void set_err(SearchError* err, int code, u32 pos, const char* msg)
{
  if (err) {
    err->code = code;
    err->pos = pos;
    es_copy(err->msg, sizeof(err->msg), msg);
  }
}

static int hexv(ecp c)
{
  if (c >= '0' && c <= '9') return (int)(c - '0');
  if (c >= 'a' && c <= 'f') return (int)(c - 'a' + 10);
  if (c >= 'A' && c <= 'F') return (int)(c - 'A' + 10);
  return -1;
}

static int read_hexn(const ecp* in, u32 n, u32 at, u32 count, u32* v)
{
  u32 k, r = 0;
  if (at + count > n) return 0;
  for (k = 0; k < count; k++) {
    int h = hexv(in[at + k]);
    if (h < 0) return 0;
    r = r * 16 + (u32)h;
  }
  *v = r;
  return 1;
}

int search_parse_extended(const ecp* in, u32 n, Vec<ecp>* out, SearchError* err)
{
  u32 i = 0;
  if (err) { err->code = E_OK; err->pos = 0; err->msg[0] = 0; }
  if (!out) return E_RANGE;
  out->clear();
  while (i < n) {
    ecp c = in[i], v = 0;
    u32 hv = 0;
    if (c != '\\') {
      if (out->push(c) != E_OK) { set_err(err, E_NOMEM, i, "Out of memory"); return E_NOMEM; }
      i++;
      continue;
    }
    if (i + 1 >= n) { set_err(err, E_SYNTAX, i, "The text ends with a backslash (use \\\\ for one)"); return E_SYNTAX; }
    switch (in[i + 1]) {
    case 'n': v = 10; i += 2; break;
    case 'r': v = 13; i += 2; break;
    case 't': v = 9; i += 2; break;
    case '0': v = 0; i += 2; break;
    case '\\': v = '\\'; i += 2; break;
    case 'x':
      if (!read_hexn(in, n, i + 2, 2, &hv)) { set_err(err, E_SYNTAX, i, "\\x needs two hex digits"); return E_SYNTAX; }
      v = hv;
      i += 4;
      break;
    case 'u':
      if (!read_hexn(in, n, i + 2, 4, &hv)) { set_err(err, E_SYNTAX, i, "\\u needs four hex digits"); return E_SYNTAX; }
      if (hv >= 0xD800 && hv <= 0xDBFF) {
        u32 lo = 0;
        if (i + 12 <= n && in[i + 6] == '\\' && in[i + 7] == 'u' && read_hexn(in, n, i + 8, 4, &lo) &&
            lo >= 0xDC00 && lo <= 0xDFFF) {
          v = 0x10000 + ((hv - 0xD800) << 10) + (lo - 0xDC00);
          i += 12;
          break;
        }
      }
      if (hv >= 0xD800 && hv <= 0xDFFF) { set_err(err, E_SYNTAX, i, "Invalid character code (lone surrogate)"); return E_SYNTAX; }
      v = hv;
      i += 6;
      break;
    case 'd': {
      u32 k, d = 0;
      for (k = 0; k < 3; k++) {
        ecp e = i + 2 + k < n ? in[i + 2 + k] : 0;
        if (e < '0' || e > '9') { set_err(err, E_SYNTAX, i, "\\d needs three decimal digits (000-255)"); return E_SYNTAX; }
        d = d * 10 + (e - '0');
      }
      if (d > 255) { set_err(err, E_SYNTAX, i, "\\dNNN must be 000-255"); return E_SYNTAX; }
      v = d;
      i += 5;
      break;
    }
    default:
      set_err(err, E_SYNTAX, i, "Invalid escape (use \\n \\r \\t \\0 \\\\ \\xHH \\uHHHH \\dNNN)");
      return E_SYNTAX;
    }
    if (out->push(v) != E_OK) { set_err(err, E_NOMEM, i, "Out of memory"); return E_NOMEM; }
  }
  return E_OK;
}

// Hex bytes use hexfmt's parsers (same syntax as the hex view: "DE AD ?? EF", "4?", "0x4D,0x5A", "4Dh").
static int parse_hex_pattern(const ecp* p, u32 n, EBytes* bytes, EBytes* mask, SearchError* err)
{
  int r = hex_parse_pattern(p, n, bytes, mask);
  if (r == E_NOMEM) { set_err(err, E_NOMEM, 0, "Out of memory"); return E_NOMEM; }
  if (r != E_OK || bytes->empty() || mask->size() != bytes->size()) {
    set_err(err, E_SYNTAX, 0, "Invalid hex bytes (use pairs like DE AD ?\? EF; ? is a wildcard digit)");
    return E_SYNTAX;
  }
  return E_OK;
}

static int parse_hex_bytes(const ecp* p, u32 n, EBytes* out)
{
  char buf[RX_MAX_PATTERN + 1];
  u32 k;
  int r;
  if (n > RX_MAX_PATTERN) return E_SYNTAX;
  for (k = 0; k < n; k++) {
    if (p[k] >= 128) return E_SYNTAX;
    buf[k] = (char)p[k];
  }
  buf[n] = 0;
  if (n == 0) { out->clear(); return E_OK; }
  r = hex_parse_bytes(buf, n, out);
  return r == E_OK || r == E_NOMEM ? r : E_SYNTAX;
}

// Progress adaptor: inner calls report bytes done from their own start; the outer callback sees the whole scope.
class SearchSpanCb : public ProgressCb {
public:
  ProgressCb* outer; epos lo; epos total; epos base; int fixed; epos fixed_done; epos next;
  SearchSpanCb(ProgressCb* o, epos l, epos t) : outer(o), lo(l), total(t), base(l), fixed(0), fixed_done(0), next(l + 65536) {}
  int tick(epos pos)                 // between the searches of an iteration: nonzero = cancel
  {
    epos d;
    if (!outer || pos < next) return 0;
    next = pos + 65536;
    if (next < pos) next = EPOS_NONE;
    d = pos - lo;
    if (d > total) d = total;
    return outer->progress(d, total);
  }
  int progress(u32 done, u32 t)
  {
    epos d;
    (void)t;
    if (!outer) return 0;
    d = fixed ? fixed_done : base - lo + done;
    if (d > total) d = total;
    return outer->progress(d, total);
  }
};

static epos next_char(ByteSource* s, const Codec* c, int bytes, epos pos)
{
  RxReader rd;
  ecp cp;
  u32 len;
  if (bytes || !c) return pos + 1;
  rxr_init(&rd, s, c);
  len = rxr_decode(&rd, pos, &cp);
  return pos + (len ? len : 1);
}

// ---------------------------------------------------------------------------------------------------------------

Searcher::Searcher() : prog_(0), impossible_(0), compiled_(0)
{
  opts_.mode = SM_NORMAL; opts_.match_case = 0; opts_.whole_word = 0; opts_.dot_all = 0;
  enc_ = enc_ansi();
}

Searcher::~Searcher()
{
  mt_.init(0);
  rx_free(prog_);
}

int Searcher::compile(const ecp* pat, u32 n, const SearchOpts& o, const EncSpec& doc_enc, SearchError* err)
{
  RxError re;
  Vec<ecp> lit;
  const Codec* c;
  u32 flags = 0;
  int r;
  mt_.init(0);
  rx_free(prog_);
  prog_ = 0;
  compiled_ = 0;
  impossible_ = 0;
  hex_.clear();
  hexmask_.clear();
  opts_ = o;
  enc_ = doc_enc;
  if (err) { err->code = E_OK; err->pos = 0; err->msg[0] = 0; }
  if (n == 0 || !pat) { set_err(err, E_SYNTAX, 0, "Nothing to find"); return E_SYNTAX; }
  if (n > RX_MAX_PATTERN) {
    set_err(err, E_TOOBIG, RX_MAX_PATTERN, "The search text is too long (max 1000 characters)");
    return E_TOOBIG;
  }
  if (o.mode == SM_HEX) {
    r = parse_hex_pattern(pat, n, &hex_, &hexmask_, err);
    if (r != E_OK) return r;
    compiled_ = 1;
    return E_OK;
  }
  if (!o.match_case) flags |= RXF_ICASE;
  if (o.whole_word) flags |= RXF_WORD;
  if (o.mode == SM_REGEX) {
    if (o.dot_all) flags |= RXF_DOTALL;
    r = rx_compile(pat, n, flags, &prog_, &re);
  } else if (o.mode == SM_EXTENDED) {
    SearchError xe;
    r = search_parse_extended(pat, n, &lit, &xe);
    if (r != E_OK) { if (err) *err = xe; return r; }
    if (lit.empty()) { set_err(err, E_SYNTAX, 0, "Nothing to find"); return E_SYNTAX; }
    r = rx_compile(lit.data(), lit.size(), flags | RXF_LITERAL_, &prog_, &re);
  } else if (o.mode == SM_NORMAL) {
    r = rx_compile(pat, n, flags | RXF_LITERAL_, &prog_, &re);
  } else {
    set_err(err, E_RANGE, 0, "Unknown search mode");
    return E_RANGE;
  }
  if (r != E_OK) {
    if (err) { err->code = re.code; err->pos = re.pos; es_copy(err->msg, sizeof(err->msg), re.msg); }
    return r;
  }
  r = mt_.init(prog_);
  if (r != E_OK) {
    rx_free(prog_);
    prog_ = 0;
    set_err(err, r, 0, "Out of memory");
    return r;
  }
  c = codec_get(doc_enc);
  if (c) {
    int ic = 0;
    u32 k;
    lit.clear();
    if (rx_literal_prefix(prog_, &lit, &ic) == E_OK)
      for (k = 0; k < lit.size(); k++)
        if (!rxpre_encodable(c, lit[k], ic)) { impossible_ = 1; break; }
  }
  compiled_ = 1;
  return E_OK;
}

int Searcher::find_hex_(ByteSource* s, epos from, epos hi, RxMatch* m, ProgressCb* cb)
{
  u32 plen = hex_.size(), aoff = 0, alen = 0, k, run = 0;
  const u8* hb = hex_.data();
  const u8* hm = hexmask_.data();
  RxPre pf;
  RxProgress g;
  epos pos;
  if (hi < plen || from > hi - plen) return 0;
  for (k = 0; k <= plen; k++) {      // longest run of fully fixed bytes is the anchor
    if (k < plen && hm[k] == 0xFF) { run++; continue; }
    if (run > alen) { alen = run; aoff = k - run; }
    run = 0;
  }
  rxp_init(&g, cb, from, hi - from);
  if (alen == 0) {                   // no fully fixed byte: check every position
    u8 tmp[RX_MAX_PATTERN];
    for (pos = from; ; pos++) {
      int r;
      if (s->read(pos, tmp, plen) != E_OK) return s->err() != E_OK ? s->err() : E_IO;
      for (k = 0; k < plen; k++) if ((tmp[k] & hm[k]) != (hb[k] & hm[k])) break;
      if (k == plen) break;
      if (pos >= hi - plen) return 0;
      if ((r = rxp_tick(&g, pos)) != E_OK) return r;
    }
    m->pos = pos;
    m->len = plen;
  } else {
    rxpre_bytes(&pf, hb + aoff, alen);
    pos = from + aoff;
    for (;;) {
      epos q = 0, start;
      u8 tmp[RX_MAX_PATTERN];
      int r = rxpre_find(&pf, s, pos, hi - plen + aoff, hi - (plen - aoff - alen), 0, &q, &g);
      if (r <= 0) return r;
      start = q - aoff;
      if (s->read(start, tmp, plen) != E_OK) return s->err() != E_OK ? s->err() : E_IO;
      for (k = 0; k < plen; k++) if ((tmp[k] & hm[k]) != (hb[k] & hm[k])) break;
      if (k == plen) { m->pos = start; m->len = plen; break; }
      pos = q + 1;
    }
  }
  m->gpos[0] = m->pos;
  m->glen[0] = m->len;
  for (k = 1; k < RX_GROUPS; k++) { m->gpos[k] = EPOS_NONE; m->glen[k] = 0; }
  return 1;
}

int Searcher::find_fwd_(ByteSource* s, const Codec* c, epos lo, epos from, epos hi, epos ctx, RxMatch* m,
                        ProgressCb* cb)
{
  if (opts_.mode == SM_HEX) return find_hex_(s, from, hi, m, cb);
  mt_.set_context_end(ctx);
  return mt_.search(s, c, lo, from, hi, m, cb);
}

// Block start for a backward search round: a character boundary at or before b, moved forward to just after the
// first line break in [b, limit) so the forward iteration is in phase with one started at lo.
static epos back_block_start(ByteSource* s, const Codec* c, int bytes, epos lo, epos b, epos limit)
{
  u8 buf[512];
  epos at, end;
  if (bytes || b <= lo) return b < lo ? lo : b;
  if (c->unit == 2) {
    u8 u[4];
    if ((b - lo) & 1) b--;
    if (b >= lo + 2 && s->read(b - 2, u, 4) == E_OK) {      // don't split a surrogate pair
      u32 u0 = c->kind == EK_UTF16BE ? ((u32)u[0] << 8 | u[1]) : ((u32)u[1] << 8 | u[0]);
      u32 u1 = c->kind == EK_UTF16BE ? ((u32)u[2] << 8 | u[3]) : ((u32)u[3] << 8 | u[2]);
      if (u0 >= 0xD800 && u0 <= 0xDBFF && u1 >= 0xDC00 && u1 <= 0xDFFF) b -= 2;
    }
  } else if (c->max_len > 1 && c->self_sync) {            // UTF-8: back over continuation bytes
    u32 k;
    for (k = 0; k < 3 && b > lo; k++) {
      u8 x;
      if (s->read(b, &x, 1) != E_OK || (x & 0xC0) != 0x80) break;
      b--;
    }
  } else if (c->max_len > 1) {                            // DBCS: only after a CR/LF byte (never a trail byte)
    at = b;
    while (at > lo) {
      epos n = at - lo, k;
      if (n > sizeof(buf)) n = sizeof(buf);
      if (s->read(at - n, buf, n) != E_OK) return lo;
      for (k = n; k > 0; k--) if (buf[k - 1] == 10 || buf[k - 1] == 13) break;
      if (k > 0) { at = at - n + k; break; }
      at -= n;
    }
    b = at;
  }
  // sync to a line start
  at = b;
  end = limit;
  if (end > b + 65536) end = b + 65536;
  while (at < end) {
    epos n = end - at, k;
    if (n > sizeof(buf)) n = sizeof(buf);
    if (n < c->unit) break;
    if (s->read(at, buf, n) != E_OK) return b;
    for (k = 0; k + c->unit <= n; k += c->unit) {
      u32 ch = c->unit == 2 ? (c->kind == EK_UTF16BE ? ((u32)buf[k] << 8 | buf[k + 1]) : ((u32)buf[k + 1] << 8 | buf[k]))
                            : buf[k];
      if (ch == 10) {
        epos x = at + k + c->unit;
        return x < limit ? x : b;
      }
      if (ch == 13) {
        epos x = at + k + c->unit;
        u8 nb[2];
        if (x + c->unit <= s->size() && s->read(x, nb, c->unit) == E_OK) {
          u32 nx = c->unit == 2 ? (c->kind == EK_UTF16BE ? ((u32)nb[0] << 8 | nb[1]) : ((u32)nb[1] << 8 | nb[0])) : nb[0];
          if (nx == 10) x += c->unit;
        }
        return x < limit ? x : b;
      }
    }
    at += k;
  }
  return b;
}

int Searcher::find_back_(ByteSource* s, const Codec* c, epos lo, epos hi, epos from, RxMatch* m, ProgressCb* cb)
{
  epos end = from, blk_hi = from, width = 2048;
  int bytes = opts_.mode == SM_HEX;
  SearchSpanCb scb(cb, lo, from - lo);
  scb.fixed = 1;
  if (end <= lo) return 0;
  for (;;) {
    epos b = blk_hi - lo > width ? blk_hi - width : lo, pos;
    RxMatch mm, best;
    int have = 0, r;
    b = back_block_start(s, c, bytes, lo, b, blk_hi);
    scb.fixed_done = end - b;
    pos = b;
    for (;;) {
      r = find_fwd_(s, c, lo, pos, end, hi, &mm, cb ? &scb : 0);
      if (r < 0) return r;
      if (r == 0 || mm.pos >= blk_hi) break;
      best = mm;
      have = 1;
      pos = mm.len ? mm.pos + mm.len : next_char(s, c, bytes, mm.pos);
      if (pos > end) break;
    }
    if (have) { *m = best; return 1; }
    if (b <= lo) return 0;
    blk_hi = b;
    if (width < 0x40000000u) width *= 2;
  }
}

int Searcher::find(ByteSource* s, const Codec* c, epos lo, epos hi, epos from, int backward, RxMatch* m, ProgressCb* cb)
{
  epos size;
  if (!compiled_) return E_STATE;
  if (!s || !m) return E_RANGE;
  if (opts_.mode != SM_HEX && !c) return E_STATE;
  if (impossible_) return 0;
  size = s->size();
  if (hi > size) hi = size;
  if (lo > hi) return 0;
  if (from < lo) from = lo;
  if (from > hi) {
    if (!backward) return 0;
    from = hi;
  }
  if (!backward) return find_fwd_(s, c, lo, from, hi, hi, m, cb);
  return find_back_(s, c, lo, hi, from, m, cb);
}

int Searcher::count(ByteSource* s, const Codec* c, epos lo, epos hi, u32* n, ProgressCb* cb)
{
  RxMatch mm;
  epos pos = lo;
  SearchSpanCb scb(cb, lo, 0);
  int r, bytes = opts_.mode == SM_HEX;
  u32 cnt = 0;
  if (n) *n = 0;
  if (!s) return E_RANGE;
  if (hi > s->size()) hi = s->size();
  if (lo > hi) return E_OK;
  scb.total = hi - lo;
  for (;;) {
    scb.base = pos;
    r = find(s, c, lo, hi, pos, 0, &mm, cb ? &scb : 0);
    if (r < 0) { if (n) *n = cnt; return r; }
    if (r == 0) break;
    cnt++;
    pos = mm.len ? mm.pos + mm.len : next_char(s, c, bytes, mm.pos);
    if (pos > hi) break;
    if (scb.tick(pos)) { if (n) *n = cnt; return E_CANCEL; }
  }
  if (n) *n = cnt;
  return E_OK;
}

int Searcher::find_all(ByteSource* s, const Codec* c, epos lo, epos hi, Vec<MarkRange>* out, u32 max, ProgressCb* cb)
{
  RxMatch mm;
  epos pos = lo;
  SearchSpanCb scb(cb, lo, 0);
  int r, bytes = opts_.mode == SM_HEX;
  if (!out || !s) return E_RANGE;
  out->clear();
  if (hi > s->size()) hi = s->size();
  if (lo > hi) return E_OK;
  scb.total = hi - lo;
  for (;;) {
    MarkRange mr;
    if (out->size() >= max) return 1;
    scb.base = pos;
    r = find(s, c, lo, hi, pos, 0, &mm, cb ? &scb : 0);
    if (r < 0) return r;
    if (r == 0) break;
    mr.pos = mm.pos;
    mr.len = mm.len;
    if (out->push(mr) != E_OK) return E_NOMEM;
    pos = mm.len ? mm.pos + mm.len : next_char(s, c, bytes, mm.pos);
    if (pos > hi) break;
    if (scb.tick(pos)) return E_CANCEL;
  }
  return E_OK;
}

// ---- replacement expansion ----

struct CaseState { int mode; int one; };   // mode/one: 0 none, 1 upper, 2 lower

static ecp apply_case(CaseState* cs, ecp ch)
{
  int o = cs->one;
  if (o) cs->one = 0;
  if (ECP_IS_BAD(ch)) return ch;
  if (o) return o == 1 ? uc_upper(ch) : uc_lower(ch);
  if (cs->mode == 1) return uc_upper(ch);
  if (cs->mode == 2) return uc_lower(ch);
  return ch;
}

static int put_char(const Codec* c, ecp ch, EBytes* out, u32* bad)
{
  u8 buf[8];
  u32 n = c->encode(ch, buf);
  if (n == 0) {
    (*bad)++;
    n = c->encode('?', buf);
  }
  if (n && out->append(buf, n) != E_OK) return E_NOMEM;
  return E_OK;
}

static int put_group(ByteSource* s, const Codec* c, const RxMatch& m, u32 k, CaseState* cs, EBytes* out, u32* bad)
{
  epos gp, gl;
  if (k >= RX_GROUPS) return E_OK;
  gp = k == 0 ? m.pos : m.gpos[k];
  gl = k == 0 ? m.len : m.glen[k];
  if (gp == EPOS_NONE || gl == 0) return E_OK;
  if (gp > s->size() || gl > s->size() - gp) return E_RANGE;
  if (!cs->mode && !cs->one) {       // raw bytes: keeps undecodable bytes exactly
    u8 buf[1024];
    while (gl > 0) {
      epos n = gl > sizeof(buf) ? (epos)sizeof(buf) : gl;
      if (s->read(gp, buf, n) != E_OK) return s->err() != E_OK ? s->err() : E_IO;
      if (out->append(buf, n) != E_OK) return E_NOMEM;
      gp += n;
      gl -= n;
    }
    return E_OK;
  }
  {
    RxReader rd;
    epos at = gp, end = gp + gl;
    rxr_init(&rd, s, c);
    while (at < end) {
      ecp ch;
      u32 len = rxr_decode(&rd, at, &ch);
      int r;
      if (len == 0) return rd.err ? rd.err : E_IO;
      if ((r = put_char(c, apply_case(cs, ch), out, bad)) != E_OK) return r;
      at += len;
    }
  }
  return E_OK;
}

int Searcher::expand(const ecp* rep, u32 n, ByteSource* s, const Codec* c, const RxMatch& m, EBytes* out,
                     u32* unrepresentable)
{
  u32 bad = 0, i = 0;
  int r = E_OK;
  CaseState cs;
  if (unrepresentable) *unrepresentable = 0;
  if (!out) return E_RANGE;
  out->clear();
  if (!compiled_) return E_STATE;
  if (n && !rep) return E_RANGE;
  cs.mode = 0;
  cs.one = 0;
  if (opts_.mode == SM_HEX) {
    EBytes b;
    r = parse_hex_bytes(rep, n, &b);
    if (r != E_OK) return r;
    if (b.size() && out->append(b.data(), b.size()) != E_OK) return E_NOMEM;
    return E_OK;
  }
  if (!c) return E_STATE;
  if (opts_.mode == SM_NORMAL || opts_.mode == SM_EXTENDED) {
    Vec<ecp> lit;
    const ecp* p = rep;
    u32 k;
    if (opts_.mode == SM_EXTENDED) {
      r = search_parse_extended(rep, n, &lit, 0);
      if (r != E_OK) return r;
      p = lit.data();
      n = lit.size();
    }
    for (k = 0; k < n && r == E_OK; k++) r = put_char(c, p[k], out, &bad);
    if (unrepresentable) *unrepresentable = bad;
    return r;
  }
  while (i < n && r == E_OK) {
    ecp ch = rep[i];
    if (ch == '$' && i + 1 < n) {
      ecp d = rep[i + 1];
      if (d >= '0' && d <= '9') { r = put_group(s, c, m, d - '0', &cs, out, &bad); i += 2; continue; }
      if (d == '&') { r = put_group(s, c, m, 0, &cs, out, &bad); i += 2; continue; }
      if (d == '$') { r = put_char(c, apply_case(&cs, '$'), out, &bad); i += 2; continue; }
      if (d == '{') {
        u32 j = i + 2, g = 0, nd = 0;
        while (j < n && rep[j] >= '0' && rep[j] <= '9' && nd < 4) { g = g * 10 + (rep[j] - '0'); j++; nd++; }
        if (nd && j < n && rep[j] == '}') {
          r = put_group(s, c, m, g, &cs, out, &bad);
          i = j + 1;
          continue;
        }
      }
      r = put_char(c, apply_case(&cs, '$'), out, &bad);
      i++;
      continue;
    }
    if (ch == '\\' && i + 1 < n) {
      ecp d = rep[i + 1];
      i += 2;
      if (d >= '0' && d <= '9') { r = put_group(s, c, m, d - '0', &cs, out, &bad); continue; }
      switch (d) {
      case 'n': r = put_char(c, 10, out, &bad); break;
      case 'r': r = put_char(c, 13, out, &bad); break;
      case 't': r = put_char(c, 9, out, &bad); break;
      case 'U': cs.mode = 1; break;
      case 'L': cs.mode = 2; break;
      case 'E': cs.mode = 0; cs.one = 0; break;
      case 'u': cs.one = 1; break;
      case 'l': cs.one = 2; break;
      case 'x': {
        u32 v = 0;
        if (i < n && rep[i] == '{') {
          u32 j = i + 1, nd = 0;
          while (j < n && hexv(rep[j]) >= 0 && nd < 6) { v = v * 16 + (u32)hexv(rep[j]); j++; nd++; }
          if (nd && j < n && rep[j] == '}' && v <= 0x10FFFF && (v < 0xD800 || v > 0xDFFF)) {
            r = put_char(c, apply_case(&cs, v), out, &bad);
            i = j + 1;
            break;
          }
        } else if (read_hexn(rep, n, i, 2, &v)) {
          r = put_char(c, apply_case(&cs, v), out, &bad);
          i += 2;
          break;
        }
        r = put_char(c, apply_case(&cs, 'x'), out, &bad);
        break;
      }
      default:
        r = put_char(c, apply_case(&cs, d), out, &bad);
        break;
      }
      continue;
    }
    r = put_char(c, apply_case(&cs, ch), out, &bad);
    i++;
  }
  if (unrepresentable) *unrepresentable = bad;
  return r;
}

int Searcher::impossible() const
{
  return impossible_;
}

const SearchOpts& Searcher::opts() const
{
  return opts_;
}

// ---- Replace All ----

struct RepItem { epos pos; epos len; u32 off; u32 n; };

int search_replace_all(Searcher& s, ByteSource* src, const Codec* c, epos lo, epos hi, const ecp* rep, u32 n,
                       EditSink* sink, ProgressCb* cb, u32* replaced, u32* unrepresentable)
{
  Vec<RepItem> items;
  EBytes buf, one;
  RxMatch mm;
  epos pos = lo;
  SearchSpanCb scb(cb, lo, 0);
  u32 bad_total = 0, k;
  int r, bytes = s.opts().mode == SM_HEX;
  if (replaced) *replaced = 0;
  if (unrepresentable) *unrepresentable = 0;
  if (!src || !sink) return E_RANGE;
  if (hi > src->size()) hi = src->size();
  if (lo > hi) return E_OK;
  scb.total = hi - lo;
  for (;;) {
    RepItem it;
    u32 bad = 0;
    scb.base = pos;
    r = s.find(src, c, lo, hi, pos, 0, &mm, cb ? &scb : 0);
    if (r < 0) return r;
    if (r == 0) break;
    r = s.expand(rep, n, src, c, mm, &one, &bad);
    if (r != E_OK) return r;
    bad_total += bad;
    if (items.size() >= 1000000) return E_TOOBIG;
    it.pos = mm.pos;
    it.len = mm.len;
    it.off = buf.size();
    it.n = one.size();
    if (one.size() && buf.append(one.data(), one.size()) != E_OK) return E_NOMEM;
    if (items.push(it) != E_OK) return E_NOMEM;
    pos = mm.len ? mm.pos + mm.len : next_char(src, c, bytes, mm.pos);
    if (pos > hi) break;
    if (scb.tick(pos)) return E_CANCEL;
  }
  for (k = items.size(); k > 0; k--) {
    const RepItem& it = items[k - 1];
    if (it.len == 0 && it.n == 0) continue;
    r = sink->apply(it.pos, it.len, it.n ? buf.data() + it.off : buf.data(), it.n);
    if (r < 0) {
      if (replaced) *replaced = items.size() - k;
      if (unrepresentable) *unrepresentable = bad_total;
      return r;
    }
  }
  if (replaced) *replaced = items.size();
  if (unrepresentable) *unrepresentable = bad_total;
  return E_OK;
}
