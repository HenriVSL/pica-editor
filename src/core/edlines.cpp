// edlines.cpp - line commands, comments, case, indent, brace match (DESIGN 6.18). Owner: T12.
//
// Behaviour follows Notepad++ (Scintilla) exactly where users rely on it:
// - ed_duplicate (Ctrl+D) = SCI_SELECTIONDUPLICATE: a selection is duplicated right after itself and stays selected;
//   without one the caret line is copied below (EOL + line text inserted at the line end), the caret stays.
// - ed_move_lines (Ctrl+Shift+Up/Down) = Scintilla MoveSelectedLines: whole touched lines move one line; afterwards
//   the moved lines are selected (caret at their start); the last line without an EOL is handled by moving EOLs.
// - ed_delete_lines (Ctrl+Shift+L, and Ctrl+L after copying, = LINEDELETE/LINECUT): deletes the touched lines with
//   their EOL; on the last line the EOL before it stays (an empty last line remains). Caret at the start.
// - ed_toggle_comment (Ctrl+Q) = Notepad++ doBlockComment(cm_toggle): every touched non-blank line is toggled on its
//   own: "tok" or "tok " right after the indentation is removed, otherwise "tok " is inserted there. Languages with
//   only a block comment wrap each line as "/* line */" (Notepad++ "advanced mode"). Selection adjusted like N++.
// - ed_indent = Scintilla Indent: multi-line selections re-indent every touched line by one indent step (columns),
//   the indentation is rewritten with tabs or spaces as configured; then the whole lines are selected.
// - ed_change_case: the selection (kept as it was, end adjusted); without a selection the character at the caret,
//   and the caret moves past it (VC6 behaviour). Characters the encoding can't store and bad markers are untouched.
// Every command is one undo group; on failure nothing changes.
#include "edops.h"
#include "edlines_int.h"
#include "ucd.h"
#include "layout.h"
#include <string.h>

enum { EL_BRACE_LINES = 20000, EL_LINE_CAP = 1 << 20, EL_LEX_BUDGET = 40000 };

// ---------------------------------------------------------------------------------------------------------------
// helpers

static epos el_vstart(Document* d, u32 line)   // visible line start (line 0 starts after the BOM)
{
  epos s = d->line_start(line), o = d->text_origin();
  return s < o ? o : s;
}

static int el_check(Document* d)
{
  if (d->read_only()) return E_READONLY;
  if (!d->editable()) return E_BUSY;
  return E_OK;
}

static void el_clamp(Document* d, Selection* s)
{
  epos o = d->text_origin(), n = d->length();
  if (s->anchor < o) s->anchor = o;
  if (s->caret < o) s->caret = o;
  if (s->anchor > n) s->anchor = n;
  if (s->caret > n) s->caret = n;
}

static int el_read_append(Document* d, epos a, epos b, EBytes* out)   // appends bytes [a, b)
{
  u32 base = out->size();
  if (b <= a) return E_OK;
  if (out->resize(base + (b - a)) != E_OK) return E_NOMEM;
  return d->read(a, out->data() + base, b - a);
}

static int el_enc_append(Document* d, const ecp* s, u32 n, EBytes* out)   // E_RANGE if a char can't be stored
{
  u32 i;
  for (i = 0; i < n; i++) {
    u8 b[8];
    u32 len = d->codec()->encode(s[i], b);
    if (!len) return E_RANGE;
    if (out->append(b, len) != E_OK) return E_NOMEM;
  }
  return E_OK;
}

// An undo group that remembers whether anything changed, so a failure half-way can be rolled back.
struct ElGroup { Document* d; Selection before; int changed; };

static void el_begin(ElGroup* g, Document* d, const Selection& before, u32 now_ms)
{
  g->d = d; g->before = before; g->changed = 0;
  d->begin_edit(UK_OTHER, before, now_ms);
}

static int el_rep(ElGroup* g, epos pos, epos del, const u8* ins, epos n)
{
  int rc;
  if (del == 0 && n == 0) return E_OK;
  rc = g->d->replace(pos, del, ins, n);
  if (rc == E_OK) g->changed = 1;
  return rc;
}

static int el_end(ElGroup* g, const Selection& after, int rc)
{
  g->d->end_edit(rc == E_OK ? after : g->before);
  if (rc != E_OK && g->changed) {           // roll back the partial group (rare: out of memory)
    Selection tmp;
    g->d->undo(&tmp);
  }
  return rc;
}

