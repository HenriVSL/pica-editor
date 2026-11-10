// layout.cpp - LineLayout, LongLineIndex (DESIGN 6.12, 8.1, 14.2). Owner: T06.
//
// One display cell per column of the monospace grid. Rules (DESIGN 6.12):
//   TAB      LK_TAB, advances to the next multiple of tab_width (absolute cell), glyphs: spaces (show_ws: U+00BB first)
//   ' '      LK_SPACE, 1 cell, glyph ' ' (show_ws: U+00B7)
//   C0/DEL   LK_CTRL, control picture "NUL".."US", "DEL" (width = name length). Extension: C1 controls U+0080-U+009F
//            are control pictures too ("PAD".."APC"; 2-3 cells).
//   bad      LK_BAD, "xHH" (3 cells) for ECP_BADBYTE, "uHHHH" (5 cells) for ECP_BADUNIT
//   width 0  LK_ZERO, width 0, cell = cell of the base character (drawn overlapping). A zero-width character with no
//            preceding character in the same build is laid out as a normal 1-cell character (so it stays visible
//            and clickable); LongLineIndex never puts a checkpoint in front of a zero-width character, so layouts
//            built from checkpoints agree with a full layout.
//   width 2  LK_WIDE, 2 cells (the renderer uses a CJK font if present, else a placeholder box)
//   other    LK_CHAR (drawable) or LK_NODRAW (uc_is_drawable false), 1 cell
//   > FFFF   LK_NODRAW, 1 cell, glyph U+FFFD (9x GDI has no surrogate support)
#include "layout.h"
#include "layout_int.h"
#include "ucd.h"

enum { LAY_GLYPH_MAX = 65535, LAY_TAB_MAX = 255 };

static LayPropFn g_width_fn = uc_width;
static LayPropFn g_draw_fn = uc_is_drawable;

void lay_set_props(LayPropFn width, LayPropFn drawable)
{
  g_width_fn = width ? width : uc_width;
  g_draw_fn = drawable ? drawable : uc_is_drawable;
}

static const char* const k_c0_names[32] = {
  "NUL", "SOH", "STX", "ETX", "EOT", "ENQ", "ACK", "BEL", "BS", "HT", "LF", "VT", "FF", "CR", "SO", "SI",
  "DLE", "DC1", "DC2", "DC3", "DC4", "NAK", "SYN", "ETB", "CAN", "EM", "SUB", "ESC", "FS", "GS", "RS", "US"
};
static const char* const k_c1_names[32] = {
  "PAD", "HOP", "BPH", "NBH", "IND", "NEL", "SSA", "ESA", "HTS", "HTJ", "VTS", "PLD", "PLU", "RI", "SS2", "SS3",
  "DCS", "PU1", "PU2", "STS", "CCH", "MW", "SPA", "EPA", "SOS", "SGC", "SCI", "CSI", "ST", "OSC", "PM", "APC"
};
static const char k_hexdig[] = "0123456789ABCDEF";

const char* lay_ctrl_name(u8 c)
{
  if (c < 0x20) return k_c0_names[c];
  if (c == 0x7F) return "DEL";
  if (c >= 0x80 && c <= 0x9F) return k_c1_names[c - 0x80];
  return 0;
}

// Fallback when no codec is given: one byte = one character with the byte's value.
class LayByteCodec : public Codec {
public:
  LayByteCodec() { kind = EK_SBCS; unit = 1; max_len = 1; ascii_compat = 1; self_sync = 1; pad = 0; cp = 28591; }
  u32 decode(const u8* p, u32 avail, int eof, ecp* out) const { (void)avail; (void)eof; *out = p[0]; return 1; }
  u32 encode(ecp c, u8* out) const { if (c > 0xFF) return 0; out[0] = (u8)c; return 1; }
};
static const LayByteCodec g_byte_codec;

static int fast_ascii(const Codec* c)
{
  return c->ascii_compat && (c->kind == EK_SBCS || c->kind == EK_UTF8);
}

static u32 sat_add(u32 a, u32 b)
{
  return a > 0xFFFFFFFFu - b ? 0xFFFFFFFFu : a + b;
}

