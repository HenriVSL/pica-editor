// textview.cpp - the text view window (DESIGN 8, 10, 14.1, 14.2). Owner: T08.
//
// One window class, two kinds of instances: the main view (g_app.textview) and the find-results panel (is_panel = 1:
// read-only use, no gutter, Enter / double-click go to a PanelHandler). The view keeps no document state of its own:
// selection and scroll position live in the attached Tab (Tab::sel, Tab::tv), so switching tabs or views keeps them.
//
// Structure:
//   line cache   starts/ends of the lines around the viewport, found by scanning bytes from one known line start
//                (PieceTree::break_pos scans up to 64 KB per lookup, so painting never looks up line by line)
//   segment      the LineLayout of one line (whole line up to LAY_LONG bytes; above that a LAY_WINDOW window from a
//                LongLineIndex checkpoint), plus its wrap rows and syntax styles
//   row map      screen row -> (line, subrow, byte range, cell range) for the current scroll position
//   painting     direct ExtTextOutW per run of equal attributes (render.cpp), every pixel painted once
//   input        WM_KEYDOWN (navigation, editing keys), WM_CHAR / WM_IME_CHAR (typing), mouse, wheel, context menu
//   DocListener  invalidates rows, keeps caches consistent, shifts the selection for edits made elsewhere
#include <windows.h>
#include <imm.h>
#include "app.h"
#include "tab.h"
#include "cmds.h"
#include "ui.h"
#include "views.h"
#include "selftest.h"
#include "render.h"
#include "render_int.h"
#include "layout.h"
#include "lexer.h"
#include "edops.h"
#include "winenc.h"
#include "docio.h"
#include "progress.h"
#include "estr.h"
#include "ucd.h"
#include "resource.h"
#include "sessmgr.h"
#include "fileops.h"
#include "fsys.h"
#include <string.h>
#include <stdlib.h>

#ifndef WM_MOUSEWHEEL
#define WM_MOUSEWHEEL 0x020A
#endif
#ifndef SPI_GETWHEELSCROLLLINES
#define SPI_GETWHEELSCROLLLINES 104
#endif
#ifndef WHEEL_DELTA
#define WHEEL_DELTA 120
#endif

static const char k_tv_class[] = APP_NAME "TextView";
enum { TVT_DRAG = 1, TVT_LEX = 2 };
enum { WM_TV_NOTIFY = WM_APP + 60 };
enum { NF_CARET = 1, NF_DOC = 2, NF_SCROLL = 4 };
enum { TV_LLI = 8, TV_LC_MAX = 2048, TV_SCAN_LIMIT = 262144, TV_RUN_MAX = 1024 };
enum { DRAG_NONE = 0, DRAG_TEXT, DRAG_WORD, DRAG_LINES };
#define NODESIRED 0xFFFFFFFFu
#define NOLINE 0xFFFFFFFFu

struct LineInfo { epos start, end, next; };       // end = EOL position (or length), next = start of the next line
struct RowEnt { u32 line, sub; epos a, b; u32 c0, c1; u8 last, pad[3]; };

class TV : public DocListener {
public:
  TV();
  ~TV();
  void doc_changed(Document* d, const EditEvent& e);
  HWND w; int panel; Tab* t; Document* d; PanelHandler* ph;
  int cw, ch, cli_w, cli_h, rows_full, rows_part, gut_px, text_left; u32 cols, digits;
  // line cache
  u32 lc_seq, lc_first; Vec<LineInfo> lc;
  // long-line indices (LRU)
  LongLineIndex* lli[TV_LLI]; epos lli_ls[TV_LLI], lli_le[TV_LLI]; u32 lli_tw[TV_LLI], lli_use[TV_LLI], lli_tick;
  int building;
  // current segment
  LineLayout lay; EBytes buf;
  u32 sg_line, sg_seq, sg_cell; epos sg_ls, sg_le, sg_pos; u8 sg_whole, sg_valid, sg_tw, sg_ws, sg_eof;
  Vec<u32> wrows; u32 wr_w; int wr_valid;
  Vec<ecp> dec; Vec<u8> sty; u32 sty_n; int sty_valid;
  // row map
  Vec<RowEnt> rmap; int rm_valid; u32 mt_seq, mt_line, mt_sub, mt_w, mt_rows; int mt_valid;
  u32 max_cells;
  // caret, mouse, input
  int focus, caret_on, caret_w, caret_h, affinity;
  int drag, drag_timer, click_n, click_x, click_y, mx, my; DWORD click_t; epos drag_a0, drag_a1; u32 drag_line0;
  u8 lead; int wheel_acc, ime_on;
  epos br_a, br_b; int br_bad;
  int lex_timer, own_edit, last_mod, in_paint;
  u32 nflags; int notify_posted;
  u32 vs_max, vs_page; int vs_scaled; u32 hs_total; int hs_scaled; int hs_shown;
  u32 last_digits;
};

static Vec<TV*> g_tvs;

TV::TV() : w(0), panel(0), t(0), d(0), ph(0), cw(8), ch(16), cli_w(0), cli_h(0), rows_full(1), rows_part(1),
  gut_px(0), text_left(4), cols(1), digits(3), lc_seq(0), lc_first(0), lli_tick(0), building(0), sg_line(0),
  sg_seq(0), sg_cell(0), sg_ls(0), sg_le(0), sg_pos(0), sg_whole(0), sg_valid(0), sg_tw(0), sg_ws(0), sg_eof(0),
  wr_w(0), wr_valid(0), sty_n(0), sty_valid(0), rm_valid(0), mt_seq(0), mt_line(0), mt_sub(0), mt_w(0), mt_rows(0),
  mt_valid(0), max_cells(0), focus(0), caret_on(0), caret_w(0), caret_h(0), affinity(0), drag(0), drag_timer(0),
  click_n(0), click_x(0), click_y(0), mx(0), my(0), click_t(0), drag_a0(0), drag_a1(0), drag_line0(0), lead(0),
  wheel_acc(0), ime_on(0), br_a(EPOS_NONE), br_b(EPOS_NONE), br_bad(0), lex_timer(0), own_edit(0), last_mod(0), in_paint(0),
  nflags(0), notify_posted(0), vs_max(0), vs_page(1), vs_scaled(0), hs_total(0), hs_scaled(0), hs_shown(1),
  last_digits(0)
{
  u32 i;
  for (i = 0; i < TV_LLI; i++) { lli[i] = 0; lli_ls[i] = lli_le[i] = 0; lli_tw[i] = lli_use[i] = 0; }
}

TV::~TV()
{
  u32 i;
  for (i = 0; i < TV_LLI; i++) delete lli[i];
}

static TV* tv_get(HWND w) { return w ? (TV*)GetWindowLongA(w, GWL_USERDATA) : 0; }

static u8 tv_tw(TV* v) { return (u8)(v->t && v->t->tv.tab_width >= 1 && v->t->tv.tab_width <= 16 ? v->t->tv.tab_width : 4); }
static u8 tv_ws(TV* v) { return (u8)(v->panel ? 0 : (g_app.cfg.show_ws ? 1 : 0)); }
static int tv_wrap(TV* v) { return v->t && v->d && v->t->tv.wrap && !v->d->indexing(); }
static u32 tv_wrapw(TV* v) { return v->cols ? v->cols : 1; }
static int row_lt(u32 l1, u32 s1, u32 l2, u32 s2) { return l1 < l2 || (l1 == l2 && s1 < s2); }

static void tv_notify(TV* v, u32 f)
{
  v->nflags |= f;
  if (!v->notify_posted && v->w) { v->notify_posted = 1; PostMessageA(v->w, WM_TV_NOTIFY, 0, 0); }
}

static void tv_lex_timer(TV* v)
{
  if (!v->lex_timer && v->w && v->t && v->t->hl) { SetTimer(v->w, TVT_LEX, 30, 0); v->lex_timer = 1; }
}

// ---------------------------------------------------------------------------------------------------------------
// Metrics

static u32 tv_digits(u32 n) { u32 k = 1; while (n >= 10) { n /= 10; k++; } return k < 3 ? 3 : k; }

static void tv_metrics(TV* v)
{
  const RenderFonts* rf = render_fonts();
  RECT rc;
  v->cw = rf && rf->cw > 0 ? rf->cw : 8;
  v->ch = rf && rf->ch > 0 ? rf->ch : 16;
  v->digits = tv_digits(v->d && !v->d->indexing() ? v->d->line_count() : 1);
  if (v->panel) v->gut_px = 0;
  else if (g_app.cfg.line_numbers && !(v->d && v->d->indexing())) v->gut_px = (int)(v->digits + 2) * v->cw;
  else v->gut_px = v->cw;
  v->text_left = v->gut_px + 4;
  GetClientRect(v->w, &rc);
  v->cli_w = rc.right; v->cli_h = rc.bottom;
  v->rows_full = v->cli_h / v->ch;
  if (v->rows_full < 1) v->rows_full = 1;
  v->rows_part = (v->cli_h + v->ch - 1) / v->ch;
  if (v->rows_part < 1) v->rows_part = 1;
  v->cols = v->cli_w > v->text_left ? (u32)((v->cli_w - v->text_left) / v->cw) : 1;
  if (v->cols < 1) v->cols = 1;
  v->last_digits = v->digits;
  v->rm_valid = 0; v->mt_valid = 0; v->wr_valid = 0;
}

// ---------------------------------------------------------------------------------------------------------------
// Line cache (byte scanning from a known line start)

static u32 tv_unit(Document* d) { return d->tree()->break_spec().unit == 2 ? 2 : 1; }

// Break unit at or after from, within limit bytes: 1 found (*bp), 0 none until the end, -1 limit reached / error.
static int scan_break(Document* d, epos from, epos limit, epos* bp)
{
  const BreakSpec& bs = d->tree()->break_spec();
  epos len = d->length(), p = from, stop = (len - from > limit) ? from + limit : len;
  if (from >= len) return 0;
  if (bs.unit != 2) {
    ByteSource* src = d->bytes();
    while (p < stop) {
      epos avail = 0;
      const u8* q = src->get(p, &avail);
      const u8* hit;
      u32 n;
      if (!q || !avail) return -1;
      n = avail < stop - p ? avail : stop - p;
      hit = (const u8*)memchr(q, bs.b0, n);
      if (hit) { *bp = p + (epos)(hit - q); return 1; }
      p += n;
    }
    return stop >= len ? 0 : -1;
  } else {
    u8 tmp[4096];
    if (p & 1) p++;
    while (p < stop) {
      u32 n = stop - p > sizeof(tmp) ? (u32)sizeof(tmp) : (u32)(stop - p), i;
      n &= ~1u;
      if (n == 0) break;
      if (d->read(p, tmp, n) != E_OK) return -1;
      for (i = 0; i + 1 < n; i += 2) if (tmp[i] == bs.b0 && tmp[i + 1] == bs.b1) { *bp = p + i; return 1; }
      p += n;
    }
    return stop >= len ? 0 : -1;
  }
}

static int tv_is_cr_unit(Document* d, epos at)
{
  const BreakSpec& bs = d->tree()->break_spec();
  u8 b[2];
  if (bs.unit != 2) {
    if (bs.b0 != 0x0A) return 0;                       // CR mode: the break itself is the CR
    return d->read(at, b, 1) == E_OK && b[0] == 0x0D;
  }
  if (bs.b0 != 0x0A && bs.b1 != 0x0A) return 0;
  if (d->read(at, b, 2) != E_OK) return 0;
  return bs.b0 == 0x0A ? (b[0] == 0x0D && b[1] == 0) : (b[0] == 0 && b[1] == 0x0D);
}

static void walk_line(TV* v, u32 line, epos start, LineInfo* li)
{
  Document* d = v->d;
  u32 unit = tv_unit(d);
  epos bp = 0;
  int r;
  li->start = start;
  if (line + 1 >= d->line_count()) { li->end = li->next = d->length(); return; }
  r = scan_break(d, start, TV_SCAN_LIMIT, &bp);
  if (r == 1) {
    li->next = bp + unit;
    li->end = (bp >= start + unit && tv_is_cr_unit(d, bp - unit)) ? bp - unit : bp;
    return;
  }
  li->end = d->line_end(line);
  li->next = d->line_next(line);
  if (li->end < start) li->end = start;
}

static void lc_get(TV* v, u32 line, LineInfo* li)
{
  Document* d = v->d;
  u32 seq = d->change_seq(), n;
  epos o = d->text_origin();
  if (seq != v->lc_seq) { v->lc.clear(); v->lc_seq = seq; }
  n = v->lc.size();
  if (n && line >= v->lc_first && line < v->lc_first + n) { *li = v->lc[line - v->lc_first]; return; }
  if (n && line == v->lc_first + n && n < TV_LC_MAX) {
    walk_line(v, line, v->lc[n - 1].next, li);
    v->lc.push(*li);
    return;
  }
  if (n && line + 1 == v->lc_first && n < TV_LC_MAX) {
    epos s = line == 0 ? o : d->line_start(line);
    if (s < o) s = o;
    walk_line(v, line, s, li);
    if (li->next == v->lc[0].start && v->lc.insert(0, li, 1) == E_OK) { v->lc_first = line; return; }
  }
  {
    epos s = line == 0 ? o : d->line_start(line);
    if (s < o) s = o;
    v->lc.clear();
    v->lc_first = line;
    walk_line(v, line, s, li);
    v->lc.push(*li);
  }
}

static u32 tv_line_of(TV* v, epos pos)
{
  Document* d = v->d;
  u32 n = v->lc.size();
  if (v->lc_seq == d->change_seq() && n) {
    const LineInfo* a = v->lc.data();
    if (pos >= a[0].start && (pos < a[n - 1].next || (pos == a[n - 1].next && a[n - 1].next == d->length() &&
                                                      v->lc_first + n == d->line_count()))) {
      u32 lo = 0, hi = n;
      while (hi - lo > 1) { u32 mid = (lo + hi) / 2; if (a[mid].start <= pos) lo = mid; else hi = mid; }
      return v->lc_first + lo;
    }
  }
  return d->line_of(pos);
}

// ---------------------------------------------------------------------------------------------------------------
// Long-line indices

static void lli_clear(TV* v)
{
  u32 i;
  for (i = 0; i < TV_LLI; i++) { delete v->lli[i]; v->lli[i] = 0; }
}

static LongLineIndex* lli_get(TV* v, epos ls, epos le, int may_build)
{
  u32 i, slot = 0;
  u8 tw = tv_tw(v);
  LongLineIndex* x;
  LayoutOpts o;
  int rc;
  for (i = 0; i < TV_LLI; i++) {
    if (v->lli[i] && v->lli_ls[i] == ls && v->lli_le[i] == le && v->lli_tw[i] == tw) {
      v->lli_use[i] = ++v->lli_tick;
      return v->lli[i];
    }
  }
  if (!may_build || v->building) return 0;
  for (i = 0; i < TV_LLI; i++) {
    if (!v->lli[i]) { slot = i; break; }
    if (v->lli_use[i] < v->lli_use[slot]) slot = i;
  }
  delete v->lli[slot];
  v->lli[slot] = 0;
  x = new LongLineIndex();
  if (!x) return 0;
  o.tab_width = tw; o.show_ws = 0; o.pad[0] = o.pad[1] = 0;
  v->building = 1;
  if (le - ls > 4u * 1024u * 1024u && !v->in_paint) {
    UiProgress p(APP_NAME, "Measuring a long line...", 1);
    rc = x->build(v->d->bytes(), v->d->codec(), o, ls, le, &p);
    v->building = 0;
    InvalidateRect(v->w, 0, FALSE);                  // paints during the progress pump were skipped
  } else rc = x->build(v->d->bytes(), v->d->codec(), o, ls, le, 0);
  v->building = 0;
  if (rc != E_OK) { delete x; return 0; }
  v->lli[slot] = x; v->lli_ls[slot] = ls; v->lli_le[slot] = le; v->lli_tw[slot] = tw;
  v->lli_use[slot] = ++v->lli_tick;
  if (x->total_cells() > v->max_cells) { v->max_cells = x->total_cells(); tv_notify(v, NF_SCROLL); }
  return x;
}

// ---------------------------------------------------------------------------------------------------------------
// Segments: the layout of (part of) one line

