// doc.cpp - the Document (DESIGN 6.7, 7). Owner: T01.
//
// All content changes go through prim_() (tree edit + exact EOL count adjustment + change_seq + one EditEvent).
// replace() records the change in the undo log (prim_ with record = 1: the undo capacity is reserved after the
// removal, when the number of removed pieces is known, and a failure rolls the removal back). Undo/redo replay the
// recorded ops through prim_/state_ without recording. Encoding and break-mode changes are "state" ops (kind 3).
//
// EOL counts (EolCounts) are kept for both break modes with one classification of code units: an LF unit is a
// CRLF pair if the unit before it is CR, else a bare LF; a CR unit not followed by LF is a lone CR. (Same rules as
// detect_eol, so file_index results can be passed to finish_indexing.) In CR mode, breaks = cr + crlf.
#include "doc.h"
#include "doc_int.h"
#include <string.h>

#define UNIT_NONE 0xFFFFFFFFu

// ---------------------------------------------------------------------------------------------------------------
// Fallback codecs for encodings codec_get() does not support (lossless: bytes >= 0x80 become bad markers)

class RawByteCodec : public Codec {
public:
  RawByteCodec() { kind = EK_SBCS; unit = 1; max_len = 1; ascii_compat = 1; self_sync = 1; pad = 0; cp = 0; }
  u32 decode(const u8* p, u32 avail, int eof, ecp* out) const
  {
    (void)avail; (void)eof;
    *out = p[0] < 0x80 ? (ecp)p[0] : ECP_BADBYTE(p[0]);
    return 1;
  }
  u32 encode(ecp c, u8* out) const
  {
    if (c < 0x80) { out[0] = (u8)c; return 1; }
    if (c >= 0x110000u && c < 0x110100u) { out[0] = (u8)(c - 0x110000u); return 1; }
    return 0;
  }
};

class RawU16Codec : public Codec {
public:
  explicit RawU16Codec(int big) : be(big)
  { kind = (u8)(big ? EK_UTF16BE : EK_UTF16LE); unit = 2; max_len = 2; ascii_compat = 0; self_sync = 1; pad = 0;
    cp = (u16)(big ? 1201 : 1200); }
  u32 decode(const u8* p, u32 avail, int eof, ecp* out) const
  {
    u32 u;
    if (avail < 2) {
      if (!eof) return 0;
      *out = ECP_BADBYTE(p[0]);
      return 1;
    }
    u = be ? ((u32)p[0] << 8 | p[1]) : ((u32)p[1] << 8 | p[0]);
    *out = (u >= 0xD800 && u <= 0xDFFF) ? ECP_BADUNIT(u) : u;
    return 2;
  }
  u32 encode(ecp c, u8* out) const
  {
    u32 u;
    if (c >= 0x110000u && c < 0x110100u) { out[0] = (u8)(c - 0x110000u); return 1; }
    if (c >= 0x120000u && c < 0x130000u) u = c - 0x120000u;
    else if (c < 0x10000u && !(c >= 0xD800 && c <= 0xDFFF)) u = c;
    else return 0;
    if (be) { out[0] = (u8)(u >> 8); out[1] = (u8)u; } else { out[0] = (u8)u; out[1] = (u8)(u >> 8); }
    return 2;
  }
  int be;
};

static const RawByteCodec g_raw8;
static const RawU16Codec g_raw16le(0);
static const RawU16Codec g_raw16be(1);

static u32 pack_enc(const EncSpec& e) { return (u32)e.kind | ((u32)e.bom << 8) | ((u32)e.cp << 16); }
static EncSpec unpack_enc(u32 v)
{
  EncSpec e;
  e.kind = (u8)(v & 0xFF); e.bom = (u8)((v >> 8) & 0xFF); e.cp = (u16)(v >> 16);
  return e;
}
static u32 pack_eol(EolStyle s, bool crm) { return (u32)s | (crm ? 0x100u : 0u); }
static bool is_u16(const EncSpec& e) { return e.kind == EK_UTF16LE || e.kind == EK_UTF16BE; }
static epos add_clamp(epos x, epos d, epos lim) { return (lim - x >= d) ? x + d : lim; }
static int eol_count1_(ByteSource* s, epos a, epos b, u8 known_b0, u32 known_n, EolCounts* c, ProgressCb* cb);
static int eol_count2_(ByteSource* s, epos a, epos b, int be, u8 known_b0, u32 known_n, EolCounts* c, ProgressCb* cb);
static bool spec_eq(const BreakSpec& a, const BreakSpec& b) { return a.unit == b.unit && a.b0 == b.b0 && a.b1 == b.b1; }

// ---------------------------------------------------------------------------------------------------------------
// View (bytes())

Document::View::View() : d_(0), p_(0), pos_(0), len_(0), stamp_(0), valid_(0) {}

epos Document::View::size() const { return d_->tree_.length(); }

const u8* Document::View::get(epos off, epos* avail_out)
{
  epos dummy;
  epos* avail = avail_out ? avail_out : &dummy;
  PSpan s;
  if (valid_ && stamp_ == d_->tree_.stamp() && off >= pos_ && off - pos_ < len_) {
    *avail = len_ - (off - pos_);
    return p_ + (off - pos_);
  }
  if (d_->tree_.span(off, &s) != E_OK) { valid_ = 0; *avail = 0; return 0; }
  p_ = s.p; pos_ = off; len_ = s.len; stamp_ = d_->tree_.stamp(); valid_ = 1;
  *avail = s.len;
  return s.p;
}

// ---------------------------------------------------------------------------------------------------------------
// Construction and lifecycle

Document::Document()
  : src_(0), codec_(0), origin_(0), eol_(EOL_CRLF), eolb_n_(0), ro_(false), busy_(false), indexing_(false),
    always_mod_(false), change_seq_(0), ondemand_(false), cr_mode_(false), depth_(0), dispatching_(0),
    lst_dirty_(false)
{
  view_.d_ = this;
  eolc_.crlf = eolc_.lf = eolc_.cr = 0;
  eolb_[0] = eolb_[1] = eolb_[2] = eolb_[3] = 0;
  set_enc_(enc_ansi(), false, EOL_CRLF);
}

Document::~Document()
{
  tree_.clear();
  if (src_) delete src_;
}

const Codec* Document::cc_() const
{
  if (codec_) return codec_;
  if (enc_.kind == EK_UTF16LE) return &g_raw16le;
  if (enc_.kind == EK_UTF16BE) return &g_raw16be;
  return &g_raw8;
}

u32 Document::unit_() const { return is_u16(enc_) ? 2u : 1u; }

