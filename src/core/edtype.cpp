// edtype.cpp - typing and deleting, word movement, smart home, ed_lang_from (DESIGN 6.18, 8.3). Owner: T08.
//
// Conventions (all functions):
// - Positions are byte offsets on character boundaries, never below the text origin (BOM). The selection in EdCtx is
//   clamped to [origin, length] before use.
// - Every mutation is one Document::replace inside a begin_edit/end_edit group. Kinds: typing UK_TYPING (merges per
//   DESIGN 6.6), Backspace UK_BACKSPACE, Del UK_DELETE, Enter/paste/word deletes UK_OTHER (never merge).
// - On failure nothing changes (Document guarantees it) and e->sel is left as it was.
// - A line's EOL (CRLF, LF or CR, in code units of the encoding) is always deleted as a whole.
#include "edops.h"
#include "ucd.h"
#include "layout.h"
#include <string.h>

enum { CC_SPACE = 1, CC_WORD = 2, CC_PUNCT = 3 };
enum { ED_SCAN_MAX = 1000000 };          // word scans stop after this many characters (huge one-word lines)

static int ed_class(ecp c)
{
  if (ECP_IS_BAD(c)) return CC_PUNCT;
  if (uc_is_space(c)) return CC_SPACE;
  if (uc_is_word(c)) return CC_WORD;
  return CC_PUNCT;
}

static epos ed_vstart(Document* d, u32 line)   // visible line start (line 0 starts after the BOM)
{
  epos s = d->line_start(line), o = d->text_origin();
  return s < o ? o : s;
}

static int ed_check(Document* d)
{
  if (d->read_only()) return E_READONLY;
  if (!d->editable()) return E_BUSY;
  return E_OK;
}

static void ed_clamp(EdCtx* e)
{
  Document* d = e->doc;
  epos o = d->text_origin(), n = d->length();
  if (e->sel.anchor < o) e->sel.anchor = o;
  if (e->sel.caret < o) e->sel.caret = o;
  if (e->sel.anchor > n) e->sel.anchor = n;
  if (e->sel.caret > n) e->sel.caret = n;
}

// One replace as one undo group; on success the selection collapses to caret_after.
static int ed_apply(EdCtx* e, u32 kind, epos pos, epos del, const u8* ins, epos n, epos caret_after)
{
  Document* d = e->doc;
  Selection after;
  int rc;
  d->begin_edit(kind, e->sel, e->now_ms);
  rc = d->replace(pos, del, ins, n);
  if (rc == E_OK) { after.anchor = after.caret = caret_after; }
  else after = e->sel;
  d->end_edit(after);
  if (rc == E_OK) e->sel = after;
  return rc;
}

static int ed_delete_range(EdCtx* e, u32 kind, epos a, epos b)
{
  if (b <= a) return E_OK;
  return ed_apply(e, kind, a, b - a, 0, 0, a);
}

// Display cell of pos within its line (tabs, wide chars, control pictures), via LineLayout. Lines whose prefix is
// longer than LAY_WINDOW bytes report 0 (only used for tab stops).
static u32 ed_cell_of(Document* d, epos pos, u8 tab_width)
{
  u32 line = d->line_of(pos);
  epos ls = ed_vstart(d, line);
  EBytes buf;
  LineLayout lay;
  LayoutOpts o;
  if (pos <= ls || pos - ls > LAY_WINDOW) return 0;
  if (buf.resize(pos - ls) != E_OK) return 0;
  if (d->read(ls, buf.data(), pos - ls) != E_OK) return 0;
  o.tab_width = tab_width ? tab_width : 4; o.show_ws = 0; o.pad[0] = o.pad[1] = 0;
  if (lay.build(buf.data(), buf.size(), d->codec(), o, 0, 1) != E_OK) return 0;
  return lay.end_cell();
}

static int ed_encode(Document* d, ecp c, u8* out)   // 0 = not representable
{
  return (int)d->codec()->encode(c, out);
}

static int ed_is_eol_at(Document* d, epos pos)       // pos is the end of its line (EOL or end of document)
{
  return pos >= d->line_end(d->line_of(pos));
}