void ed_lines_touched(Document* d, const Selection& s, u32* first, u32* last)
{
  epos lo = sel_lo(s), hi = sel_hi(s);
  u32 a = d->line_of(lo), b = d->line_of(hi);
  if (b > a && hi == el_vstart(d, b)) b--;
  *first = a; *last = b;
}

void ed_lines_range(Document* d, const Selection& s, epos* a, epos* b)
{
  u32 first, last;
  ed_lines_touched(d, s, &first, &last);
  *a = el_vstart(d, first);
  *b = d->line_next(last);
  if (*b < *a) *b = *a;
}

// ---------------------------------------------------------------------------------------------------------------
// Ctrl+D

int ed_duplicate(EdCtx* e)
{
  Document* d = e->doc;
  EBytes buf;
  ElGroup g;
  epos lo, hi, at;
  int rc = el_check(d);
  if (rc != E_OK) return rc;
  el_clamp(d, &e->sel);
  lo = sel_lo(e->sel); hi = sel_hi(e->sel);
  if (lo == hi) {
    u32 line = d->line_of(e->sel.caret), en = 0;
    const u8* eb = d->eol_bytes(&en);
    epos a = el_vstart(d, line), b = d->line_end(line);
    if (buf.append(eb, en) != E_OK) return E_NOMEM;
    rc = el_read_append(d, a, b, &buf);
    at = b;
  } else {
    rc = el_read_append(d, lo, hi, &buf);
    at = hi;
  }
  if (rc != E_OK) return rc;
  el_begin(&g, d, e->sel, e->now_ms);
  rc = el_rep(&g, at, 0, buf.data(), buf.size());
  return el_end(&g, e->sel, rc);
}

// ---------------------------------------------------------------------------------------------------------------
// Ctrl+Shift+L

int ed_delete_lines(EdCtx* e)
{
  Document* d = e->doc;
  ElGroup g;
  Selection after;
  epos a, b;
  int rc = el_check(d);
  if (rc != E_OK) return rc;
  el_clamp(d, &e->sel);
  ed_lines_range(d, e->sel, &a, &b);
  if (a == b) return E_OK;
  after.anchor = after.caret = a;
  el_begin(&g, d, e->sel, e->now_ms);
  rc = el_rep(&g, a, b - a, 0, 0);
  rc = el_end(&g, after, rc);
  if (rc == E_OK) e->sel = after;
  return rc;
}

// ---------------------------------------------------------------------------------------------------------------
// Ctrl+Shift+Up / Down (Scintilla Editor::MoveSelectedLines, translated step by step)

int ed_move_lines(EdCtx* e, int down)
{
  Document* d = e->doc;
  EBytes text;
  ElGroup g;
  Selection after;
  const u8* eb;
  u32 en = 0, start_line, end_line, cur_line;
  epos lo, hi, sel_start, sel_end, len, pos, n;
  int append_eol = 0, end_line_empty, rc;
  rc = el_check(d);
  if (rc != E_OK) return rc;
  el_clamp(d, &e->sel);
  lo = sel_lo(e->sel); hi = sel_hi(e->sel);
  len = d->length();
  eb = d->eol_bytes(&en);
  start_line = d->line_of(lo);
  sel_start = el_vstart(d, start_line);
  sel_end = hi;
  end_line = d->line_of(hi);
  if (hi > el_vstart(d, end_line) || lo == hi) {
    sel_end = d->line_next(end_line);
    append_eol = (sel_end == len && d->line_of(sel_end) == end_line) ? 1 : 0;
    end_line = d->line_of(sel_end);
  }
  end_line_empty = el_vstart(d, end_line) == len;
  if ((sel_start == d->text_origin() && !down) ||
      (sel_end == len && down && !end_line_empty) ||
      (sel_start == sel_end && !(!down && end_line_empty && sel_end == len)))
    return E_OK;                                  // nowhere to move

  el_begin(&g, d, e->sel, e->now_ms);
  rc = E_OK;
  if (down && sel_end == el_vstart(d, d->line_count() - 1) && sel_end > d->text_origin()) {
    // the block ends right before the last line: drop the EOL at its end first
    epos q = d->line_end(d->line_of(sel_end) - 1);
    rc = el_rep(&g, q, sel_end - q, 0, 0);
    sel_end = q;
  }
  if (rc == E_OK) rc = el_read_append(d, sel_start, sel_end, &text);
  cur_line = d->line_of(sel_start);
  if (rc == E_OK) {
    epos del_start = sel_start;
    if (append_eol && sel_start > d->text_origin()) del_start = d->line_end(d->line_of(sel_start) - 1);
    rc = el_rep(&g, del_start, sel_end - del_start, 0, 0);
  }
  if (rc == E_OK && (down ? cur_line + 1 : cur_line - 1) >= d->line_count())
    rc = el_rep(&g, d->length(), 0, eb, en);
  pos = 0; n = 0;
  if (rc == E_OK) {
    u32 target = down ? cur_line + 1 : cur_line - 1;
    pos = el_vstart(d, target);
    rc = el_rep(&g, pos, 0, text.data(), text.size());
    n = text.size();
    if (rc == E_OK && append_eol) {
      rc = el_rep(&g, pos + n, 0, eb, en);
      n += en;
    }
  }
  after.caret = pos; after.anchor = pos + n;
  rc = el_end(&g, after, rc);
  if (rc == E_OK) e->sel = after;
  return rc;
}