void Document::set_enc_(const EncSpec& e, bool crm, EolStyle st)
{
  u8 u[2];
  u32 n, i;
  enc_ = e; codec_ = codec_get(e); cr_mode_ = crm; eol_ = st;
  if (st == EOL_CRLF) { u[0] = 0x0D; u[1] = 0x0A; n = 2; }
  else if (st == EOL_LF) { u[0] = 0x0A; n = 1; }
  else { u[0] = 0x0D; n = 1; }
  eolb_n_ = 0;
  for (i = 0; i < n; i++) {
    if (e.kind == EK_UTF16LE) { eolb_[eolb_n_++] = u[i]; eolb_[eolb_n_++] = 0; }
    else if (e.kind == EK_UTF16BE) { eolb_[eolb_n_++] = 0; eolb_[eolb_n_++] = u[i]; }
    else eolb_[eolb_n_++] = u[i];
  }
}

int Document::reset_(ByteSource* src, const EncSpec& enc_in, epos bom_len, EolStyle eol, ProgressCb* cb, int indexing,
                     int fire)
{
  PieceTree tmp;
  BreakSpec bs;
  EolCounts ec;
  EncSpec enc = enc_in;
  EditEvent e;
  bool crm = eol == EOL_CR;
  int rc;
  if (!src) return E_RANGE;
  if (bom_len > src->size()) bom_len = 0;
  enc.bom = (u8)(bom_len > 0 ? 1 : 0);
  codec_break_spec(enc, crm ? 1 : 0, &bs);
  ec.crlf = ec.lf = ec.cr = 0;
  rc = indexing ? tmp.init_uncounted(src, bs) : tmp.init(src, bs, cb);
  if (rc == E_OK && !indexing) {
    if (is_u16(enc)) rc = eol_count2_(src, 0, src->size(), enc.kind == EK_UTF16BE, (u8)(bs.b0 | bs.b1), tmp.break_count(), &ec, 0);
    else rc = eol_count1_(src, 0, src->size(), bs.b0, tmp.break_count(), &ec, 0);   // one pass: the tree counted b0
  }
  if (rc != E_OK) {
    if (src != src_) delete src;
    return rc;
  }
  e.pos = 0; e.removed = tree_.length(); e.inserted = src->size(); e.line = 0;
  e.lines_removed = tree_.break_count(); e.lines_added = tmp.break_count(); e.flags = EV_RESET;
  tree_.swap(tmp);
  tmp.clear();
  if (src_ && src_ != src) delete src_;
  src_ = src;
  set_enc_(enc, crm, eol);
  origin_ = bom_len;
  eolc_ = ec;
  indexing_ = indexing ? true : false;
  undo_.clear();
  undo_.set_save_point();
  always_mod_ = false;
  view_.valid_ = 0;
  change_seq_++;
  if (fire) emit_(e);
  return E_OK;
}

int Document::init_empty(const EncSpec& enc, EolStyle eol)
{
  u8 bom[4];
  u32 n = enc_bom(enc, bom);
  MemSource* m = new MemSource();
  int rc;
  if (!m) return E_NOMEM;
  rc = m->copy_from(bom, n);
  if (rc != E_OK) { delete m; return rc; }
  ondemand_ = false;
  return reset_(m, enc, n, eol, 0, 0, 0);
}

int Document::init_source(ByteSource* src, const EncSpec& enc, epos bom_len, EolStyle eol, ProgressCb* cb)
{
  ondemand_ = false;
  return reset_(src, enc, bom_len, eol, cb, 0, 0);
}

int Document::init_source_indexing(ByteSource* src, const EncSpec& enc, epos bom_len, EolStyle eol)
{
  int rc = reset_(src, enc, bom_len, eol, 0, 1, 0);
  if (rc == E_OK) ondemand_ = true;
  return rc;
}

int Document::finish_indexing(const u32* block_breaks, u32 nblocks, const EolCounts& eol)
{
  EditEvent e;
  int rc;
  if (!indexing_) return E_STATE;
  rc = tree_.set_counts(block_breaks, nblocks);
  if (rc != E_OK) return rc;
  eolc_ = eol;
  indexing_ = false;
  view_.valid_ = 0;
  change_seq_++;
  e.pos = 0; e.removed = 0; e.inserted = 0; e.line = 0; e.lines_removed = 0; e.lines_added = tree_.break_count();
  e.flags = EV_INDEXED;
  emit_(e);
  return E_OK;
}

int Document::reset_source(ByteSource* src, const EncSpec& enc, epos bom_len, EolStyle eol, ProgressCb* cb)
{
  if (busy_ || depth_) {
    if (src && src != src_) delete src;
    return busy_ ? E_BUSY : E_STATE;
  }
  return reset_(src, enc, bom_len, eol, cb, 0, 1);
}

int Document::reset_source_indexing(ByteSource* src, const EncSpec& enc, epos bom_len, EolStyle eol)
{
  int rc;
  if (busy_ || depth_) {
    if (src && src != src_) delete src;
    return busy_ ? E_BUSY : E_STATE;
  }
  rc = reset_(src, enc, bom_len, eol, 0, 1, 1);
  if (rc == E_OK) ondemand_ = true;
  return rc;
}

int Document::reload_as_edit(ByteSource* src)
{
  epos n;
  u8* buf;
  int rc, implicit = depth_ == 0;
  Selection s0;
  if (!src) return E_RANGE;
  if (ro_) return E_READONLY;
  if (busy_ || indexing_) return E_BUSY;
  n = src->size();
  buf = (u8*)e_valloc(n ? n : 1);
  if (!buf) return E_NOMEM;
  rc = src->read(0, buf, n);
  if (rc != E_OK) { e_vfree(buf, n ? n : 1); return rc; }
  rc = check_edit_(0, length(), n & ~(epos)1);   // whole replacement: an odd UTF-16 file size is fine
  if (rc == E_OK && n > EPOS_MAX) rc = E_TOOBIG;
  if (rc != E_OK) { e_vfree(buf, n ? n : 1); return rc; }
  s0.anchor = s0.caret = origin_;
  if (implicit) begin_edit(UK_OTHER, s0, 0);
  rc = undo_.reserve(3, 0);
  if (rc == E_OK) rc = edit_record_(0, length(), buf, n);
  if (rc == E_OK && enc_.kind != EK_SBCS && enc_.kind != EK_DBCS) {
    u8 bom[4];
    EncSpec e = enc_;
    u32 k;
    e.bom = 1;
    k = enc_bom(e, bom);
    e.bom = (u8)(k && n >= k && memcmp(buf, bom, k) == 0 ? 1 : 0);
    if (e.bom != enc_.bom) {
      u32 eo = pack_enc(enc_), en = pack_enc(e), ep = pack_eol(eol_, cr_mode_);
      if (state_(en, ep, 0) == E_OK) undo_.rec_state(eo, en, ep, ep);
    }
  }
  if (implicit) {
    s0.anchor = s0.caret = origin_;
    end_edit(s0);
  }
  e_vfree(buf, n ? n : 1);
  return rc;
}