// Classify one character. g (may be NULL) receives the glyphs (room for LAY_TAB_MAX + 1); *ng = glyph count.
// Returns the width in cells.
static u32 lay_classify(ecp cp, u32 cell, const LayoutOpts& o, int has_prev, u8* kind, u16* g, u32* ng)
{
  u32 w, i;
  int pw;
  if (cp == 9) {
    u32 tw = o.tab_width ? o.tab_width : 1;
    w = tw - cell % tw;
    *kind = LK_TAB;
    if (g) {
      for (i = 0; i < w; i++) g[i] = ' ';
      if (o.show_ws) g[0] = 0xBB;
    }
    *ng = w;
    return w;
  }
  if (cp == ' ') {
    *kind = LK_SPACE;
    if (g) g[0] = (u16)(o.show_ws ? 0xB7 : ' ');
    *ng = 1;
    return 1;
  }
  if (cp < 0x20 || (cp >= 0x7F && cp <= 0x9F)) {
    const char* nm = lay_ctrl_name((u8)cp);
    w = 0;
    while (nm[w]) { if (g) g[w] = (u8)nm[w]; w++; }
    *kind = LK_CTRL;
    *ng = w;
    return w;
  }
  if (ECP_IS_BAD(cp)) {
    *kind = LK_BAD;
    if (cp >= 0x120000u && cp <= 0x12FFFFu) {
      u32 u = cp - 0x120000u;
      if (g) {
        g[0] = 'u';
        g[1] = (u8)k_hexdig[(u >> 12) & 15]; g[2] = (u8)k_hexdig[(u >> 8) & 15];
        g[3] = (u8)k_hexdig[(u >> 4) & 15]; g[4] = (u8)k_hexdig[u & 15];
      }
      *ng = 5;
      return 5;
    }
    {
      u32 b = cp & 0xFF;            // ECP_BADBYTE (anything else out of range is shown as its low byte)
      if (g) { g[0] = 'x'; g[1] = (u8)k_hexdig[b >> 4]; g[2] = (u8)k_hexdig[b & 15]; }
      *ng = 3;
      return 3;
    }
  }
  *ng = 1;
  if (cp > 0xFFFF) {
    *kind = LK_NODRAW;
    if (g) g[0] = 0xFFFD;
    return 1;
  }
  if (g) g[0] = (u16)cp;
  pw = g_width_fn(cp);
  if (pw == 0) {
    if (has_prev) { *kind = LK_ZERO; return 0; }
    pw = 1;
  }
  if (pw >= 2) { *kind = LK_WIDE; return 2; }
  *kind = (u8)(g_draw_fn(cp) ? LK_CHAR : LK_NODRAW);
  return 1;
}

// ---- LineLayout ----

LineLayout::LineLayout() : start_cell_(0), end_cell_(0), nbytes_(0) {}
LineLayout::~LineLayout() {}

int LineLayout::build(const u8* p, u32 n, const Codec* c, const LayoutOpts& o, u32 start_cell, int eof)
{
  u16 gbuf[LAY_TAB_MAX + 1];
  u32 i = 0, cell = start_cell, base_cell = start_cell;
  int has_prev = 0, fast;
  chars_.clear();
  glyphs_.clear();
  start_cell_ = end_cell_ = start_cell;
  nbytes_ = 0;
  if (!p) n = 0;
  if (!c) c = &g_byte_codec;
  fast = fast_ascii(c);
  while (i < n) {
    LayChar lc;
    ecp cp;
    u32 used, w, ng, k;
    u8 kind;
    if (fast && p[i] < 0x80) { cp = p[i]; used = 1; }
    else {
      used = c->decode(p + i, n - i, eof, &cp);
      if (used == 0) break;                         // incomplete character at the end, more bytes follow
      if (used > n - i) used = n - i;
    }
    if (fast && cp > 0x20 && cp < 0x7F) {           // printable ASCII
      kind = LK_CHAR; w = 1; ng = 1; gbuf[0] = (u16)cp;
    } else {
      w = lay_classify(cp, cell, o, has_prev, &kind, gbuf, &ng);
    }
    if (glyphs_.size() + ng > LAY_GLYPH_MAX) break;
    lc.off = i;
    lc.glyph = (u16)glyphs_.size();
    lc.width = (u8)w;
    lc.kind = kind;
    lc.nglyph = (u8)ng;
    lc.pad[0] = lc.pad[1] = lc.pad[2] = 0;
    if (kind == LK_ZERO) lc.cell = base_cell;
    else { lc.cell = cell; base_cell = cell; cell = sat_add(cell, w); }
    if (chars_.push(lc) != E_OK) goto nomem;
    for (k = 0; k < ng; k++) if (glyphs_.push(gbuf[k]) != E_OK) goto nomem;
    has_prev = 1;
    i += used;
  }
  nbytes_ = i;
  end_cell_ = cell;
  return E_OK;
nomem:
  chars_.clear();
  glyphs_.clear();
  nbytes_ = 0;
  end_cell_ = start_cell_;
  return E_NOMEM;
}

u32 LineLayout::count() const { return chars_.size(); }
const LayChar* LineLayout::chars() const { return chars_.data(); }
const u16* LineLayout::glyphs() const { return glyphs_.data(); }
u32 LineLayout::glyph_count() const { return glyphs_.size(); }
u32 LineLayout::end_cell() const { return end_cell_; }
u32 LineLayout::bytes() const { return nbytes_; }