static int seg_read_build(TV* v, epos from, epos n, u32 cell, int eof)
{
  LayoutOpts o;
  if (v->buf.resize(n) != E_OK) return E_NOMEM;
  if (n && v->d->read(from, v->buf.data(), n) != E_OK) { v->buf.clear(); n = 0; }
  o.tab_width = tv_tw(v); o.show_ws = tv_ws(v); o.pad[0] = o.pad[1] = 0;
  return v->lay.build(v->buf.data(), n, v->d->codec(), o, cell, eof);
}

static int seg_covers(TV* v, epos pos, u32 cell, int by_cell)
{
  if (v->sg_whole) return 1;
  if (by_cell) return cell >= v->sg_cell && (cell < v->lay.end_cell() || v->sg_eof);
  return pos >= v->sg_pos && (pos < v->sg_pos + v->lay.bytes() || (v->sg_eof && pos <= v->sg_pos + v->lay.bytes()));
}

// Lays out line `line` so that pos (by_cell = 0) or cell (by_cell = 1) is inside the segment.
static void seg_line(TV* v, u32 line, epos pos, u32 cell, int by_cell)
{
  LineInfo li;
  u32 seq = v->d->change_seq();
  u8 tw = tv_tw(v), ws = tv_ws(v);
  epos len;
  lc_get(v, line, &li);
  if (v->sg_valid && v->sg_seq == seq && v->sg_line == line && v->sg_tw == tw && v->sg_ws == ws &&
      v->sg_ls == li.start && seg_covers(v, pos, cell, by_cell)) return;
  v->sg_line = line; v->sg_ls = li.start; v->sg_le = li.end; v->sg_seq = seq; v->sg_tw = tw; v->sg_ws = ws;
  v->wr_valid = 0; v->sty_valid = 0; v->sg_valid = 1;
  len = li.end - li.start;
  if (len <= LAY_LONG) {
    seg_read_build(v, li.start, len, 0, 1);
    v->sg_pos = li.start; v->sg_cell = 0; v->sg_eof = 1; v->sg_whole = 1;
    if (v->lay.end_cell() > v->max_cells) { v->max_cells = v->lay.end_cell(); tv_notify(v, NF_SCROLL); }
    return;
  }
  v->sg_whole = 0;
  {
    LongLineIndex* x = lli_get(v, li.start, li.end, 0);
    epos cp = li.start, n;
    u32 cc = 0;
    if (!x && (by_cell ? cell < 4096 : pos - li.start < LAY_WINDOW / 2)) {
      // near the line start: the first window is enough (no index needed yet)
    } else {
      if (!x) x = lli_get(v, li.start, li.end, 1);
      if (x) {
        if (by_cell) x->at_cell(cell, &cp, &cc);
        else x->at_offset(pos < li.start ? li.start : (pos > li.end ? li.end : pos), &cp, &cc);
      }
    }
    n = li.end - cp;
    if (n > LAY_WINDOW) n = LAY_WINDOW;
    seg_read_build(v, cp, n, cc, cp + n >= li.end);
    v->sg_pos = cp; v->sg_cell = cc;
    v->sg_eof = cp + v->lay.bytes() >= li.end;
    if (v->lay.end_cell() > v->max_cells) { v->max_cells = v->lay.end_cell(); tv_notify(v, NF_SCROLL); }
  }
}

static u32 seg_total_cells(TV* v)            // cells of the whole line of the segment (long lines: index or window)
{
  LongLineIndex* x;
  if (v->sg_whole) return v->lay.end_cell();
  x = lli_get(v, v->sg_ls, v->sg_le, 1);
  return x ? x->total_cells() : v->lay.end_cell();
}

static void seg_wrap(TV* v)                   // wrap rows of a whole segment
{
  u32 ww = tv_wrapw(v);
  if (v->wr_valid && v->wr_w == ww) return;
  v->wrows.clear();
  if (v->lay.wrap_rows(ww, 1, &v->wrows) != E_OK || v->wrows.empty()) { u32 z = 0; v->wrows.clear(); v->wrows.push(z); }
  v->wr_w = ww; v->wr_valid = 1;
}

// Syntax styles of the segment's chars (only when the segment starts at the line start).
static void seg_styles(TV* v)
{
  u32 n, used = 0, cnt;
  if (v->sty_valid) return;
  v->sty_valid = 1; v->sty_n = 0;
  if (!v->t || !v->t->hl || v->sg_pos != v->sg_ls) return;
  if (!v->t->hl->enabled() || !v->t->lang) return;
  cnt = v->lay.count();
  n = cnt < LEX_LINE_MAX ? cnt : LEX_LINE_MAX;
  if (v->dec.resize(n + 1) != E_OK || v->sty.resize(n + 1) != E_OK) return;
  n = codec_decode_buf(v->d->codec(), v->buf.data(), v->lay.bytes(), v->sg_eof, v->dec.data(), n, &used);
  v->t->hl->style_line(v->sg_line, v->dec.data(), n, v->sty.data());
  if (!v->t->hl->state_known(v->sg_line + 1 < v->d->line_count() ? v->sg_line + 1 : v->sg_line)) tv_lex_timer(v);
  v->sty_n = n;
}

// ---------------------------------------------------------------------------------------------------------------
// Rows (wrap) and the row map

static u32 line_rows(TV* v, u32 line)
{
  LineInfo li;
  if (!tv_wrap(v)) return 1;
  lc_get(v, line, &li);
  seg_line(v, line, li.start, 0, 0);
  if (v->sg_whole) { seg_wrap(v); return v->wrows.size(); }
  {
    u32 tot = seg_total_cells(v), ww = tv_wrapw(v);
    u32 r = (tot + ww - 1) / ww;
    return r ? r : 1;
  }
}

// Cell range and byte range of row `sub` of `line`.
static void row_geo(TV* v, u32 line, u32 sub, RowEnt* re)
{
  LineInfo li;
  lc_get(v, line, &li);
  re->line = line; re->sub = sub;
  if (!tv_wrap(v)) { re->a = li.start; re->b = li.next; re->c0 = 0; re->c1 = 0xFFFFFFFFu; re->last = 1; return; }
  seg_line(v, line, li.start, 0, 0);
  if (v->sg_whole) {
    const LayChar* c = v->lay.chars();
    u32 cnt = v->lay.count(), nr, ci0, ci1;
    seg_wrap(v);
    nr = v->wrows.size();
    if (sub >= nr) sub = nr - 1;
    re->sub = sub;
    ci0 = v->wrows[sub];
    ci1 = sub + 1 < nr ? v->wrows[sub + 1] : cnt;
    re->a = li.start + (ci0 < cnt ? c[ci0].off : v->lay.bytes());
    re->b = sub + 1 < nr ? li.start + c[ci1].off : li.next;
    re->c0 = ci0 < cnt ? c[ci0].cell : v->lay.end_cell();
    re->c1 = ci1 < cnt ? c[ci1].cell : v->lay.end_cell();
    re->last = (u8)(sub + 1 >= nr);
    return;
  }
  {
    u32 tot = seg_total_cells(v), ww = tv_wrapw(v), nr = (tot + ww - 1) / ww;
    if (!nr) nr = 1;
    if (sub >= nr) sub = nr - 1;
    re->sub = sub;
    re->c0 = sub * ww; re->c1 = re->c0 + ww;
    re->last = (u8)(sub + 1 >= nr);
    seg_line(v, line, 0, re->c0, 1);
    re->a = v->sg_pos + v->lay.offset_of_cell(re->c0, 0);
    if (re->last) re->b = li.next;
    else { seg_line(v, line, 0, re->c1, 1); re->b = v->sg_pos + v->lay.offset_of_cell(re->c1, 0); }
  }
}

static int step_row(TV* v, u32* line, u32* sub, int dir)
{
  if (dir > 0) {
    if (*sub + 1 < line_rows(v, *line)) { (*sub)++; return 1; }
    if (*line + 1 < v->d->line_count()) { (*line)++; *sub = 0; return 1; }
    return 0;
  }
  if (*sub > 0) { (*sub)--; return 1; }
  if (*line > 0) { (*line)--; *sub = line_rows(v, *line) - 1; return 1; }
  return 0;
}

static void max_top(TV* v, u32* line, u32* sub)
{
  u32 n = v->d->line_count(), k;
  if (!tv_wrap(v)) { *line = n > (u32)v->rows_full ? n - (u32)v->rows_full : 0; *sub = 0; return; }
  if (v->mt_valid && v->mt_seq == v->d->change_seq() && v->mt_w == tv_wrapw(v) && v->mt_rows == (u32)v->rows_full) {
    *line = v->mt_line; *sub = v->mt_sub; return;
  }
  *line = n - 1; *sub = line_rows(v, n - 1) - 1;
  for (k = 1; k < (u32)v->rows_full; k++) if (!step_row(v, line, sub, -1)) break;
  v->mt_line = *line; v->mt_sub = *sub; v->mt_seq = v->d->change_seq(); v->mt_w = tv_wrapw(v);
  v->mt_rows = (u32)v->rows_full; v->mt_valid = 1;
}

static void clamp_top(TV* v)
{
  u32 ml, ms;
  TextViewState* s = &v->t->tv;
  if (!tv_wrap(v)) s->top_subrow = 0;
  max_top(v, &ml, &ms);
  if (row_lt(ml, ms, s->top_line, s->top_subrow)) { s->top_line = ml; s->top_subrow = ms; }
  if (tv_wrap(v) && s->top_subrow > 0 && s->top_subrow >= line_rows(v, s->top_line)) s->top_subrow = 0;
}

static void rmap_build(TV* v)
{
  u32 line, sub, r, n;
  if (v->rm_valid) return;
  v->rmap.clear();
  if (!v->t || !v->d) { v->rm_valid = 1; return; }
  clamp_top(v);
  line = v->t->tv.top_line; sub = v->t->tv.top_subrow;
  n = v->d->line_count();
  for (r = 0; r < (u32)v->rows_part && line < n; r++) {
    RowEnt re;
    row_geo(v, line, sub, &re);
    v->rmap.push(re);
    if (!re.last) sub++;
    else { line++; sub = 0; }
  }
  v->rm_valid = 1;
}

// ---------------------------------------------------------------------------------------------------------------
// Positions <-> rows and cells

// Row of pos: line, subrow, absolute cell of pos, start cell of the row.
static void pos_rowcell(TV* v, epos pos, u32* line, u32* sub, u32* cell, u32* rowc0)
{
  u32 l = tv_line_of(v, pos), c;
  seg_line(v, l, pos, 0, 0);
  c = v->lay.cell_of_offset(pos - v->sg_pos);
  *line = l; *sub = 0; *cell = c; *rowc0 = 0;
  if (!tv_wrap(v)) return;
  if (v->sg_whole) {
    u32 ci = v->lay.char_at_offset(pos - v->sg_pos), nr, lo = 0, hi;
    const LayChar* ch = v->lay.chars();
    seg_wrap(v);
    nr = v->wrows.size(); hi = nr;
    while (hi - lo > 1) { u32 mid = (lo + hi) / 2; if (v->wrows[mid] <= ci) lo = mid; else hi = mid; }
    if (v->affinity && lo > 0 && ci == v->wrows[lo]) lo--;
    *sub = lo;
    *rowc0 = v->wrows[lo] < v->lay.count() ? ch[v->wrows[lo]].cell : v->lay.end_cell();
    return;
  }
  {
    u32 ww = tv_wrapw(v), tot, nr, k;
    tot = seg_total_cells(v);
    nr = (tot + ww - 1) / ww;
    if (!nr) nr = 1;
    seg_line(v, l, pos, 0, 0);
    k = c / ww;
    if (k >= nr) k = nr - 1;
    if (v->affinity && k > 0 && c % ww == 0) k--;
    *sub = k; *rowc0 = k * ww;
  }
}

// Position in row (line, sub) nearest to cell (absolute); sets v->affinity when it lands on a wrapped row's end.
static epos row_pos_at_cell(TV* v, const RowEnt* re, u32 cell, int* aff)
{
  epos pos;
  *aff = 0;
  seg_line(v, re->line, 0, cell, 1);
  pos = v->sg_pos + v->lay.offset_of_cell(cell, 1);
  if (tv_wrap(v)) {
    if (pos < re->a) pos = re->a;
    if (!re->last && pos >= re->b) { pos = re->b; *aff = 1; }
  }
  if (pos > v->sg_le) pos = v->sg_le;
  return pos;
}

static int rmap_find(TV* v, u32 line, u32 sub)
{
  u32 i;
  rmap_build(v);
  for (i = 0; i < v->rmap.size(); i++) if (v->rmap[i].line == line && v->rmap[i].sub == sub) return (int)i;
  return -1;
}

static u32 tv_left_cell(TV* v, u32 rowc0) { return tv_wrap(v) ? rowc0 : v->t->tv.hscroll; }

static int pos_to_xy(TV* v, epos pos, int* x, int* y)
{
  u32 line, sub, cell, rc0;
  int r;
  pos_rowcell(v, pos, &line, &sub, &cell, &rc0);
  r = rmap_find(v, line, sub);
  if (r < 0) return 0;
  *y = r * v->ch;
  *x = v->text_left + ((int)cell - (int)tv_left_cell(v, rc0)) * v->cw;
  return *x >= v->text_left - 1 && *x <= v->cli_w;
}

static epos hit_pos(TV* v, int x, int y, int* aff)
{
  int r = y < 0 ? 0 : y / v->ch;
  u32 cell, c0;
  int dx;
  const RowEnt* re;
  *aff = 0;
  rmap_build(v);
  if (v->rmap.empty()) return v->d->text_origin();
  if (r >= (int)v->rmap.size()) r = (int)v->rmap.size() - 1;
  re = &v->rmap[(u32)r];
  c0 = tv_left_cell(v, re->c0);
  dx = x - v->text_left;
  if (dx < 0) dx = 0;
  cell = c0 + (u32)((dx + v->cw / 2) / v->cw);
  return row_pos_at_cell(v, re, cell, aff);
}

// ---------------------------------------------------------------------------------------------------------------
// Scrolling

static int rows_between(TV* v, u32 l1, u32 s1, u32 l2, u32 s2, int limit)   // rows from (l1,s1) forward to (l2,s2)
{
  int k = 0;
  if (!tv_wrap(v)) {
    u32 dl = l2 - l1;
    return dl > (u32)limit ? limit + 1 : (int)dl;
  }
  while (row_lt(l1, s1, l2, s2)) {
    if (++k > limit) return limit + 1;
    if (!step_row(v, &l1, &s1, 1)) break;
  }
  return k;
}

static void update_scrollbars(TV* v);

static void tv_set_top(TV* v, u32 line, u32 sub)
{
  TextViewState* s = &v->t->tv;
  u32 ol = s->top_line, os = s->top_subrow, ml, ms;
  int dr = 0, big = v->rows_part + 1;
  max_top(v, &ml, &ms);
  if (row_lt(ml, ms, line, sub)) { line = ml; sub = ms; }
  if (!tv_wrap(v)) sub = 0;
  if (line == ol && sub == os) return;
  if (row_lt(ol, os, line, sub)) dr = rows_between(v, ol, os, line, sub, v->rows_part);
  else dr = -rows_between(v, line, sub, ol, os, v->rows_part);
  UpdateWindow(v->w);                                 // flush rows invalidated for the old position
  s->top_line = line; s->top_subrow = sub;
  v->rm_valid = 0;
  if (dr != 0 && dr < big && -dr < big && (dr < 0 ? -dr : dr) < v->rows_part) {
    ScrollWindowEx(v->w, 0, -dr * v->ch, 0, 0, 0, 0, SW_INVALIDATE);
    UpdateWindow(v->w);
  } else InvalidateRect(v->w, 0, FALSE);
  update_scrollbars(v);
}

static void scroll_rows(TV* v, int n)
{
  u32 line = v->t->tv.top_line, sub = v->t->tv.top_subrow;
  int k;
  if (!tv_wrap(v)) {
    i64 nl = (i64)line + n;
    if (nl < 0) nl = 0;
    if (nl > (i64)0xFFFFFFF0) nl = 0xFFFFFFF0;
    tv_set_top(v, (u32)nl, 0);
    return;
  }
  for (k = 0; k < (n < 0 ? -n : n); k++) if (!step_row(v, &line, &sub, n < 0 ? -1 : 1)) break;
  tv_set_top(v, line, sub);
}