bool Document::indexing() const { return indexing_; }
ByteSource* Document::source() const { return src_; }
ByteSource* Document::bytes() { return &view_; }

int Document::snapshot(SnapshotSource* out, ByteSource* original_private)
{
  PieceVec list;
  int rc = tree_.snapshot(&list);
  if (rc != E_OK) return rc;
  return out->init(list, tree_.add_buffer(), original_private ? original_private : src_);
}

// ---------------------------------------------------------------------------------------------------------------
// Lines

epos Document::length() { return tree_.length(); }
u32 Document::line_count() { return tree_.break_count() + 1; }
epos Document::text_origin() const { return origin_; }

epos Document::line_start(u32 line)
{
  epos bp;
  if (line == 0) return 0;
  if (line > tree_.break_count()) return length();
  bp = tree_.break_pos(line - 1);
  if (bp == EPOS_NONE) return length();
  return bp + unit_();
}

epos Document::line_end(u32 line)
{
  epos bp;
  u32 unit = unit_();
  if (line >= tree_.break_count()) return length();
  bp = tree_.break_pos(line);
  if (bp == EPOS_NONE) return length();
  if (!cr_mode_ && bp >= unit) {
    u8 b[2];
    if (tree_.read(bp - unit, b, unit) == E_OK) {
      bool cr = unit == 1 ? b[0] == 0x0D
                          : (enc_.kind == EK_UTF16BE ? (b[0] == 0 && b[1] == 0x0D) : (b[0] == 0x0D && b[1] == 0));
      if (cr) return bp - unit;
    }
  }
  return bp;
}

epos Document::line_next(u32 line)
{
  epos bp;
  if (line >= tree_.break_count()) return length();
  bp = tree_.break_pos(line);
  if (bp == EPOS_NONE) return length();
  return bp + unit_();
}

u32 Document::line_of(epos pos)
{
  if (pos >= length()) return tree_.break_count();
  return tree_.breaks_before(pos);
}

// ---------------------------------------------------------------------------------------------------------------
// Bytes and characters

int Document::read(epos pos, u8* out, epos n) { return tree_.read(pos, out, n); }
int Document::span(epos pos, PSpan* s) { return tree_.span(pos, s); }

u32 Document::decode(epos pos, ecp* cp)
{
  const Codec* c = cc_();
  epos len = length(), avail = 0;
  const u8* p;
  u32 used;
  ecp dummy;
  if (!cp) cp = &dummy;
  if (pos >= len) return 0;
  p = view_.get(pos, &avail);
  if (!p) { *cp = ECP_BADBYTE(0); return 1; }
  used = c->decode(p, avail, pos + avail >= len, cp);
  if (used == 0) {                           // character continues in the next piece
    u8 buf[8];
    epos n = len - pos;
    if (n > c->max_len) n = c->max_len;
    if (tree_.read(pos, buf, n) != E_OK) { *cp = ECP_BADBYTE(0); return 1; }
    used = c->decode(buf, n, pos + n >= len, cp);
    if (used == 0) { *cp = ECP_BADBYTE(buf[0]); used = 1; }
  }
  if (used > len - pos) used = len - pos;
  return used;
}

epos Document::char_next(epos pos)
{
  epos len = length();
  if (pos < origin_) return origin_ < len ? origin_ : len;
  if (pos >= len) return len;
  return pos + decode(pos, 0);
}

epos Document::char_prev(epos pos)
{
  const Codec* c = cc_();
  epos len = length(), floor;
  if (pos > len) pos = len;
  if (pos <= origin_) return origin_ < len ? origin_ : len;
  if (unit_() == 2 && (pos & 1)) return pos - 1;
  if (c->self_sync) floor = origin_;
  else {
    floor = line_start(line_of(pos - 1));
    if (pos > 65536 && floor < pos - 65536) floor = pos - 65536;
    if (floor < origin_) floor = origin_;
  }
  return codec_prev(&view_, c, pos, floor);
}

// Start of the character containing pos. Self-synchronising codecs: the earliest start s in
// [pos - max_len + unit, pos) whose character covers pos (valid sequences cannot overlap, and a stray continuation
// unit decodes as one bad unit, so a covering start is unique). DBCS: decode forward from a known boundary.
epos Document::snap_char(epos pos)
{
  const Codec* c = cc_();
  epos len = length(), q;
  u32 used, unit = unit_(), d;
  if (pos <= origin_) return origin_ < len ? origin_ : len;
  if (pos >= len) return len;
  if (unit == 2 && (pos & 1)) pos--;
  if (pos <= origin_) return origin_;
  if (c->self_sync) {
    for (d = c->max_len >= unit ? c->max_len - unit : 0; d >= unit; d -= unit) {
      if (d > pos - origin_) continue;
      q = pos - d;
      if (q + decode(q, 0) > pos) return q;
    }
    return pos;
  }
  q = char_prev(pos);
  used = decode(q, 0);
  return (q + used > pos) ? q : pos;
}

// ---------------------------------------------------------------------------------------------------------------
// EOL counting

// Unit-1 EOL counts over [a, b) with memchr (T16: the per-byte state machine below cost ~30 cycles/byte on the PII;
// opening 10 MB spent 1 s here). Same classification as the general loop: an LF whose previous byte is CR is a CRLF
// (the CR may be at a - 1), other LFs are bare; a CR is lone unless the next byte (which may be at b) is LF.
// P = LFs in [a, b) preceded by CR, Q = CRs in [a, b) followed by LF; Q = P - x + y with x = "LF at a preceded by
// CR", y = "LF at b preceded by CR". known_b0 (0x0A or 0x0D, 0 = none) with known_n = the number of those bytes in
// [a, b) (the piece tree's break count after init) saves the pass over that byte.
static int eol_scan1_(ByteSource* s, epos a, epos b, u8 target, u32* n, u32* paired, ProgressCb* cb, epos total)
{
  epos o = a, len = s->size();
  u32 cnt = 0, pr = 0;
  int last = -1, pending = 0;
  u8 nb = 0;
  if (target == 0x0A && a > 0 && s->read(a - 1, &nb, 1) == E_OK) last = nb;
  while (o < b) {
    epos av = 0;
    const u8* p = s->get(o, &av);
    const u8* q;
    const u8* e;
    if (!p || av == 0) return s->err() != E_OK ? s->err() : E_IO;
    if (av > b - o) av = b - o;
    if (pending) { if (p[0] == 0x0A) pr++; pending = 0; }
    q = p; e = p + av;
    while (q < e) {
      const u8* f = (const u8*)memchr(q, target, (size_t)(e - q));
      if (!f) break;
      cnt++;
      if (target == 0x0A) { if ((f > p ? (int)f[-1] : last) == 0x0D) pr++; }
      else if (f + 1 < e) { if (f[1] == 0x0A) pr++; }
      else pending = 1;
      q = f + 1;
    }
    last = p[av - 1];
    o += av;
    if (cb && cb->progress(o - a, total)) return E_CANCEL;
  }
  if (pending && b < len && s->read(b, &nb, 1) == E_OK && nb == 0x0A) pr++;
  *n = cnt; *paired = pr;
  return E_OK;
}