// ---------------------------------------------------------------------------------------------------------------
// Ctrl+Q (Notepad++ doBlockComment(cm_toggle))

static int el_match_at(Document* d, epos p, epos limit, const ecp* tok, u32 n, epos* end)   // case-insensitive
{
  u32 k;
  for (k = 0; k < n; k++) {
    ecp c;
    u32 used;
    if (p >= limit) return 0;
    used = d->decode(p, &c);
    if (!used || uc_fold(c) != uc_fold(tok[k])) return 0;
    p += used;
  }
  *end = p;
  return 1;
}

static int el_match_before(Document* d, epos p, epos floor, const ecp* tok, u32 n, epos* start)   // tok ends at p
{
  u32 k;
  for (k = n; k > 0; k--) {
    ecp c;
    epos q;
    if (p <= floor) return 0;
    q = d->char_prev(p);
    if (q < floor) return 0;
    d->decode(q, &c);
    if (uc_fold(c) != uc_fold(tok[k - 1])) return 0;
    p = q;
  }
  *start = p;
  return 1;
}

static int el_char_is(Document* d, epos p, epos limit, ecp want, u32* used)
{
  ecp c;
  if (p >= limit) return 0;
  *used = d->decode(p, &c);
  return *used && c == want;
}

int ed_toggle_comment(EdCtx* e)
{
  Document* d = e->doc;
  ecp tok[8];
  u32 tok_n, k, sl, el, i, sp_n = 0;
  u8 sp[8];
  EBytes cmt, adv_start, adv_end;
  ElGroup g;
  Selection after;
  epos ss, se;
  int adv = 0, move_caret, rc;
  rc = el_check(d);
  if (rc != E_OK) return rc;
  el_clamp(d, &e->sel);
  tok_n = e->lang.lc_len > 8 ? 8 : e->lang.lc_len;
  for (k = 0; k < tok_n; k++) tok[k] = e->lang.lc[k];
  while (tok_n > 0 && (tok[tok_n - 1] == ' ' || tok[tok_n - 1] == '\t')) tok_n--;   // "REM " -> "REM"
  if (tok_n == 0) {
    if (e->lang.bo_len == 0 || e->lang.bc_len == 0) return E_UNSUPPORTED;
    adv = 1;
  }
  sp_n = d->codec()->encode(' ', sp);
  if (!sp_n) return E_RANGE;
  if (!adv) {
    if ((rc = el_enc_append(d, tok, tok_n, &cmt)) != E_OK) return rc;
    if (cmt.append(sp, sp_n) != E_OK) return E_NOMEM;
  } else {
    if ((rc = el_enc_append(d, e->lang.bo, e->lang.bo_len, &adv_start)) != E_OK) return rc;
    if (adv_start.append(sp, sp_n) != E_OK) return E_NOMEM;
    if (adv_end.append(sp, sp_n) != E_OK) return E_NOMEM;
    if ((rc = el_enc_append(d, e->lang.bc, e->lang.bc_len, &adv_end)) != E_OK) return rc;
  }
  ss = sel_lo(e->sel); se = sel_hi(e->sel);
  move_caret = e->sel.caret < se;
  sl = d->line_of(ss); el = d->line_of(se);
  if (el > sl && se == el_vstart(d, el)) el--;

  el_begin(&g, d, e->sel, e->now_ms);
  rc = E_OK;
  for (i = sl; i <= el && rc == E_OK; i++) {
    epos lstart = el_vstart(d, i), lend = d->line_end(i), lind = lstart, p;
    ecp c;
    u32 used;
    while (lind < lend) {
      used = d->decode(lind, &c);
      if (!used || (c != ' ' && c != '\t')) break;
      lind += used;
    }
    if (lind >= lend) continue;                  // blank lines are not commented
    if (!adv) {
      int word_tok = uc_is_word(tok[tok_n - 1]);
      if (el_match_at(d, lind, lend, tok, tok_n, &p)) {
        ecp nc = 0;
        u32 nu = p < lend ? d->decode(p, &nc) : 0;
        if (!(word_tok && nu && uc_is_word(nc))) {   // "REM" must not match "REMARK"
          epos len = p - lind;
          if (nu && nc == ' ') len += nu;
          rc = el_rep(&g, lind, len, 0, 0);
          if (rc != E_OK) break;
          if (i == sl) {
            if (ss > lind + len) ss -= len;
            else if (ss > lind) ss = lind;
          }
          if (i == el) {
            if (se > lind + len) se -= len;
            else if (se > lind) {
              se = lind;
              if (lind == lstart && i != sl) se++;   // N++: "avoid caret return in this case"
            }
          } else se -= len;
          continue;
        }
      }
      rc = el_rep(&g, lind, 0, cmt.data(), cmt.size());
      if (rc != E_OK) break;
      if (i == sl && ss >= lind) ss += cmt.size();
      if (i == el) { if (se >= lind) se += cmt.size(); }
      else se += cmt.size();
    } else {
      epos bo_end, bc_start;
      if (el_match_at(d, lind, lend, e->lang.bo, e->lang.bo_len, &bo_end) &&
          el_match_before(d, lend, bo_end, e->lang.bc, e->lang.bc_len, &bc_start)) {
        epos start_len = bo_end - lind, end_len = lend - bc_start;
        epos room = lend - lind;
        if (el_char_is(d, bo_end, lend, ' ', &used)) start_len += used;
        if (bc_start > bo_end) {
          epos q = d->char_prev(bc_start);
          ecp pc;
          d->decode(q, &pc);
          if (pc == ' ') end_len += bc_start - q;
        }
        if (start_len + end_len > room) start_len = bo_end - lind;   // "/* */": the space belongs to one side
        if (start_len + end_len > room) end_len = lend - bc_start;
        rc = el_rep(&g, lind, start_len, 0, 0);
        if (rc == E_OK) rc = el_rep(&g, lend - start_len - end_len, end_len, 0, 0);
        if (rc != E_OK) break;
        if (i == sl) {
          if (ss > lend - end_len) ss = lend - start_len - end_len;
          else if (ss > lind + start_len) ss -= start_len;
          else if (ss > lind) ss = lind;
        }
        if (i == el) {
          if (se > lend) se -= start_len + end_len;
          else if (se > lend - end_len) se = lend - start_len - end_len;
          else if (se > lind + start_len) se -= start_len;
          else if (se > lind) {
            se = lind;
            if (lind == lstart && i != sl) se++;
          }
        } else se -= start_len + end_len;
        continue;
      }
      rc = el_rep(&g, lind, 0, adv_start.data(), adv_start.size());
      if (rc == E_OK) rc = el_rep(&g, lend + adv_start.size(), 0, adv_end.data(), adv_end.size());
      if (rc != E_OK) break;
      if (i == sl && ss >= lind) ss += adv_start.size();
      if (i == el) {
        if (se > lend) se += adv_start.size() + adv_end.size();
        else if (se >= lind) se += adv_start.size();
      } else se += adv_start.size() + adv_end.size();
    }
  }
  if (ss > se) ss = se;
  if (se > d->length()) se = d->length();
  if (ss > se) ss = se;
  if (move_caret) { after.caret = ss; after.anchor = se; }
  else { after.anchor = ss; after.caret = se; }
  rc = el_end(&g, after, rc);
  if (rc == E_OK) e->sel = after;
  return rc;
}