static void tv_set_hscroll(TV* v, u32 h)
{
  u32 old = v->t->tv.hscroll;
  RECT rc;
  if (tv_wrap(v)) h = 0;
  if (h == old) return;
  UpdateWindow(v->w);
  v->t->tv.hscroll = h;
  rc.left = v->text_left; rc.top = 0; rc.right = v->cli_w; rc.bottom = v->cli_h;
  if ((h > old ? h - old : old - h) < v->cols) {
    int dx = ((int)old - (int)h) * v->cw;
    ScrollWindowEx(v->w, dx, 0, &rc, &rc, 0, 0, SW_INVALIDATE);
    UpdateWindow(v->w);
  } else InvalidateRect(v->w, &rc, FALSE);
  update_scrollbars(v);
}

static void ensure_visible_pos(TV* v, epos pos, int center)
{
  u32 line, sub, cell, rc0, tl, ts;
  int r;
  if (!v->t || !v->d || v->d->indexing()) return;
  pos_rowcell(v, pos, &line, &sub, &cell, &rc0);
  tl = v->t->tv.top_line; ts = v->t->tv.top_subrow;
  r = row_lt(line, sub, tl, ts) ? -1 : rows_between(v, tl, ts, line, sub, v->rows_full);
  if (r < 0 || r >= v->rows_full) {
    u32 nl = line, ns = sub, k, back;
    back = center ? (u32)v->rows_full / 2 : (r < 0 ? 0 : (u32)v->rows_full - 1);
    for (k = 0; k < back; k++) if (!step_row(v, &nl, &ns, -1)) break;
    tv_set_top(v, nl, ns);
  }
  if (!tv_wrap(v)) {
    u32 h = v->t->tv.hscroll, cols = v->cols, margin = cols / 4;
    if (cell < h || cell >= h + cols) {
      if (center) h = cell > cols / 2 ? cell - cols / 2 : 0;
      else if (cell < h) h = cell > margin ? cell - margin : 0;
      else h = cell - cols + 1 + margin;
      tv_set_hscroll(v, h);
    }
  }
}

static void update_scrollbars(TV* v)
{
  SCROLLINFO si;
  if (!v->w) return;
  si.cbSize = sizeof(si);
  si.fMask = SIF_ALL | SIF_DISABLENOSCROLL;
  si.nMin = 0;
  if (!v->t || !v->d || v->d->indexing()) {
    si.nMax = 0; si.nPage = 1; si.nPos = 0;
    v->vs_scaled = 0; v->vs_max = 0;
    SetScrollInfo(v->w, SB_VERT, &si, TRUE);
    SetScrollInfo(v->w, SB_HORZ, &si, TRUE);
    return;
  }
  {
    u32 n = v->d->line_count(), page = (u32)v->rows_full, maxpos, tl = v->t->tv.top_line, ml, ms;
    max_top(v, &ml, &ms);
    maxpos = tv_wrap(v) ? (n ? n - 1 : 0) : ml;
    v->vs_max = maxpos;
    if (maxpos + page <= 32767) {
      v->vs_scaled = 0;
      si.nMax = (int)(maxpos + page - 1); si.nPage = page; si.nPos = (int)tl;
    } else {
      u32 sp = (u32)((u64)page * 32767 / ((u64)maxpos + page));
      if (sp < 1) sp = 1;
      v->vs_scaled = 1; v->vs_page = sp;
      si.nMax = 32767; si.nPage = sp;
      si.nPos = (int)((u64)tl * (32767 - sp + 1) / (maxpos ? maxpos : 1));
    }
    SetScrollInfo(v->w, SB_VERT, &si, TRUE);
  }
  if (tv_wrap(v)) {
    if (v->hs_shown) { ShowScrollBar(v->w, SB_HORZ, FALSE); v->hs_shown = 0; }
    return;
  }
  if (!v->hs_shown) { ShowScrollBar(v->w, SB_HORZ, TRUE); v->hs_shown = 1; }
  {
    u32 h = v->t->tv.hscroll, total = v->max_cells + 1;
    if (total < h + v->cols) total = h + v->cols;
    v->hs_total = total;
    si.fMask = SIF_ALL | SIF_DISABLENOSCROLL;
    if (total <= 32767) {
      v->hs_scaled = 0;
      si.nMax = (int)total - 1; si.nPage = v->cols; si.nPos = (int)h;
    } else {
      u32 sp = (u32)((u64)v->cols * 32767 / total);
      if (sp < 1) sp = 1;
      v->hs_scaled = 1;
      si.nMax = 32767; si.nPage = sp;
      si.nPos = (int)((u64)h * (32767 - sp + 1) / (total - v->cols ? total - v->cols : 1));
    }
    SetScrollInfo(v->w, SB_HORZ, &si, TRUE);
  }
}

// ---------------------------------------------------------------------------------------------------------------
// Painting

struct RunBuf {
  WCHAR s[TV_RUN_MAX + 8]; int dx[TV_RUN_MAX + 8]; u32 n; int needdx;
  RunAttr a; u32 c0, c1; int open, xoff;
};

static int attr_eq(const RunAttr& x, const RunAttr& y)
{
  return x.fg == y.fg && x.bg == y.bg && x.bold == y.bold && x.font == y.font && x.box == y.box;
}

static void run_flush(TV* v, HDC dc, int y, u32 left_cell, RunBuf* rb)
{
  RECT rc;
  int x;
  if (!rb->open) return;
  rb->open = 0;
  x = v->text_left + ((int)rb->c0 - (int)left_cell) * v->cw;
  rc.left = x < v->text_left ? v->text_left : x;
  rc.right = v->text_left + ((int)rb->c1 - (int)left_cell) * v->cw;
  if (rc.right > v->cli_w) rc.right = v->cli_w;
  rc.top = y; rc.bottom = y + v->ch;
  if (rc.right > rc.left) render_run(dc, x + rb->xoff, y, &rc, rb->s, rb->n, rb->a, rb->needdx ? rb->dx : 0);
  rb->n = 0; rb->needdx = 0; rb->xoff = 0;
}

static void run_add(TV* v, HDC dc, int y, u32 left_cell, RunBuf* rb, const RunAttr& a, u32 cell, u32 width,
                    const u16* g, u32 ng, int kind, int needdx)
{
  u32 i;
  if (rb->open && (!attr_eq(rb->a, a) || rb->n + ng > TV_RUN_MAX || (rb->c1 != cell && kind != LK_ZERO) || a.box))
    run_flush(v, dc, y, left_cell, rb);
  if (!rb->open) {
    rb->open = 1; rb->a = a; rb->c0 = cell; rb->c1 = cell; rb->n = 0; rb->needdx = 0;
    rb->xoff = a.box && width > 1 ? (int)(width - 1) * v->cw / 2 : 0;   // centre a wide placeholder's '?'
  }
  for (i = 0; i < ng; i++) {
    int adv;
    if (kind == LK_WIDE) adv = (int)width * v->cw;
    else if (kind == LK_ZERO) adv = 0;
    else if (ng == 1) adv = (int)width * v->cw;
    else adv = v->cw;
    rb->s[rb->n] = (WCHAR)g[i];
    rb->dx[rb->n] = adv;
    if (kind == LK_ZERO && rb->n > 0) {             // draw the mark over its base: move the base's advance to it
      rb->dx[rb->n] = rb->dx[rb->n - 1];
      rb->dx[rb->n - 1] = 0;
    }
    rb->n++;
  }
  if (needdx || a.bold) rb->needdx = 1;
  if (cell + width > rb->c1) rb->c1 = cell + width;
  if (a.box) run_flush(v, dc, y, left_cell, rb);
}

struct PaintCtx {
  COLORREF def_fg, def_bg, hl_fg, hl_bg, ws_fg, ctrl_fg, ctrl_bg, bad_fg, bad_bg, mark_bg, brace_bg, brace_bad;
  epos sel_lo, sel_hi;
};

static void paint_gutter(TV* v, HDC dc, int y, const RowEnt* re)
{
  RECT rc;
  RunAttr a;
  WCHAR s[24];
  u32 i, n = 0;
  if (v->gut_px <= 0) return;
  a.bg = render_ui(UIC_GUTTER_BG); a.bold = 0; a.font = RF_NORMAL; a.box = 0;
  rc.left = 0; rc.top = y; rc.right = v->cw; rc.bottom = y + v->ch;
  if (re && re->sub == 0 && v->t->bookmarks.has(re->line)) {
    u16 bullet = 0x2022;
    a.fg = render_ui(UIC_BOOKMARK);
    a.font = (u8)render_glyph_font(bullet);
    s[0] = (WCHAR)bullet;
    render_run(dc, 0, y, &rc, s, 1, a, 0);
    a.font = RF_NORMAL;
  } else render_fill(dc, &rc, a.bg);
  if (v->gut_px <= v->cw) return;
  rc.left = v->cw; rc.right = v->gut_px;
  a.fg = render_ui(UIC_GUTTER_FG);
  if (re && re->sub == 0) {
    char num[16];
    u32 len, pad;
    es_fmt(num, sizeof(num), "%u", re->line + 1);
    len = es_len(num);
    pad = v->digits > len ? v->digits - len : 0;
    for (i = 0; i < pad; i++) s[n++] = ' ';
    for (i = 0; i < len && n < 22; i++) s[n++] = (WCHAR)num[i];
    s[n++] = ' ';
    render_run(dc, v->cw, y, &rc, s, n, a, 0);
  } else render_fill(dc, &rc, a.bg);
}

static void eol_label(TV* v, const LineInfo& li, const char** s)
{
  u32 unit = tv_unit(v->d);
  *s = 0;
  if (li.next <= li.end) return;
  if (li.next - li.end >= 2 * unit) { *s = "CRLF"; return; }
  *s = tv_is_cr_unit(v->d, li.end) ? "CR" : "LF";
}

static void paint_text(TV* v, HDC dc, int y, const RowEnt* re, const PaintCtx& pc)
{
  LineInfo li;
  RunBuf rb;
  RunAttr a;
  u32 left, rend, i, cnt, ci_from, ci_to, mi = 0, nmarks;
  const LayChar* chs;
  const u16* gl;
  int x_end, wrap = tv_wrap(v), cjk = render_fonts()->cjk != 0, last_row;
  RECT rc;
  lc_get(v, re->line, &li);
  left = tv_left_cell(v, re->c0);
  rend = wrap ? re->c1 : left + v->cols + 1;
  seg_line(v, re->line, 0, wrap ? re->c0 : left, 1);
  seg_styles(v);
  if (wrap && v->sg_whole) seg_wrap(v);
  chs = v->lay.chars(); gl = v->lay.glyphs(); cnt = v->lay.count();
  // char range of this row
  if (wrap && v->sg_whole) {
    u32 sub = re->sub < v->wrows.size() ? re->sub : v->wrows.size() - 1;
    ci_from = v->wrows[sub];
    ci_to = sub + 1 < v->wrows.size() ? v->wrows[sub + 1] : cnt;
  } else {
    u32 lo = 0, hi = cnt;
    while (lo < hi) { u32 mid = (lo + hi) / 2; if (chs[mid].cell < left) lo = mid + 1; else hi = mid; }
    while (lo > 0 && (chs[lo - 1].cell + chs[lo - 1].width > left || chs[lo - 1].kind == LK_ZERO ||
                      (lo < cnt && chs[lo].kind == LK_ZERO))) lo--;
    ci_from = lo; ci_to = cnt;
  }
  rb.open = 0; rb.n = 0; rb.needdx = 0; rb.xoff = 0;
  nmarks = v->t->marks.count();
  if (nmarks) mi = v->t->marks.first_ending_after(v->sg_pos + (ci_from < cnt ? chs[ci_from].off : v->lay.bytes()));
  x_end = v->text_left;
  for (i = ci_from; i < ci_to; i++) {
    const LayChar& c = chs[i];
    epos at = v->sg_pos + c.off;
    u8 st = i < v->sty_n ? v->sty[i] : (u8)ST_DEFAULT;
    u16 ph = '?';
    const u16* g = gl + c.glyph;
    u32 ng = c.nglyph, width = c.width;
    int needdx = 0, kind = c.kind;
    if ((!wrap || !v->sg_whole) && c.cell >= rend) break;
    a.fg = render_style_fg(st); a.bg = render_style_bg(st); a.bold = (u8)render_style_bold(st);
    a.font = RF_NORMAL; a.box = 0;
    switch (c.kind) {
    case LK_CTRL: a.fg = pc.ctrl_fg; a.bg = pc.ctrl_bg; a.bold = 0; break;
    case LK_BAD: a.fg = pc.bad_fg; a.bg = pc.bad_bg; a.bold = 0; break;
    case LK_NODRAW: a.fg = pc.ctrl_fg; a.bg = pc.ctrl_bg; a.bold = 0; a.box = 1; g = &ph; ng = 1; needdx = 1; break;
    case LK_WIDE:
      needdx = 1;
      if (cjk) a.font = RF_CJK;
      else if (!(ng == 1 && uc_is_drawable(g[0]))) { a.fg = pc.ctrl_fg; a.bg = pc.ctrl_bg; a.bold = 0; a.box = 1; g = &ph; ng = 1; }
      break;
    case LK_ZERO: needdx = 1; break;
    case LK_SPACE: if (tv_ws(v)) a.fg = pc.ws_fg; break;
    case LK_TAB: if (tv_ws(v)) a.fg = pc.ws_fg; break;
    default: break;
    }
    if (a.font == RF_NORMAL && !a.box && ng == 1 && g[0] >= 0x80) {
      needdx = 1;
      a.font = (u8)render_glyph_font(g[0]);
    }
    if (nmarks) {
      while (mi < nmarks && v->t->marks.at(mi).pos + v->t->marks.at(mi).len <= at) mi++;
      if (mi < nmarks && at >= v->t->marks.at(mi).pos) a.bg = pc.mark_bg;
    }
    if (at == v->br_a || at == v->br_b) {
      if (v->br_bad) a.fg = pc.brace_bad;
      else { a.bg = pc.brace_bg; a.bold = (u8)(render_fonts()->bold_ok ? 1 : 0); }
    }
    if (at >= pc.sel_lo && at < pc.sel_hi) { a.fg = pc.hl_fg; a.bg = pc.hl_bg; }
    run_add(v, dc, y, left, &rb, a, c.cell, width, g, ng, kind, needdx);
  }
  run_flush(v, dc, y, left, &rb);
  // end of the row: EOL marker (last row of the line, when the segment reaches the line end)
  last_row = re->last && v->sg_eof;
  {
    u32 endc;
    if (wrap && v->sg_whole) endc = ci_to < cnt ? chs[ci_to].cell : v->lay.end_cell();
    else if (wrap) endc = re->c1 < v->lay.end_cell() ? re->c1 : v->lay.end_cell();
    else endc = v->lay.end_cell();
    if (!wrap && !v->sg_eof && endc < rend) endc = rend;
    x_end = v->text_left + ((int)endc - (int)left) * v->cw;
    if (x_end < v->text_left) x_end = v->text_left;
  }
  if (last_row && x_end < v->cli_w) {
    const char* lab;
    int eol_sel = li.next > li.end && li.end >= pc.sel_lo && li.end < pc.sel_hi;
    eol_label(v, li, &lab);
    if (lab && (tv_ws(v) || eol_sel)) {
      WCHAR s[4];
      u32 n = 0;
      if (tv_ws(v)) { while (lab[n] && n < 4) { s[n] = (WCHAR)lab[n]; n++; } }
      else { s[0] = ' '; n = 1; }
      a.fg = eol_sel ? pc.hl_fg : pc.ws_fg; a.bg = eol_sel ? pc.hl_bg : pc.def_bg;
      a.bold = 0; a.font = RF_NORMAL; a.box = 0;
      rc.left = x_end; rc.top = y; rc.right = x_end + (int)n * v->cw; rc.bottom = y + v->ch;
      if (rc.right > v->cli_w) rc.right = v->cli_w;
      render_run(dc, x_end, y, &rc, s, n, a, 0);
      x_end = rc.right;
    }
  }
  if (x_end < v->cli_w) {
    rc.left = x_end; rc.top = y; rc.right = v->cli_w; rc.bottom = y + v->ch;
    render_fill(dc, &rc, pc.def_bg);
  }
}