static int eol_count1_(ByteSource* s, epos a, epos b, u8 known_b0, u32 known_n, EolCounts* c, ProgressCb* cb)
{
  epos len = s->size();
  u32 L = 0, C = 0, P = 0, Q = 0, x = 0, y = 0, dummy = 0;
  u8 two[2];
  int rc;
  c->crlf = c->lf = c->cr = 0;
  if (b > len) b = len;
  if (a >= b) return E_OK;
  if (a > 0 && s->read(a - 1, two, 2) == E_OK && two[0] == 0x0D && two[1] == 0x0A) x = 1;
  if (b < len && s->read(b - 1, two, 2) == E_OK && two[0] == 0x0D && two[1] == 0x0A) y = 1;
  if (known_b0 == 0x0A) {
    L = known_n;
    rc = eol_scan1_(s, a, b, 0x0D, &C, &Q, cb, b - a);
    if (rc != E_OK) return rc;
    P = Q + x - y;
  } else {
    rc = eol_scan1_(s, a, b, 0x0A, &L, &P, cb, b - a);
    if (rc != E_OK) return rc;
    Q = P + y - x;
    if (known_b0 == 0x0D) C = known_n;
    else {
      rc = eol_scan1_(s, a, b, 0x0D, &C, &dummy, 0, 0);
      if (rc != E_OK) return rc;
    }
  }
  c->crlf = P; c->lf = L - P; c->cr = C - Q;
  return E_OK;
}

// Unit 2 (UTF-16), same algebra with code units at offsets a, a + 2, ... (a even): memchr for the unit's low byte
// over 4 KB windows read with 2 bytes of context on each side, then parity, high byte and neighbour checks.
enum { EOL2_CH = 4096 };

static u32 unit2_(const u8* p, int be) { return be ? ((u32)p[0] << 8 | p[1]) : ((u32)p[1] << 8 | p[0]); }

static int eol_scan2_(ByteSource* s, epos a, epos b, int be, u8 target, u32* n, u32* paired, ProgressCb* cb)
{
  u8 buf[EOL2_CH + 4];
  epos len = s->size(), o, lim2 = b + 2 < len ? b + 2 : len;   // units may be read up to min(b + 2, len)
  u32 cnt = 0, pr = 0;
  int rc;
  for (o = a; o < b; o += EOL2_CH) {
    epos lo = o >= 2 ? o - 2 : o, hi = (len - o > EOL2_CH + 2) ? o + EOL2_CH + 2 : len, end = o + EOL2_CH < b ? o + EOL2_CH : b;
    const u8* q;
    const u8* e;
    rc = s->read(lo, buf, hi - lo);
    if (rc != E_OK) return rc;
    q = buf + (o - lo) + (be ? 1 : 0);       // low byte of the first unit
    e = buf + (end - lo) + (be ? 1 : 0);
    if (e > buf + (hi - lo)) e = buf + (hi - lo);
    while (q < e) {
      const u8* f = (const u8*)memchr(q, target, (size_t)(e - q));
      epos u;
      if (!f) break;
      q = f + 1;
      u = lo + (epos)(f - buf) - (be ? 1 : 0);
      if (((u - a) & 1) || u >= end || u + 2 > lim2) continue;
      if (unit2_(buf + (u - lo), be) != target) continue;
      cnt++;
      if (target == 0x0A) { if (u >= 2 && unit2_(buf + (u - 2 - lo), be) == 0x0D) pr++; }
      else if (u + 4 <= lim2 && unit2_(buf + (u + 2 - lo), be) == 0x0A) pr++;
    }
    if (cb && cb->progress(end - a, b - a)) return E_CANCEL;
  }
  *n = cnt; *paired = pr;
  return E_OK;
}

static int eol_count2_(ByteSource* s, epos a, epos b, int be, u8 known_b0, u32 known_n, EolCounts* c, ProgressCb* cb)
{
  epos len = s->size();
  u32 L = 0, C = 0, P = 0, Q = 0, x = 0, y = 0, dummy = 0;
  u8 four[4];
  int rc;
  c->crlf = c->lf = c->cr = 0;
  if (b > len) b = len;
  if (a >= b) return E_OK;
  if (a >= 2 && a + 2 <= len && s->read(a - 2, four, 4) == E_OK && unit2_(four, be) == 0x0D && unit2_(four + 2, be) == 0x0A) x = 1;
  if (!((b - a) & 1) && b + 2 <= len && s->read(b - 2, four, 4) == E_OK && unit2_(four, be) == 0x0D &&
      unit2_(four + 2, be) == 0x0A) y = 1;
  if (known_b0 == 0x0A) {
    L = known_n;
    rc = eol_scan2_(s, a, b, be, 0x0D, &C, &Q, cb);
    if (rc != E_OK) return rc;
    P = Q + x - y;
  } else {
    rc = eol_scan2_(s, a, b, be, 0x0A, &L, &P, cb);
    if (rc != E_OK) return rc;
    Q = P + y - x;
    if (known_b0 == 0x0D) C = known_n;
    else {
      rc = eol_scan2_(s, a, b, be, 0x0D, &C, &dummy, 0);
      if (rc != E_OK) return rc;
    }
  }
  c->crlf = P; c->lf = L - P; c->cr = C - Q;
  return E_OK;
}

static int count_eol_loop_(ByteSource* s, epos a, epos b, u32 unit, int be, EolCounts* c, ProgressCb* cb);

int Document::count_eol_(ByteSource* s, epos a, epos b, u32 unit, int be, EolCounts* c, ProgressCb* cb)
{
  if (unit == 1) return eol_count1_(s, a, b, 0, 0, c, cb);
  if (!(a & 1)) return eol_count2_(s, a, b, be, 0, 0, c, cb);
  return count_eol_loop_(s, a, b, unit, be, c, cb);
}

int doc_count_eol_test(ByteSource* s, epos a, epos b, u32 unit, int be, int mode, u8 known_b0, u32 known_n, EolCounts* c)
{
  if (mode == 1) return count_eol_loop_(s, a, b, unit, be, c, 0);
  if (unit == 1) return eol_count1_(s, a, b, known_b0, known_n, c, 0);
  if (!(a & 1)) return eol_count2_(s, a, b, be, known_b0, known_n, c, 0);
  return count_eol_loop_(s, a, b, unit, be, c, 0);
}