// ---------------------------------------------------------------------------------------------------------------
// Ctrl+Shift+U / Ctrl+U

int ed_change_case(EdCtx* e, int upper)
{
  Document* d = e->doc;
  const Codec* c = d->codec();
  EBytes src, dst;
  ElGroup g;
  Selection after;
  epos lo, hi, p;
  u32 i, a, b, n;
  int caret_only = 0, rc = el_check(d);
  if (rc != E_OK) return rc;
  el_clamp(d, &e->sel);
  lo = sel_lo(e->sel); hi = sel_hi(e->sel);
  if (lo == hi) {
    u32 line = d->line_of(lo);
    if (lo >= d->line_end(line)) return E_OK;   // at the EOL: nothing to change
    hi = d->char_next(lo);
    if (hi <= lo) return E_OK;
    caret_only = 1;
  }
  if ((rc = el_read_append(d, lo, hi, &src)) != E_OK) return rc;
  n = src.size();
  if (dst.reserve(n + 16) != E_OK) return E_NOMEM;
  i = 0;
  while (i < n) {
    ecp ch, m;
    u8 out[8];
    u32 used = c->decode(src.data() + i, n - i, 1, &ch), len = 0;
    if (!used) used = 1;
    if (used > n - i) used = n - i;
    m = ch;
    if (!ECP_IS_BAD(ch)) m = upper ? uc_upper(ch) : uc_lower(ch);
    if (m != ch) len = c->encode(m, out);
    if (len) { if (dst.append(out, len) != E_OK) return E_NOMEM; }
    else if (dst.append(src.data() + i, used) != E_OK) return E_NOMEM;   // unchanged or not storable
    i += used;
  }
  // replace only the part that differs (as Scintilla does)
  a = 0;
  while (a < n && a < dst.size() && src[a] == dst[a]) a++;
  if (a == n && n == dst.size()) {
    if (caret_only) { e->sel.anchor = e->sel.caret = hi; }
    return E_OK;
  }
  b = 0;
  while (b < n - a && b < dst.size() - a && src[n - 1 - b] == dst[dst.size() - 1 - b]) b++;
  if (c->unit == 2) { a &= ~1u; b &= ~1u; }      // UTF-16: whole code units only (E_ALIGN otherwise)
  p = lo + a;
  if (caret_only) { after.anchor = after.caret = lo + dst.size(); }
  else {
    after = e->sel;
    if (dst.size() != n) {                        // the higher end moves by the size difference
      epos diff_hi = hi + dst.size() - n;
      if (after.anchor > after.caret) after.anchor = diff_hi; else after.caret = diff_hi;
    }
  }
  el_begin(&g, d, e->sel, e->now_ms);
  rc = el_rep(&g, p, n - a - b, dst.data() + a, dst.size() - a - b);
  rc = el_end(&g, after, rc);
  if (rc == E_OK) e->sel = after;
  return rc;
}