void ed_lang_from(const LangInfo* li, u8 tab_width, u8 use_spaces, EdLang* out)
{
  u32 i;
  memset(out, 0, sizeof(*out));
  out->tab_width = tab_width ? tab_width : 4;
  out->use_spaces = use_spaces ? 1 : 0;
  if (!li) { memcpy(out->braces, "()[]{}", 7); return; }
  out->lc_len = li->line_comment_len > 8 ? 8 : li->line_comment_len;
  for (i = 0; i < out->lc_len; i++) out->lc[i] = li->line_comment[i];
  out->bo_len = li->block_open_len > 4 ? 4 : li->block_open_len;
  for (i = 0; i < out->bo_len; i++) out->bo[i] = li->block_open[i];
  out->bc_len = li->block_close_len > 4 ? 4 : li->block_close_len;
  for (i = 0; i < out->bc_len; i++) out->bc[i] = li->block_close[i];
  if (li->tab_width) out->tab_width = li->tab_width;
  if (li->use_tabs == 0) out->use_spaces = 1;
  else if (li->use_tabs == 1) out->use_spaces = 0;
  for (i = 0; i + 1 < sizeof(out->braces) && li->braces[i]; i++) out->braces[i] = li->braces[i];
  out->braces[i] = 0;
}

int ed_type(EdCtx* e, ecp c)
{
  Document* d = e->doc;
  u8 enc[8];
  int n, rc;
  epos lo, hi;
  rc = ed_check(d);
  if (rc != E_OK) return rc;
  ed_clamp(e);
  lo = sel_lo(e->sel); hi = sel_hi(e->sel);
  if (c == '\t' && e->lang.use_spaces) {           // spaces to the next tab stop
    u8 sp[64 * 4];
    u32 tw = e->lang.tab_width ? e->lang.tab_width : 4, col, k, i, sl;
    u8 one[8];
    sl = (u32)ed_encode(d, ' ', one);
    if (!sl) return E_RANGE;
    col = ed_cell_of(d, lo, (u8)tw);
    k = tw - col % tw;
    if (k > 64) k = 64;
    for (i = 0; i < k; i++) memcpy(sp + i * sl, one, sl);
    return ed_apply(e, UK_TYPING, lo, hi - lo, sp, k * sl, lo + k * sl);
  }
  n = ed_encode(d, c, enc);
  if (n <= 0) return E_RANGE;
  if (lo == hi && e->overwrite && !ed_is_eol_at(d, lo)) {
    epos nx = d->char_next(lo);
    if (nx > lo) hi = nx;
  }
  return ed_apply(e, UK_TYPING, lo, hi - lo, enc, (epos)n, lo + (epos)n);
}

int ed_newline(EdCtx* e)
{
  Document* d = e->doc;
  EBytes ins;
  const u8* eb;
  u32 en = 0;
  int rc;
  epos lo, hi;
  rc = ed_check(d);
  if (rc != E_OK) return rc;
  ed_clamp(e);
  lo = sel_lo(e->sel); hi = sel_hi(e->sel);
  eb = d->eol_bytes(&en);
  if (ins.append(eb, en) != E_OK) return E_NOMEM;
  if (e->auto_indent) {                            // copy the leading blanks of the line (only those before lo)
    u32 line = d->line_of(lo);
    epos ls = ed_vstart(d, line), p = ls;
    while (p < lo && p - ls < 4096) {
      ecp ch;
      u32 used = d->decode(p, &ch);
      if (!used || (ch != ' ' && ch != '\t')) break;
      p += used;
    }
    if (p > ls) {
      u32 base = ins.size();
      if (ins.resize(base + (p - ls)) != E_OK) return E_NOMEM;
      if (d->read(ls, ins.data() + base, p - ls) != E_OK) return E_IO;
    }
  }
  d->break_undo_merge();
  rc = ed_apply(e, UK_OTHER, lo, hi - lo, ins.data(), ins.size(), lo + ins.size());
  d->break_undo_merge();
  return rc;
}