// The reference loop (per code unit): odd UTF-16 windows and tests.
static int count_eol_loop_(ByteSource* s, epos a, epos b, u32 unit, int be, EolCounts* c, ProgressCb* cb)
{
  u8 buf[4096];
  epos len = s->size(), rs, re, o;
  u32 pv = UNIT_NONE, cu = UNIT_NONE, v, i;
  epos cu_off = 0;
  int have = 0, rc;
  c->crlf = c->lf = c->cr = 0;
  if (b > len) b = len;
  if (a >= b) return E_OK;
  rs = a >= unit ? a - unit : 0;
  re = add_clamp(b, unit, len);
  for (o = rs; o < re; ) {
    epos n = re - o;
    if (n > sizeof(buf)) n = sizeof(buf);
    if (unit == 2) n &= ~(epos)1;
    if (n == 0) break;
    rc = s->read(o, buf, n);
    if (rc != E_OK) return rc;
    for (i = 0; i + unit <= n; i += unit) {
      if (unit == 1) v = buf[i];
      else v = be ? ((u32)buf[i] << 8 | buf[i + 1]) : ((u32)buf[i + 1] << 8 | buf[i]);
      if (have && cu_off >= a && cu_off < b) {
        if (cu == 0x0A) { if (pv == 0x0D) c->crlf++; else c->lf++; }
        else if (cu == 0x0D && v != 0x0A) c->cr++;
      }
      if (have) pv = cu;
      cu = v; cu_off = o + i; have = 1;
    }
    o += n;
    if (cb && cb->progress(o - rs, re - rs)) return E_CANCEL;
  }
  if (have && cu_off >= a && cu_off < b) {
    if (cu == 0x0A) { if (pv == 0x0D) c->crlf++; else c->lf++; }
    else if (cu == 0x0D) c->cr++;
  }
  return E_OK;
}

void Document::recount_eol_()
{
  EolCounts c;
  int rc;
  const BreakSpec& bs = tree_.break_spec();
  if (unit_() == 1 && bs.unit == 1 && !indexing_)
    rc = eol_count1_(&view_, 0, length(), bs.b0, tree_.break_count(), &c, 0);
  else if (unit_() == 2 && bs.unit == 2 && !indexing_)
    rc = eol_count2_(&view_, 0, length(), enc_.kind == EK_UTF16BE, (u8)(bs.b0 | bs.b1), tree_.break_count(), &c, 0);
  else rc = count_eol_(&view_, 0, length(), unit_(), enc_.kind == EK_UTF16BE, &c, 0);
  if (rc == E_OK) eolc_ = c;
}

// ---------------------------------------------------------------------------------------------------------------
// Editing primitives

int Document::check_edit_(epos pos, epos del, epos ins_len)
{
  epos len = length();
  if (ro_) return E_READONLY;
  if (busy_ || indexing_) return E_BUSY;
  if (pos > len || del > len - pos) return E_RANGE;
  if (ins_len > EPOS_MAX - (len - del)) return E_TOOBIG;
  if (unit_() == 2) {
    if ((pos & 1) || (ins_len & 1)) return E_ALIGN;
    if ((del & 1) && pos + del != len) return E_ALIGN;
  }
  return E_OK;
}

void Document::emit_(const EditEvent& e)
{
  u32 i;
  dispatching_++;
  for (i = 0; i < listeners_.size(); i++) if (listeners_[i]) listeners_[i]->doc_changed(this, e);
  dispatching_--;
  if (!dispatching_ && lst_dirty_) {
    for (i = listeners_.size(); i > 0; i--) if (!listeners_[i - 1]) listeners_.erase(i - 1, 1);
    lst_dirty_ = false;
  }
}

static u32 refs_bound(epos n)
{
  return n / PT_PIECE_MAX + n / PT_ADD_CHUNK + 3;
}

// Remove del bytes at pos (refs into *removed), then insert either bytes (ins) or refs. record: reserve undo space
// after the removal and record both ops. On failure nothing has changed.
int Document::prim_(epos pos, epos del, PieceVec* removed, const u8* ins, epos ins_len, const PieceRef* refs,
                    u32 nrefs, PieceVec* added, u32 flags, int record)
{
  PieceVec rtmp, atmp;
  EolCounts before, after;
  EditEvent e;
  u32 unit = unit_(), line, b0, b1, b2, rbase, i;
  int be = enc_.kind == EK_UTF16BE, full, rc = E_OK;
  epos len = length(), inserted = 0;
  if (ins) inserted = ins_len;
  else for (i = 0; i < nrefs; i++) inserted += refs[i].len;
  if (!del && !inserted) return E_OK;
  if (record) { removed = &rtmp; added = &atmp; }  // fresh vectors: exactly this op's refs get recorded
  else if (!removed && inserted) removed = &rtmp;
  rbase = removed ? removed->size() : 0;
  full = unit == 2 && ((pos & 1) || (((del | inserted) & 1) && pos + del != len));
  before.crlf = before.lf = before.cr = 0;
  if (!full) {
    rc = count_eol_(&view_, pos >= unit ? pos - unit : 0, add_clamp(pos + del, unit, len), unit, be, &before, 0);
    if (rc != E_OK) return rc;
  }
  line = tree_.breaks_before(pos);
  b0 = tree_.break_count();
  if (del) {
    rc = tree_.remove(pos, del, removed);
    if (rc != E_OK) return rc;
  }
  b1 = tree_.break_count();
  if (record) {
    rc = undo_.reserve(2, (removed->size() - rbase) + (ins ? refs_bound(ins_len) : nrefs));
  }
  if (rc == E_OK && inserted) {
    if (ins) rc = tree_.insert(pos, ins, ins_len, added);
    else rc = tree_.insert_refs(pos, refs, nrefs);
  }
  if (rc != E_OK) {
    if (del) {                               // roll the removal back (its nodes are on the free list)
      tree_.insert_refs(pos, removed->data() + rbase, removed->size() - rbase);
      removed->resize(rbase);
    }
    view_.valid_ = 0;
    return rc;
  }
  b2 = tree_.break_count();
  view_.valid_ = 0;
  if (full) recount_eol_();
  else if (count_eol_(&view_, pos >= unit ? pos - unit : 0, add_clamp(pos + inserted, unit, length()), unit, be,
                      &after, 0) == E_OK) {
    eolc_.crlf = eolc_.crlf - before.crlf + after.crlf;
    eolc_.lf = eolc_.lf - before.lf + after.lf;
    eolc_.cr = eolc_.cr - before.cr + after.cr;
  } else recount_eol_();
  if (record) {                              // capacity reserved above: cannot fail
    if (del) undo_.rec_delete(pos, del, *removed);
    if (inserted) undo_.rec_insert(pos, inserted, *added);
  }
  change_seq_++;
  e.pos = pos; e.removed = del; e.inserted = inserted; e.line = line;
  e.lines_removed = b0 - b1; e.lines_added = b2 - b1; e.flags = flags | (full ? EV_RESET : 0);
  if (full) { e.lines_removed = b0; e.lines_added = b2; }
  emit_(e);
  return E_OK;
}