// ---------------------------------------------------------------------------------------------------------------
// Tab / Shift+Tab on lines (Scintilla Document::Indent + SetLineIndentation)

struct ElIndent { epos ws_end; u32 cols; };

static void el_get_indent(Document* d, u32 line, u32 tw, ElIndent* out)
{
  epos p = el_vstart(d, line), le = d->line_end(line);
  u32 cols = 0;
  while (p < le) {
    ecp c;
    u32 used = d->decode(p, &c);
    if (!used) break;
    if (c == ' ') cols++;
    else if (c == '\t') cols = (cols / tw + 1) * tw;
    else break;
    p += used;
  }
  out->ws_end = p;
  out->cols = cols;
}

// Rewrites the indentation of line to cols columns; *delta = new length - old length (bytes).
static int el_set_indent(ElGroup* g, u32 line, u32 cols, u32 tw, int spaces, i32* delta)
{
  Document* d = g->d;
  ElIndent cur;
  EBytes ws;
  u8 tb[8], sb[8];
  u32 tn, sn, k, ntab = spaces ? 0 : cols / tw, nsp = spaces ? cols : cols % tw;
  epos ls = el_vstart(d, line);
  *delta = 0;
  el_get_indent(d, line, tw, &cur);
  if (cur.cols == cols) return E_OK;
  tn = d->codec()->encode('\t', tb);
  sn = d->codec()->encode(' ', sb);
  if (!tn || !sn) return E_RANGE;
  for (k = 0; k < ntab; k++) if (ws.append(tb, tn) != E_OK) return E_NOMEM;
  for (k = 0; k < nsp; k++) if (ws.append(sb, sn) != E_OK) return E_NOMEM;
  *delta = (i32)ws.size() - (i32)(cur.ws_end - ls);
  return el_rep(g, ls, cur.ws_end - ls, ws.data(), ws.size());
}