// Indexing documents (14.1): the first screen from offset 0, lines found by scanning, no numbers.
static void paint_indexing(TV* v, HDC dc, const PaintCtx& pc)
{
  Document* d = v->d;
  epos p = d->text_origin(), len = d->length();
  int r;
  RECT rc;
  for (r = 0; r < v->rows_part; r++) {
    int y = r * v->ch;
    paint_gutter(v, dc, y, 0);
    rc.left = v->gut_px; rc.top = y; rc.right = v->text_left; rc.bottom = y + v->ch;
    render_fill(dc, &rc, pc.def_bg);
    rc.left = v->text_left; rc.right = v->cli_w;
    if (p < len) {
      epos bp = 0, e, n;
      int k = scan_break(d, p, LAY_WINDOW, &bp);
      e = k == 1 ? bp : (p + LAY_WINDOW < len ? p + LAY_WINDOW : len);
      n = e - p;
      if (k == 1 && n >= tv_unit(d) && tv_is_cr_unit(d, e - tv_unit(d))) n -= tv_unit(d);
      seg_read_build(v, p, n, 0, 1);
      v->sg_valid = 0;
      {
        u32 i, cnt = v->lay.count();
        const LayChar* chs = v->lay.chars();
        RunBuf rb;
        RunAttr a;
        rb.open = 0; rb.n = 0; rb.needdx = 0; rb.xoff = 0;
        for (i = 0; i < cnt && chs[i].cell < v->cols + 1; i++) {
          a.fg = pc.def_fg; a.bg = pc.def_bg; a.bold = 0; a.font = RF_NORMAL; a.box = 0;
          run_add(v, dc, y, 0, &rb, a, chs[i].cell, chs[i].width, v->lay.glyphs() + chs[i].glyph, chs[i].nglyph,
                  chs[i].kind, 1);
        }
        run_flush(v, dc, y, 0, &rb);
        rc.left = v->text_left + (int)emin<u32>(v->lay.end_cell(), v->cols + 1) * v->cw;
      }
      p = k == 1 ? bp + tv_unit(d) : e;
    }
    if (rc.left < v->cli_w) render_fill(dc, &rc, pc.def_bg);
  }
}

static void paint_ctx(TV* v, PaintCtx* pc)
{
  epos lo = sel_lo(v->t->sel), hi = sel_hi(v->t->sel);
  pc->def_fg = render_style_fg(ST_DEFAULT); pc->def_bg = render_style_bg(ST_DEFAULT);
  pc->hl_fg = GetSysColor(COLOR_HIGHLIGHTTEXT); pc->hl_bg = GetSysColor(COLOR_HIGHLIGHT);
  pc->ws_fg = render_ui(UIC_WS_FG);
  pc->ctrl_fg = render_ui(UIC_CTRL_FG); pc->ctrl_bg = render_ui(UIC_CTRL_BG);
  pc->bad_fg = render_ui(UIC_BAD_FG); pc->bad_bg = render_ui(UIC_BAD_BG);
  pc->mark_bg = render_ui(UIC_MARK_BG); pc->brace_bg = render_ui(UIC_BRACE_BG); pc->brace_bad = render_ui(UIC_BRACE_BAD_FG);
  pc->sel_lo = lo; pc->sel_hi = hi;
}

static void tv_take_dirty(TV* v);

static void tv_paint(TV* v)
{
  PAINTSTRUCT ps;
  HDC dc = BeginPaint(v->w, &ps);
  RECT rc;
  if (!v->t || !v->d || v->building) {             // no tab, or a long-line index is being built (progress pump)
    render_fill(dc, &ps.rcPaint, GetSysColor(v->t ? COLOR_WINDOW : (v->panel ? COLOR_WINDOW : COLOR_APPWORKSPACE)));
    EndPaint(v->w, &ps);
    return;
  }
  v->in_paint = 1;
  {
    PaintCtx pc;
    u32 r, nrows;
    paint_ctx(v, &pc);
    if (tv_digits(v->d->indexing() ? 1 : v->d->line_count()) != v->last_digits) {
      tv_metrics(v);
      InvalidateRect(v->w, 0, FALSE);
    }
    if (v->d->indexing()) {
      paint_indexing(v, dc, pc);
    } else {
      rmap_build(v);
      nrows = v->rmap.size();
      if (v->t->hl && nrows) v->t->hl->set_target(v->rmap[nrows - 1].line);
      for (r = 0; r < (u32)v->rows_part; r++) {
        int y = (int)r * v->ch;
        if (y + v->ch <= ps.rcPaint.top || y >= ps.rcPaint.bottom) continue;
        if (r < nrows) {
          paint_gutter(v, dc, y, &v->rmap[r]);
          rc.left = v->gut_px; rc.top = y; rc.right = v->text_left; rc.bottom = y + v->ch;
          render_fill(dc, &rc, pc.def_bg);
          if (ps.rcPaint.right > v->text_left) paint_text(v, dc, y, &v->rmap[r], pc);
        } else {
          paint_gutter(v, dc, y, 0);
          rc.left = v->gut_px; rc.top = y; rc.right = v->cli_w; rc.bottom = y + v->ch;
          render_fill(dc, &rc, pc.def_bg);
        }
      }
    }
    if (v->rows_part * v->ch < v->cli_h) {
      rc.left = 0; rc.top = v->rows_part * v->ch; rc.right = v->cli_w; rc.bottom = v->cli_h;
      render_fill(dc, &rc, pc.def_bg);
    }
  }
  v->in_paint = 0;
  EndPaint(v->w, &ps);
  tv_take_dirty(v);
  if (v->nflags & NF_SCROLL) tv_notify(v, 0);
}

static void inval_lines(TV* v, u32 first, u32 last)
{
  u32 i;
  RECT rc;
  rmap_build(v);
  for (i = 0; i < v->rmap.size(); i++) {
    if (v->rmap[i].line < first || v->rmap[i].line > last) continue;
    rc.left = 0; rc.right = v->cli_w; rc.top = (int)i * v->ch; rc.bottom = rc.top + v->ch;
    InvalidateRect(v->w, &rc, FALSE);
  }
}

static void tv_take_dirty(TV* v)
{
  u32 a, b;
  if (!v->t || !v->t->hl) return;
  while (v->t->hl->take_dirty(&a, &b)) inval_lines(v, a, b);
}

// rows showing any byte of [lo, hi] (inclusive: a caret at hi counts)
static void inval_range(TV* v, epos lo, epos hi)
{
  u32 i;
  RECT rc;
  rmap_build(v);
  for (i = 0; i < v->rmap.size(); i++) {
    const RowEnt& re = v->rmap[i];
    if (re.a > hi || re.b < lo) continue;
    rc.left = 0; rc.right = v->cli_w; rc.top = (int)i * v->ch; rc.bottom = rc.top + v->ch;
    InvalidateRect(v->w, &rc, FALSE);
  }
}

static void inval_sel_change(TV* v, const Selection& o, const Selection& n)
{
  if (sel_empty(o) && sel_empty(n)) return;
  if (sel_empty(o)) { inval_range(v, sel_lo(n), sel_hi(n)); return; }
  if (sel_empty(n)) { inval_range(v, sel_lo(o), sel_hi(o)); return; }
  inval_range(v, emin(sel_lo(o), sel_lo(n)), emax(sel_hi(o), sel_hi(n)));
}

// ---------------------------------------------------------------------------------------------------------------
// Caret

static void caret_update(TV* v)
{
  int want_w, x = 0, y = 0, vis;
  if (!v->focus || !v->w) return;
  want_w = v->t && v->t->tv.overwrite ? v->cw : 2;
  if (v->caret_w != want_w || v->caret_h != v->ch) {
    DestroyCaret();
    CreateCaret(v->w, 0, want_w, v->ch);
    v->caret_w = want_w; v->caret_h = v->ch; v->caret_on = 0;
  }
  vis = v->t && v->d && !v->d->indexing() && pos_to_xy(v, v->t->sel.caret, &x, &y);
  if (vis && x >= v->text_left - 1 && x < v->cli_w && y < v->cli_h) {
    SetCaretPos(x > v->text_left - 1 ? x - (v->t->tv.overwrite ? 0 : 1) : x, y);
    if (!v->caret_on) { ShowCaret(v->w); v->caret_on = 1; }
    if (v->ime_on) {
      HIMC imc = ImmGetContext(v->w);
      if (imc) {
        COMPOSITIONFORM cf;
        cf.dwStyle = CFS_POINT; cf.ptCurrentPos.x = x; cf.ptCurrentPos.y = y;
        ImmSetCompositionWindow(imc, &cf);
        ImmReleaseContext(v->w, imc);
      }
    }
  } else if (v->caret_on) { HideCaret(v->w); v->caret_on = 0; }
}

// ---------------------------------------------------------------------------------------------------------------
// Selection, brace match, notifications

static void sel_clamp(TV* v)
{
  Document* d = v->d;
  epos o = d->text_origin(), n = d->length();
  Selection* s = &v->t->sel;
  if (s->anchor < o) s->anchor = o;
  if (s->caret < o) s->caret = o;
  if (s->anchor > n) s->anchor = n;
  if (s->caret > n) s->caret = n;
  if (!d->indexing()) {
    s->anchor = d->snap_char(s->anchor);
    s->caret = d->snap_char(s->caret);
    if (s->anchor < o) s->anchor = o;
    if (s->caret < o) s->caret = o;
  }
}

static void brace_update(TV* v)
{
  epos a = EPOS_NONE, b = EPOS_NONE, oa = v->br_a, ob = v->br_b;
  int bad = 0;
  if (v->t && v->d && !v->panel && g_app.cfg.brace_match && !v->t->large && !v->d->indexing() && sel_empty(v->t->sel)) {
    const char* braces = v->t->lang ? v->t->lang->braces : "()[]{}";
    if (braces && braces[0]) {
      epos br = EPOS_NONE, m = EPOS_NONE, c = v->t->sel.caret;
      int rc = ed_match_brace(v->d, c, v->t->hl, braces, &br, &m);   // checks the char before c, then at c
      if (rc >= 0 && br != EPOS_NONE) { a = br; b = rc == 1 ? m : EPOS_NONE; bad = rc != 1 || m == EPOS_NONE; }
    }
  }
  if (a == oa && b == ob && bad == v->br_bad) return;
  v->br_a = a; v->br_b = b; v->br_bad = bad;
  if (oa != EPOS_NONE) inval_range(v, oa, oa);
  if (ob != EPOS_NONE) inval_range(v, ob, ob);
  if (a != EPOS_NONE) inval_range(v, a, a);
  if (b != EPOS_NONE) inval_range(v, b, b);
}

static void tv_on_notify(TV* v)
{
  u32 f = v->nflags;
  v->nflags = 0; v->notify_posted = 0;
  if (!v->t || !v->d) return;
  if (f & (NF_SCROLL | NF_DOC)) update_scrollbars(v);
  if (f & NF_DOC) {
    int mod = v->d->modified() ? 1 : 0;
    if (!v->panel) app_on_doc_changed(v->t);
    if (mod != v->last_mod) { v->last_mod = mod; if (!v->panel && g_app.frame) PostMessageA(g_app.frame, WM_APP_DOCSTATE, 0, 0); }
  }
  if (f & (NF_CARET | NF_DOC)) {
    brace_update(v);
    if (!v->panel && g_app.frame) PostMessageA(g_app.frame, WM_APP_CARET, 0, 0);
  }
}

// Moves the caret (and the anchor unless extending); scrolls it into view.
static void tv_move(TV* v, epos np, int extend, int keep_desired)
{
  Selection old = v->t->sel;
  v->t->sel.caret = np;
  if (!extend) v->t->sel.anchor = np;
  if (!keep_desired) v->t->tv.desired_cell = NODESIRED;
  inval_sel_change(v, old, v->t->sel);
  ensure_visible_pos(v, np, 0);
  caret_update(v);
  tv_notify(v, NF_CARET);
}

// ---------------------------------------------------------------------------------------------------------------
// Editing from the keyboard

static void tv_ctx(TV* v, EdCtx* e)
{
  memset(e, 0, sizeof(*e));
  e->doc = v->d;
  e->sel = v->t->sel;
  ed_lang_from(v->t->lang, tv_tw(v), v->t->tv.use_spaces, &e->lang);
  e->lang.tab_width = tv_tw(v);                      // Tab::tv holds the effective per-tab settings
  e->lang.use_spaces = v->t->tv.use_spaces ? 1 : 0;
  e->overwrite = v->t->tv.overwrite;
  e->auto_indent = g_app.cfg.auto_indent;
  e->now_ms = app_now_ms();
}

static void tv_edit_error(TV* v, int rc)
{
  char enc[64];
  MessageBeep(MB_OK);
  switch (rc) {
  case E_RANGE: {
    char msg[200];
    enc_label(v->d->enc(), enc, sizeof(enc));
    es_fmt(msg, sizeof(msg), "This character can't be stored in %s.", enc);
    app_status_note("%s", msg);
    doccmd_offer_utf8(v->t, msg);
    break;
  }
  case E_READONLY: app_status_note("The document is read-only"); break;
  case E_BUSY: app_status_note("The document is busy (saving or indexing)"); break;
  case E_ALIGN: app_status_note("Delete the odd trailing byte first"); break;
  case E_NOMEM: app_status_note("Out of memory: close some tabs"); break;
  case E_NOTIMPL: app_status_note("Not implemented yet"); break;
  default: app_status_note("Edit failed (%s)", e_errname(rc)); break;
  }
}

enum { OP_TYPE = 1, OP_NEWLINE, OP_BACK, OP_DEL, OP_WORDBACK, OP_WORDDEL, OP_INDENT, OP_OUTDENT };

static int tv_edit(TV* v, int op, ecp c)
{
  EdCtx e;
  int rc = E_STATE;
  if (!v->t || !v->d || v->panel) return E_STATE;
  sel_clamp(v);
  tv_ctx(v, &e);
  v->own_edit = 1;
  switch (op) {
  case OP_TYPE: rc = ed_type(&e, c); break;
  case OP_NEWLINE: rc = ed_newline(&e); break;
  case OP_BACK: rc = ed_backspace(&e); break;
  case OP_DEL: rc = ed_delete(&e); break;
  case OP_WORDBACK: rc = ed_delete_word(&e, 0); break;
  case OP_WORDDEL: rc = ed_delete_word(&e, 1); break;
  case OP_INDENT: rc = ed_indent(&e, 0); break;
  case OP_OUTDENT: rc = ed_indent(&e, 1); break;
  default: break;
  }
  v->own_edit = 0;
  if (rc != E_OK) { tv_edit_error(v, rc); return rc; }
  {
    Selection old = v->t->sel;
    v->t->sel = e.sel;
    v->t->tv.desired_cell = NODESIRED;
    v->affinity = 0;
    inval_sel_change(v, old, v->t->sel);
    ensure_visible_pos(v, v->t->sel.caret, 0);
    caret_update(v);
    tv_notify(v, NF_CARET | NF_DOC);
  }
  return rc;
}

// ---------------------------------------------------------------------------------------------------------------
// Keyboard navigation

static u32 tv_cur_rel_cell(TV* v)
{
  u32 line, sub, cell, rc0;
  pos_rowcell(v, v->t->sel.caret, &line, &sub, &cell, &rc0);
  return cell - rc0;
}

static epos tv_vertical(TV* v, int rows, int* moved)
{
  u32 line, sub, cell, rc0, k, n = (u32)(rows < 0 ? -rows : rows);
  RowEnt re;
  int aff = 0;
  epos p;
  pos_rowcell(v, v->t->sel.caret, &line, &sub, &cell, &rc0);
  if (v->t->tv.desired_cell == NODESIRED) v->t->tv.desired_cell = cell - rc0;
  *moved = 0;
  for (k = 0; k < n; k++) { if (!step_row(v, &line, &sub, rows < 0 ? -1 : 1)) break; (*moved)++; }
  if (!*moved) return v->t->sel.caret;
  row_geo(v, line, sub, &re);
  {
    u32 base = tv_wrap(v) ? re.c0 : 0;
    p = row_pos_at_cell(v, &re, base + v->t->tv.desired_cell, &aff);
  }
  v->affinity = aff;
  return p;
}