u32 LineLayout::char_at_offset(u32 off) const
{
  u32 lo = 0, hi = chars_.size();
  if (off >= nbytes_ || hi == 0) return chars_.size();
  // last char with off <= target
  while (hi - lo > 1) {
    u32 mid = lo + (hi - lo) / 2;
    if (chars_[mid].off <= off) lo = mid; else hi = mid;
  }
  return lo;
}

u32 LineLayout::cell_of_offset(u32 off) const
{
  u32 i = char_at_offset(off);
  if (i >= chars_.size()) return end_cell_;
  if (chars_[i].kind == LK_ZERO) {                  // caret before a combining mark: after its base
    while (i > 0 && chars_[i].kind == LK_ZERO) i--;
    return sat_add(chars_[i].cell, chars_[i].width);
  }
  return chars_[i].cell;
}

u32 LineLayout::offset_of_cell(u32 cell, int nearest) const
{
  u32 n = chars_.size(), lo, hi, i;
  const LayChar* ch;
  if (n == 0) return 0;
  if (cell <= start_cell_) return 0;
  if (cell >= end_cell_) return nbytes_;
  lo = 0; hi = n;                                   // last char with cell <= target
  while (hi - lo > 1) {
    u32 mid = lo + (hi - lo) / 2;
    if (chars_[mid].cell <= cell) lo = mid; else hi = mid;
  }
  i = lo;
  while (i > 0 && chars_[i].kind == LK_ZERO) i--;
  ch = &chars_[i];
  if (!nearest || cell == ch->cell || (cell - ch->cell) * 2 < (u32)ch->width) return ch->off;
  for (i = i + 1; i < n; i++) if (chars_[i].kind != LK_ZERO) return chars_[i].off;
  return nbytes_;
}

// Can a row break after this character (word mode)?
static int break_after(const LayChar& ch, const u16* glyphs)
{
  u16 g;
  if (ch.kind == LK_SPACE || ch.kind == LK_TAB || ch.kind == LK_WIDE) return 1;
  if (ch.kind != LK_CHAR) return 0;
  g = glyphs[ch.glyph];
  if (g < 0x80) {
    switch (g) {
    case '-': case ',': case '.': case ';': case ':': case '!': case '?': case ')': case ']': case '}':
    case '/': case '\\': case '|': case '&': case '+': case '=': case '*': case '%': case '>':
      return 1;
    default:
      return 0;
    }
  }
  return g == 0x2013 || g == 0x2014 || g == 0x2026 || g == 0x00BB;
}

int LineLayout::wrap_rows(u32 width_cells, int word, Vec<u32>* row_starts) const
{
  u32 n = chars_.size(), w = width_cells ? width_cells : 1, k = 0, i = 0, row_cell;
  if (!row_starts) return E_RANGE;
  row_starts->clear();
  if (row_starts->push(0) != E_OK) return E_NOMEM;
  if (n == 0) return E_OK;
  row_cell = chars_[0].cell;
  while (i < n) {
    const LayChar& ch = chars_[i];
    u32 brk, b;
    if (ch.kind == LK_ZERO || i == k || ch.cell - row_cell + ch.width <= w) { i++; continue; }
    brk = i;                                        // hard break before i (never a zero-width char)
    if (word) {
      if (ch.kind == LK_SPACE || ch.kind == LK_TAB) {   // whitespace may hang past the edge
        b = i;
        while (b < n && (chars_[b].kind == LK_SPACE || chars_[b].kind == LK_TAB || chars_[b].kind == LK_ZERO)) b++;
        brk = b;
      } else {
        for (b = i; b > k; b--) {                   // last break opportunity inside the row
          u32 base;
          if (chars_[b].kind == LK_ZERO) continue;
          base = b - 1;
          while (base > k && chars_[base].kind == LK_ZERO) base--;
          if (break_after(chars_[base], glyphs_.data())) { brk = b; break; }
        }
      }
    }
    if (brk >= n) break;
    if (row_starts->push(brk) != E_OK) return E_NOMEM;
    k = brk;
    row_cell = chars_[brk].cell;
    i = brk;
  }
  return E_OK;
}

// ---- LongLineIndex ----

LongLineIndex::LongLineIndex() : total_(0), start_(0), end_(0) {}
LongLineIndex::~LongLineIndex() {}