// Single-line indent: text positions move with the text; positions inside the old indentation go to the end of
// the new one (Scintilla's caret after Shift+Tab in the indentation).
static epos el_shift_pos(epos p, epos old_ws_end, i32 delta)
{
  epos new_ws_end = (epos)((i32)old_ws_end + delta);
  if (p >= old_ws_end) return (epos)((i32)p + delta);
  return new_ws_end;
}

int ed_indent(EdCtx* e, int outdent)
{
  Document* d = e->doc;
  ElGroup g;
  Selection after;
  u32 tw = e->lang.tab_width ? e->lang.tab_width : 4, la, lc, top, bottom, line;
  epos anchor_on_line, caret_on_line;
  int spaces = e->lang.use_spaces ? 1 : 0, rc = el_check(d);
  if (rc != E_OK) return rc;
  el_clamp(d, &e->sel);
  la = d->line_of(e->sel.anchor);
  lc = d->line_of(e->sel.caret);
  if (la != lc) {
    top = la < lc ? la : lc;
    bottom = la < lc ? lc : la;
    if (el_vstart(d, bottom) == e->sel.anchor || el_vstart(d, bottom) == e->sel.caret) bottom--;
    anchor_on_line = e->sel.anchor - el_vstart(d, la);
    caret_on_line = e->sel.caret - el_vstart(d, lc);
    el_begin(&g, d, e->sel, e->now_ms);
    for (line = bottom + 1; line-- > top && rc == E_OK; ) {
      ElIndent cur;
      i32 delta;
      el_get_indent(d, line, tw, &cur);
      if (!outdent) {
        if (el_vstart(d, line) < d->line_end(line)) rc = el_set_indent(&g, line, cur.cols + tw, tw, spaces, &delta);
      } else rc = el_set_indent(&g, line, cur.cols > tw ? cur.cols - tw : 0, tw, spaces, &delta);
    }
    if (la < lc) {
      after.caret = caret_on_line == 0 ? el_vstart(d, lc) : d->line_next(lc);
      after.anchor = el_vstart(d, la);
    } else {
      after.caret = el_vstart(d, lc);
      after.anchor = anchor_on_line == 0 ? el_vstart(d, la) : d->line_next(la);
    }
    rc = el_end(&g, after, rc);
  } else {
    ElIndent cur;
    i32 delta = 0;
    epos ls = el_vstart(d, lc);
    el_get_indent(d, lc, tw, &cur);
    if (!outdent && ls >= d->line_end(lc)) return E_OK;
    el_begin(&g, d, e->sel, e->now_ms);
    rc = el_set_indent(&g, lc, outdent ? (cur.cols > tw ? cur.cols - tw : 0) : cur.cols + tw, tw, spaces, &delta);
    after.anchor = el_shift_pos(e->sel.anchor, cur.ws_end, delta);
    after.caret = el_shift_pos(e->sel.caret, cur.ws_end, delta);
    rc = el_end(&g, after, rc);
  }
  if (rc == E_OK) e->sel = after;
  return rc;
}

// ---------------------------------------------------------------------------------------------------------------
// Columns (Go To line:col, status bar compatible)

u32 ed_col_of(Document* d, epos pos, u8 tab_width)
{
  u32 line = d->line_of(pos);
  epos ls = el_vstart(d, line);
  EBytes buf;
  LineLayout lay;
  LayoutOpts o;
  if (pos <= ls) return 0;
  if (pos - ls > LAY_WINDOW) {                   // very long line: count characters
    u32 n = 0;
    epos p = ls;
    while (p < pos) { epos q = d->char_next(p); if (q <= p) break; p = q; n++; }
    return n;
  }
  if (el_read_append(d, ls, pos, &buf) != E_OK) return 0;
  o.tab_width = tab_width ? tab_width : 4; o.show_ws = 0; o.pad[0] = o.pad[1] = 0;
  if (lay.build(buf.data(), buf.size(), d->codec(), o, 0, 1) != E_OK) return 0;
  return lay.end_cell();
}