static void tv_key(TV* v, UINT vk)
{
  int shift = GetKeyState(VK_SHIFT) < 0, ctrl = GetKeyState(VK_CONTROL) < 0;
  Document* d = v->d;
  epos c, np;
  int moved;
  if (!v->t || !d) return;
  if (v->panel && v->ph && v->ph->panel_key(vk)) return;
  if (d->indexing()) return;
  sel_clamp(v);
  c = v->t->sel.caret;
  switch (vk) {
  case VK_LEFT:
    v->affinity = 0;
    if (!shift && !ctrl && !sel_empty(v->t->sel)) np = sel_lo(v->t->sel);
    else if (ctrl) np = ed_word_left(d, c);
    else {
      u32 l = tv_line_of(v, c);
      LineInfo li;
      lc_get(v, l, &li);
      if (c <= li.start && l > 0) { LineInfo lp; lc_get(v, l - 1, &lp); np = lp.end; }   // over the whole EOL
      else np = c > d->text_origin() ? d->char_prev(c) : c;
    }
    tv_move(v, np, shift, 0);
    break;
  case VK_RIGHT:
    v->affinity = 0;
    if (!shift && !ctrl && !sel_empty(v->t->sel)) np = sel_hi(v->t->sel);
    else if (ctrl) np = ed_word_right(d, c);
    else {
      u32 l = tv_line_of(v, c);
      LineInfo li;
      lc_get(v, l, &li);
      np = c >= li.end ? li.next : d->char_next(c);
    }
    tv_move(v, np, shift, 0);
    break;
  case VK_UP:
  case VK_DOWN:
    if (ctrl) { scroll_rows(v, vk == VK_UP ? -1 : 1); caret_update(v); break; }
    np = tv_vertical(v, vk == VK_UP ? -1 : 1, &moved);
    tv_move(v, np, shift, 1);
    break;
  case VK_PRIOR:
  case VK_NEXT: {
    int n = v->rows_full > 1 ? v->rows_full - 1 : 1;
    if (ctrl) break;
    np = tv_vertical(v, vk == VK_PRIOR ? -n : n, &moved);
    if (moved) scroll_rows(v, vk == VK_PRIOR ? -moved : moved);
    tv_move(v, np, shift, 1);
    break;
  }
  case VK_HOME:
    if (ctrl) np = d->text_origin();
    else {
      u32 line, sub, cell, rc0;
      pos_rowcell(v, c, &line, &sub, &cell, &rc0);   // with the current affinity (row the caret is shown on)
      np = ed_smart_home(d, c);
      if (tv_wrap(v) && sub > 0) {
        RowEnt re;
        row_geo(v, line, sub, &re);
        if (c != re.a) np = re.a;
      }
    }
    v->affinity = 0;
    tv_move(v, np, shift, 0);
    break;
  case VK_END:
    if (ctrl) { np = d->length(); v->affinity = 0; }
    else {
      u32 line, sub, cell, rc0;
      int aff0 = v->affinity, naff = 0;
      LineInfo li;
      pos_rowcell(v, c, &line, &sub, &cell, &rc0);
      lc_get(v, line, &li);
      np = li.end;
      if (tv_wrap(v)) {                                // first press: end of the display row
        RowEnt re;
        row_geo(v, line, sub, &re);
        if (!re.last && !(c == re.b && aff0)) { np = re.b; naff = 1; }
      }
      v->affinity = naff;
    }
    tv_move(v, np, shift, 0);
    break;
  case VK_BACK: tv_edit(v, ctrl ? OP_WORDBACK : OP_BACK, 0); break;
  case VK_DELETE: if (!shift) tv_edit(v, ctrl ? OP_WORDDEL : OP_DEL, 0); break;
  case VK_RETURN:
    if (v->panel) { if (v->ph) v->ph->panel_open(c); break; }
    if (!ctrl) tv_edit(v, OP_NEWLINE, 0);
    break;
  case VK_TAB: {
    epos lo = sel_lo(v->t->sel), hi = sel_hi(v->t->sel);
    int multi = !sel_empty(v->t->sel) && tv_line_of(v, lo) != tv_line_of(v, hi);
    if (ctrl) break;
    if (multi || shift) tv_edit(v, shift ? OP_OUTDENT : OP_INDENT, 0);
    else tv_edit(v, OP_TYPE, '\t');
    break;
  }
  case VK_INSERT:
    if (shift || ctrl || v->panel) break;
    v->t->tv.overwrite = (u8)!v->t->tv.overwrite;
    caret_update(v);
    if (g_app.frame) PostMessageA(g_app.frame, WM_APP_DOCSTATE, 0, 0);
    break;
  case VK_ESCAPE:
    if (v->t->marks.count()) { v->t->marks.clear(); InvalidateRect(v->w, 0, FALSE); }
    break;
  default: break;
  }
}

static void tv_char(TV* v, WPARAM wp)
{
  u8 b = (u8)wp;
  int ctrl = GetKeyState(VK_CONTROL) < 0, alt = GetKeyState(VK_MENU) < 0;
  u16 cp;
  Vec<ecp> out;
  ecp c = 0;
  if (!v->t || v->panel) return;
  if (ctrl && !alt) { v->lead = 0; return; }        // Ctrl+key = control key (AltGr = Ctrl+Alt types)
  if (b < 0x20 || b == 0x7F) { v->lead = 0; return; }   // Tab/Enter/Backspace/Esc come through WM_KEYDOWN
  cp = winenc_input_cp();
  if (v->lead) {
    char two[2];
    two[0] = (char)v->lead; two[1] = (char)b;
    v->lead = 0;
    if (winenc_mb_to_ecp(cp, two, 2, &out) != E_OK || out.empty()) { MessageBeep(MB_OK); return; }
    c = out[0];
  } else if (b >= 0x80 && IsDBCSLeadByteEx(cp, b)) {
    v->lead = b;
    return;
  } else if (b < 0x80) c = b;
  else {
    char one = (char)b;
    if (winenc_mb_to_ecp(cp, &one, 1, &out) != E_OK || out.empty()) { MessageBeep(MB_OK); return; }
    c = out[0];
  }
  tv_edit(v, OP_TYPE, c);
}

static void tv_ime_char(TV* v, WPARAM wp)
{
  char s[2];
  u32 n = 0;
  Vec<ecp> out;
  u32 i;
  if (!v->t || v->panel) return;
  if (wp > 0xFF) s[n++] = (char)((wp >> 8) & 0xFF);
  s[n++] = (char)(wp & 0xFF);
  if (winenc_mb_to_ecp(winenc_input_cp(), s, n, &out) != E_OK) { MessageBeep(MB_OK); return; }
  for (i = 0; i < out.size(); i++) tv_edit(v, OP_TYPE, out[i]);
}

// ---------------------------------------------------------------------------------------------------------------
// Mouse

static void line_bounds(TV* v, u32 line, epos* s, epos* nx)
{
  LineInfo li;
  lc_get(v, line, &li);
  *s = li.start; *nx = li.next;
}

static void drag_update(TV* v)
{
  int aff;
  epos p = hit_pos(v, v->mx, v->my, &aff);
  Selection old = v->t->sel, ns = old;
  if (v->drag == DRAG_TEXT) { ns.caret = p; v->affinity = aff; }
  else if (v->drag == DRAG_WORD) {
    Selection w;
    ed_word_at(v->d, p, &w);
    if (p < v->drag_a0) { ns.anchor = v->drag_a1; ns.caret = sel_lo(w); }
    else { ns.anchor = v->drag_a0; ns.caret = sel_hi(w) > v->drag_a1 ? sel_hi(w) : v->drag_a1; }
  } else if (v->drag == DRAG_LINES) {
    u32 l = tv_line_of(v, p);
    epos s0, n0, s1, n1;
    line_bounds(v, v->drag_line0, &s0, &n0);
    line_bounds(v, l, &s1, &n1);
    if (l >= v->drag_line0) { ns.anchor = s0; ns.caret = n1; }
    else { ns.anchor = n0; ns.caret = s1; }
  }
  if (ns.anchor == old.anchor && ns.caret == old.caret) return;
  v->t->sel = ns;
  v->t->tv.desired_cell = NODESIRED;
  inval_sel_change(v, old, ns);
  caret_update(v);
  tv_notify(v, NF_CARET);
}

static void tv_lbutton(TV* v, int x, int y)
{
  int shift = GetKeyState(VK_SHIFT) < 0, aff;
  DWORD now = (DWORD)GetMessageTime();
  epos p;
  SetFocus(v->w);
  if (!v->t || !v->d || v->d->indexing()) return;
  sel_clamp(v);
  if (now - v->click_t <= GetDoubleClickTime() && abs(x - v->click_x) <= GetSystemMetrics(SM_CXDOUBLECLK) &&
      abs(y - v->click_y) <= GetSystemMetrics(SM_CYDOUBLECLK)) v->click_n = v->click_n >= 3 ? 1 : v->click_n + 1;
  else v->click_n = 1;
  v->click_t = now; v->click_x = x; v->click_y = y;
  v->mx = x; v->my = y;
  p = hit_pos(v, x, y, &aff);
  SetCapture(v->w);
  if (x < v->text_left - 2 && !v->panel) {                 // gutter
    u32 l = tv_line_of(v, p);
    rmap_build(v);
    {
      int r = y / v->ch;
      if (r >= 0 && r < (int)v->rmap.size()) l = v->rmap[(u32)r].line;
    }
    if (x < v->cw) {
      v->t->bookmarks.toggle(l);
      inval_lines(v, l, l);
      ReleaseCapture();
      tv_notify(v, NF_CARET);
      return;
    }
    v->drag = DRAG_LINES; v->drag_line0 = l;
    if (shift) v->drag_line0 = tv_line_of(v, v->t->sel.anchor);
    drag_update(v);
    return;
  }
  if (v->panel && v->click_n == 2) {
    ReleaseCapture();
    v->t->sel.anchor = v->t->sel.caret = p;
    InvalidateRect(v->w, 0, FALSE);
    caret_update(v);
    if (v->ph) v->ph->panel_open(p);
    return;
  }
  if (v->click_n == 1) {
    Selection old = v->t->sel;
    v->affinity = aff;
    v->t->sel.caret = p;
    if (!shift) v->t->sel.anchor = p;
    v->t->tv.desired_cell = NODESIRED;
    inval_sel_change(v, old, v->t->sel);
    v->drag = DRAG_TEXT;
    caret_update(v);
    tv_notify(v, NF_CARET);
  } else if (v->click_n == 2) {
    Selection old = v->t->sel, w;
    ed_word_at(v->d, p, &w);
    v->drag_a0 = sel_lo(w); v->drag_a1 = sel_hi(w);
    v->t->sel.anchor = v->drag_a0; v->t->sel.caret = v->drag_a1;
    v->affinity = 0;
    inval_sel_change(v, old, v->t->sel);
    v->drag = DRAG_WORD;
    caret_update(v);
    tv_notify(v, NF_CARET);
  } else {
    v->drag = DRAG_LINES;
    v->drag_line0 = tv_line_of(v, p);
    v->affinity = 0;
    drag_update(v);
  }
}

static void tv_drag_timer(TV* v)
{
  int dy = 0, dx = 0;
  if (!v->drag || !v->t) { KillTimer(v->w, TVT_DRAG); v->drag_timer = 0; return; }
  if (v->my < 0) dy = -(1 + (-v->my) / v->ch);
  else if (v->my >= v->cli_h) dy = 1 + (v->my - v->cli_h) / v->ch;
  if (!tv_wrap(v)) {
    if (v->mx < v->text_left) dx = -(1 + (v->text_left - v->mx) / v->cw);
    else if (v->mx > v->cli_w) dx = 1 + (v->mx - v->cli_w) / v->cw;
  }
  if (dy) scroll_rows(v, dy);
  if (dx) {
    i64 h = (i64)v->t->tv.hscroll + dx * 2;
    tv_set_hscroll(v, h < 0 ? 0 : (u32)h);
  }
  if (!dx && !dy) { KillTimer(v->w, TVT_DRAG); v->drag_timer = 0; }
  drag_update(v);
}

static void tv_mousemove(TV* v, int x, int y)
{
  int out;
  if (!v->drag || GetCapture() != v->w) return;
  v->mx = x; v->my = y;
  out = y < 0 || y >= v->cli_h || (!tv_wrap(v) && (x < v->text_left || x > v->cli_w));
  if (out && !v->drag_timer) { SetTimer(v->w, TVT_DRAG, 50, 0); v->drag_timer = 1; }
  drag_update(v);
}

static void tv_wheel(TV* v, int delta)
{
  UINT lines = 3;
  int n;
  if (!v->t || !v->d) return;
  if (!SystemParametersInfoA(SPI_GETWHEELSCROLLLINES, 0, &lines, 0)) lines = 3;
  v->wheel_acc += delta;
  n = v->wheel_acc / WHEEL_DELTA;
  if (!n) return;
  v->wheel_acc -= n * WHEEL_DELTA;
  if (lines == 0xFFFFFFFFu || lines > 1000) scroll_rows(v, -n * (v->rows_full > 1 ? v->rows_full - 1 : 1));
  else scroll_rows(v, -n * (int)lines);
  caret_update(v);
}

static void menu_add(HMENU m, int id, const char* text)
{
  UINT st = cmd_state(id);
  AppendMenuA(m, MF_STRING | (st & MF_GRAYED ? MF_GRAYED : 0), (UINT)id, text);
}

static void tv_context_menu(TV* v, int sx, int sy)
{
  HMENU m = CreatePopupMenu();
  int id;
  if (!m) return;
  if (sx == -1 && sy == -1) {
    int x = 0, y = 0;
    POINT pt;
    if (!v->t || !pos_to_xy(v, v->t->sel.caret, &x, &y)) { x = v->text_left; y = 0; }
    pt.x = x; pt.y = y + v->ch;
    ClientToScreen(v->w, &pt);
    sx = pt.x; sy = pt.y;
  }
  if (!v->panel) {
    menu_add(m, IDM_EDIT_UNDO, "&Undo");
    menu_add(m, IDM_EDIT_REDO, "&Redo");
    AppendMenuA(m, MF_SEPARATOR, 0, 0);
    menu_add(m, IDM_EDIT_CUT, "Cu&t");
  }
  menu_add(m, IDM_EDIT_COPY, "&Copy");
  if (!v->panel) {
    menu_add(m, IDM_EDIT_PASTE, "&Paste");
    menu_add(m, IDM_EDIT_DELETE, "&Delete");
  }
  AppendMenuA(m, MF_SEPARATOR, 0, 0);
  menu_add(m, IDM_EDIT_SELECTALL, "Select &All");
  if (!v->panel) {
    AppendMenuA(m, MF_SEPARATOR, 0, 0);
    menu_add(m, IDM_EDIT_COMMENT, "Toggle C&omment");
    menu_add(m, IDM_EDIT_UPPER, "UPPERCAS&E");
    menu_add(m, IDM_EDIT_LOWER, "&lowercase");
  }
  id = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_LEFTALIGN, sx, sy, 0, v->w, 0);
  DestroyMenu(m);
  if (id) app_command(id);
}

// ---------------------------------------------------------------------------------------------------------------
// DocListener