int ed_backspace(EdCtx* e)
{
  Document* d = e->doc;
  epos pos, prev;
  u32 line;
  int rc = ed_check(d);
  if (rc != E_OK) return rc;
  ed_clamp(e);
  if (!sel_empty(e->sel)) return ed_delete_range(e, UK_BACKSPACE, sel_lo(e->sel), sel_hi(e->sel));
  pos = e->sel.caret;
  if (pos <= d->text_origin()) return E_OK;
  line = d->line_of(pos);
  if (line > 0 && pos == d->line_start(line)) prev = d->line_end(line - 1);   // the whole EOL
  else prev = d->char_prev(pos);
  return ed_delete_range(e, UK_BACKSPACE, prev, pos);
}

int ed_delete(EdCtx* e)
{
  Document* d = e->doc;
  epos pos, nx;
  u32 line;
  int rc = ed_check(d);
  if (rc != E_OK) return rc;
  ed_clamp(e);
  if (!sel_empty(e->sel)) return ed_delete_range(e, UK_DELETE, sel_lo(e->sel), sel_hi(e->sel));
  pos = e->sel.caret;
  if (pos >= d->length()) return E_OK;
  line = d->line_of(pos);
  if (pos >= d->line_end(line)) nx = d->line_next(line);                     // the whole EOL
  else nx = d->char_next(pos);
  return ed_delete_range(e, UK_DELETE, pos, nx);
}

int ed_delete_word(EdCtx* e, int forward)
{
  Document* d = e->doc;
  epos pos, to;
  int rc = ed_check(d);
  if (rc != E_OK) return rc;
  ed_clamp(e);
  if (!sel_empty(e->sel)) return ed_delete_range(e, UK_OTHER, sel_lo(e->sel), sel_hi(e->sel));
  pos = e->sel.caret;
  if (forward) {
    to = ed_word_right(d, pos);
    return ed_delete_range(e, UK_OTHER, pos, to);
  }
  to = ed_word_left(d, pos);
  return ed_delete_range(e, UK_OTHER, to, pos);
}

int ed_insert_text(EdCtx* e, const ecp* s, u32 n, u32* replaced)
{
  Document* d = e->doc;
  EBytes out;
  const u8* eb;
  u32 en = 0, i = 0, bad_total = 0;
  int rc;
  epos lo, hi;
  if (replaced) *replaced = 0;
  rc = ed_check(d);
  if (rc != E_OK) return rc;
  ed_clamp(e);
  lo = sel_lo(e->sel); hi = sel_hi(e->sel);
  eb = d->eol_bytes(&en);
  while (i < n) {
    u32 j = i, bad = 0;
    while (j < n && s[j] != '\r' && s[j] != '\n') j++;
    if (j > i) {
      rc = codec_encode_str(d->codec(), s + i, j - i, &out, &bad, '?');
      if (rc != E_OK) return rc;
      bad_total += bad;
    }
    if (j < n) {
      if (s[j] == '\r' && j + 1 < n && s[j + 1] == '\n') j++;
      if (out.append(eb, en) != E_OK) return E_NOMEM;
      j++;
    }
    i = j;
  }
  rc = ed_apply(e, UK_OTHER, lo, hi - lo, out.data(), out.size(), lo + out.size());
  if (rc == E_OK && replaced) *replaced = bad_total;
  return rc;
}

int ed_insert_bytes(EdCtx* e, const u8* p, epos n)
{
  Document* d = e->doc;
  epos lo, hi;
  int rc = ed_check(d);
  if (rc != E_OK) return rc;
  ed_clamp(e);
  lo = sel_lo(e->sel); hi = sel_hi(e->sel);
  return ed_apply(e, UK_OTHER, lo, hi - lo, p, n, lo + n);
}