epos ed_pos_of_col(Document* d, u32 line, u32 col, u8 tab_width)
{
  epos ls, le, n;
  EBytes buf;
  LineLayout lay;
  LayoutOpts o;
  u32 off;
  if (line >= d->line_count()) line = d->line_count() - 1;
  ls = el_vstart(d, line);
  le = d->line_end(line);
  if (le <= ls || col == 0) return ls;
  n = le - ls;
  if (n > LAY_WINDOW) {                          // very long line: characters
    epos p = ls;
    u32 k = 0;
    while (p < le && k < col) { epos q = d->char_next(p); if (q <= p) break; p = q; k++; }
    return p;
  }
  if (el_read_append(d, ls, le, &buf) != E_OK) return ls;
  o.tab_width = tab_width ? tab_width : 4; o.show_ws = 0; o.pad[0] = o.pad[1] = 0;
  if (lay.build(buf.data(), buf.size(), d->codec(), o, 0, 1) != E_OK) return ls;
  if (col >= lay.end_cell()) return le;
  off = lay.offset_of_cell(col, 0);
  if (off > n) off = n;
  return ls + off;
}

// ---------------------------------------------------------------------------------------------------------------
// Brace matching (Ctrl+B and the view's highlight). Braces whose style is a comment, string or char are ignored,
// both at the caret and while scanning. Priority: the character before pos, then the one at pos (Notepad++).
// Returns 1 matched (*brace, *match), 0 brace without a match (*match = EPOS_NONE), E_NOTFOUND no brace next to pos.
// The scan stops after 20,000 lines each way; lines longer than 1 MB are scanned in a 1 MB window without styles.

struct ElLine {
  Vec<ecp> ch; Vec<epos> off; Vec<u8> st;   // characters of the window, their byte offsets, styles
  epos a, b;                                // window [a, b)
};

static int el_skip_style(u8 s) { return s == ST_COMMENT || s == ST_STRING || s == ST_CHAR; }

static int el_style(Highlighter* hl, u32 line, ElLine* L)
{
  u32 n = L->ch.size(), m = n > LEX_LINE_MAX ? (u32)LEX_LINE_MAX : n;
  if (L->st.resize(n) != E_OK) return E_NOMEM;
  if (n) memset(L->st.data(), ST_DEFAULT, n);
  if (!hl || !hl->enabled() || !m) return E_OK;
  if (!hl->style_line(line, L->ch.data(), m, L->st.data())) {
    u32 budget = EL_LEX_BUDGET;                  // far away: lex up to the line (bounded), then try again
    hl->set_target(line);
    while (budget > 0 && !hl->state_known(line)) {
      if (!hl->work(2000)) break;
      budget = budget > 2000 ? budget - 2000 : 0;
    }
    if (!hl->style_line(line, L->ch.data(), m, L->st.data())) memset(L->st.data(), ST_DEFAULT, n);
  }
  return E_OK;
}

// Loads the window [a, b) of line. prefilter: skip (return 0) if neither byte c1 nor c2 occurs.
static int el_load(Document* d, Highlighter* hl, u32 line, epos a, epos b, int prefilter, u8 c1, u8 c2,
                   EBytes* raw, ElLine* L)
{
  const Codec* c = d->codec();
  u32 i = 0, n;
  int styled;
  raw->clear();
  L->ch.clear(); L->off.clear(); L->st.clear();
  L->a = a; L->b = b;
  if (el_read_append(d, a, b, raw) != E_OK) return E_NOMEM;
  n = raw->size();
  if (prefilter && n && !memchr(raw->data(), c1, n) && !memchr(raw->data(), c2, n)) return 0;
  while (i < n) {
    ecp ch;
    epos at = a + i;
    u32 used = c->decode(raw->data() + i, n - i, b == d->line_end(line) ? 1 : 0, &ch);
    if (!used) break;                            // incomplete character at the window end
    if (L->ch.push(ch) != E_OK || L->off.push(at) != E_OK) return E_NOMEM;
    i += used;
  }
  L->b = a + i;
  styled = a == el_vstart(d, line);
  if (styled) return el_style(hl, line, L) == E_OK ? 1 : E_NOMEM;
  if (L->st.resize(L->ch.size()) != E_OK) return E_NOMEM;
  if (L->ch.size()) memset(L->st.data(), ST_DEFAULT, L->ch.size());
  return 1;
}