void TV::doc_changed(Document* doc, const EditEvent& e)
{
  TV* v = this;
  u32 seq;
  if (doc != v->d || !v->t) return;
  seq = doc->change_seq();
  // line cache: keep it valid for in-line edits, truncate it for line-count changes
  if (e.flags & (EV_RESET | EV_INDEXED)) { v->lc.clear(); v->lc_seq = seq; }
  else if (v->lc_seq + 1 == seq && !v->lc.empty()) {
    u32 n = v->lc.size(), i;
    if (e.lines_added == 0 && e.lines_removed == 0 && e.line >= v->lc_first && e.line < v->lc_first + n) {
      i32 delta = (i32)e.inserted - (i32)e.removed;
      LineInfo li;
      u32 k = e.line - v->lc_first;
      for (i = k + 1; i < n; i++) {
        v->lc[i].start = (epos)((i32)v->lc[i].start + delta);
        v->lc[i].end = (epos)((i32)v->lc[i].end + delta);
        v->lc[i].next = (epos)((i32)v->lc[i].next + delta);
      }
      v->lc_seq = seq;
      walk_line(v, e.line, v->lc[k].start, &li);
      v->lc[k] = li;
      if (k + 1 < n && v->lc[k + 1].start != li.next) v->lc.clear();
    } else {
      if (e.line <= v->lc_first) v->lc.clear();
      else if (e.line - v->lc_first < n) v->lc.resize(e.line - v->lc_first);
      v->lc_seq = seq;
    }
  } else { v->lc.clear(); v->lc_seq = seq; }
  // long-line indices at or after the edit are stale (they hold absolute offsets)
  {
    u32 i;
    for (i = 0; i < TV_LLI; i++) {
      if (v->lli[i] && ((e.flags & (EV_RESET | EV_INDEXED)) || e.pos <= v->lli_le[i])) { delete v->lli[i]; v->lli[i] = 0; }
    }
  }
  v->sg_valid = 0; v->mt_valid = 0;
  if (e.flags & (EV_RESET | EV_INDEXED)) v->br_a = v->br_b = EPOS_NONE;
  // selection: edits made elsewhere (commands, other views) shift it like marks
  if (!v->own_edit) {
    Selection* s = &v->t->sel;
    epos* ps[2];
    u32 k;
    ps[0] = &s->anchor; ps[1] = &s->caret;
    for (k = 0; k < 2; k++) {
      epos p = *ps[k];
      if (e.flags & (EV_RESET | EV_INDEXED)) { if (p > doc->length()) p = doc->length(); }
      else if (p >= e.pos + e.removed) p = p - e.removed + e.inserted;
      else if (p > e.pos) p = e.pos;
      *ps[k] = p;
    }
  }
  // repaint
  {
    TextViewState* tv = &v->t->tv;
    int r = -1;
    u32 i;
    if (e.flags & (EV_RESET | EV_INDEXED)) {
      v->max_cells = 0;
      InvalidateRect(v->w, 0, FALSE);
    } else if (e.line < tv->top_line) {
      if (e.line + e.lines_removed < tv->top_line) {
        i64 nt = (i64)tv->top_line + (i64)e.lines_added - (i64)e.lines_removed;
        tv->top_line = nt < 0 ? 0 : (u32)nt;
      } else { tv->top_line = e.line; tv->top_subrow = 0; }
      if (e.lines_added != e.lines_removed) InvalidateRect(v->w, 0, FALSE);
    } else {
      for (i = 0; i < v->rmap.size() && v->rm_valid; i++) if (v->rmap[i].line == e.line) { r = (int)i; break; }
      if (r >= 0) {
        RECT rc;
        rc.left = 0; rc.right = v->cli_w; rc.top = r * v->ch; rc.bottom = v->cli_h;
        if (e.lines_added == 0 && e.lines_removed == 0 && !tv_wrap(v)) rc.bottom = rc.top + v->ch;
        InvalidateRect(v->w, &rc, FALSE);
      } else if (!v->rm_valid) InvalidateRect(v->w, 0, FALSE);
    }
    v->rm_valid = 0;
    if (tv_digits(doc->line_count()) != v->last_digits) { tv_metrics(v); InvalidateRect(v->w, 0, FALSE); }
  }
  tv_lex_timer(v);
  tv_notify(v, NF_DOC | NF_CARET);
}

// ---------------------------------------------------------------------------------------------------------------
// Window procedure

static void tv_reset_caches(TV* v)
{
  v->lc.clear(); v->sg_valid = 0; v->wr_valid = 0; v->sty_valid = 0; v->rm_valid = 0; v->mt_valid = 0;
  lli_clear(v);
}

static void tv_vscroll(TV* v, int code)
{
  SCROLLINFO si;
  u32 line;
  if (!v->t || !v->d || v->d->indexing()) return;
  switch (code) {
  case SB_LINEUP: scroll_rows(v, -1); break;
  case SB_LINEDOWN: scroll_rows(v, 1); break;
  case SB_PAGEUP: scroll_rows(v, -(v->rows_full > 1 ? v->rows_full - 1 : 1)); break;
  case SB_PAGEDOWN: scroll_rows(v, v->rows_full > 1 ? v->rows_full - 1 : 1); break;
  case SB_TOP: tv_set_top(v, 0, 0); break;
  case SB_BOTTOM: { u32 l, s; max_top(v, &l, &s); tv_set_top(v, l, s); break; }
  case SB_THUMBTRACK:
  case SB_THUMBPOSITION:
    si.cbSize = sizeof(si); si.fMask = SIF_TRACKPOS | SIF_PAGE;
    if (!GetScrollInfo(v->w, SB_VERT, &si)) break;
    if (v->vs_scaled) {
      u32 den = 32767 - v->vs_page + 1;
      line = (u32)((u64)(u32)si.nTrackPos * v->vs_max / (den ? den : 1));
    } else line = (u32)si.nTrackPos;
    tv_set_top(v, line, 0);
    break;
  default: return;
  }
  caret_update(v);
}

static void tv_hscroll(TV* v, int code)
{
  SCROLLINFO si;
  u32 h;
  if (!v->t || !v->d || tv_wrap(v) || v->d->indexing()) return;
  h = v->t->tv.hscroll;
  switch (code) {
  case SB_LINELEFT: h = h > 4 ? h - 4 : 0; break;
  case SB_LINERIGHT: h += 4; break;
  case SB_PAGELEFT: h = h > v->cols - 1 ? h - (v->cols - 1) : 0; break;
  case SB_PAGERIGHT: h += v->cols > 1 ? v->cols - 1 : 1; break;
  case SB_LEFT: h = 0; break;
  case SB_RIGHT: h = v->max_cells > v->cols ? v->max_cells - v->cols + 1 : 0; break;
  case SB_THUMBTRACK:
  case SB_THUMBPOSITION:
    si.cbSize = sizeof(si); si.fMask = SIF_TRACKPOS | SIF_PAGE;
    if (!GetScrollInfo(v->w, SB_HORZ, &si)) return;
    if (v->hs_scaled) {
      u32 den = 32767 - si.nPage + 1;
      h = (u32)((u64)(u32)si.nTrackPos * (v->hs_total - v->cols) / (den ? den : 1));
    } else h = (u32)si.nTrackPos;
    break;
  default: return;
  }
  tv_set_hscroll(v, h);
  caret_update(v);
}

static LRESULT CALLBACK tv_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
  TV* v = tv_get(w);
  switch (msg) {
  case WM_NCCREATE: {
    CREATESTRUCTA* cs = (CREATESTRUCTA*)lp;
    v = new TV();
    if (!v) return FALSE;
    v->w = w;
    v->panel = cs->lpCreateParams ? 1 : 0;
    SetWindowLongA(w, GWL_USERDATA, (LONG)v);
    g_tvs.push(v);
    break;
  }
  case WM_NCDESTROY:
    if (v) {
      u32 i;
      if (v->d) v->d->remove_listener(v);
      for (i = 0; i < g_tvs.size(); i++) if (g_tvs[i] == v) { g_tvs.erase(i, 1); break; }
      SetWindowLongA(w, GWL_USERDATA, 0);
      delete v;
    }
    break;
  case WM_CREATE:
    tv_metrics(v);
    return 0;
  case WM_ERASEBKGND:
    return 1;
  case WM_PAINT:
    if (v) { tv_paint(v); return 0; }
    break;
  case WM_SIZE:
    if (v) {
      tv_metrics(v);
      if (v->t && v->d) { clamp_top(v); update_scrollbars(v); }
      InvalidateRect(w, 0, FALSE);
      caret_update(v);
    }
    return 0;
  case WM_SETFOCUS:
    if (v) { v->focus = 1; v->caret_w = 0; caret_update(v); }
    return 0;
  case WM_KILLFOCUS:
    if (v) { v->focus = 0; DestroyCaret(); v->caret_on = 0; v->caret_w = 0; v->lead = 0; }
    return 0;
  case WM_GETDLGCODE:
    return DLGC_WANTALLKEYS | DLGC_WANTARROWS | DLGC_WANTCHARS | DLGC_WANTTAB;
  case WM_KEYDOWN:
    if (v) tv_key(v, (UINT)wp);
    return 0;
  case WM_CHAR:
    if (v) tv_char(v, wp);
    return 0;
  case WM_IME_CHAR:
    if (v) tv_ime_char(v, wp);
    return 0;
  case WM_IME_STARTCOMPOSITION:
    if (v) { v->ime_on = 1; caret_update(v); }
    break;
  case WM_IME_ENDCOMPOSITION:
    if (v) v->ime_on = 0;
    break;
  case WM_LBUTTONDOWN:
    if (v) tv_lbutton(v, (short)LOWORD(lp), (short)HIWORD(lp));
    return 0;
  case WM_MOUSEMOVE:
    if (v) tv_mousemove(v, (short)LOWORD(lp), (short)HIWORD(lp));
    return 0;
  case WM_LBUTTONUP:
    if (v) {
      if (v->drag_timer) { KillTimer(w, TVT_DRAG); v->drag_timer = 0; }
      v->drag = DRAG_NONE;
      if (GetCapture() == w) ReleaseCapture();
    }
    return 0;
  case WM_CAPTURECHANGED:
    if (v) { v->drag = DRAG_NONE; if (v->drag_timer) { KillTimer(w, TVT_DRAG); v->drag_timer = 0; } }
    return 0;
  case WM_RBUTTONDOWN:
    if (v && v->t && v->d && !v->d->indexing()) {
      int aff;
      epos p = hit_pos(v, (short)LOWORD(lp), (short)HIWORD(lp), &aff);
      SetFocus(w);
      if (p < sel_lo(v->t->sel) || p > sel_hi(v->t->sel) || sel_empty(v->t->sel)) {
        v->affinity = aff;
        tv_move(v, p, 0, 0);
      }
    }
    return 0;
  case WM_CONTEXTMENU:
    if (v) tv_context_menu(v, (short)LOWORD(lp), (short)HIWORD(lp));
    return 0;
  case WM_MOUSEWHEEL:
    if (v) tv_wheel(v, (short)HIWORD(wp));
    return 0;
  case WM_VSCROLL:
    if (v) tv_vscroll(v, LOWORD(wp));
    return 0;
  case WM_HSCROLL:
    if (v) tv_hscroll(v, LOWORD(wp));
    return 0;
  case WM_TIMER:
    if (!v) return 0;
    if (wp == TVT_DRAG) tv_drag_timer(v);
    else if (wp == TVT_LEX) {
      int more = 0;
      if (v->t && v->t->hl && v->t->hl->enabled()) {
        DWORD start = GetTickCount();
        do { more = v->t->hl->work(200); } while (more && GetTickCount() - start < 15);
        tv_take_dirty(v);
      }
      if (!more) { KillTimer(w, TVT_LEX); v->lex_timer = 0; }
    }
    return 0;
  case WM_SETCURSOR:
    if (v && LOWORD(lp) == HTCLIENT) {
      POINT pt;
      GetCursorPos(&pt);
      ScreenToClient(w, &pt);
      SetCursor(LoadCursorA(0, (LPCSTR)(pt.x < v->text_left - 2 ? IDC_ARROW : IDC_IBEAM)));
      return TRUE;
    }
    break;
  case WM_SYSCOLORCHANGE:
    render_refresh_colors();
    InvalidateRect(w, 0, FALSE);
    break;
  case WM_TV_NOTIFY:
    if (v) tv_on_notify(v);
    return 0;
  default:
    break;
  }
  return DefWindowProcA(w, msg, wp, lp);
}

// ---------------------------------------------------------------------------------------------------------------
// API (views.h)

HWND textview_create(HWND parent, int id, int is_panel)
{
  static int registered = 0;
  if (!registered) {
    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = tv_proc;
    wc.hInstance = g_app.hinst;
    wc.hCursor = 0;
    wc.hbrBackground = 0;
    wc.lpszClassName = k_tv_class;
    if (!RegisterClassA(&wc)) return 0;
    registered = 1;
  }
  if (!render_fonts()->normal) render_init_from_config();
  return CreateWindowExA(WS_EX_CLIENTEDGE, k_tv_class, "", WS_CHILD | WS_VSCROLL | WS_HSCROLL | WS_CLIPSIBLINGS |
                         (is_panel ? WS_VISIBLE : 0), 0, 0, 100, 100, parent, (HMENU)id, g_app.hinst,
                         (LPVOID)(is_panel ? 1 : 0));
}

void textview_attach(HWND w, Tab* t)
{
  TV* v = tv_get(w);
  if (!v) return;
  if (v->t == t && t) { textview_invalidate(w); return; }
  if (v->d) v->d->remove_listener(v);
  v->t = t; v->d = t ? t->doc : 0;
  tv_reset_caches(v);
  v->max_cells = 0; v->br_a = v->br_b = EPOS_NONE; v->br_bad = 0; v->lead = 0; v->affinity = 0; v->drag = DRAG_NONE;
  v->nflags = 0;
  if (v->lex_timer) { KillTimer(w, TVT_LEX); v->lex_timer = 0; }
  if (v->d) { v->d->add_listener(v); v->last_mod = v->d->modified() ? 1 : 0; }
  if (!v->panel) ShowWindow(w, t ? SW_SHOW : SW_HIDE);
  tv_metrics(v);
  if (t && v->d) {
    sel_clamp(v);
    if (tv_wrap(v)) t->tv.hscroll = 0;
    clamp_top(v);
    update_scrollbars(v);
    tv_notify(v, NF_CARET);
  }
  InvalidateRect(w, 0, FALSE);
  caret_update(v);
}

void textview_set_selection(HWND w, epos anchor, epos caret, int scroll_mode)
{
  TV* v = tv_get(w);
  Selection old;
  if (!v || !v->t || !v->d) return;
  old = v->t->sel;
  v->t->sel.anchor = anchor; v->t->sel.caret = caret;
  sel_clamp(v);
  v->t->tv.desired_cell = NODESIRED;
  v->affinity = 0;
  inval_sel_change(v, old, v->t->sel);
  if (old.caret != v->t->sel.caret) { inval_range(v, old.caret, old.caret); }
  if (scroll_mode == SCROLL_CENTER) {
    int x, y;
    if (!pos_to_xy(v, v->t->sel.caret, &x, &y) || y + v->ch > v->rows_full * v->ch) ensure_visible_pos(v, v->t->sel.caret, 1);
    else ensure_visible_pos(v, v->t->sel.caret, 0);
  } else if (scroll_mode == SCROLL_MIN) ensure_visible_pos(v, v->t->sel.caret, 0);
  caret_update(v);
  tv_notify(v, NF_CARET);
}

void textview_ensure_visible(HWND w, epos pos, int center)
{
  TV* v = tv_get(w);
  if (!v || !v->t || !v->d) return;
  ensure_visible_pos(v, pos, center);
  caret_update(v);
}

void textview_invalidate(HWND w)
{
  TV* v = tv_get(w);
  if (!v) return;
  tv_reset_caches(v);
  tv_metrics(v);
  if (v->t && v->d) {
    sel_clamp(v);
    if (tv_wrap(v)) v->t->tv.hscroll = 0;
    clamp_top(v);
    update_scrollbars(v);
    tv_notify(v, NF_CARET);
  }
  v->caret_w = 0;
  InvalidateRect(w, 0, FALSE);
  caret_update(v);
}

void textview_config_changed(HWND w)
{
  u32 i;
  (void)w;
  render_init_from_config();
  for (i = 0; i < g_tvs.size(); i++) textview_invalidate(g_tvs[i]->w);
}

void textview_caret_info(HWND w, CaretInfo* out)
{
  TV* v = tv_get(w);
  memset(out, 0, sizeof(*out));
  out->view = VIEW_TEXT;
  if (!v || !v->t || !v->d) return;
  {
    Document* d = v->d;
    Selection s = v->t->sel;
    epos lo = sel_lo(s), hi = sel_hi(s);
    out->pos = s.caret;
    out->overwrite = v->t->tv.overwrite;
    out->sel_len = hi - lo;
    if (d->indexing()) return;
    {
      u32 line = tv_line_of(v, s.caret);
      LineInfo li;
      seg_line(v, line, s.caret, 0, 0);
      out->line = line;
      out->col = v->lay.cell_of_offset(s.caret - v->sg_pos) + 1;
      lc_get(v, line, &li);
      if (s.caret < li.end) { ecp c = 0; if (d->decode(s.caret, &c)) { out->ch = c; out->has_ch = 1; } }
    }
    if (hi > lo) {
      u32 l1 = tv_line_of(v, lo), l2 = tv_line_of(v, hi);
      if (l2 > l1 && hi == d->line_start(l2)) l2--;
      out->sel_lines = l2 - l1 + 1;
    }
  }
}