// Ctrl+Right: skip the run of the class at pos, then blanks; the EOL is one stop.
epos ed_word_right(Document* d, epos pos)
{
  u32 line, k = 0;
  epos le;
  ecp c;
  u32 used;
  int cls;
  if (pos < d->text_origin()) pos = d->text_origin();
  if (pos >= d->length()) return d->length();
  line = d->line_of(pos);
  le = d->line_end(line);
  if (pos >= le) return d->line_next(line);
  used = d->decode(pos, &c);
  cls = ed_class(c);
  while (pos < le && used && ed_class(c) == cls && k++ < ED_SCAN_MAX) {
    pos += used;
    used = pos < le ? d->decode(pos, &c) : 0;
  }
  while (pos < le && used && ed_class(c) == CC_SPACE && k++ < ED_SCAN_MAX) {
    pos += used;
    used = pos < le ? d->decode(pos, &c) : 0;
  }
  return pos;
}

// Ctrl+Left: skip blanks backwards, then the run of the class before; the line start is one stop.
epos ed_word_left(Document* d, epos pos)
{
  u32 line, k = 0;
  epos ls, p;
  ecp c = 0;
  int cls;
  if (pos <= d->text_origin()) return d->text_origin();
  if (pos > d->length()) pos = d->length();
  line = d->line_of(pos);
  ls = ed_vstart(d, line);
  if (pos <= ls) return line > 0 ? d->line_end(line - 1) : ls;
  if (pos > d->line_end(line)) pos = d->line_end(line);   // inside the EOL (never a caret position)
  while (pos > ls && k++ < ED_SCAN_MAX) {
    p = d->char_prev(pos);
    d->decode(p, &c);
    if (ed_class(c) != CC_SPACE) break;
    pos = p;
  }
  if (pos <= ls) return ls;
  p = d->char_prev(pos);
  d->decode(p, &c);
  cls = ed_class(c);
  while (pos > ls && k++ < ED_SCAN_MAX) {
    p = d->char_prev(pos);
    d->decode(p, &c);
    if (ed_class(c) != cls) break;
    pos = p;
  }
  return pos;
}

// Double-click: the run of same-class characters around pos (words, punctuation or blanks). At the end of a line or
// next to a non-word character after a word, the word before pos is taken.
void ed_word_at(Document* d, epos pos, Selection* out)
{
  u32 line, used, k = 0;
  epos ls, le, a, b;
  ecp c = 0, cb = 0;
  int cls;
  if (pos < d->text_origin()) pos = d->text_origin();
  if (pos > d->length()) pos = d->length();
  out->anchor = out->caret = pos;
  line = d->line_of(pos);
  ls = ed_vstart(d, line);
  le = d->line_end(line);
  if (pos > le) pos = le;
  if (le <= ls) { out->anchor = out->caret = ls; return; }
  if (pos < le) d->decode(pos, &c);
  if (pos > ls) d->decode(d->char_prev(pos), &cb);
  if (pos >= le || (pos > ls && ed_class(c) != CC_WORD && ed_class(cb) == CC_WORD)) {
    pos = d->char_prev(pos);
    c = cb;
  }
  cls = ed_class(c);
  a = pos;
  while (a > ls && k++ < ED_SCAN_MAX) {
    epos p = d->char_prev(a);
    d->decode(p, &c);
    if (ed_class(c) != cls) break;
    a = p;
  }
  b = pos;
  while (b < le && k++ < ED_SCAN_MAX) {
    used = d->decode(b, &c);
    if (!used || ed_class(c) != cls) break;
    b += used;
  }
  out->anchor = a;
  out->caret = b;
}

// Home: first non-blank of the line; when already there (or the line is blank up to pos), the line start.
epos ed_smart_home(Document* d, epos pos)
{
  u32 line, used, k = 0;
  epos ls, le, p;
  ecp c;
  if (pos < d->text_origin()) pos = d->text_origin();
  if (pos > d->length()) pos = d->length();
  line = d->line_of(pos);
  ls = ed_vstart(d, line);
  le = d->line_end(line);
  p = ls;
  while (p < le && k++ < ED_SCAN_MAX) {
    used = d->decode(p, &c);
    if (!used || (c != ' ' && c != '\t')) break;
    p += used;
  }
  return pos == p ? ls : p;
}