static void el_line_window(Document* d, u32 line, epos center, int dir, epos* a, epos* b)
{
  epos ls = el_vstart(d, line), le = d->line_end(line);
  *a = ls; *b = le;
  if (le - ls <= EL_LINE_CAP) return;
  if (center != EPOS_NONE) {                     // around the brace
    epos lo = center - ls > EL_LINE_CAP / 2 ? center - EL_LINE_CAP / 2 : ls;
    epos hi = le - center > EL_LINE_CAP / 2 ? center + EL_LINE_CAP / 2 : le;
    *a = d->snap_char(lo); *b = hi;
  } else if (dir > 0) *b = ls + EL_LINE_CAP;
  else *a = d->snap_char(le - EL_LINE_CAP);
}

static int el_index_of(const ElLine& L, epos pos)   // char index with offset pos, or -1
{
  u32 lo = 0, hi = L.off.size();
  while (lo < hi) {
    u32 mid = (lo + hi) / 2;
    if (L.off[mid] < pos) lo = mid + 1; else hi = mid;
  }
  return (lo < L.off.size() && L.off[lo] == pos) ? (int)lo : -1;
}

static int el_brace_index(const char* braces, ecp c)
{
  int i;
  if (c == 0 || c > 0x7F) return -1;
  for (i = 0; braces[i] && braces[i + 1]; i += 2) {
    if ((ecp)(u8)braces[i] == c) return i;
    if ((ecp)(u8)braces[i + 1] == c) return i + 1;
  }
  return -1;
}

int ed_match_brace(Document* d, epos pos, Highlighter* hl, const char* braces, epos* brace, epos* match)
{
  ElLine L;
  EBytes raw;
  epos cand[2], q = EPOS_NONE;
  u32 ncand = 0, k, line = 0, lines_done;
  int bi = -1, idx = -1, dir, rc;
  ecp open, close;
  i32 depth;
  if (brace) *brace = EPOS_NONE;
  if (match) *match = EPOS_NONE;
  if (!braces || !braces[0]) braces = "()[]{}";
  if (pos > d->length()) pos = d->length();
  if (pos > d->text_origin()) cand[ncand++] = d->char_prev(pos);
  if (pos < d->length()) cand[ncand++] = pos;
  for (k = 0; k < ncand; k++) {
    ecp c;
    epos a, b;
    if (!d->decode(cand[k], &c)) continue;
    bi = el_brace_index(braces, c);
    if (bi < 0) continue;
    line = d->line_of(cand[k]);
    if (cand[k] >= d->line_end(line)) { bi = -1; continue; }
    el_line_window(d, line, cand[k], 0, &a, &b);
    rc = el_load(d, hl, line, a, b, 0, 0, 0, &raw, &L);
    if (rc < 0) return rc;
    idx = el_index_of(L, cand[k]);
    if (idx < 0 || el_skip_style(L.st[(u32)idx])) { bi = -1; continue; }
    q = cand[k];
    break;
  }
  if (bi < 0) return E_NOTFOUND;
  if (brace) *brace = q;
  open = (ecp)(u8)braces[bi & ~1];
  close = (ecp)(u8)braces[bi | 1];
  dir = (bi & 1) ? -1 : 1;
  depth = 0;
  lines_done = 0;
  for (;;) {
    u32 n = L.ch.size();
    if (dir > 0) {
      u32 i;
      for (i = (u32)(idx + 1); i < n; i++) {
        if (el_skip_style(L.st[i])) continue;
        if (L.ch[i] == open) depth++;
        else if (L.ch[i] == close) {
          if (depth == 0) { if (match) *match = L.off[i]; return 1; }
          depth--;
        }
      }
    } else {
      u32 i = (u32)idx;
      while (i-- > 0) {
        if (el_skip_style(L.st[i])) continue;
        if (L.ch[i] == close) depth++;
        else if (L.ch[i] == open) {
          if (depth == 0) { if (match) *match = L.off[i]; return 1; }
          depth--;
        }
      }
    }
    // next line that contains one of the two braces
    for (;;) {
      epos a, b;
      if (++lines_done > EL_BRACE_LINES) return 0;
      if (dir > 0) { if (line + 1 >= d->line_count()) return 0; line++; }
      else { if (line == 0) return 0; line--; }
      el_line_window(d, line, EPOS_NONE, dir, &a, &b);
      rc = el_load(d, hl, line, a, b, 1, (u8)open, (u8)close, &raw, &L);
      if (rc < 0) return rc;
      if (rc == 0) continue;
      idx = dir > 0 ? -1 : (int)L.ch.size();
      break;
    }
  }
}