int Document::edit_record_(epos pos, epos del, const u8* ins, epos ins_len)
{
  return prim_(pos, del, 0, ins, ins_len, 0, 0, 0, 0, 1);
}

int Document::state_(u32 enc_packed, u32 eol_packed, u32 flags)
{
  EncSpec e = unpack_enc(enc_packed);
  EolStyle st = (EolStyle)(eol_packed & 0xFF);
  bool crm = (eol_packed & 0x100) != 0;
  BreakSpec bs;
  EditEvent ev;
  u8 bom[4];
  u32 old_breaks = tree_.break_count();
  int rc;
  codec_break_spec(e, crm ? 1 : 0, &bs);
  if (!spec_eq(bs, tree_.break_spec())) {
    rc = tree_.set_break_spec(bs, 0);
    if (rc != E_OK) return rc;
  }
  set_enc_(e, crm, st);
  origin_ = enc_bom(e, bom);               // may exceed length() inside a convert group (bytes follow)
  view_.valid_ = 0;
  recount_eol_();
  change_seq_++;
  ev.pos = 0; ev.removed = 0; ev.inserted = 0; ev.line = 0;
  ev.lines_removed = old_breaks; ev.lines_added = tree_.break_count(); ev.flags = EV_RESET | flags;
  emit_(ev);
  return E_OK;
}

int Document::replace(epos pos, epos del, const u8* ins, epos ins_len)
{
  int rc = check_edit_(pos, del, ins_len), implicit;
  Selection s;
  if (rc != E_OK) return rc;
  if (!del && !ins_len) return E_OK;
  implicit = depth_ == 0;
  if (implicit) { s.anchor = s.caret = pos; begin_edit(UK_OTHER, s, 0); }
  rc = edit_record_(pos, del, ins, ins_len);
  if (implicit) { s.anchor = s.caret = rc == E_OK ? pos + ins_len : pos; end_edit(s); }
  return rc;
}

int Document::replace_text(epos pos, epos del, const ecp* s, u32 n, u32* unrepresentable)
{
  const Codec* c = cc_();
  EBytes out;
  u8 buf[8];
  u32 i, bad = 0, k;
  int rc;
  if (unrepresentable) *unrepresentable = 0;
  rc = check_edit_(pos, del, 0);
  if (rc != E_OK) return rc;
  if (n && out.reserve(n < 0x3FFFFFFFu ? n : 0x3FFFFFFFu) != E_OK) return E_NOMEM;
  for (i = 0; i < n; i++) {
    k = c->encode(s[i], buf);
    if (!k) { bad++; continue; }
    if (!bad && out.append(buf, k) != E_OK) return E_NOMEM;
  }
  if (unrepresentable) *unrepresentable = bad;
  if (bad) return E_RANGE;
  return replace(pos, del, out.data(), out.size());
}

void Document::begin_edit(u32 kind, const Selection& before, u32 now_ms)
{
  UndoSel u;
  if (depth_++ > 0) return;
  u.anchor = before.anchor; u.caret = before.caret;
  undo_.begin(kind, u, now_ms);
}

void Document::end_edit(const Selection& after)
{
  UndoSel u;
  if (depth_ == 0) return;
  if (--depth_ > 0) return;
  u.anchor = after.anchor; u.caret = after.caret;
  undo_.end(u);
}

// ---------------------------------------------------------------------------------------------------------------
// Undo / redo

int Document::apply_op_(const UndoOp& op, const PieceRef* refs, int forward, u32 flags)
{
  switch (op.kind) {
  case 1:   // insert
    if (forward) return prim_(op.pos, 0, 0, 0, 0, refs + op.ref_first, op.ref_count, 0, flags, 0);
    return prim_(op.pos, op.len, 0, 0, 0, 0, 0, 0, flags, 0);
  case 2:   // delete
    if (forward) return prim_(op.pos, op.len, 0, 0, 0, 0, 0, 0, flags, 0);
    return prim_(op.pos, 0, 0, 0, 0, refs + op.ref_first, op.ref_count, 0, flags, 0);
  case 3:   // state
    if (forward) return state_(op.len, op.ref_count, flags);
    return state_(op.pos, op.ref_first, flags);
  default:
    return E_STATE;
  }
}

int Document::apply_step_(const UndoStep& st, int forward, u32 flags)
{
  u32 i, need = 0;
  int rc = E_OK;
  for (i = 0; i < st.nops; i++) {
    u32 k = st.ops[i].kind == 3 ? 0 : st.ops[i].ref_count;
    if (k > 0x0FFFFFFFu - need) { need = 0x0FFFFFFFu; break; }
    need += k + 2;
  }
  if (tree_.reserve_nodes(need) != E_OK) return E_NOMEM;
  if (forward) {
    for (i = 0; i < st.nops; i++) {
      rc = apply_op_(st.ops[i], st.refs, 1, flags);
      if (rc != E_OK) {
        while (i > 0) { i--; apply_op_(st.ops[i], st.refs, 0, flags); }
        return rc;
      }
    }
  } else {
    for (i = st.nops; i > 0; i--) {
      rc = apply_op_(st.ops[i - 1], st.refs, 0, flags);
      if (rc != E_OK) {
        for (; i < st.nops; i++) apply_op_(st.ops[i], st.refs, 1, flags);
        return rc;
      }
    }
  }
  return E_OK;
}

int Document::undo(Selection* sel)
{
  UndoStep st, dummy;
  int rc;
  if (depth_ || undo_.in_group()) return E_STATE;
  if (ro_) return E_READONLY;
  if (busy_ || indexing_) return E_BUSY;
  rc = undo_.pop_undo(&st);
  if (rc != E_OK) return rc;
  rc = apply_step_(st, 0, EV_UNDO);
  if (rc != E_OK) { undo_.pop_redo(&dummy); return rc; }
  if (sel) { sel->anchor = st.before.anchor; sel->caret = st.before.caret; }
  return E_OK;
}

int Document::redo(Selection* sel)
{
  UndoStep st, dummy;
  int rc;
  if (depth_ || undo_.in_group()) return E_STATE;
  if (ro_) return E_READONLY;
  if (busy_ || indexing_) return E_BUSY;
  rc = undo_.pop_redo(&st);
  if (rc != E_OK) return rc;
  rc = apply_step_(st, 1, EV_REDO);
  if (rc != E_OK) { undo_.pop_undo(&dummy); return rc; }
  if (sel) { sel->anchor = st.after.anchor; sel->caret = st.after.caret; }
  return E_OK;
}