int textview_pos_rect(HWND w, epos pos, RECT* screen)
{
  TV* v = tv_get(w);
  u32 line, sub, cell, rc0, ci, next_cell;
  int r, aff, x;
  POINT p;
  if (!screen) return 0;
  SetRectEmpty(screen);
  if (!v || !v->t || !v->d || v->d->indexing() || !v->ch || !v->cw) return 0;
  if (pos < v->d->text_origin()) pos = v->d->text_origin();
  if (pos > v->d->length()) pos = v->d->length();
  pos = v->d->snap_char(pos);
  rmap_build(v);
  aff = v->affinity;
  v->affinity = 0;                                 // a match start sits at the start of its wrap row
  pos_rowcell(v, pos, &line, &sub, &cell, &rc0);
  v->affinity = aff;
  r = rmap_find(v, line, sub);
  if (r < 0) return 0;
  next_cell = cell + 1;                            // cell width of the character (2 for wide ones)
  if (pos >= v->sg_pos) {
    ci = v->lay.char_at_offset(pos - v->sg_pos);
    if (ci < v->lay.count() && v->lay.chars()[ci].cell == cell && v->lay.chars()[ci].width > 1)
      next_cell = cell + v->lay.chars()[ci].width;
  }
  x = v->text_left + ((int)cell - (int)tv_left_cell(v, rc0)) * v->cw;
  p.x = x; p.y = r * v->ch;
  ClientToScreen(w, &p);
  screen->left = p.x; screen->top = p.y;
  screen->right = p.x + (int)(next_cell - cell) * v->cw;
  screen->bottom = p.y + v->ch;
  return 1;
}

void textview_set_panel_handler(HWND w, PanelHandler* h)
{
  TV* v = tv_get(w);
  if (v) v->ph = h;
}

// ---------------------------------------------------------------------------------------------------------------
// Commands: View > Word Wrap / Show Whitespace / Line Numbers

void cmd_view_wrap(void)
{
  Tab* t = app_active();
  TV* v = tv_get(g_app.textview);
  epos caret;
  if (!t) return;
  t->tv.wrap = (u8)!t->tv.wrap;
  t->tv.top_subrow = 0;
  if (t->tv.wrap) t->tv.hscroll = 0;
  if (v && v->t == t) {
    caret = t->sel.caret;
    textview_invalidate(g_app.textview);
    ensure_visible_pos(v, caret, 0);
    caret_update(v);
  }
  sessmgr_state_changed();
}

void cmd_view_showws(void)
{
  u32 i;
  g_app.cfg.show_ws = (u8)!g_app.cfg.show_ws;
  for (i = 0; i < g_tvs.size(); i++) if (!g_tvs[i]->panel) textview_invalidate(g_tvs[i]->w);
}

void cmd_view_linenum(void)
{
  u32 i;
  g_app.cfg.line_numbers = (u8)!g_app.cfg.line_numbers;
  for (i = 0; i < g_tvs.size(); i++) if (!g_tvs[i]->panel) textview_invalidate(g_tvs[i]->w);
}

UINT cmdstate_view_wrap(void)
{
  Tab* t = app_active();
  if (!t) return MF_GRAYED;
  return t->tv.wrap ? MF_CHECKED : 0;
}
UINT cmdstate_view_showws(void) { return g_app.cfg.show_ws ? MF_CHECKED : 0; }
UINT cmdstate_view_linenum(void) { return g_app.cfg.line_numbers ? MF_CHECKED : 0; }

// ---------------------------------------------------------------------------------------------------------------
// Selftest (st_textview)

// SELFTEST
// Tabs are built directly from bytes (no docio), shown in the main view, then closed. Timings use QPC with the
// window visible; a GetPixel on the view's DC waits until the display driver has drawn.
// Demo mode for screenshots: if <cfg dir>\tvshow.txt exists when st_textview starts, the test ends by showing scenes
// chosen by the file's first character (1 C file, 2 wrap, 3 whitespace/EOLs, 4 UTF-8 mix, 5 Fixedsys, 6 selection
// and marks; q = quit, max 10 minutes).

class StTvLangs : public LangSource {
public:
  u32 count() { return IDR_LANG_COUNT; }
  int get(u32 i, const char** name, const char** text, u32* len)
  {
    static const char* const names[IDR_LANG_COUNT] = { "cpp.lng", "rc.lng", "make.lng", "bat.lng", "ini.lng",
      "asm.lng", "python.lng", "html.lng", "js.lng", "css.lng", "xml.lng", "md.lng", "diff.lng" };
    HRSRC r = FindResourceA(g_app.hinst, MAKEINTRESOURCEA(IDR_LANG_FIRST + i), (LPCSTR)RT_RCDATA);
    HGLOBAL h;
    if (!r || i >= IDR_LANG_COUNT) return E_NOTFOUND;
    h = LoadResource(g_app.hinst, r);
    if (!h) return E_NOTFOUND;
    *text = (const char*)LockResource(h);
    *len = SizeofResource(g_app.hinst, r);
    *name = names[i];
    return E_OK;
  }
};
static StTvLangs g_st_langs;

static double st_tv_ms(LARGE_INTEGER a, LARGE_INTEGER b)
{
  LARGE_INTEGER f;
  QueryPerformanceFrequency(&f);
  return (double)(i64)(b.QuadPart - a.QuadPart) * 1000.0 / (double)(i64)f.QuadPart;
}

static void st_tv_sync(HWND w)
{
  HDC dc = GetDC(w);
  GetPixel(dc, 1, 1);
  ReleaseDC(w, dc);
}

static double st_tv_median(double* x, u32 n)
{
  u32 i, j;
  for (i = 1; i < n; i++) for (j = i; j > 0 && x[j - 1] > x[j]; j--) { double t = x[j]; x[j] = x[j - 1]; x[j - 1] = t; }
  return n ? x[n / 2] : 0.0;
}

static void st_tv_fmt_ms(char* out, u32 cap, double ms)
{
  i32 us = (i32)(ms * 1000.0 + 0.5);
  es_fmt(out, cap, "%d.%03d ms", us / 1000, us % 1000);
}

static Tab* st_tv_make_src(StCtx* c, ByteSource* src, const EncSpec& enc, epos bom, EolStyle eol, const char* lang)
{
  Document* d = new Document();
  Tab* t;
  int rc;
  if (!d) { delete src; st_checkf(c, 0, "st_tv_make: no memory"); return 0; }
  rc = d->init_source(src, enc, bom, eol, 0);
  if (rc != E_OK) { st_checkf(c, 0, "st_tv_make: init_source %s", e_errname(rc)); delete d; return 0; }
  t = tab_new(d);
  if (!t) { delete d; st_checkf(c, 0, "st_tv_make: tab_new failed"); return 0; }
  t->untitled = 1;
  t->untitled_no = 900 + t->id;
  if (lang) {
    t->lang = lang_find(lang);
    if (t->lang) t->hl->attach(&t->lines, t->lang);
  }
  if (app_add_tab(t, 1) != E_OK) { tab_free(t); return 0; }
  if (g_app.textview) {
    if (t->view != VIEW_TEXT) { t->view = VIEW_TEXT; app_activate(app_tab_index(t)); }
    SetFocus(g_app.textview);
  }
  st_pump(c, 30);
  return t;
}

static Tab* st_tv_make(StCtx* c, const char* bytes, u32 n, const EncSpec& enc, epos bom, EolStyle eol, const char* lang)
{
  MemSource* s = new MemSource();
  if (!s) return 0;
  s->copy_from((const u8*)bytes, n);
  return st_tv_make_src(c, s, enc, bom, eol, lang);
}

static void st_tv_close(StCtx* c, Tab* t)
{
  if (!t) return;
  app_remove_tab(t);
  tab_free(t);
  st_pump(c, 10);
}

static int st_tv_bytes_eq(Document* d, const char* want, u32 n)
{
  EBytes b;
  if (d->length() != n) return 0;
  if (b.resize(n) != E_OK || d->read(0, b.data(), n) != E_OK) return 0;
  return memcmp(b.data(), want, n) == 0;
}