int LongLineIndex::build(ByteSource* s, const Codec* c, const LayoutOpts& o, epos start, epos end, ProgressCb* cb)
{
  epos pos, last_cp, next_report;
  u32 cell = 0;
  int has_prev = 0, fast, rc = E_OK;
  clear();
  if (!s || start > end || end > s->size()) return E_RANGE;
  if (!c) c = &g_byte_codec;
  fast = fast_ascii(c);
  start_ = start;
  end_ = end;
  if (cp_pos_.push(start) != E_OK || cp_cell_.push(0) != E_OK) { rc = E_NOMEM; goto fail; }
  last_cp = start;
  next_report = start + (end - start < 262144u ? end - start : 262144u);
  pos = start;
  while (pos < end) {
    epos avail = 0, i = 0;
    int eof;
    const u8* p = s->get(pos, &avail);
    if (!p || avail == 0) { rc = s->err() != E_OK ? s->err() : E_IO; goto fail; }
    if (avail > end - pos) avail = end - pos;
    if (avail > 65536u) avail = 65536u;             // progress and cancel at least every 64 KB
    eof = (pos + avail == end);
    while (i < avail) {
      ecp cp;
      u32 used, w, ng;
      u8 kind;
      u8 tmp[8];
      int straddle = 0;
      if (fast && p[i] > 0x20 && p[i] < 0x7F) {     // printable ASCII: never a checkpoint problem
        if (pos + i - last_cp >= LAY_CHECKPOINT) {
          if (cp_pos_.push(pos + i) != E_OK || cp_cell_.push(cell) != E_OK) { rc = E_NOMEM; goto fail; }
          last_cp = pos + i;
        }
        cell = sat_add(cell, 1);
        has_prev = 1;
        i++;
        continue;
      }
      used = c->decode(p + i, avail - i, eof, &cp);
      if (used == 0) {                              // character straddles the chunk end
        epos have = avail - i, more = end - (pos + avail), take;
        epos k;
        int eof2;
        for (k = 0; k < have && k < 8; k++) tmp[k] = p[i + k];
        take = 8 - have;
        if (take > more) take = more;
        if (take > 0 && s->read(pos + avail, tmp + have, take) != E_OK) {
          rc = s->err() != E_OK ? s->err() : E_IO; goto fail;
        }
        eof2 = (pos + avail + take == end);
        used = c->decode(tmp, have + take, eof2 || have + take >= 8, &cp);
        if (used == 0) { used = 1; cp = ECP_BADBYTE(tmp[0]); }
        straddle = 1;
      }
      w = lay_classify(cp, cell, o, has_prev, &kind, 0, &ng);
      if (kind != LK_ZERO && pos + i - last_cp >= LAY_CHECKPOINT) {
        if (cp_pos_.push(pos + i) != E_OK || cp_cell_.push(cell) != E_OK) { rc = E_NOMEM; goto fail; }
        last_cp = pos + i;
      }
      if (kind != LK_ZERO) cell = sat_add(cell, w);
      has_prev = 1;
      i += used;
      if (straddle) break;                          // p is invalid after read(): fetch the next chunk
    }
    pos += i;
    if (pos > end) pos = end;
    if (cb && pos >= next_report) {
      if (cb->progress(pos - start, end - start)) { rc = E_CANCEL; goto fail; }
      next_report = (end - pos < 262144u) ? end : pos + 262144u;
    }
  }
  total_ = cell;
  return E_OK;
fail:
  clear();
  return rc;
}

int LongLineIndex::at_offset(epos pos, epos* cp_pos, u32* cp_cell) const
{
  u32 lo = 0, hi = cp_pos_.size();
  if (hi == 0) return E_STATE;
  if (pos < start_ || pos > end_) return E_RANGE;
  while (hi - lo > 1) {
    u32 mid = lo + (hi - lo) / 2;
    if (cp_pos_[mid] <= pos) lo = mid; else hi = mid;
  }
  if (cp_pos) *cp_pos = cp_pos_[lo];
  if (cp_cell) *cp_cell = cp_cell_[lo];
  return E_OK;
}

int LongLineIndex::at_cell(u32 cell, epos* cp_pos, u32* cp_cell) const
{
  u32 lo = 0, hi = cp_cell_.size();
  if (hi == 0) return E_STATE;
  while (hi - lo > 1) {
    u32 mid = lo + (hi - lo) / 2;
    if (cp_cell_[mid] <= cell) lo = mid; else hi = mid;
  }
  if (cp_pos) *cp_pos = cp_pos_[lo];
  if (cp_cell) *cp_cell = cp_cell_[lo];
  return E_OK;
}

u32 LongLineIndex::total_cells() const { return total_; }
epos LongLineIndex::start() const { return start_; }
epos LongLineIndex::end() const { return end_; }
void LongLineIndex::clear() { cp_pos_.release(); cp_cell_.release(); total_ = 0; start_ = end_ = 0; }