bool Document::can_undo() const { return undo_.can_undo(); }
bool Document::can_redo() const { return undo_.can_redo(); }
void Document::break_undo_merge() { undo_.break_merge(); }
void Document::clear_undo() { if (!depth_) undo_.clear(); }
bool Document::modified() const { return !undo_.at_save_point() || always_mod_; }
void Document::set_saved() { undo_.set_save_point(); undo_.break_merge(); always_mod_ = false; }
u32 Document::undo_seq() const { return undo_.seq(); }
void Document::set_saved_seq(u32 seq) { undo_.set_save_point_seq(seq); undo_.break_merge(); always_mod_ = false; }
void Document::set_always_modified(bool on) { always_mod_ = on; }

// ---------------------------------------------------------------------------------------------------------------
// Encoding and line ends

const EncSpec& Document::enc() const { return enc_; }
const Codec* Document::codec() const { return cc_(); }

int Document::reinterpret(const EncSpec& enc_in, epos bom_len, ProgressCb* cb)
{
  EncSpec enc = enc_in;
  BreakSpec bs;
  EditEvent e;
  u32 old_breaks = tree_.break_count();
  int rc;
  if (busy_ || indexing_) return E_BUSY;
  if (depth_) return E_STATE;
  if (bom_len > length()) bom_len = 0;
  enc.bom = (u8)(bom_len > 0 ? 1 : 0);
  codec_break_spec(enc, cr_mode_ ? 1 : 0, &bs);
  if (!spec_eq(bs, tree_.break_spec())) {
    rc = tree_.set_break_spec(bs, cb);
    if (rc != E_OK) return rc;
  }
  set_enc_(enc, cr_mode_, eol_);
  origin_ = bom_len;
  view_.valid_ = 0;
  recount_eol_();
  undo_.break_merge();
  change_seq_++;
  e.pos = 0; e.removed = 0; e.inserted = 0; e.line = 0; e.lines_removed = old_breaks;
  e.lines_added = tree_.break_count(); e.flags = EV_RESET;
  emit_(e);
  return E_OK;
}

// Decode [origin, length) and (write) encode into out with the 7.5 substitutions; counts into rep.
int Document::build_text_(EBytes* out, const Codec* to, int write, ConvertReport* rep, ProgressCb* cb)
{
  CharReader r;
  ecp c;
  epos at = 0, len = length(), next_cb = 65536;
  u8 buf[8];
  u32 k;
  int rc;
  rep->unrepresentable = 0; rep->invalid_src = 0; rep->first_bad = EPOS_NONE;
  r.init(&view_, cc_(), origin_, len);
  while ((rc = r.next(&c, &at)) == 1) {
    if (ECP_IS_BAD(c)) {
      rep->invalid_src++;
      if (rep->first_bad == EPOS_NONE) rep->first_bad = at;
      if (!write) continue;
      k = to->encode(ECP_REPLACEMENT, buf);
      if (!k) k = to->encode('?', buf);
    } else {
      k = to->encode(c, buf);
      if (!k) {
        rep->unrepresentable++;
        if (rep->first_bad == EPOS_NONE) rep->first_bad = at;
        if (!write) continue;
        k = to->encode('?', buf);
      }
    }
    if (write && k && out->append(buf, k) != E_OK) return E_NOMEM;
    if (cb && at >= next_cb) {
      next_cb = at + 65536;
      if (cb->progress(at, len)) return E_CANCEL;
    }
  }
  return rc < 0 ? rc : E_OK;
}

int Document::check_convert(const EncSpec& to, ConvertReport* rep, ProgressCb* cb)
{
  ConvertReport lr;
  const Codec* tc = codec_get(to);
  if (!rep) rep = &lr;
  rep->unrepresentable = 0; rep->invalid_src = 0; rep->first_bad = EPOS_NONE;
  if (!tc) return E_UNSUPPORTED;
  if (to.kind == enc_.kind && to.cp == enc_.cp) return E_OK;   // only the BOM changes: lossless
  return build_text_(0, tc, 0, rep, cb);
}

int Document::convert(const EncSpec& to, ConvertReport* rep, ProgressCb* cb)
{
  ConvertReport lr;
  const Codec* tc = codec_get(to);
  Selection s;
  int rc, implicit = depth_ == 0;
  u32 eo = pack_enc(enc_), en = pack_enc(to), ep = pack_eol(eol_, cr_mode_);
  if (!rep) rep = &lr;
  rep->unrepresentable = 0; rep->invalid_src = 0; rep->first_bad = EPOS_NONE;
  if (ro_) return E_READONLY;
  if (busy_ || indexing_) return E_BUSY;
  if (ondemand_) return E_TOOBIG;
  if (!tc) return E_UNSUPPORTED;
  if (enc_equal(to, enc_)) return E_OK;
  s.anchor = s.caret = origin_;
  if (to.kind == enc_.kind && to.cp == enc_.cp) {   // BOM only
    u8 bom[4];
    u32 nb = enc_bom(to, bom);
    if (implicit) begin_edit(UK_OTHER, s, 0);
    rc = undo_.reserve(3, 0);
    if (rc == E_OK && (origin_ || nb)) rc = edit_record_(0, origin_, bom, nb);
    if (rc == E_OK) {
      rc = state_(en, ep, 0);
      if (rc == E_OK) undo_.rec_state(eo, en, ep, ep);
    }
    if (implicit) { s.anchor = s.caret = origin_; end_edit(s); }
    return rc;
  }
  {
    EBytes out;
    PieceVec removed, added;
    u8 bom[4];
    u32 nb = enc_bom(to, bom);
    epos old_len = length(), new_len;
    rc = out.append(bom, nb);
    if (rc == E_OK) rc = build_text_(&out, tc, 1, rep, cb);
    if (rc != E_OK) return rc;
    if (out.size() > EPOS_MAX) return E_TOOBIG;
    new_len = out.size();
    if (new_len) {
      rc = tree_.add_buffer()->append_aligned(out.data(), new_len, is_u16(to) ? 2 : 1, &added);
      if (rc != E_OK) return rc;
    }
    out.release();
    if (removed.reserve(tree_.piece_count() + 1) != E_OK) return E_NOMEM;
    if (tree_.reserve_nodes(added.size() + 4) != E_OK) return E_NOMEM;
    if (implicit) begin_edit(UK_OTHER, s, 0);
    rc = undo_.reserve(3, tree_.piece_count() + added.size());
    if (rc == E_OK && old_len) rc = prim_(0, old_len, &removed, 0, 0, 0, 0, 0, 0, 0);
    if (rc == E_OK) {
      rc = state_(en, ep, 0);                // empty tree: cannot fail
      if (rc != E_OK && old_len) prim_(0, 0, 0, 0, 0, removed.data(), removed.size(), 0, 0, 0);
    }
    if (rc == E_OK && new_len) {
      tree_.count_refs(added.data(), added.size());
      rc = prim_(0, 0, 0, 0, 0, added.data(), added.size(), 0, 0, 0);
      if (rc != E_OK) {
        state_(eo, ep, 0);
        if (old_len) prim_(0, 0, 0, 0, 0, removed.data(), removed.size(), 0, 0, 0);
      }
    }
    if (rc == E_OK) {
      if (old_len) undo_.rec_delete(0, old_len, removed);
      undo_.rec_state(eo, en, ep, ep);
      if (new_len) undo_.rec_insert(0, new_len, added);
    }
    if (implicit) { s.anchor = s.caret = origin_; end_edit(s); }
    return rc;
  }
}