static void st_tv_click(StCtx* c, HWND w, int x, int y)
{
  PostMessageA(w, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
  PostMessageA(w, WM_LBUTTONUP, 0, MAKELPARAM(x, y));
  st_pump(c, 0);
}

// deterministic C-like text (like the probe: ~43 chars/line, several colour runs)
static void st_tv_gen_c(EBytes* out, u32 lines)
{
  static const char* const pat[] = {
    "    if (count > 0 && value != NULL) {  /* check */\r\n",
    "        int i = 42; char* s = \"hello world\";\r\n",
    "#include <windows.h>\r\n",
    "    for (i = 0; i < n; i++) total += table[i] * 3;\r\n",
    "    return result;   // done\r\n",
    "static unsigned long hash(const char* p, int len)\r\n",
    "    }\r\n",
    "\r\n",
  };
  u32 i;
  for (i = 0; i < lines; i++) {
    const char* p = pat[(i * 7 + i / 3) % 8];
    out->append(p, es_len(p));
  }
}

static void st_tv_demo(StCtx* c);

void st_textview(StCtx* c)
{
  HWND w = g_app.textview;
  TV* v = tv_get(w);
  Tab* t;
  CaretInfo ci;
  Document* d;
  char buf[200];
  if (!w || !v) { st_skip(c, "no text view"); return; }
  if (lang_count() == 0) { EStr err; lang_init(&g_st_langs, 0, &err); }
  g_app.cfg.auto_indent = 1;

  // ---- 1. caret, typing, selection, undo (ANSI, CRLF, C++ colouring)
  {
    static const char src[] = "int main(void)\r\n{\r\n  return 0;\r\n}\r\n";
    u32 n = sizeof(src) - 1;
    t = st_tv_make(c, src, n, enc_ansi(), 0, EOL_CRLF, "cpp");
    if (!t) return;
    d = t->doc;
    ST_CHECK(c, v->t == t, "view attached to the new tab");
    ST_CHECK(c, IsWindowVisible(w), "text view visible");
    ST_CHECK(c, GetFocus() == w, "text view has focus");
    textview_caret_info(w, &ci);
    st_checkf(c, ci.line == 0 && ci.col == 1 && ci.pos == 0, "start caret line %u col %u pos %u", ci.line, ci.col, ci.pos);
    st_keys(c, "^{END}");
    textview_caret_info(w, &ci);
    st_checkf(c, ci.line == 4 && ci.col == 1 && ci.pos == n, "Ctrl+End: line %u col %u pos %u", ci.line, ci.col, ci.pos);
    st_keys(c, "abc");
    ST_CHECK(c, st_tv_bytes_eq(d, "int main(void)\r\n{\r\n  return 0;\r\n}\r\nabc", n + 3), "typed abc at the end");
    textview_caret_info(w, &ci);
    st_checkf(c, ci.col == 4, "caret col after typing %u", ci.col);
    st_keys(c, "{HOME}+{END}");
    textview_caret_info(w, &ci);
    st_checkf(c, ci.sel_len == 3 && ci.sel_lines == 1, "Home, Shift+End: sel %u bytes %u lines", ci.sel_len, ci.sel_lines);
    st_keys(c, "X");
    ST_CHECK(c, st_tv_bytes_eq(d, "int main(void)\r\n{\r\n  return 0;\r\n}\r\nX", n + 1), "typing replaces the selection");
    st_keys(c, "{BS}{BS}");
    ST_CHECK(c, st_tv_bytes_eq(d, src, n - 2), "Backspace removed X, then the whole CRLF");
    st_keys(c, "{ENTER}");
    ST_CHECK(c, st_tv_bytes_eq(d, src, n), "Enter restores the CRLF");
    textview_set_selection(w, 0, 0, SCROLL_MIN);
    st_keys(c, "^{RIGHT}");
    st_checkf(c, t->sel.caret == 4, "Ctrl+Right -> %u", t->sel.caret);
    st_keys(c, "^+{RIGHT}");
    st_checkf(c, t->sel.anchor == 4 && t->sel.caret == 8, "Ctrl+Shift+Right selects 'main' (%u..%u)", t->sel.anchor, t->sel.caret);
    st_keys(c, "{RIGHT}");
    st_checkf(c, sel_empty(t->sel) && t->sel.caret == 8, "Right collapses to the selection end (%u)", t->sel.caret);
    textview_set_selection(w, d->line_end(2), d->line_end(2), SCROLL_MIN);   // after "  return 0;"
    st_keys(c, "{ENTER}x");
    ST_CHECK(c, st_tv_bytes_eq(d, "int main(void)\r\n{\r\n  return 0;\r\n  x\r\n}\r\n", n + 5), "Enter copies the indent");
    textview_caret_info(w, &ci);
    st_checkf(c, ci.line == 3 && ci.col == 4, "caret after auto-indent line %u col %u", ci.line, ci.col);
    st_keys(c, "{UP}");
    textview_caret_info(w, &ci);
    st_checkf(c, ci.line == 2 && ci.col == 4, "Up keeps the column: line %u col %u", ci.line, ci.col);
    st_keys(c, "{UP}{UP}");
    textview_caret_info(w, &ci);
    st_checkf(c, ci.line == 0 && ci.col == 4, "Up over a short line keeps the desired column: line %u col %u", ci.line, ci.col);
    st_keys(c, "{DOWN}");
    textview_caret_info(w, &ci);
    st_checkf(c, ci.line == 1 && ci.col == 2, "Down to '{' clamps to its end: col %u", ci.col);
    st_keys(c, "{DOWN}");
    textview_caret_info(w, &ci);
    st_checkf(c, ci.line == 2 && ci.col == 4, "desired column restored: line %u col %u", ci.line, ci.col);
    {   // undo through the document (the Edit menu is T12's) and the view follows the selection
      Selection s;
      int rc = d->undo(&s);
      ST_CHECK(c, rc == E_OK, "undo");
      textview_set_selection(w, s.anchor, s.caret, SCROLL_MIN);
      ST_CHECK(c, st_tv_bytes_eq(d, "int main(void)\r\n{\r\n  return 0;\r\n  \r\n}\r\n", n + 4), "undo removed 'x'");
      d->undo(&s);
      textview_set_selection(w, s.anchor, s.caret, SCROLL_MIN);
      ST_CHECK(c, st_tv_bytes_eq(d, src, n), "undo removed the new line");
      st_checkf(c, t->sel.caret == d->line_end(2), "undo selection at the old caret (%u)", t->sel.caret);
    }
    textview_set_selection(w, 0, 0, SCROLL_MIN);
    st_keys(c, "{INS}Z{INS}");
    ST_CHECK(c, d->length() == n && st_tv_bytes_eq(d, "Znt main(void)\r\n{\r\n  return 0;\r\n}\r\n", n), "overwrite mode");
    ST_CHECK(c, t->tv.overwrite == 0, "Ins toggled back");
    textview_set_selection(w, 8, 8, SCROLL_MIN);
    st_keys(c, "^{BS}");
    ST_CHECK(c, st_tv_bytes_eq(d, "Znt (void)\r\n{\r\n  return 0;\r\n}\r\n", n - 4), "Ctrl+Backspace deletes the word");
    st_keys(c, "{END}{DEL}");
    ST_CHECK(c, st_tv_bytes_eq(d, "Znt (void){\r\n  return 0;\r\n}\r\n", n - 6), "Del at the line end joins lines");
    // mouse: click on row 1, after 3 cells; double-click a word; triple-click the line; gutter click
    {
      int x = v->text_left + 3 * v->cw + 1, y = v->ch + v->ch / 2;
      Selection s;
      st_tv_click(c, w, x, y);
      st_checkf(c, sel_empty(t->sel) && t->sel.caret == d->line_start(1) + 3, "click -> %u (want %u)", t->sel.caret,
                d->line_start(1) + 3);
      st_tv_click(c, w, x, y);
      s = t->sel;
      st_checkf(c, sel_lo(s) == d->line_start(1) + 2 && sel_hi(s) == d->line_start(1) + 8, "double-click selects 'return' (%u..%u)",
                sel_lo(s), sel_hi(s));
      st_tv_click(c, w, x, y);
      s = t->sel;
      st_checkf(c, sel_lo(s) == d->line_start(1) && sel_hi(s) == d->line_start(2), "triple-click selects the line (%u..%u)",
                sel_lo(s), sel_hi(s));
      Sleep(GetDoubleClickTime() + 50);
      st_tv_click(c, w, v->cw + 2, y);                 // line number area
      s = t->sel;
      st_checkf(c, sel_lo(s) == d->line_start(1) && sel_hi(s) == d->line_start(2), "gutter click selects the line");
      st_tv_click(c, w, 2, y);                          // marker cell: bookmark
      ST_CHECK(c, t->bookmarks.has(1), "marker click sets a bookmark");
      Sleep(GetDoubleClickTime() + 50);
      st_tv_click(c, w, 2, y);
      ST_CHECK(c, !t->bookmarks.has(1), "second marker click clears it");
    }
    // show whitespace and line numbers toggles repaint without trouble
    cmd_view_showws();
    st_pump(c, 20);
    ST_CHECK(c, cmdstate_view_showws() == MF_CHECKED, "show whitespace on");
    cmd_view_showws();
    cmd_view_linenum();
    st_pump(c, 20);
    ST_CHECK(c, v->gut_px == v->cw, "line numbers off: marker cell only");
    cmd_view_linenum();
    st_pump(c, 20);
    ST_CHECK(c, v->gut_px == (int)(v->digits + 2) * v->cw, "line numbers on");
    st_tv_close(c, t);
  }

  // ---- 2. word wrap: End goes to the row end first, rows follow the window width
  {
    EBytes b;
    u32 i;
    for (i = 0; i < 60; i++) b.append("word ", 5);
    b.append("\r\nnext\r\n", 8);
    t = st_tv_make(c, (const char*)b.data(), b.size(), enc_ansi(), 0, EOL_CRLF, 0);
    if (t) {
      d = t->doc;
      cmd_view_wrap();
      st_pump(c, 20);
      ST_CHECK(c, t->tv.wrap == 1 && cmdstate_view_wrap() == MF_CHECKED, "wrap on");
      ST_CHECK(c, line_rows(v, 0) >= 2, "300-char line wraps into several rows");
      textview_set_selection(w, 0, 0, SCROLL_MIN);
      st_keys(c, "{END}");
      st_checkf(c, t->sel.caret > 0 && t->sel.caret < 300 && t->sel.caret % 5 == 0, "End in wrap mode -> row end %u",
                t->sel.caret);
      st_keys(c, "{END}");
      st_checkf(c, t->sel.caret == 300, "second End -> line end %u", t->sel.caret);
      st_keys(c, "{DOWN}");
      textview_caret_info(w, &ci);
      st_checkf(c, ci.line == 1, "Down from the last row of a wrapped line reaches the next line (%u)", ci.line);
      st_keys(c, "^{HOME}{DOWN}");
      textview_caret_info(w, &ci);
      st_checkf(c, ci.line == 0 && t->sel.caret > 0, "Down inside a wrapped line stays on it (pos %u)", t->sel.caret);
      cmd_view_wrap();
      st_pump(c, 20);
      ST_CHECK(c, t->tv.wrap == 0, "wrap off");
      st_tv_close(c, t);
    }
  }

  // ---- 3. encodings: unrepresentable typing, bad bytes, UTF-8 caret steps
  {
    t = st_tv_make(c, "abc", 3, enc_oem(), 0, EOL_CRLF, 0);
    if (t) {
      textview_set_selection(w, 3, 3, SCROLL_MIN);
      PostMessageA(w, WM_CHAR, 0x80, 0);                // euro sign in cp1252: not in OEM 437
      st_pump(c, 20);
      ST_CHECK(c, st_tv_bytes_eq(t->doc, "abc", 3), "unrepresentable character refused");
      PostMessageA(w, WM_CHAR, 0xE9, 0);                // e acute: in cp437 (0x82)
      st_pump(c, 20);
      ST_CHECK(c, st_tv_bytes_eq(t->doc, "abc\x82", 4), "e-acute typed as OEM 0x82");
      st_tv_close(c, t);
    }
    {
      static const char u8s[] = "\xD0\x96\xD0\xB8 \xC5\x81\xC3\xB3\xC5\xBA \xFF \xE4\xB8\xAD x";   // Zhi, Lodz, bad, CJK
      t = st_tv_make(c, u8s, sizeof(u8s) - 1, enc_utf8(0), 0, EOL_LF, 0);
      if (t) {
        textview_set_selection(w, 0, 0, SCROLL_MIN);
        st_keys(c, "{RIGHT}");
        st_checkf(c, t->sel.caret == 2, "Right over a 2-byte char -> %u", t->sel.caret);
        textview_set_selection(w, 12, 12, SCROLL_MIN);
        textview_caret_info(w, &ci);
        st_checkf(c, ci.has_ch && ci.ch == ECP_BADBYTE(0xFF), "caret on the bad byte: ch %X", ci.ch);
        st_checkf(c, ci.col == 8, "bad byte column %u", ci.col);
        st_keys(c, "{RIGHT}");
        st_checkf(c, t->sel.caret == 13, "Right over a bad byte -> %u", t->sel.caret);
        st_keys(c, "{RIGHT}{RIGHT}");
        textview_caret_info(w, &ci);
        st_checkf(c, t->sel.caret == 17 && ci.col == 14, "after the wide char: pos %u col %u", t->sel.caret, ci.col);
        st_tv_close(c, t);
      }
    }
  }

  // ---- 4. timings (window visible; meaningful only without VNC)
  {
    EBytes b;
    double rep[30], key[30], keyp[30];
    u32 i;
    LARGE_INTEGER a0, a1;
    char s1[32], s2[32], s3[32];
    st_tv_gen_c(&b, 3000);
    t = st_tv_make(c, (const char*)b.data(), b.size(), enc_ansi(), 0, EOL_CRLF, "cpp");
    if (t) {
      d = t->doc;
      textview_set_selection(w, d->line_start(1500), d->line_start(1500), SCROLL_CENTER);
      st_pump(c, 50);
      for (i = 0; i < 30; i++) {
        InvalidateRect(w, 0, FALSE);
        QueryPerformanceCounter(&a0);
        UpdateWindow(w);
        st_tv_sync(w);
        QueryPerformanceCounter(&a1);
        rep[i] = st_tv_ms(a0, a1);
      }
      for (i = 0; i < 30; i++) {
        MSG m;
        st_pump(c, 0);
        QueryPerformanceCounter(&a0);
        SendMessageA(w, WM_CHAR, 'x', 0);
        UpdateWindow(w);
        st_tv_sync(w);
        QueryPerformanceCounter(&a1);
        key[i] = st_tv_ms(a0, a1);
        while (PeekMessageA(&m, 0, 0, 0, PM_REMOVE)) app_dispatch(&m);   // caret/status notifications
        UpdateWindow(g_app.frame);
        st_tv_sync(w);
        QueryPerformanceCounter(&a1);
        keyp[i] = st_tv_ms(a0, a1);
      }
      st_tv_fmt_ms(s1, sizeof(s1), st_tv_median(rep, 30));
      st_tv_fmt_ms(s2, sizeof(s2), st_tv_median(key, 30));
      st_tv_fmt_ms(s3, sizeof(s3), st_tv_median(keyp, 30));
      render_font_desc(buf, sizeof(buf));
      selftest_log("textview timing (%s, %d rows): full repaint %s, keystroke %s (with status/notify %s)", buf,
                   v->rows_part, s1, s2, s3);
      st_checkf(c, st_tv_median(rep, 30) < 60.0, "full repaint median %s", s1);
      {   // scrolling: one line (ScrollWindowEx) and one page
        double sl[20], pg[20];
        for (i = 0; i < 20; i++) {
          QueryPerformanceCounter(&a0);
          SendMessageA(w, WM_VSCROLL, i & 1 ? SB_LINEUP : SB_LINEDOWN, 0);
          st_tv_sync(w);
          QueryPerformanceCounter(&a1);
          sl[i] = st_tv_ms(a0, a1);
        }
        for (i = 0; i < 20; i++) {
          QueryPerformanceCounter(&a0);
          SendMessageA(w, WM_VSCROLL, i & 1 ? SB_PAGEUP : SB_PAGEDOWN, 0);
          st_tv_sync(w);
          QueryPerformanceCounter(&a1);
          pg[i] = st_tv_ms(a0, a1);
        }
        st_tv_fmt_ms(s1, sizeof(s1), st_tv_median(sl, 20));
        st_tv_fmt_ms(s2, sizeof(s2), st_tv_median(pg, 20));
        selftest_log("textview timing: line scroll %s, page scroll %s", s1, s2);
      }
      {   // line lookups through the piece tree (no line-start cache yet)
        u32 nl = d->line_count(), k, seed = 12345;
        epos sum = 0;
        QueryPerformanceCounter(&a0);
        for (k = 0; k < 1000; k++) { seed = seed * 1103515245u + 12345u; sum += d->line_start((seed >> 8) % nl); }
        QueryPerformanceCounter(&a1);
        st_tv_fmt_ms(s1, sizeof(s1), st_tv_ms(a0, a1));
        QueryPerformanceCounter(&a0);
        for (k = 0; k < 1000; k++) { seed = seed * 1103515245u + 12345u; sum += d->line_of((seed >> 8) % d->length()); }
        QueryPerformanceCounter(&a1);
        st_tv_fmt_ms(s2, sizeof(s2), st_tv_ms(a0, a1));
        selftest_log("textview timing: 1000 Document::line_start %s, 1000 line_of %s (%u lines, %u bytes, sum %u)", s1,
                     s2, nl, d->length(), sum & 1);
      }
      st_tv_close(c, t);
    }
  }

  // ---- 5. a 5 MB single line: End, horizontal scrolling, Home
  {
    epos n = 5u * 1024u * 1024u, i;
    u8* p = (u8*)e_valloc(n);
    MemSource* s = new MemSource();
    LARGE_INTEGER a0, a1;
    char s1[32];
    if (p && s) {
      for (i = 0; i < n; i++) p[i] = (u8)(i % 11 == 10 ? ' ' : 'a' + i % 11);
      s->adopt(p, n, 1);
      t = st_tv_make_src(c, s, enc_ansi(), 0, EOL_CRLF, 0);
      if (t) {
        QueryPerformanceCounter(&a0);
        SendMessageA(w, WM_KEYDOWN, VK_END, 0);
        UpdateWindow(w);
        st_tv_sync(w);
        QueryPerformanceCounter(&a1);
        st_tv_fmt_ms(s1, sizeof(s1), st_tv_ms(a0, a1));
        selftest_log("textview timing: End on a 5 MB line %s", s1);
        textview_caret_info(w, &ci);
        st_checkf(c, t->sel.caret == n && ci.col == n + 1, "End on the 5 MB line: pos %u col %u", t->sel.caret, ci.col);
        st_checkf(c, t->tv.hscroll > n - 200, "scrolled to the end (hscroll %u)", t->tv.hscroll);
        SendMessageA(w, WM_HSCROLL, SB_PAGELEFT, 0);
        st_pump(c, 10);
        st_checkf(c, t->tv.hscroll < n - v->cols, "page left (hscroll %u)", t->tv.hscroll);
        QueryPerformanceCounter(&a0);
        SendMessageA(w, WM_KEYDOWN, VK_HOME, 0);
        UpdateWindow(w);
        QueryPerformanceCounter(&a1);
        st_checkf(c, t->sel.caret == 0 && t->tv.hscroll == 0, "Home back to the start (%u, %u)", t->sel.caret, t->tv.hscroll);
        st_tv_fmt_ms(s1, sizeof(s1), st_tv_ms(a0, a1));
        selftest_log("textview timing: Home on the 5 MB line %s", s1);
        st_tv_close(c, t);
      }
    } else {
      if (p) e_vfree(p, n);
      delete s;
      st_checkf(c, 0, "5 MB allocation failed");
    }
  }

  {
    char path[300];
    ui_path_join(c->cfg_dir, "tvshow.txt", path, sizeof(path));
    if (ui_file_exists(path)) st_tv_demo(c);
  }
}

// ---- demo scenes for screenshots ----

static Tab* st_tv_demo_tab(StCtx* c, int scene)
{
  EBytes b;
  switch (scene) {
  case 1: case 2: case 5: case 6:
    if (file_read_all(fs_os(), "M:\\dev\\editor\\src\\core\\edtype.cpp", &b, 1u << 20) != E_OK) { b.clear(); st_tv_gen_c(&b, 300); }
    return st_tv_make(c, (const char*)b.data(), b.size(), enc_ansi(), 0, EOL_LF, "cpp");
  case 3: {
    static const char ws[] = "\tTabbed line with  two spaces\r\nUnix line\nOld Mac\rline with a lone CR\r\n"
                             "  trailing spaces   \r\n\ttab\tand\ttabs\r\nctrl \x01\x02 chars \x7F end\r\nlast line no EOL";
    return st_tv_make(c, ws, sizeof(ws) - 1, enc_ansi(), 0, EOL_CRLF, 0);
  }
  case 4: {
    static const char u8[] = "Cyrillic: \xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82, \xD0\xBC\xD0\xB8\xD1\x80!\n"
                             "Polish: Za\xC5\xBC\xC3\xB3\xC5\x82\xC4\x87 g\xC4\x99\xC5\x9Bl\xC4\x85 ja\xC5\xBA\xC5\x84\n"
                             "Invalid byte: [\xFF] and a cut sequence [\xE2\x82]\n"
                             "CJK: [\xE4\xB8\xAD\xE6\x96\x87] (placeholder when no CJK font)\n"
                             "Combining: e\xCC\x81 a\xCC\x8A  Greek: \xCE\xB1\xCE\xB2\xCE\xB3  Box: \xE2\x94\x8C\xE2\x94\x80\xE2\x94\x90\n"
                             "Emoji (outside the BMP): [\xF0\x9F\x98\x80]  C1 control: [\xC2\x85]\n";
    return st_tv_make(c, u8, sizeof(u8) - 1, enc_utf8(0), 0, EOL_LF, 0);
  }
  default: return 0;
  }
}

static void st_tv_demo(StCtx* c)
{
  char path[300], cur = 0;
  Tab* t = 0;
  DWORD start = GetTickCount();
  char face[32];
  i32 pt = g_app.cfg.font_pt, px = g_app.cfg.font_px;
  es_copy(face, sizeof(face), g_app.cfg.font_face);
  ui_path_join(c->cfg_dir, "tvshow.txt", path, sizeof(path));
  selftest_log("textview demo: waiting for scenes in %s", path);
  while (GetTickCount() - start < 600000) {
    EBytes b;
    char k = 0;
    if (file_read_all(fs_os(), path, &b, 64) == E_OK && b.size()) k = (char)b[0];
    if (!k || k == 'q') break;
    if (k != cur) {
      if (t) { st_tv_close(c, t); t = 0; }
      if (cur == '5') {
        es_copy(g_app.cfg.font_face, sizeof(g_app.cfg.font_face), face);
        g_app.cfg.font_pt = pt; g_app.cfg.font_px = px;
        textview_config_changed(g_app.textview);
      }
      if (cur == '3') g_app.cfg.show_ws = 0;
      cur = k;
      if (k == '5') {
        es_copy(g_app.cfg.font_face, sizeof(g_app.cfg.font_face), "Fixedsys");
        g_app.cfg.font_pt = 9; g_app.cfg.font_px = 0;
        textview_config_changed(g_app.textview);
      }
      if (k == '3') g_app.cfg.show_ws = 1;
      t = st_tv_demo_tab(c, k - '0');
      if (t && k == '2') cmd_view_wrap();
      if (t && k == '6') {
        MarkRange mr[3];
        Document* d = t->doc;
        mr[0].pos = d->line_start(3); mr[0].len = 4;
        mr[1].pos = d->line_start(8); mr[1].len = 6;
        mr[2].pos = d->line_start(12) + 2; mr[2].len = 5;
        t->marks.set(mr, 3);
        t->bookmarks.toggle(5);
        t->bookmarks.toggle(9);
        textview_set_selection(g_app.textview, d->line_start(6) + 3, d->line_start(10) + 7, SCROLL_MIN);
      }
      if (t && (k == '1' || k == '5')) {
        Document* d = t->doc;
        epos p = d->line_start(d->line_count() > 30 ? 30 : 0);
        textview_set_selection(g_app.textview, p, p, SCROLL_NONE);
      }
      if (t) textview_invalidate(g_app.textview);
    }
    st_pump(c, 200);
  }
  if (t) st_tv_close(c, t);
  if (cur == '3') g_app.cfg.show_ws = 0;
  if (cur == '5') {
    es_copy(g_app.cfg.font_face, sizeof(g_app.cfg.font_face), face);
    g_app.cfg.font_pt = pt; g_app.cfg.font_px = px;
    textview_config_changed(g_app.textview);
  }
}