EolStyle Document::eol_style() const { return eol_; }
void Document::set_eol_style(EolStyle s) { set_enc_(enc_, cr_mode_, s); }
const EolCounts& Document::eol_counts() const { return eolc_; }

bool Document::eol_mixed() const
{
  u32 k = (eolc_.crlf > 0 ? 1u : 0u) + (eolc_.lf > 0 ? 1u : 0u) + (cr_mode_ && eolc_.cr > 0 ? 1u : 0u);
  return k > 1;
}

struct EolScan {                             // convert_eol output and the changed span
  EBytes out;
  epos first, last, out_first, out_last;
};

// Append one EOL: the target bytes if this EOL changes (recording the changed span), else the original bytes.
static int eol_put(EolScan* sc, int change, const u8* tgt, u32 tn, const u8* orig, u32 on, epos s, epos e)
{
  if (!change) return sc->out.append(orig, on);
  if (sc->first == EPOS_NONE) { sc->first = s; sc->out_first = sc->out.size(); }
  if (sc->out.append(tgt, tn) != E_OK) return E_NOMEM;
  sc->last = e;
  sc->out_last = sc->out.size();
  return E_OK;
}

int Document::convert_eol(EolStyle to, ProgressCb* cb)
{
  EolScan sc;
  u8 buf[4096], tgt[4], crb[2], lfb[2], crlfb[4];
  u32 unit = unit_(), tn = 0, v, i, j, n;
  int be = enc_.kind == EK_UTF16BE, rc = E_OK, implicit = depth_ == 0, pend_cr = 0;
  epos len = length(), o, pend_at = 0;
  bool new_crm = to == EOL_CR;
  Selection s;
  if (ro_) return E_READONLY;
  if (busy_ || indexing_) return E_BUSY;
  if (ondemand_) return E_TOOBIG;
  if (unit == 1) { crb[0] = 0x0D; lfb[0] = 0x0A; }
  else if (be) { crb[0] = 0; crb[1] = 0x0D; lfb[0] = 0; lfb[1] = 0x0A; }
  else { crb[0] = 0x0D; crb[1] = 0; lfb[0] = 0x0A; lfb[1] = 0; }
  for (j = 0; j < unit; j++) { crlfb[j] = crb[j]; crlfb[unit + j] = lfb[j]; }
  if (to == EOL_CRLF) { memcpy(tgt, crlfb, 2 * unit); tn = 2 * unit; }
  else if (to == EOL_LF) { memcpy(tgt, lfb, unit); tn = unit; }
  else { memcpy(tgt, crb, unit); tn = unit; }
  sc.first = EPOS_NONE; sc.last = sc.out_first = sc.out_last = 0;
  // one pass over the units of [origin, len): copy, rewriting every CRLF, bare LF and lone CR that differs
  for (o = origin_; o < len && rc == E_OK; ) {
    n = len - o;
    if (n > sizeof(buf)) n = sizeof(buf);
    if (unit == 2) n &= ~1u;
    if (n == 0) break;
    rc = tree_.read(o, buf, n);
    if (rc != E_OK) return rc;
    for (i = 0; i + unit <= n && rc == E_OK; i += unit) {
      epos at = o + i;
      v = unit == 1 ? buf[i] : (be ? ((u32)buf[i] << 8 | buf[i + 1]) : ((u32)buf[i + 1] << 8 | buf[i]));
      if (pend_cr) {
        pend_cr = 0;
        if (v == 0x0A) {                     // CRLF
          rc = eol_put(&sc, to != EOL_CRLF, tgt, tn, crlfb, 2 * unit, pend_at, at + unit);
          continue;
        }
        rc = eol_put(&sc, to != EOL_CR, tgt, tn, crb, unit, pend_at, pend_at + unit);   // lone CR
        if (rc != E_OK) break;
      }
      if (v == 0x0D) { pend_cr = 1; pend_at = at; }
      else if (v == 0x0A) rc = eol_put(&sc, to != EOL_LF, tgt, tn, lfb, unit, at, at + unit);
      else rc = sc.out.append(buf + i, unit);
    }
    o += n;
    if (rc == E_OK && cb && cb->progress(o, len)) rc = E_CANCEL;
  }
  if (rc == E_OK && pend_cr) rc = eol_put(&sc, to != EOL_CR, tgt, tn, crb, unit, pend_at, pend_at + unit);
  if (rc != E_OK) return rc;
  if (sc.first == EPOS_NONE && new_crm == cr_mode_ && to == eol_) return E_OK;
  s.anchor = s.caret = origin_;
  if (implicit) begin_edit(UK_OTHER, s, 0);
  rc = undo_.reserve(3, 0);
  if (rc == E_OK && sc.first != EPOS_NONE)
    rc = edit_record_(sc.first, sc.last - sc.first, sc.out.data() + sc.out_first, sc.out_last - sc.out_first);
  sc.out.release();
  if (rc == E_OK && (new_crm != cr_mode_ || to != eol_)) {
    u32 eo = pack_enc(enc_), po = pack_eol(eol_, cr_mode_), pn = pack_eol(to, new_crm);
    rc = state_(eo, pn, 0);
    if (rc == E_OK) undo_.rec_state(eo, eo, po, pn);
  }
  if (implicit) end_edit(s);
  return rc;
}

const u8* Document::eol_bytes(u32* n) const { if (n) *n = eolb_n_; return eolb_; }

// ---------------------------------------------------------------------------------------------------------------
// State and listeners

void Document::set_read_only(bool ro) { ro_ = ro; }
bool Document::read_only() const { return ro_; }
void Document::set_busy(bool busy) { busy_ = busy; }
bool Document::editable() const { return !ro_ && !indexing_ && !busy_; }

int Document::add_listener(DocListener* l)
{
  u32 i;
  if (!l) return E_RANGE;
  for (i = 0; i < listeners_.size(); i++) if (listeners_[i] == l) return E_OK;
  return listeners_.push(l);
}

void Document::remove_listener(DocListener* l)
{
  u32 i;
  for (i = 0; i < listeners_.size(); i++) {
    if (listeners_[i] != l) continue;
    if (dispatching_) { listeners_[i] = 0; lst_dirty_ = true; }
    else listeners_.erase(i, 1);
    return;
  }
}

PieceTree* Document::tree() { return &tree_; }
u32 Document::change_seq() const { return change_seq_; }
